#pragma once
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include "engine/render/representation_selector.hpp"
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/draw_arguments.hpp"
#include "engine/render/geometry/instanced.hpp"
#include "engine/render/mesh_renderer.hpp"
#include "game/world/scene_scatter.hpp"
#include "game/world/scene_entities.hpp"
#include "game/world/scene_placement.hpp"
#include "game/world/scene_view.hpp"
#include "engine/render/systems/frustum_cull.hpp"
#include "engine/render/systems/cluster_draws.hpp"
#include "engine/render/systems/source_cluster_draws.hpp"
#include "engine/render/geometry/cluster_cull.hpp"
#include "engine/render/geometry/mesh_root_cull.hpp"
#include "engine/render/geometry/instance_root_cull.hpp"
#include "engine/render/geometry/instance_hierarchy_cull.hpp"
#include "engine/render/geometry/hiz_pyramid.hpp"
#include "engine/geometry/cluster_asset.hpp"
#include "engine/geometry/region_mass.hpp"
#include "game/render/forest_proxy_cache.hpp"
#include "game/render/forest_hierarchy.hpp"

namespace engine { struct Scene; class RenderPipeline; }
namespace game {
class GpuTerrain;
class SceneModelsPass final : public engine::DrawPass {
public:
    explicit SceneModelsPass(world::ScenePlacement& source,SDL_GPUTextureSamplerBinding shadow,GpuTerrain* pages=nullptr)
        :source_(source),shadow_(shadow),pages_(pages) {}
    ~SceneModelsPass() override;
    engine::PassPlace setup(engine::Device&,engine::RenderPipeline&) override;
    bool anything(const engine::Frame&) const override;
    void collect(const engine::Frame&,engine::DrawQueue&) override;
    // Builds the selected very-far instance regions before Runner uploads the
    // scene uniforms. Terrain consumes these regions as its density shading
    // representation; no one-frame-late handoff is allowed.
    void prepareDensity(const engine::Scene&, double viewportWidth, engine::Scene&);
    void view(engine::camera::ViewState state);
    void focus(double x,double y) { x_=x;y_=y; }
    void worldBounds(world::terrain::ViewBounds bounds) { worldBounds_=bounds; }
    // Metres of ground beyond which nothing of this pass is drawn: the far
    // forest hierarchy stops there and the fog is opaque there.
    void drawDistance(double metres) { drawDistance_=std::max(500.0,metres); }
    [[nodiscard]] double drawDistance() const { return drawDistance_; }
    // Graphics settings: the 32 m runtime proxies and the far hierarchy.
    void forestOptions(bool proxies,bool far,bool mass=true) { forestProxiesOn_=proxies; farForestOn_=far; massOn_=mass; }
    // Past this distance vegetation is always an impostor; 0 leaves it to the error.
    // Vegetation is a mesh only when at least this many pixels tall AND among
    // the largest that fit `triangles`; everything else is an impostor.
    void vegetationMesh(double pixels,std::size_t triangles) {
        vegetationMeshPixels_=pixels; vegetationTriangleBudget_=triangles;
    }
    // Where the GPU far trees take over (0: they are off and objects run to
    // the draw distance). Placement stops there; placed objects dissolve
    // over `band` metres before it.
    void farTrees(double start,double band) { farTreesStart_=start; farTreesBand_=band; }
    double objectReach() const {
        return farTreesStart_>0 ? std::min(drawDistance_,farTreesStart_) : drawDistance_;
    }
    // Draw the objects as edges. Every representation goes through it - the
    // per-object cluster cut, the impostor cards and the region aggregates -
    // so what a frame is really made of can be looked at rather than inferred.
    void wireframe(bool on) { wireframe_=on && wireframeReady_; }
    [[nodiscard]] bool wireframe() const { return wireframe_; }
    void reset(); // releases only presentation state; jobs belong to the world
    bool ready() const;
#if ASR_ENABLE_PROFILING
    std::string report() const;
#endif
private:
    void updatePlacement(const engine::Scene&, double viewportWidth);
    // The mass batch: whole regions of objects drawn as one instance of one
    // aggregate, baked from those objects rather than from a stand-in asset.
    void updateMassRegions(const engine::render::ScreenScale&, double pixelError);
    // Groups the published objects into the 128 m regions both the aggregate
    // and the terrain density speak in. Once per placement, never per frame.
    void updateRegionMembers();
    [[nodiscard]] int massSlotOf(const world::decor::ScatterBounds&) const;
    // What a bake needs from a model, kept on the processor after upload. A
    // worker holds it by shared_ptr, so a reload cannot pull it out from under
    // a job that is still running.
    struct MassSource {
        std::vector<float> positions;          // three per vertex, model-local
        std::vector<std::uint32_t> indices;    // the finest level
        std::vector<float> layers;             // one per vertex
    };
    // One vertex buffer and one index buffer per model; a level is a range of
    // the second. `errors` are the measured shape errors of those levels in
    // model metres, which is what decides between them on screen.
    struct Model {
        engine::SmartMeshRenderer renderer;
        // Ranges of the ONE index buffer this pass binds, not of a buffer per
        // model: every model's geometry, and the impostor card's, live in one
        // vertex buffer and one index buffer so that the whole pass is a single
        // indirect draw rather than one per model and level.
        std::vector<engine::IndexRange> levels;
        std::vector<float> errors;
        std::int32_t vertexBase=0;
        // The cluster DAG built offline by tools/scene_model_clusters, and
        // where its indices sit in the shared buffer. Empty when the sidecar is
        // missing, which is what every model is until the tool has been run.
        std::vector<engine::geometry::MeshCluster> clusters;
        engine::render::SourceClusterIndex clusterIndex;
        std::uint32_t clusterBase=0;
        std::int32_t clusterVertexBase=0;
        std::uint32_t gpuClusterBase=0;
        std::uint32_t gpuRootClusterOffset=0;
        std::uint32_t gpuRootFamilyOffset=0;
        std::vector<engine::IndexRange> cardLevels;
        std::uint32_t cardBase=0;
        // The crown built offline from this model's own cards, with its own
        // vertices in the shared buffer.
        std::vector<engine::geometry::MeshCluster> crownClusters;
        std::uint32_t crownBase=0;
        std::int32_t crownVertexBase=0;
        // A DAG covers the SOLID part of a model. A model that also has alpha
        // cards cannot be drawn from its clusters alone without losing them, so
        // it keeps the chain until the cards are a range of their own.
        bool clusterReady=false;
        std::uint32_t cardTriangles=0;
        float width=0,height=0;
        int impostor=0;
        bool vegetation=false;
        bool depthImpostor=false;
        int hemisphereImpostor=-1;
        double hemisphereTexel=0;
        // The finest level, kept for baking region aggregates out of the very
        // objects this model is drawn as.
        std::shared_ptr<const MassSource> mass;
        double extent() const { return std::max(width,height); }
    };
    struct Vertex { float position[3],normal[3],uv[2],colour[3],layer,coverage,morph[3],morphNormal[3]; };
    // One resident aggregate. Its geometry lives in this pass's own buffers at
    // a fixed slot, so a region arriving or leaving never moves another's.
    struct MassSlot {
        world::decor::ScatterBounds region{};
        std::vector<engine::geometry::MeshCluster> clusters;
        std::uint32_t indexBase=0;
        std::int32_t vertexBase=0;
        // The tier this slot belongs to, and therefore how much it can hold. A
        // bake may only be filed into a slot of the tier it was requested for.
        std::uint32_t tier=0;
        std::uint32_t vertexCapacity=0;
        std::uint32_t indexCapacity=0;
        float origin[3]{};       // world position the baked vertices are local to
        float centre[3]{};       // world centre of what it covers
        float radius=0;
        float error=0;           // metres the aggregate's finest surface may be out by
        std::size_t members=0;
        std::uint64_t used=0;
        bool live=false;
    };
    struct MassBake {
        world::decor::ScatterBounds region{};
        std::uint64_t revision=0;
        engine::geometry::RegionMass mass;
        float origin[3]{};
        std::size_t members=0;
        std::uint32_t tier=0;
    };
    // `layer` and `layerNext` are the two impostor views the object sits
    // between; `viewBlend` is how far. The shader picks one of them per pixel
    // by a dither rather than blending them, because two silhouettes of a tree
    // averaged together are a ghost, and a dither over an alpha-tested card is
    // the same technique the mesh/card crossover already uses.
    struct Instance { float position[3],scale,yaw,phase,tint,vegetation,width,height,layer,mesh,
                            mode,coverage,layerNext,viewBlend; };
    world::ScenePlacement& source_;
    SDL_GPUTextureSamplerBinding shadow_{};
    GpuTerrain* pages_=nullptr;
    ForestProxyCache forestProxies_;
    ForestHierarchy farForest_;
    double drawDistance_=60000;
    bool forestProxiesOn_=true,farForestOn_=true,massOn_=true;
    double vegetationMeshPixels_=0,vegetationMeshFloor_=0;
    std::size_t vegetationTriangleBudget_=world::decor::kMeshTriangleBudget;
    std::vector<world::decor::MeshDemand> vegetationDemand_;   // reused every frame
    // Per-frame working storage for collect(): cleared each frame, never
    // freed, so steady-state frames allocate nothing here.
    struct FrameScratch {
        std::vector<std::vector<Instance>> batches, clusteredCards;
        std::vector<double> batchPixels, shellAllowances, pixels;
        std::vector<float> meshWeights, detail, hierarchyNearCoverage, complement;
        std::vector<bool> hemisphereAllowed;
        std::vector<char> keep, hierarchyReplaced;
        std::vector<std::unordered_map<int,std::size_t>> cutCost;
        std::vector<std::uint32_t> cutClusters;
        std::vector<Instance> drawn;
        engine::render::GatheredInstances sourceInstances;
        std::vector<engine::render::DrawRun> runs;
        std::vector<Instance> proxyInstances;
        std::vector<engine::render::SourceGeometry> sourceAssets;
        std::vector<engine::MeshRootInstance> gpuRoots;
        std::vector<engine::DrawArguments> arguments;
        std::vector<std::uint32_t> presentModels;
    } scratch_;
    double farTreesStart_=0,farTreesBand_=160;
    // A grove: nine trees merged into one surface offline and clustered, drawn
    // as an instance like anything else. `layer` is the baked impostor it
    // replaces, kept for the model with no shell.
    struct Grove {
        float width=0,height=0;
        int layer=0;
        std::vector<engine::geometry::MeshCluster> clusters;
        std::uint32_t base=0;
        std::int32_t vertexBase=0;
        float shellWidth=0;          // what the shell itself spans, in model units
        std::vector<engine::geometry::MeshCluster> canopyClusters;
        bool shelled() const { return !clusters.empty(); }
    };
    std::vector<Grove> groves_;
    bool detailWanted_=true;
    double detailBlend_=0;
    world::terrain::ViewBounds worldBounds_;
    std::vector<world::decor::ScatterBounds> wantedRegions_;
    world::decor::SceneRegionStats regionQuery_;
    // Members of the published scatter, grouped by the ownership region that
    // an aggregate would replace. Rebuilt only when the placement changes.
    std::map<world::decor::ScatterBounds,std::vector<engine::geometry::RegionMember>> massMembers_;
    // The snapshot the grouping above was built from. Its own tracker: the
    // entity publication consumes `published_` earlier in the same frame.
    const world::ScenePlacementSnapshot* massPublished_=nullptr;
    std::uint64_t massObjectsVersion_=0;
    world::ScenePlacementSnapshot::Revisions massRevisions_;
    std::vector<MassSlot> massSlots_;
    std::vector<std::future<MassBake>> massJobs_;
    std::vector<world::decor::ScatterBounds> massBaking_;
    // A region and the tier it was asked for. The same region is a different
    // question at a different tier, so a refusal or a measured error recorded
    // for one must not answer for another.
    using MassKey=std::pair<world::decor::ScatterBounds,std::uint32_t>;
    // Regions whose aggregate would not fit a slot even after coarsening. Kept
    // so the two workers do not spend the session rebaking them every frame.
    std::set<MassKey> massRefused_;
    // The error a region's aggregate was MEASURED to carry, kept after the slot
    // it lived in has been taken. Without it the estimate below promises the
    // nominal cell every time a region is evicted, so a region whose bake had
    // to coarsen is requested, stored, found too coarse, evicted and requested
    // again for as long as the camera sits still.
    std::map<MassKey,float> massError_;
    std::map<world::decor::ScatterBounds,float> massWeights_; // 0 individuals, 1 aggregate
    // Regions the terrain draws as density instead of objects, and how far the
    // crossover has gone. Filled by prepareDensity, read by collect.
    std::map<world::decor::ScatterBounds,float> densityWeights_;
    // Where the aggregate slots sit in `geometry_`. They are entries of the one
    // geometry table, in the one vertex and index buffer, so an aggregate is a
    // run of the same plan as everything else rather than a command of its own.
    std::uint32_t massGeometryBase_=0;
    std::uint64_t massClock_=0;
    bool massReady_=false;
    // Milliseconds of the frame thread, by phase. Printed with ASR_SCENE_DEBUG.
    double densityCullMs_=0,densitySelectMs_=0,densityHierarchyMs_=0;
    double collectHorizonMs_=0,collectCullMs_=0,collectSelectMs_=0,collectHierarchyMs_=0;
    double collectBudgetMs_=0,collectEmitMs_=0,collectMassMs_=0,collectPlanMs_=0;
#if ASR_ENABLE_DIAGNOSTICS
    std::size_t massDrawn_=0,massReplaced_=0,massRecords_=0;
#endif
    std::size_t proxyCards_=0; // operational budget
#if ASR_ENABLE_DIAGNOSTICS
    std::size_t proxyBlocks_=0,proxyBytes_=0;
    double proxyReach_=0;
#endif
    std::vector<Model> models_;
    // What the render systems are handed: one description per model, built once
    // the catalogue is loaded so the error spans stay put. The cull and the
    // level selection read the same entries, which is what keeps them agreeing
    // about where a tree is and how large it is.
    std::vector<engine::render::MeshDescription> assets_;
    // What the draw planner is handed: one entry per model, then one for the
    // impostor card, all pointing into the shared buffers.
    std::vector<engine::render::MeshGeometry> geometry_;
    // Card-only companion assets used by clustered source runs. Their ranges
    // live in the model's shared index buffer, but they must not ask
    // planDraws() to draw the model's solid DAG a second time.
    std::uint32_t cardGeometryBase_=0;
    engine::Geometry shared_;
    engine::IndexRange cardRange_{0,0};
    std::int32_t cardVertexBase_=0;
    engine::ClusterCuller gpuClusters_;
    engine::MeshRootCuller gpuMeshRoots_;
    engine::Buffer gpuInstances_;
    engine::Buffer gpuMorphData_;
    engine::ComputeSlot gpuGather_=0;
    std::size_t gpuClusterBuckets_=0;
    bool gpuReady_=false;
    engine::InstanceHierarchyCuller gpuHierarchy_;
    engine::InstanceRootCuller gpuHierarchyRoots_;
    engine::Buffer gpuHierarchyInstances_;
    engine::ComputeSlot gpuHierarchyGather_=0;
    std::size_t gpuHierarchyBuckets_=0;
    bool gpuHierarchyReady_=false;
    engine::HiZPyramid hiz_;
    engine::RenderPipeline* pipeline_=nullptr;
    // Held for the mass batch only: a region aggregate is baked while the game
    // runs and has to reach the card between frames.
    engine::Device* device_=nullptr;
    // The scatter as entities. Republished when the region changes, walked by
    // the render systems every frame, and the only place a drawn instance's
    // identity lives.
    engine::ecs::Registry entities_;
    const world::ScenePlacementSnapshot* published_=nullptr;
    std::uint64_t publishedObjectsVersion_=0;
    // The published objects as flat arrays, gathered once. Walking the registry
    // and rebuilding these every frame cost milliseconds for geometry that does
    // not move: a tree is where the scatter put it until the scatter changes.
    engine::render::GatheredInstances gathered_;
    engine::Texture colours_,normals_,depths_;
    engine::Sampler sampler_,depthSampler_;
    engine::MeshRenderer renderer_;
    engine::MeshRenderer wireframeRenderer_;
    bool wireframe_=false,wireframeReady_=false;
    std::shared_ptr<const world::ScenePlacementSnapshot> placement_;
    // Rebuilt every frame from where the eye is; kept as a member only so its
    // two kilobytes are not allocated and thrown away sixty times a second.
    engine::render::Horizon horizon_;
    double x_=0,y_=0;
    engine::camera::ViewState view_;
    bool viewReady_=false;
    std::chrono::steady_clock::time_point viewTime_{};
    struct RepresentationHistory {
        std::size_t candidate=engine::render::kNoRepresentation;
        // Negative: never selected yet. A new entry starts AT its target
        // representation - it used to start as a full mesh and dissolve to
        // its impostor, so every placement publication (which resets this
        // table) and every tree coming into view drew the whole forest as
        // meshes for 0.4 s: the burst of triangles while moving.
        float mesh=-1;
        double lastUse=0;
    };
    std::unordered_map<std::uint32_t,RepresentationHistory> representationHistory_;
    double selectionTime_=0;
    double meshStartPixels_=22;
    bool enabled_=false,failed_=false;
#if ASR_ENABLE_DIAGNOSTICS
    std::size_t meshes_=0,cards_=0,culled_=0,draws_=0,triangles_=0,meshTriangles_=0,geometryBytes_=0;
    std::size_t indirectDraws_=0,clusterDraws_=0,cardDraws_=0,crownDraws_=0,outside_=0,faded_=0;
#endif
    // The longest chain in the catalogue. Batches are one per model and level,
    // so a model with fewer levels simply leaves its tail empty.
    std::size_t levels_=1;
#if ASR_ENABLE_DIAGNOSTICS
    std::vector<std::size_t> levelInstances_,levelTriangles_;
#endif
};
} // namespace game
