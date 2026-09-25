#include "game/render/passes/scene_models_pass.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/gpu_terrain.hpp"
#include "game/render/world_materials.hpp"
#include "engine/render/systems/instance_hierarchy_select.hpp"
#include "engine/geometry/cluster_morph.hpp"
#include "engine/core/diagnostics.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace game {
namespace {
using Json=nlohmann::json;
void require(bool value,const char* why) { if (!value) throw std::runtime_error(why); }
double fade(double a,double b,double x) { const double t=std::clamp((x-a)/(b-a),0.0,1.0);return t*t*(3-2*t); }
constexpr std::size_t kGpuClusterCapacity=262144;
constexpr std::size_t kGpuClusterInstanceCapacity=2048;
// The mass batch. A slot is one region's aggregate, and the capacity is
// measured rather than guessed: a 128 m region of 300 trees merges to 3425
// vertices and 35712 indices at a four-metre cell, every level of its DAG
// included (tests/test_region_mass.cpp prints the table).
//
// The pool is TIERED, for the same reason the mesh inside it is: a region four
// times further away is inside the same pixel error at four times the cell, and
// a bake at that cell is an order of magnitude smaller.
//
// What the tiers must not do is take slots from the near band. Measured in a
// real orbital frame, the 8 m and 16 m tiers were never chosen at all - a
// region only reaches them once it is about a dozen pixels across, and by then
// it is past the horizon or behind the ground. Spreading the slots evenly
// therefore made the pool cover FEWER regions than the flat pool it replaced.
// The finest tier keeps the whole of that pool; the coarse tiers are extra.
constexpr engine::geometry::RegionMassTier kMassTiers[]{
    {4.0, 48, 6144, 65536},
    {8.0, 64, 1536, 16384},
    {16.0, 64, 768, 8192},
};
constexpr std::uint32_t kMassSlots=48+64+64;
// How coarse an aggregate is allowed to be, and therefore how near it may be
// used: its surface is two cells out, so a four-metre cell is within the three
// pixel error once a region is about fifty pixels across. Finer cells merge
// trees that have not visually merged yet and cost far more than they save -
// at 2.5 m the same region is 124k indices and does not fit a slot at all.
// Below this a region is cheaper and more honest as its own objects.
constexpr std::size_t kMassMinimumMembers=12;
// Bakes are the bottleneck once the pool is large: an orbital frame kept both
// workers busy for its whole run while slots sat empty. Bounded, because these
// compete with the placement pool for the same cores.
inline std::size_t massJobLimit() {
    const auto cores=std::size_t(std::max(1u,std::thread::hardware_concurrency()));
    return std::clamp<std::size_t>(cores/3,2,4);
}
// Where a frame's object time actually goes, printed beside the scene debug
// line. Guessing at this produced two full culls per frame that nobody noticed.
struct PhaseClock {
    std::chrono::steady_clock::time_point mark=std::chrono::steady_clock::now();
    double lap() {
        const auto now=std::chrono::steady_clock::now();
        const double ms=std::chrono::duration<double,std::milli>(now-mark).count();
        mark=now;
        return ms;
    }
};
constexpr auto kSpatialRepresentations=
    engine::render::representationBit(engine::geometry::InstanceRepresentation::Individual) |
    engine::render::representationBit(engine::geometry::InstanceRepresentation::DensityShading);
}
SceneModelsPass::~SceneModelsPass() { reset(); }
void SceneModelsPass::reset() {
    placement_.reset();failed_=false;
    detailBlend_=0;detailWanted_=true;
    wantedRegions_.clear();
    massMembers_.clear();massWeights_.clear();massPublished_=nullptr;
    massRefused_.clear();massError_.clear();
    for (auto& slot:massSlots_) slot.live=false;
    entities_.clear();published_=nullptr;
    gathered_={};
}
void SceneModelsPass::updatePlacement(const engine::Scene& scene,double viewportWidth) {
    world::decor::SceneView view;
    std::copy_n(scene.viewProjection,16,view.matrix.begin());
    view.world=worldBounds_;view.viewportWidth=viewportWidth;
    view.pixelsPerMetre=scene.camera[3];
    for (const auto& model:models_) view.maxExtent=std::max(view.maxExtent,model.extent()*1.4);
    const bool perspective=view.matrix[12]!=0 || view.matrix[13]!=0 || view.matrix[14]!=0;
    view.priorityX=perspective?scene.camera[0]:x_;
    view.priorityY=perspective?scene.camera[1]:y_;
    wantedRegions_=scene.extra[2]>0.01f?world::decor::visibleSceneRegions(view):
                                       std::vector<world::decor::ScatterBounds>{};
    detailWanted_=!wantedRegions_.empty();
    source_.updateRegions(wantedRegions_,true);
    failed_=!source_.error().empty();
    placement_=source_.read();
}
bool SceneModelsPass::ready() const {
    return enabled_ && !failed_ && (!detailWanted_ || (!source_.busy() && detailBlend_>=0.99 &&
        placement_ && placement_->complete));
}
#if ASR_ENABLE_PROFILING
std::string SceneModelsPass::report() const {
    static const world::decor::Scatter empty;
    const auto& scatter_ = placement_ ? placement_->scatter : empty;
    return Json{{"ready",ready()},{"enabled",enabled_},{"failed",failed_},
        {"objects",scatter_.objects.size()},{"mesh_instances",meshes_},{"impostor_instances",cards_},
        {"culled",culled_},{"culled_outside_view",outside_},{"culled_faded",faded_},
        {"draws",draws_},{"triangles",triangles_},
        // One recorded command, and how many draws the card reads out of it.
        // The second number is the interesting one: how much of the frame came
        // from a cluster cut rather than from a whole level of the chain.
        {"indirect_draws",indirectDraws_},{"cluster_draws",clusterDraws_},
        {"card_draws",cardDraws_},{"crown_draws",crownDraws_},
        {"cluster_models",[this]{
            std::vector<std::string> ready;
            for (std::size_t i=0;i<models_.size();++i)
                if (models_[i].clusterReady) ready.push_back(world::decor::kModels[i]);
            return ready;
        }()},
        {"gpu_cluster_selection",gpuReady_},
        {"gpu_cluster_buckets",gpuClusterBuckets_},
        {"mesh_triangles",meshTriangles_},{"mesh_triangle_budget",world::decor::kMeshTriangleBudget},
        {"populations",scatter_.populations},{"candidate_spacing_m",world::decor::kCell},
        {"shared_geometry_bytes",geometryBytes_},{"instance_upload_bytes",(meshes_+cards_)*sizeof(Instance)},
        {"sampled_sites",scatter_.sampled},{"water_tiles_skipped",scatter_.waterTilesSkipped},
        {"focus_m",{x_,y_}},{"requested_regions",wantedRegions_.size()},
        {"resident_regions",placement_?placement_->regions.size():0},
        {"detail_requested",detailWanted_},{"detail_blend",detailBlend_},
        {"grove_impostors",proxyCards_},{"grove_blocks",proxyBlocks_},{"grove_max_focus_distance_m",proxyReach_},
        {"grove_candidate_step_m",0},{"grove_budget",32768},{"grove_root_bytes",proxyBytes_},
        {"view_wide_vegetation",worldBounds_.maxX>worldBounds_.minX},
        {"lod_mesh_pixels",{meshStartPixels_,meshStartPixels_*(38.0/22)}},{"impostor_views",8},
        // Per level of the shared chain, finest first: how many objects were
        // drawn at it and what that cost. A frame that spends everything on
        // level zero is the retopology not working, whatever the total says.
        {"mesh_level_instances",levelInstances_},{"mesh_level_triangles",levelTriangles_},
        {"mesh_level_pixel_error",world::decor::kLevelPixelError},
        {"mesh_level_triangle_counts",[this] {
            std::vector<std::vector<std::size_t>> chains;
            for (const auto& model:models_) {
                chains.emplace_back();
                for (const auto& level:model.levels) chains.back().push_back(level.count/3);
            }
            return chains;
        }()}}.dump(2);
}
#endif
engine::PassPlace SceneModelsPass::setup(engine::Device& device,engine::RenderPipeline& into) {
    pipeline_=&into;
    device_=&device;
    const auto root=device.assets().parent_path()/"generated/scene_models";
    const auto place=engine::PassPlace{engine::passOf(Pass::Models),engine::stageOf(Stage::World),
                                     static_cast<engine::PassOrder>(Order::Opaque)};
    if (!std::filesystem::exists(root/"manifest.json")) {
        std::cerr<<"Scene models unavailable: run tools/prepare_scene_models.py ("<<root<<")\n";
        return place; // explicit optional content, never pretend that grass cards are models
    }
    try {
        require(std::endian::native==std::endian::little,"scene mesh endian not supported");
        std::ifstream input(root/"manifest.json");Json content;input>>content;
        require(content.at("version")==2 && content.at("views")==8,
            "rebuild scene assets: the manifest predates mesh level chains");
        require(content.at("models").size()==world::decor::kModels.size(),"scene model catalogue mismatch");
        const auto safePath=[&](const std::string& file) {
            const auto p=std::filesystem::path(file);
            require(p==p.filename() && file!="." && file!="..","invalid model resource path");return root/p;
        };
        std::vector<std::filesystem::path> colourPaths,normalPaths;
        for (const auto& p:content.at("colours")) colourPaths.push_back(safePath(p.get<std::string>()));
        for (const auto& p:content.at("normals")) normalPaths.push_back(safePath(p.get<std::string>()));
        require(!colourPaths.empty() && colourPaths.size()==normalPaths.size() && colourPaths.size()<=128,"invalid model layers");
        std::vector<std::vector<std::filesystem::path>> colourMips,normalMips;
        for (const auto& p:colourPaths) colourMips.push_back({p});
        for (const auto& p:normalPaths) normalMips.push_back({p});
        colours_=device.loadArrayMipped(colourMips,true);normals_=device.loadArrayMipped(normalMips,true);
        if (!colours_ || !normals_) return {};
        engine::Device::Uploader upload(device);
        // One vertex buffer and one index buffer for the whole catalogue, the
        // impostor card included. Every model's indices are offset into them,
        // which is what lets the pass end as a single indirect draw instead of
        // one per model and level.
        std::vector<Vertex> allVertices;
        std::vector<std::uint32_t> allIndices;
        for (const auto& m:content.at("models")) {
            require(m.at("name")==world::decor::kModels[models_.size()],"unexpected scene model ordering");
            Model model;model.width=m.at("width");model.height=m.at("height");
            model.impostor=m.at("impostor");model.vegetation=m.at("vegetation");
            require(std::isfinite(model.width+model.height) && model.width>0 && model.height>0 &&
                model.width<100 && model.height<100 && model.impostor>=0 &&
                std::size_t(model.impostor+8)<=colourPaths.size(),"invalid model bounds/layers");
            std::ifstream mesh(safePath(m.at("mesh").get<std::string>()),std::ios::binary);
            char magic[4]{};std::uint32_t vertices=0,count=0;
            mesh.read(magic,4);mesh.read(reinterpret_cast<char*>(&vertices),4);mesh.read(reinterpret_cast<char*>(&count),4);
            require(std::string(magic,4)=="SCM2" && vertices>0 && vertices<=200000 &&
                count>0 && count<=8,"invalid scene mesh header");
            require(m.at("levels").size()==count,"scene mesh and manifest disagree on levels");
            std::uint32_t indices=0;
            for (std::uint32_t level=0;level<count;++level) {
                std::uint32_t span=0;mesh.read(reinterpret_cast<char*>(&span),4);
                require(bool(mesh) && span>0 && span%3==0 && span<=600000,"invalid scene mesh level");
                require(model.levels.empty() || span<model.levels.back().count,
                    "scene mesh levels must grow coarser"); // else one is dead weight
                model.levels.push_back({indices,span});
                indices+=span;
            }
            require(indices<=900000,"scene mesh level chain too large");
            // The file's vertex is twelve floats; ours adds coverage and a
            // three-float cluster morph target.
            // not something a modeller made - it is one for everything the
            // importer produced and less than one only on a crown, which is
            // built here rather than shipped in the mesh.
            constexpr std::size_t kFileFloats=12;
            static_assert(sizeof(Vertex)==(kFileFloats+7)*sizeof(float));
            std::vector<float> raw(std::size_t(vertices)*kFileFloats);
            std::vector<Vertex> v(vertices);std::vector<std::uint32_t> ix(indices);
            mesh.read(reinterpret_cast<char*>(raw.data()),std::streamsize(raw.size()*sizeof(float)));
            for (std::uint32_t at=0;at<vertices;++at) {
                std::memcpy(&v[at],&raw[std::size_t(at)*kFileFloats],kFileFloats*sizeof(float));
                v[at].coverage=1;
                std::copy_n(v[at].position,3,v[at].morph);
                std::copy_n(v[at].normal,3,v[at].morphNormal);
            }
            mesh.read(reinterpret_cast<char*>(ix.data()),std::streamsize(ix.size()*4));
            require(bool(mesh) && mesh.peek()==std::char_traits<char>::eof(),"scene mesh size mismatch");
            for (const auto i:ix) require(i<vertices,"invalid scene mesh index");
            // The chain is only usable if its errors rise with its coarseness:
            // the renderer stops at the first level that is too coarse for the
            // size on screen, and an unordered chain would stop at the wrong one.
            for (const auto& level:m.at("levels")) {
                const double error=level.at("error_m");
                require(std::isfinite(error) && error>=0 && error<100 &&
                    (model.errors.empty() || error>=model.errors.back()),"invalid scene mesh level error");
                model.errors.push_back(float(error));
            }
            require(model.errors.front()==0,"the finest scene mesh level is the model itself");
            require(model.levels.front().count/3==m.at("triangles"),"scene mesh triangle count mismatch");
            for (const auto& a:v) {
                for (float p:a.position) require(std::isfinite(p) && std::abs(p)<=100,"invalid model position");
                for (float p:a.normal) require(std::isfinite(p),"invalid model normal");
                for (float p:a.uv) require(std::isfinite(p),"invalid model UV");
                for (float p:a.colour) require(std::isfinite(p),"invalid model colour");
                require(a.layer>=0 && a.layer<colourPaths.size() && std::floor(a.layer)==a.layer,"invalid model texture index");
            }
            model.renderer.hierarchy=std::make_shared<const engine::SmartMesh>(engine::SmartMesh::chain(
                model.levels,model.errors,indices,engine::SmartMesh::ErrorMetric::EstimatedSurfaceDistance));
            model.renderer.pixelError=world::decor::kLevelPixelError;
            model.vertexBase=std::int32_t(allVertices.size());
            const auto indexBase=std::uint32_t(allIndices.size());
            // Keep the finest level on the processor: a region aggregate is
            // baked from the objects themselves, and the objects are these.
            {
                auto source=std::make_shared<MassSource>();
                source->positions.resize(std::size_t(vertices)*3);
                source->layers.resize(vertices);
                for (std::uint32_t at=0;at<vertices;++at) {
                    std::copy_n(v[at].position,3,&source->positions[std::size_t(at)*3]);
                    source->layers[at]=v[at].layer;
                }
                const auto& finest=model.levels.front();
                source->indices.assign(ix.begin()+finest.first,
                                       ix.begin()+finest.first+finest.count);
                model.mass=std::move(source);
            }
            allVertices.insert(allVertices.end(),v.begin(),v.end());
            allIndices.insert(allIndices.end(),ix.begin(),ix.end());
            for (auto& level:model.levels) level.first+=indexBase;

            // The cluster DAG, if tools/scene_model_clusters has been over this
            // model. Its absence is not an error: the chain is what every model
            // has, and a sidecar only ever adds.
            const auto sidecar=root/(m.at("name").get<std::string>()+".clusters");
            if (std::filesystem::exists(sidecar)) {
                std::ifstream file(sidecar,std::ios::binary);
                const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                            std::istreambuf_iterator<char>());
                const std::span<const std::byte> bytes(
                        reinterpret_cast<const std::byte*>(raw.data()),raw.size());
                std::string why;
                auto asset=engine::geometry::decodeClusters(bytes,why);
                // Refused rather than trusted: a sidecar built from a different
                // mesh would index vertices that moved, which draws rubbish
                // rather than nothing.
                require(asset.empty() || (asset.sourceVertices==vertices &&
                    asset.sourceTriangles==model.levels.front().count/3),
                    "cluster sidecar does not belong to this mesh - rebuild it");
                if (!asset.empty()) {
                    if (!asset.clusters.empty()) {
                        std::vector<float> sourcePositions(std::size_t(vertices)*3);
                        for (std::uint32_t at=0;at<vertices;++at)
                            std::copy_n(v[at].position,3,&sourcePositions[std::size_t(at)*3]);
                        const auto morph=engine::geometry::buildClusterMorph(
                            sourcePositions,
                            asset.indices,asset.clusters,asset.clusterPositions);
                        require(!morph.empty(),"cluster sidecar could not build morph correspondence");
                        model.clusterVertexBase=std::int32_t(allVertices.size());
                        model.clusterBase=std::uint32_t(allIndices.size());
                        std::vector<Vertex> morphVertices;
                        morphVertices.reserve(morph.sourceVertices.size());
                        for (std::size_t at=0;at<morph.sourceVertices.size();++at) {
                            Vertex vertex=v[morph.sourceVertices[at]];
                            std::copy_n(&morph.targetPositions[at*3],3,vertex.morph);
                            const auto* targetNormal=&morph.targetNormals[at*3];
                            if (std::abs(targetNormal[0])+std::abs(targetNormal[1])+std::abs(targetNormal[2])>
                                1e-6f)
                                std::copy_n(targetNormal,3,vertex.morphNormal);
                            morphVertices.push_back(vertex);
                        }
                        allVertices.insert(allVertices.end(),morphVertices.begin(),morphVertices.end());
                        allIndices.insert(allIndices.end(),morph.indices.begin(),morph.indices.end());
                    }
                    // The leaves the DAG does not cover, one range per level of
                    // the chain, so a clustered tree still has leaves and they
                    // are the ones the offline thinning reduced for that
                    // distance rather than all of them at every distance.
                    model.cardBase=std::uint32_t(allIndices.size());
                    allIndices.insert(allIndices.end(),asset.cardIndices.begin(),
                                      asset.cardIndices.end());
                    model.cardLevels=std::move(asset.cardLevels);
                    model.clusters=std::move(asset.clusters);
                    model.cardTriangles=asset.cardTriangles;
                    model.clusterReady=!model.clusters.empty();
                    if (model.clusterReady) model.clusterIndex=engine::render::SourceClusterIndex(model.clusters);
                    // The crown comes with its own vertices - a shell of a leaf
                    // mass is not made of the model's - so they join the shared
                    // buffer beside them. Its texture coordinate is read off
                    // the surface normal: at the distance a crown is used, the
                    // foliage texture wrapped over the shell is what a viewer
                    // sees, and where exactly a leaf lands on it is not.
                    if (!asset.crownClusters.empty()) {
                        model.crownVertexBase=std::int32_t(allVertices.size());
                        const auto crownVertices=asset.crownPositions.size()/3;
                        for (std::size_t at=0;at<crownVertices;++at) {
                            Vertex vertex{};
                            for (int axis=0;axis<3;++axis) {
                                vertex.position[axis]=asset.crownPositions[at*3+axis];
                                vertex.normal[axis]=asset.crownNormals[at*3+axis];
                                vertex.colour[axis]=1;
                            }
                            const double nx=vertex.normal[0],ny=vertex.normal[1],nz=vertex.normal[2];
                            vertex.uv[0]=float(std::atan2(ny,nx)*(1/6.2831853)*4);
                            vertex.uv[1]=float(std::acos(std::clamp(nz,-1.0,1.0))*(1/3.14159265)*4);
                            // Per vertex, so the trunk inside the shell keeps
                            // its bark instead of wearing the canopy's leaves.
                            vertex.layer=at<asset.crownLayers.size()?asset.crownLayers[at]
                                                                    :asset.crownLayer;
                            vertex.coverage=at<asset.crownCoverage.size()?asset.crownCoverage[at]:1;
                            allVertices.push_back(vertex);
                        }
                        model.crownBase=std::uint32_t(allIndices.size());
                        allIndices.insert(allIndices.end(),asset.crownIndices.begin(),
                                          asset.crownIndices.end());
                        model.crownClusters=std::move(asset.crownClusters);
                        ASR_DIAGNOSTIC(std::cout<<"  "<<m.at("name").get<std::string>()
                            <<": crown of "<<asset.crownIndices.size()/3<<" triangles in "
                            <<model.crownClusters.size()<<" clusters\n");
                    }
                } else if (!why.empty()) {
                    std::cerr<<"scene models: ignoring "<<sidecar.filename()<<": "<<why<<'\n';
                }
            }
            ASR_DIAGNOSTIC(geometryBytes_+=v.size()*sizeof(Vertex)+ix.size()*4);
            levels_=std::max(levels_,model.levels.size());
            models_.push_back(std::move(model));
        }
        require(content.contains("groves") && content.at("groves").size()==2,"rebuild scene assets: missing grove impostors");
        for (const auto& g:content.at("groves")) {
            Grove grove;
            grove.width=g.at("width");grove.height=g.at("height");grove.layer=g.at("impostor");
            require(g.at("model")==groves_.size() && std::isfinite(grove.width+grove.height) &&
                grove.width>0 && grove.width<128 && grove.height>0 && grove.height<100 &&
                grove.layer>=0 && std::size_t(grove.layer+8)<=colourPaths.size(),"invalid grove impostor");
            // The shell built by tools/scene_model_clusters, if it is there.
            // Its vertices are its own and join the shared buffer beside
            // everything else, so a grove is an instance like any other.
            const auto which=g.at("model").get<std::size_t>();
            if (which<world::decor::kModels.size()) {
                const auto sidecar=root/(std::string(world::decor::kModels[which])+"-grove.clusters");
                if (std::filesystem::exists(sidecar)) {
                    std::ifstream file(sidecar,std::ios::binary);
                    const std::vector<char> raw((std::istreambuf_iterator<char>(file)),
                                                std::istreambuf_iterator<char>());
                    std::string why;
                    auto shell=engine::geometry::decodeClusters(
                        std::span<const std::byte>(reinterpret_cast<const std::byte*>(raw.data()),
                                                   raw.size()),why);
                    if (!shell.crownClusters.empty()) {
                        grove.vertexBase=std::int32_t(allVertices.size());
                        const auto count=shell.crownPositions.size()/3;
                        double low[3]{1e30,1e30,1e30},high[3]{-1e30,-1e30,-1e30};
                        for (std::size_t at=0;at<count;++at) {
                            Vertex vertex{};
                            for (int axis=0;axis<3;++axis) {
                                vertex.position[axis]=shell.crownPositions[at*3+axis];
                                vertex.normal[axis]=shell.crownNormals[at*3+axis];
                                vertex.colour[axis]=1;
                                low[axis]=std::min(low[axis],double(vertex.position[axis]));
                                high[axis]=std::max(high[axis],double(vertex.position[axis]));
                            }
                            const double nx=vertex.normal[0],ny=vertex.normal[1],nz=vertex.normal[2];
                            vertex.uv[0]=float(std::atan2(ny,nx)*(1/6.2831853)*4);
                            vertex.uv[1]=float(std::acos(std::clamp(nz,-1.0,1.0))*(1/3.14159265)*4);
                            vertex.layer=at<shell.crownLayers.size()?shell.crownLayers[at]
                                                                    :shell.crownLayer;
                            vertex.coverage=at<shell.crownCoverage.size()?shell.crownCoverage[at]:1;
                            allVertices.push_back(vertex);
                        }
                        grove.base=std::uint32_t(allIndices.size());
                        allIndices.insert(allIndices.end(),shell.crownIndices.begin(),
                                          shell.crownIndices.end());
                        grove.clusters=std::move(shell.crownClusters);
                        std::uint32_t coarsestLevel=0;
                        for (const auto& cluster:grove.clusters)
                            coarsestLevel=std::max(coarsestLevel,cluster.level);
                        for (const auto& cluster:grove.clusters)
                            if (cluster.level==coarsestLevel) {
                                auto proxy=cluster;
                                proxy.level=0;
                                proxy.bornOf=engine::geometry::MeshCluster::kNoGroup;
                                proxy.replacedBy=engine::geometry::MeshCluster::kNoGroup;
                                proxy.parentError=std::numeric_limits<float>::infinity();
                                grove.canopyClusters.push_back(proxy);
                            }
                        grove.shellWidth=float(std::max(high[0]-low[0],high[1]-low[1]));
                        ASR_DIAGNOSTIC(std::cout<<"  grove "<<world::decor::kModels[which]
                            <<": "<<shell.crownIndices.size()/3<<" triangles in "
                            <<grove.clusters.size()<<" clusters\n");
                    } else if (!why.empty()) {
                        std::cerr<<"scene models: ignoring "<<sidecar.filename()<<": "<<why<<'\n';
                    }
                }
            }
            groves_.push_back(grove);
        }
        const auto cardVertex=[](float x,float z,float u,float v) {
            Vertex vertex{};
            vertex.position[0]=x;vertex.position[2]=z;
            vertex.normal[1]=-1;
            vertex.uv[0]=u;vertex.uv[1]=v;
            std::fill_n(vertex.colour,3,1.0f);
            vertex.coverage=1;
            std::copy_n(vertex.position,3,vertex.morph);
            std::copy_n(vertex.normal,3,vertex.morphNormal);
            return vertex;
        };
        const Vertex quad[]{cardVertex(-.5f,0,0,1),cardVertex(.5f,0,1,1),
                            cardVertex(-.5f,1,0,0),cardVertex(.5f,1,1,0)};
        // The impostor card joins the same buffers. It is the same vertex and
        // the same material - the shader tells a card from a mesh by a number
        // on the instance - so keeping it apart would cost the pass its single
        // draw for nothing.
        cardVertexBase_=std::int32_t(allVertices.size());
        allVertices.insert(allVertices.end(),std::begin(quad),std::end(quad));
        cardRange_={std::uint32_t(allIndices.size()),6};
        for (const std::uint32_t index:{0u,1u,2u,2u,1u,3u}) allIndices.push_back(index);

        // The aggregate pool joins the SAME buffers, reserved blank at the end
        // and written into while the game runs. It used to have buffers of its
        // own, and that alone cost the frame a second recorded command: a draw
        // can only be one vertex buffer and one index buffer, so geometry kept
        // apart is a batch kept apart however identical its material. Reserved
        // rather than grown, because a growing buffer would move every model's
        // vertexBase under the draws that already name it.
        //
        // Fixed slots for the same reason at region scale: one region arriving
        // never moves another's geometry.
        massSlots_.clear();
        massSlots_.reserve(kMassSlots);
        // The measured table and the one reserved here are the same table. The
        // test bakes real regions against the first; a pool built from a second
        // copy that had drifted would refuse regions with nothing watching.
        {
            const auto measured=engine::geometry::defaultRegionMassTiers();
            bool same=measured.size()==std::size(kMassTiers);
            for (std::size_t at=0;same && at<measured.size();++at)
                same=measured[at].cellMetres==kMassTiers[at].cellMetres &&
                     measured[at].slots==kMassTiers[at].slots &&
                     measured[at].vertices==kMassTiers[at].vertices &&
                     measured[at].indices==kMassTiers[at].indices;
            require(same,"mass tier table does not match the measured one");
        }
        for (std::uint32_t tier=0;tier<std::size(kMassTiers);++tier)
            for (std::uint32_t at=0;at<kMassTiers[tier].slots;++at) {
                MassSlot slot;
                slot.tier=tier;
                slot.vertexCapacity=kMassTiers[tier].vertices;
                slot.indexCapacity=kMassTiers[tier].indices;
                slot.vertexBase=std::int32_t(allVertices.size());
                slot.indexBase=std::uint32_t(allIndices.size());
                allVertices.resize(allVertices.size()+slot.vertexCapacity);
                allIndices.resize(allIndices.size()+slot.indexCapacity,0);
                massSlots_.push_back(std::move(slot));
            }
        require(massSlots_.size()==kMassSlots,"mass slot pool is inconsistent");

        require(allVertices.size()<=4000000 && allIndices.size()<=16000000,"scene geometry too large");
        shared_.vertices=upload.add(SDL_GPU_BUFFERUSAGE_VERTEX,allVertices.data(),
                                    allVertices.size()*sizeof(Vertex));
        shared_.indices=upload.add(SDL_GPU_BUFFERUSAGE_INDEX,allIndices.data(),allIndices.size()*4);
        shared_.indexSize=SDL_GPU_INDEXELEMENTSIZE_32BIT;
        shared_.indexCount=std::uint32_t(allIndices.size());
        if (!upload.finish() || !shared_ || !shared_.indices) return {};
        // One description, in world_materials.hpp, so a test can build this
        // pipeline from exactly what the frame builds it from.
        static_assert(sizeof(Vertex)==materials::SceneModelStreams::kVertexBytes);
        static_assert(sizeof(Instance)==materials::SceneModelStreams::kInstanceBytes);
        static_assert(offsetof(Instance,mode)==12*sizeof(float));
        const auto wanted=materials::sceneModelLayout();
        if (!renderer_.material.setup(device,into,materials::sceneModels(),wanted)) return {};
        // The same shader and the same streams, rasterised as edges. Built
        // beside the solid one so switching to it costs nothing at the moment
        // it is asked for.
        {
            auto lines=std::make_shared<engine::Material>(*materials::sceneModels());
            lines->name="nature/mesh-and-impostor/wireframe";
            lines->wireframe=true;
            lines->blend=false;
            wireframeReady_=wireframeRenderer_.material.setup(device,into,std::move(lines),wanted);
            if (!wireframeReady_)
                std::cerr<<"scene models: wireframe pipeline unavailable: "<<device.error()<<'\n';
        }
        if (wireframeReady_ && std::getenv("ASR_SCENE_WIREFRAME")) wireframe_=true;
        SDL_GPUSamplerCreateInfo sampler{};sampler.min_filter=sampler.mag_filter=SDL_GPU_FILTER_LINEAR;
        sampler.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sampler.address_mode_u=sampler.address_mode_v=sampler.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler_=device.makeSampler(sampler);if (!sampler_) return {};
        renderer_.material.textures({{colours_.get(),sampler_.get()},{normals_.get(),sampler_.get()}});
        for (auto& model:models_) model.renderer.material=renderer_.material;
        // Built after the catalogue is complete: the error spans point into
        // models_, which must not grow again once a system holds one.
        for (const auto& model:models_)
            assets_.push_back({model.errors,model.extent(),0,
                {std::hypot(model.width,model.height)*0.6,model.height*0.5}});
        for (const auto& model:models_)
            geometry_.push_back({model.vertexBase,model.levels,
                model.clusterReady?std::span<const engine::geometry::MeshCluster>(model.clusters)
                                  :std::span<const engine::geometry::MeshCluster>(),
                model.clusterBase,model.cardLevels,model.cardBase,
                model.crownClusters,model.crownBase,model.crownVertexBase});
        geometry_.push_back({cardVertexBase_,std::span(&cardRange_,1),{},0,{},0,{},0,0});
        for (const auto& grove:groves_) {
            geometry_.push_back({0,{},{},0,{},0,grove.clusters,grove.base,grove.vertexBase});
            geometry_.push_back({grove.vertexBase,{},grove.canopyClusters,grove.base,{},0,{},0,0});
        }
        // Append experimental source-card companions only after the stable
        // model/impostor/grove slots. Runtime batching addresses those slots by
        // position; inserting here used to make the baked impostor draw model
        // zero's source indices as a normalized billboard.
        cardGeometryBase_=std::uint32_t(geometry_.size());
        for (const auto& model:models_)
            geometry_.push_back({model.vertexBase,{}, {},0,model.cardLevels,model.cardBase,{},0,0});
        require(geometry_.size()==cardGeometryBase_+models_.size() &&
                cardGeometryBase_==models_.size()+1+groves_.size()*2 &&
                geometry_[models_.size()].vertexBase==cardVertexBase_,
                "scene geometry slot layout is inconsistent");
        // The aggregate slots are entries of this same table. An empty entry
        // draws nothing, which is what an unfilled slot should do.
        massGeometryBase_=std::uint32_t(geometry_.size());
        geometry_.resize(geometry_.size()+massSlots_.size());
        require(geometry_.size()==massGeometryBase_+kMassSlots,
                "mass geometry slot layout is inconsistent");
        // Source clusters share one GPU bucket per mesh cluster. The visible
        // list stores source-instance indices; a compute gather turns those
        // indices into the instance stream consumed by the normal model
        // vertex shader. A bounded bucket is deliberate: a pathological frame
        // falls back to the already-tested CPU plan instead of allocating an
        // unbounded visibility buffer.
        std::vector<engine::MeshStaticCluster> gpuRootClusters;
        std::vector<engine::MeshFamilyRange> gpuRootFamilies;
        std::vector<std::uint32_t> gpuRootFamilyChildren;
        for (auto& model:models_) {
            model.gpuClusterBase=std::uint32_t(gpuClusterBuckets_);
            model.gpuRootClusterOffset=std::uint32_t(gpuRootClusters.size());
            model.gpuRootFamilyOffset=std::uint32_t(gpuRootFamilies.size());
            gpuClusterBuckets_+=model.clusters.size();
            for (std::size_t id=0;id<model.clusters.size();++id) {
                const auto& cluster=model.clusters[id];
                engine::MeshStaticCluster root;
                std::copy_n(cluster.centre,3,root.bounds.centre);
                root.bounds.radius=cluster.radius;
                root.bounds.error=cluster.error;
                root.bounds.parentError=cluster.parentError;
                root.bounds.bucket=std::uint32_t(id);
                root.bornOf=cluster.bornOf;
                root.replacedBy=cluster.replacedBy;
                gpuRootClusters.push_back(root);
            }
            std::uint32_t familyCount=0;
            for (const auto& cluster:model.clusters)
                for (const auto family:{cluster.bornOf,cluster.replacedBy})
                    if (family!=engine::geometry::MeshCluster::kNoGroup)
                        familyCount=std::max(familyCount,family+1);
            std::vector<std::vector<std::uint32_t>> children(familyCount);
            for (std::uint32_t id=0;id<model.clusters.size();++id) {
                const auto family=model.clusters[id].replacedBy;
                if (family!=engine::geometry::MeshCluster::kNoGroup)
                    children[family].push_back(id);
            }
            for (const auto& family:children) {
                gpuRootFamilies.push_back({std::uint32_t(gpuRootFamilyChildren.size()),
                                           std::uint32_t(family.size())});
                gpuRootFamilyChildren.insert(gpuRootFamilyChildren.end(),family.begin(),family.end());
            }
        }
        if (gpuClusterBuckets_) {
            auto gather=device.makeCompute({"scene_model_cluster_gather.hlsl","GatherCS"});
            std::vector<engine::DrawArguments> descriptions;
            std::vector<std::array<float,4>> morphData(gpuClusterBuckets_*2);
            descriptions.reserve(gpuClusterBuckets_);
            std::size_t bucket=0;
            for (const auto& model:models_)
                for (const auto& cluster:model.clusters) {
                    descriptions.push_back({cluster.indices.count,0,
                        model.clusterBase+cluster.indices.first,model.clusterVertexBase,
                        std::uint32_t(descriptions.size()*kGpuClusterInstanceCapacity)});
                    morphData[bucket*2]={cluster.centre[0],cluster.centre[1],
                                         cluster.centre[2],cluster.error};
                    morphData[bucket*2+1]={cluster.parentError,0,0,0};
                    ++bucket;
                }
            bool gpuSetup=gather && gpuClusters_.setup(device,into,kGpuClusterCapacity,
                                                       kGpuClusterInstanceCapacity,gpuClusterBuckets_);
            if (gpuSetup) {
                gpuInstances_=device.makeBuffer(
                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_BUFFERUSAGE_VERTEX,
                    kGpuClusterCapacity*sizeof(Instance));
                gpuMorphData_=device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                                 morphData.size()*sizeof(morphData.front()));
                if (!gpuInstances_ || !gpuMorphData_) gpuSetup=false;
                if (gpuSetup) {
                    engine::Device::Uploader morphUpload(device);
                    morphUpload.rewrite(gpuMorphData_.get(),morphData.data(),
                                        morphData.size()*sizeof(morphData.front()));
                    gpuSetup=morphUpload.finish();
                }
            }
            gpuReady_=gpuSetup && gpuClusters_.describe(device,descriptions);
            if (gpuReady_) {
                if (!gpuRootFamilies.empty() && !gpuRootFamilyChildren.empty())
                    gpuReady_=gpuMeshRoots_.setup(device,into,gpuRootClusters,gpuRootFamilies,
                                                  gpuRootFamilyChildren,kGpuClusterCapacity,
                                                  kGpuClusterCapacity);
                else
                    gpuReady_=gpuMeshRoots_.setup(device,into,gpuRootClusters,
                                                  std::span<const engine::MeshFamilyRange>(),
                                                  std::span<const std::uint32_t>(),
                                                  kGpuClusterCapacity,kGpuClusterCapacity);
            }
            if (gpuReady_) gpuGather_=into.take(std::move(gather));
        }
        // Instance hierarchy nodes use their own culler and output stream.
        // One bucket is a merged or canopy representation for one grove
        // family; the indirect command therefore points at the complete
        // representation, while the compute cut supplies its instance count.
        gpuHierarchyBuckets_=groves_.size()*2;
        if (gpuHierarchyBuckets_) {
            auto gather=device.makeCompute({"scene_instance_hierarchy_gather.hlsl","GatherCS"});
            std::vector<engine::DrawArguments> descriptions;
            descriptions.reserve(gpuHierarchyBuckets_);
            for (const auto& grove:groves_) {
                for (const auto* clusters:{&grove.clusters,&grove.canopyClusters}) {
                    std::uint32_t indices=0;
                    for (const auto& cluster:*clusters) indices+=cluster.indices.count;
                    descriptions.push_back({indices,0,grove.base,grove.vertexBase,
                                             std::uint32_t(descriptions.size()*kGpuClusterInstanceCapacity)});
                }
            }
            bool gpuSetup=gather && gpuHierarchy_.setup(device,into,kGpuClusterCapacity,
                                                         kGpuClusterInstanceCapacity,
                                                         gpuHierarchyBuckets_);
            if (gpuSetup) {
                gpuHierarchyInstances_=device.makeBuffer(
                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_BUFFERUSAGE_VERTEX,
                    kGpuClusterCapacity*sizeof(Instance));
                gpuSetup=bool(gpuHierarchyInstances_);
            }
            gpuHierarchyReady_=gpuSetup && gpuHierarchy_.describe(device,descriptions);
            if (gpuHierarchyReady_) {
                gpuHierarchyReady_=gpuHierarchyRoots_.setup(device,into,kGpuClusterCapacity);
                if (gpuHierarchyReady_)
                    gpuHierarchyReady_=gpuHierarchyRoots_.setupHierarchy(device,into,
                        kGpuClusterCapacity,kGpuClusterCapacity,kGpuClusterCapacity);
            }
            if (gpuHierarchyReady_) gpuHierarchyGather_=into.take(std::move(gather));
        }
        // The mass pool lives in the shared buffers reserved above, so there is
        // nothing to create here: it is ready exactly when they are.
        massReady_=bool(shared_.vertices) && bool(shared_.indices) && !massSlots_.empty();
        enabled_=true;
        ASR_DIAGNOSTIC(std::cout<<"Scene models: "<<models_.size()<<" meshes, 8-view impostors, "<<geometryBytes_<<" shared geometry bytes\n");
        return place;
    } catch (const std::exception& e) { device.fail(std::string("scene models: ")+e.what());return {}; }
}
bool SceneModelsPass::anything(const engine::Frame& frame) const { return enabled_ && frame.scene.extra[2]>0.01f; }

int SceneModelsPass::massSlotOf(const world::decor::ScatterBounds& region) const {
    for (std::size_t slot=0;slot<massSlots_.size();++slot)
        if (massSlots_[slot].live && massSlots_[slot].region==region) return int(slot);
    return -1;
}

// Members, grouped by the region that would replace them. Only when the
// placement changes: this is the whole scatter, and it does not move.
void SceneModelsPass::updateRegionMembers() {
    if (placement_.get()==massPublished_) return;
    massMembers_.clear();
    massRefused_.clear();massError_.clear();
    massPublished_=placement_.get();
    if (placement_) {
        const auto floorRegion=[](double v) {
            return std::int64_t(std::floor(v/world::decor::kRegion))*world::decor::kRegion;
        };
        for (const auto& object:placement_->scatter.objects) {
            if (object.model>=models_.size() || !models_[object.model].mass) continue;
            const world::decor::ScatterBounds region{floorRegion(object.x),floorRegion(object.y),
                floorRegion(object.x)+world::decor::kRegion,
                floorRegion(object.y)+world::decor::kRegion};
            engine::geometry::RegionMember member;
            member.model=object.model;
            member.position[0]=float(object.x-double(region.minX));
            member.position[1]=float(object.y-double(region.minY));
            member.position[2]=float(object.z);
            member.scale=object.scale;
            member.yaw=object.yaw;
            massMembers_[region].push_back(member);
        }
    }
    // A region whose members changed must not keep geometry baked from the
    // members it used to have.
    for (auto& slot:massSlots_)
        if (slot.live && !massMembers_.contains(slot.region)) slot.live=false;
}

// Which regions are far enough that their aggregate is within the pixel error,
// and getting those baked, uploaded and chosen. Everything here is measured:
// the aggregate's own reconstruction error against its own projected size.
void SceneModelsPass::updateMassRegions(const engine::render::ScreenScale& screen,
                                        double pixelError) {
    massWeights_.clear();
    if (!massReady_ || !enabled_) return;
    ++massClock_;
    updateRegionMembers();

    // What each region is worth as an aggregate right now. Far away the merged
    // surface is within the same pixel error every other representation is
    // held to; close up it is not, and the objects themselves are drawn.
    //
    // Worked out BEFORE the finished bakes are filed, because which slot a bake
    // may take is a question about what is wanted now: with every slot marked
    // used by this frame's demand, a completed bake had nowhere to go and was
    // dropped, so two workers rebaked the same regions for the whole session.
    struct Wanted { world::decor::ScatterBounds region; std::size_t members; float weight;
                    std::uint32_t tier; };
    std::vector<Wanted> wanted;
    for (const auto& [region,members]:massMembers_) {
        if (members.size()<kMassMinimumMembers) continue;
        const float centre[3]{float(double(region.minX)+world::decor::kRegion*0.5),
                              float(double(region.minY)+world::decor::kRegion*0.5),
                              members.front().position[2]};
        double depth=0;
        const double pixelsPerMetre=screen.pixelsPerMetreAt(centre,depth);
        if (!(pixelsPerMetre>0) || !std::isfinite(pixelsPerMetre)) continue;
        const int slot=massSlotOf(region);
        // How coarse this region may be drawn, in metres, to stay fully inside
        // the pixel error. The COARSEST tier that fits is the one asked for: a
        // finer one would spend capacity - and bake time - on detail that is by
        // construction below what the frame can show.
        const double allowed=pixelError*0.6/pixelsPerMetre;
        const int tier=slot>=0?int(massSlots_[std::size_t(slot)].tier)
                              :engine::geometry::chooseRegionMassTier(kMassTiers,allowed);
        if (tier<0) continue;
        if (slot<0 && massRefused_.contains({region,std::uint32_t(tier)})) continue;
        // The error that decides this: measured if this region has ever been
        // baked at this tier, and the tier's nominal only for one that never
        // has. A region whose merge had to coarsen carries more error than the
        // nominal, and forgetting that is how it gets asked for again the
        // moment its slot is taken.
        double error=kMassTiers[std::size_t(tier)].error();
        if (slot>=0) error=double(massSlots_[std::size_t(slot)].error);
        else if (const auto known=massError_.find({region,std::uint32_t(tier)});
                 known!=massError_.end())
            error=double(known->second);
        const double projected=error*pixelsPerMetre;
        const float weight=float(1.0-fade(pixelError*0.6,pixelError,projected));
        if (!(weight>0)) continue;
        wanted.push_back({region,members.size(),weight,std::uint32_t(tier)});
    }
    // The regions that replace the most objects first: that is exactly where
    // the per-object draw records were coming from.
    std::sort(wanted.begin(),wanted.end(),[](const Wanted& a,const Wanted& b) {
        if (a.members!=b.members) return a.members>b.members;
        return a.region<b.region;
    });
    if (wanted.size()>kMassSlots) wanted.resize(kMassSlots);
    std::map<world::decor::ScatterBounds,std::size_t> wantedValue;
    for (const auto& want:wanted) wantedValue[want.region]=want.members;

    // Finished bakes take a slot OF THEIR OWN TIER: an empty one, else the
    // least recently drawn slot nothing wants, else the resident region this
    // one replaces more of.
    for (auto job=massJobs_.begin();job!=massJobs_.end();) {
        if (!job->valid() || job->wait_for(std::chrono::seconds(0))!=std::future_status::ready) {
            ++job;
            continue;
        }
        MassBake baked;
        try { baked=job->get(); } catch (const std::exception&) { baked={}; }
        job=massJobs_.erase(job);
        const auto pending=std::find(massBaking_.begin(),massBaking_.end(),baked.region);
        if (pending!=massBaking_.end()) massBaking_.erase(pending);
        const auto& tier=kMassTiers[std::min<std::size_t>(baked.tier,std::size(kMassTiers)-1)];
        if (baked.mass.empty() || baked.mass.positions.size()/3>tier.vertices ||
            baked.mass.indices.size()>tier.indices) {
            // Remember the refusal. Without this the region is asked for again
            // the very next frame, and two workers spend the session baking
            // something that can never be stored.
            massRefused_.insert({baked.region,baked.tier});
            continue;
        }
        // A region resident at another tier is replaced, not duplicated: two
        // slots standing for the same objects would draw the forest twice.
        for (auto& resident:massSlots_)
            if (resident.live && resident.region==baked.region && resident.tier!=baked.tier)
                resident.live=false;
        int chosen=massSlotOf(baked.region);
        if (chosen>=0 && massSlots_[std::size_t(chosen)].tier!=baked.tier) chosen=-1;
        if (chosen<0) {
            std::uint64_t oldest=std::numeric_limits<std::uint64_t>::max();
            for (std::size_t slot=0;slot<massSlots_.size();++slot) {
                if (massSlots_[slot].tier!=baked.tier) continue;
                if (!massSlots_[slot].live) { chosen=int(slot);break; }
                if (wantedValue.contains(massSlots_[slot].region)) continue;
                if (massSlots_[slot].used<oldest) { oldest=massSlots_[slot].used;chosen=int(slot); }
            }
        }
        if (chosen<0) {
            // Everything resident in this tier is wanted. Take the one that
            // stands for the fewest objects, and only if this bake stands for
            // more.
            std::size_t fewest=baked.members;
            for (std::size_t slot=0;slot<massSlots_.size();++slot)
                if (massSlots_[slot].tier==baked.tier && massSlots_[slot].members<fewest) {
                    fewest=massSlots_[slot].members;chosen=int(slot);
                }
        }
        if (chosen<0) continue;
        auto& slot=massSlots_[std::size_t(chosen)];
        // Vertices carry the same format the shared buffer uses, so one shader
        // and one material draw both. UV comes off the surface normal, as the
        // single-model crown's does.
        std::vector<Vertex> vertices(baked.mass.positions.size()/3);
        for (std::size_t at=0;at<vertices.size();++at) {
            Vertex& vertex=vertices[at];
            std::copy_n(&baked.mass.positions[at*3],3,vertex.position);
            std::copy_n(&baked.mass.normals[at*3],3,vertex.normal);
            const double nx=vertex.normal[0],ny=vertex.normal[1],nz=vertex.normal[2];
            vertex.uv[0]=float(std::atan2(ny,nx)*(1/6.2831853)*4);
            vertex.uv[1]=float(std::acos(std::clamp(nz,-1.0,1.0))*(1/3.14159265)*4);
            std::fill_n(vertex.colour,3,1.0f);
            vertex.layer=at<baked.mass.layer.size()?baked.mass.layer[at]:0;
            vertex.coverage=at<baked.mass.coverage.size()?baked.mass.coverage[at]:1;
            // An aggregate has no finer representation of itself to morph to.
            std::copy_n(vertex.position,3,vertex.morph);
            std::copy_n(vertex.normal,3,vertex.morphNormal);
        }
        engine::Device::Uploader upload(*device_);
        upload.rewriteAt(shared_.vertices.get(),std::size_t(slot.vertexBase)*sizeof(Vertex),
                         vertices.data(),vertices.size()*sizeof(Vertex));
        upload.rewriteAt(shared_.indices.get(),std::size_t(slot.indexBase)*sizeof(std::uint32_t),
                         baked.mass.indices.data(),
                         baked.mass.indices.size()*sizeof(std::uint32_t));
        if (!upload.finish()) continue;
        slot.region=baked.region;
        slot.clusters=baked.mass.clusters;
        std::copy_n(baked.origin,3,slot.origin);
        for (int axis=0;axis<3;++axis)
            slot.centre[axis]=baked.origin[axis]+baked.mass.centre[axis];
        slot.radius=baked.mass.radius;
        slot.members=baked.members;
        slot.used=massClock_;
        slot.live=true;
        // The error the aggregate actually carries: its finest cluster is
        // already a cell away from the objects it merged.
        slot.error=std::numeric_limits<float>::infinity();
        for (const auto& cluster:slot.clusters) slot.error=std::min(slot.error,cluster.error);
        if (!std::isfinite(slot.error)) { slot.live=false;continue; }
        massError_[{baked.region,baked.tier}]=slot.error;
        geometry_[massGeometryBase_+std::uint32_t(chosen)]=
            {slot.vertexBase,{},slot.clusters,slot.indexBase,{},0,{},0,0};
    }

    for (const auto& want:wanted) {
        const int slot=massSlotOf(want.region);
        if (slot>=0) {
            massSlots_[std::size_t(slot)].used=massClock_;
            massWeights_[want.region]=want.weight;
            continue;
        }
        if (massJobs_.size()>=massJobLimit() ||
            std::find(massBaking_.begin(),massBaking_.end(),want.region)!=massBaking_.end())
            continue;
        std::vector<engine::geometry::RegionSource> sources;
        std::vector<std::shared_ptr<const MassSource>> holds;
        sources.reserve(models_.size());
        holds.reserve(models_.size());
        for (const auto& model:models_) {
            holds.push_back(model.mass);
            if (model.mass)
                sources.push_back({model.mass->positions,model.mass->indices,model.mass->layers,0});
            else sources.push_back({});
        }
        const float origin[3]{float(want.region.minX),float(want.region.minY),0};
        massBaking_.push_back(want.region);
        massJobs_.push_back(std::async(std::launch::async,
            [region=want.region,members=massMembers_[want.region],holds=std::move(holds),
             sources=std::move(sources),origin,tier=want.tier]() mutable {
                engine::geometry::RegionMassOptions options;
                options.cellMetres=kMassTiers[std::size_t(tier)].cellMetres;
                options.minimumMembers=kMassMinimumMembers;
                MassBake baked;
                baked.region=region;
                baked.tier=tier;
                std::copy_n(origin,3,baked.origin);
                baked.members=members.size();
                baked.mass=engine::geometry::bakeRegionMass(members,sources,options);
                // A denser region than the capacity was measured for merges at
                // a coarser cell rather than going without a representation.
                // Coarser is a larger error, which the selection below reads
                // off the bake itself, so this cannot make it claim more than
                // it delivers.
                for (int attempt=0;attempt<3 && !baked.mass.empty() &&
                     (baked.mass.positions.size()/3>kMassTiers[std::size_t(tier)].vertices ||
                      baked.mass.indices.size()>kMassTiers[std::size_t(tier)].indices);++attempt) {
                    options.cellMetres*=1.6;
                    baked.mass=engine::geometry::bakeRegionMass(members,sources,options);
                }
                return baked;
            }));
    }
}

void SceneModelsPass::prepareDensity(const engine::Scene& input, double viewportWidth,
                                     engine::Scene& output) {
    for (auto& region : output.vegetationDensity)
        std::fill(std::begin(region), std::end(region), 0.0f);
    if (!enabled_ || models_.empty()) return;

    // This is the same residency demand the draw pass uses, but it runs before
    // Runner uploads Scene. The density path must never lag one frame behind
    // the geometry cut it describes.
    updatePlacement(input,viewportWidth);
    if (input.extra[2]<=0.01f) return;
    static const world::decor::Scatter empty;
    const auto& scatter=placement_?placement_->scatter:empty;
    if (placement_.get()!=published_) {
        world::decor::publishScatter(entities_,scatter,std::uint32_t(models_.size()));
        published_=placement_.get();
        gathered_=engine::render::gatherInstances(entities_);
    }
    const auto* m=input.viewProjection;
    engine::render::ScreenScale screen;
    for (int i=0;i<4;++i) { screen.rowX[i]=m[i];screen.rowY[i]=m[4+i];screen.rowW[i]=m[12+i]; }
    const bool perspective=m[12]!=0 || m[13]!=0 || m[14]!=0;
    screen.focal=perspective
            ? std::hypot(std::hypot(double(m[0]),double(m[1])),double(m[2]))*
              std::max(1.0,viewportWidth)*0.5 : 0;
    screen.scale=input.camera[3];
    screen.allowance=world::decor::kLevelPixelError;
    PhaseClock clock;
    // The regions the objects are grouped into, which is a fact about where
    // they stand and not about where the camera is. Grouped once per published
    // placement; the frame only asks how large each one is on screen.
    //
    // It used to cull every object, select a level for each, and build a fresh
    // instance hierarchy out of the survivors - here AND again in collect(),
    // twice per frame. Measured on a close view of 10913 objects: 3.2 ms of
    // cull, 0.8 of selection and 10.9 of hierarchy here, and the same work
    // again below. None of it told us anything the region grouping does not:
    // the hierarchy's members changed with the camera, which is exactly what a
    // spatial structure over static objects must never do.
    updateRegionMembers();
    densityCullMs_=clock.lap();
    densitySelectMs_=0;
    struct Candidate { float x=0,y=0,radius=0,weight=0; };
    std::vector<Candidate> candidates;
    densityWeights_.clear();
    for (const auto& [region,members]:massMembers_) {
        if (members.empty()) continue;
        const float centre[3]{float(double(region.minX)+world::decor::kRegion*0.5),
                              float(double(region.minY)+world::decor::kRegion*0.5),
                              members.front().position[2]};
        double depth=0;
        const double pixelsPerMetre=screen.pixelsPerMetreAt(centre,depth);
        if (!(pixelsPerMetre>0) || !std::isfinite(pixelsPerMetre)) continue;
        // Terrain shading stands in for the objects only once the whole region
        // is small enough that half its own width is within the pixel error -
        // the same rule the instance hierarchy applied to its very-far stage,
        // over the same 128 m cell, without rebuilding anything per frame.
        const double radius=world::decor::kRegion*0.7071;
        const double weight=1.0-fade(world::decor::kLevelPixelError*0.6,
                                     world::decor::kLevelPixelError,
                                     radius*0.5*pixelsPerMetre);
        if (!(weight>0)) continue;
        densityWeights_[region]=float(weight);
        candidates.push_back({centre[0],centre[1],float(radius),float(weight)});
    }
    // The terrain's climate density covers the entire world below object pixel
    // resolution. Never substitute a focus-centred 3x3 patch for viewport demand.
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b) {
        const double aScore=double(a.radius)*a.radius*a.weight;
        const double bScore=double(b.radius)*b.radius*b.weight;
        if (aScore!=bScore) return aScore>bScore;
        if (a.x!=b.x) return a.x<b.x;
        return a.y<b.y;
    });
    const std::size_t count=std::min<std::size_t>(std::size(output.vegetationDensity),candidates.size());
    for (std::size_t i=0;i<count;++i) {
        output.vegetationDensity[i][0]=candidates[i].x;
        output.vegetationDensity[i][1]=candidates[i].y;
        output.vegetationDensity[i][2]=candidates[i].radius;
        output.vegetationDensity[i][3]=candidates[i].weight;
    }
    densityHierarchyMs_=clock.lap();
}

void SceneModelsPass::collect(const engine::Frame& frame,engine::DrawQueue& queue) {
    // The current Hi-Z build consumes a single-sample depth texture. MSAA
    // depth has no portable resolve path in SDL_GPU yet, so never bind it as a
    // Texture2D: that was the source of the Metal corruption. Keep Hi-Z
    // opt-out available for driver triage and use it only on single-sample
    // targets until depth resolve is implemented.
    const bool useHiZ = std::getenv("ASR_DISABLE_HIZ") == nullptr &&
                        frame.device && frame.device->samples() == SDL_GPU_SAMPLECOUNT_1;
    if (useHiZ && frame.device && pipeline_ && hiz_.ensure(*frame.device,*pipeline_,frame.width,frame.height))
        hiz_.recordBuild(frame,*pipeline_);
    ASR_DIAGNOSTIC(meshes_=cards_=culled_=outside_=faded_=draws_=triangles_=meshTriangles_=0);
    meshStartPixels_=22;
    ASR_DIAGNOSTIC(levelInstances_.assign(levels_,0);levelTriangles_.assign(levels_,0));
    proxyCards_=0; // operational candidate budget, not profiling
    ASR_DIAGNOSTIC(proxyBlocks_=proxyBytes_=0;proxyReach_=0);
    // Placement was frozen by prepareDensity before scene uniforms were uploaded.
    // Polling here could mix one snapshot's density with another one's objects.
    static const world::decor::Scatter empty;
    const auto& scatter_ = placement_ ? placement_->scatter : empty;
    if (!frame.instances) return;
    // One batch per model and level, then the baked per-object impostor card.
    // Spatial aggregate geometry is submitted only when it was baked from that
    // exact hierarchy region; the old generic 3x3 grove cannot represent an
    // arbitrary runtime node and is deliberately not a selectable shape.
    const auto cardSlot=models_.size()*levels_;
    const std::size_t groveSlots=groves_.size()*2; // merged shell, then canopy proxy
    std::vector<std::vector<Instance>> batches(cardSlot+1+groveSlots);
    // How large the biggest instance of each batch is on screen, which is what
    // sets the allowance the cluster cut is taken at.
    std::vector<double> batchPixels(batches.size(),0.0);
    std::vector<double> shellAllowances(groveSlots,std::numeric_limits<double>::infinity());
    const auto& s=frame.scene;const auto* m=s.viewProjection;
    const bool sceneGpu=std::getenv("ASR_DISABLE_SCENE_GPU")==nullptr;
    // Keeps every calculation and submits nothing. The only way to tell what a
    // frame spends on deciding what to draw from what it spends drawing it.
    const bool submitDraws=std::getenv("ASR_SCENE_NO_DRAW")==nullptr;
    const double rightAngle=std::atan2(m[1],m[0]);
    const bool perspective=m[12]!=0 || m[13]!=0 || m[14]!=0;
    const double focal=std::hypot(std::hypot(m[0],m[1]),m[2])*frame.width*0.5;
    const bool loaded=bool(placement_);
    const double targetBlend=loaded && detailWanted_?1:0;
    const double dt=std::clamp(frame.step,0.0,0.1);
    detailBlend_+=std::clamp(targetBlend-detailBlend_,-dt*2,dt*2);
    // The scatter becomes entities once, when the region changes; from here the
    // frame is three passes over flat arrays and never looks at a decor::Object
    // again. See src/engine/render/systems/ for what each pass promises.
    if (placement_.get()!=published_) {
        world::decor::publishScatter(entities_,scatter_,std::uint32_t(models_.size()));
        published_=placement_.get();
    }
    engine::render::ScreenScale screen;
    for (int i=0;i<4;++i) { screen.rowX[i]=m[i];screen.rowY[i]=m[4+i];screen.rowW[i]=m[12+i]; }
    screen.focal=perspective?focal:0;screen.scale=s.camera[3];
    screen.allowance=world::decor::kLevelPixelError;
    // The skyline the ground makes from here, so a forest behind a ridge is
    // dropped before it is transformed, levelled and drawn for the depth test
    // to throw away pixel by pixel.
    //
    // Built from the resident surface pages - the same heights the terrain pass
    // is about to draw, so the occluder cannot claim ground that is not there.
    // Every sample is the lowest of the four around it and the march stops at
    // the first page that is missing, which makes the horizon a floor under the
    // real skyline rather than an estimate of it.
    const engine::render::Horizon* horizon=nullptr;
    horizon_.clear();
    double eye[3];
    PhaseClock clock;
    if (perspective && pages_ && engine::render::eyeFrom(screen.rowX,screen.rowY,screen.rowW,eye)) {
        if (const auto residency=pages_->residency()) {
            horizon_.eyeX=float(eye[0]);horizon_.eyeY=float(eye[1]);horizon_.eyeZ=float(eye[2]);
            constexpr double kPageMetres=512;
            const engine::SurfacePage* page=nullptr;
            std::int32_t pageX=0,pageY=0;
            const auto groundAt=[&](double wx,double wy,double& out) {
                const auto px=std::int32_t(std::floor(wx/kPageMetres));
                const auto py=std::int32_t(std::floor(wy/kPageMetres));
                if (page==nullptr || px!=pageX || py!=pageY) {
                    const auto it=residency->surfaces.find({px,py,2});
                    if (it==residency->surfaces.end()) { page=nullptr; return false; }
                    page=it->second.get();pageX=px;pageY=py;
                }
                if (page==nullptr || page->step<=0 || page->side<=0) return false;
                const double u=(wx-px*kPageMetres)/page->step+page->padding;
                const double v=(wy-py*kPageMetres)/page->step+page->padding;
                const int i0=std::clamp(int(std::floor(u)),0,page->side-1);
                const int j0=std::clamp(int(std::floor(v)),0,page->side-1);
                const int i1=std::min(i0+1,page->side-1),j1=std::min(j0+1,page->side-1);
                // The LOWEST of the four, never an interpolation: an occluder
                // has to be under the ground it stands for, or it hides things
                // that can be seen past it.
                out=std::min({double(page->bed[std::size_t(j0)*page->side+i0]),
                              double(page->bed[std::size_t(j0)*page->side+i1]),
                              double(page->bed[std::size_t(j1)*page->side+i0]),
                              double(page->bed[std::size_t(j1)*page->side+i1])});
                return true;
            };
            for (int bin=0;bin<engine::render::Horizon::kBins;++bin) {
                const double angle=(double(bin)+0.5)/engine::render::Horizon::kBins*6.283185307179586-3.141592653589793;
                const double dx=std::cos(angle),dy=std::sin(angle);
                // Geometric steps: near ground decides the horizon over most of
                // the sky, and far ground only needs to be sampled coarsely.
                for (double distance=24;distance<6000;distance*=1.18) {
                    double ground=0;
                    if (!groundAt(eye[0]+dx*distance,eye[1]+dy*distance,ground)) break;
                    horizon_.raise(dx*distance,dy*distance,ground);
                }
            }
            if (horizon_.active) horizon=&horizon_;
        }
    }
    collectHorizonMs_=clock.lap();
    const auto culled=engine::render::cullToFrustum(gathered_,assets_,screen,horizon);
    collectCullMs_=clock.lap();
    // Culled before selected, so the triangle budget below is computed over
    // what is drawn: a forest behind the camera must not decide how coarsely
    // the forest in front of it is drawn.
    if (frame.work) {
        frame.work->instances+=std::uint32_t(culled.kept.instances.size());
        frame.work->culledFrustum+=std::uint32_t(culled.behind+culled.outside);
        frame.work->culledHorizon+=std::uint32_t(culled.hidden);
    }
    const auto selected=engine::render::selectLevels(culled.kept,assets_,screen);
    ASR_DIAGNOSTIC(outside_=culled.removed();culled_+=outside_);
    collectSelectMs_=clock.lap();

    // The mass batch. Regions far enough that their baked aggregate is within
    // the same pixel error everything else is held to are drawn as one instance
    // each, and their members are not drawn at all.
    updateMassRegions(screen,world::decor::kLevelPixelError);
    collectMassMs_=clock.lap();

    // How large each survivor is on screen, kept rather than recomputed: the
    // budget needs it once and the emission needs it again.
    std::vector<double> pixels(selected.instances.size());
    std::vector<world::decor::MeshDemand> demands;
    demands.reserve(selected.instances.size());
    // Startup fade only. Distance to camera focus is not a visibility criterion.
    std::vector<float> detail(selected.instances.size());
    std::vector<char> keep(selected.instances.size(),0);
    // One pass to size everything, so the batch's own allowance is known before
    // the budget is asked what a batch costs.
    for (const auto& batch:selected.batches) {
        const auto& model=models_[batch.mesh];
        const auto slot=batch.mesh*levels_+batch.level;
        for (std::uint32_t i=0;i<batch.count;++i) {
            const auto at=batch.first+i;
            const auto& instance=selected.instances[at];
            double depth=0;
            const float centre[3]{instance.position[0],instance.position[1],
                instance.position[2]+float(model.height*0.5*instance.scale)};
            pixels[at]=model.extent()*instance.scale*screen.pixelsPerMetreAt(centre,depth);
            detail[at]=float(detailBlend_);
            keep[at]=world::decor::objectLod(pixels[at],0).coverage*detail[at]>0;
            if (!keep[at]) { ASR_DIAGNOSTIC(++culled_;++faded_);continue; }
            batchPixels[slot]=std::max(batchPixels[slot],pixels[at]);
        }
    }
    // Vegetation has a second hierarchy around the object DAG. Near nodes keep
    // their individual model. Until a spatial node owns geometry baked from its
    // actual members, intermediate generic grove shapes are unavailable and the
    // hierarchy descends to individuals; very-far nodes become terrain density.
    std::vector<char> hierarchyReplaced(selected.instances.size(),0);
    std::vector<float> hierarchyNearCoverage(selected.instances.size(),1);
    if (!massWeights_.empty()) {
        const auto floorRegion=[](float v) {
            return std::int64_t(std::floor(double(v)/world::decor::kRegion))*world::decor::kRegion;
        };
        for (std::size_t at=0;at<selected.instances.size();++at) {
            const auto& instance=selected.instances[at];
            const world::decor::ScatterBounds region{floorRegion(instance.position[0]),
                floorRegion(instance.position[1]),
                floorRegion(instance.position[0])+world::decor::kRegion,
                floorRegion(instance.position[1])+world::decor::kRegion};
            const auto weight=massWeights_.find(region);
            if (weight==massWeights_.end()) continue;
            hierarchyNearCoverage[at]=std::min(hierarchyNearCoverage[at],1-weight->second);
            if (!(weight->second<1)) hierarchyReplaced[at]=1;
            ASR_DIAGNOSTIC(++massReplaced_);
        }
    }
    // Objects the terrain is already drawing as density do not draw themselves.
    // Only a region actually present in Scene::vegetationDensity may hide its
    // objects: that array holds eight entries, and suppressing a region outside
    // it would leave an unrepresented hole in the forest.
    if (!densityWeights_.empty()) {
        const auto floorRegion=[](float v) {
            return std::int64_t(std::floor(double(v)/world::decor::kRegion))*world::decor::kRegion;
        };
        const auto submitted=[&](const world::decor::ScatterBounds& region) {
            const float centre[2]{float(double(region.minX)+world::decor::kRegion*0.5),
                                  float(double(region.minY)+world::decor::kRegion*0.5)};
            for (const auto& entry:s.vegetationDensity) {
                if (!(entry[2]>0) || !(entry[3]>0)) continue;
                if (std::abs(entry[0]-centre[0])<=0.01f && std::abs(entry[1]-centre[1])<=0.01f)
                    return entry[3];
            }
            return 0.0f;
        };
        for (std::size_t at=0;at<selected.instances.size();++at) {
            const auto& instance=selected.instances[at];
            const world::decor::ScatterBounds region{floorRegion(instance.position[0]),
                floorRegion(instance.position[1]),
                floorRegion(instance.position[0])+world::decor::kRegion,
                floorRegion(instance.position[1])+world::decor::kRegion};
            if (!densityWeights_.contains(region)) continue;
            const float replacement=submitted(region);
            if (!(replacement>0)) continue;
            hierarchyNearCoverage[at]=std::min(hierarchyNearCoverage[at],1-replacement);
            if (!(replacement<1)) hierarchyReplaced[at]=1;
        }
    }
    // What each object will ACTUALLY cost, which for a clustered model is its
    // own source cut and not a whole level of the chain. Charging the budget for
    collectHierarchyMs_=clock.lap();
    // the chain while drawing the cut is how a frame ends up full of impostors
    // it could have afforded meshes for - and, the other way round, how a wide
    // view quietly draws many times the triangles the budget allows: the cut of
    // complete source geometry is not bounded by the coarsest chain level, so it
    // must never be clamped to it.
    //
    // The cut is memoised per model over a quantised allowance rather than taken
    // once per batch: one number for every instance of a batch charges distant
    // trees what the nearest one costs. Each bucket is rounded DOWN to its finer
    // edge, so a memoised answer is an upper bound on the instance that reads it.
    std::vector<std::unordered_map<int,std::size_t>> cutCost(models_.size());
    std::vector<std::uint32_t> cutClusters;
    const auto sourceTriangles=[&](std::uint32_t mesh,double allowance) {
        const auto& model=models_[mesh];
        const int bucket=int(std::floor(std::log2(std::max(allowance,1e-6))*4));
        auto [entry,fresh]=cutCost[mesh].try_emplace(bucket);
        if (!fresh) return entry->second;
        const double at=std::exp2(double(bucket)/4);
        std::size_t cut=0;
        if (engine::geometry::cutAtHierarchy(model.clusters,at,cutClusters)) {
            for (const auto id:cutClusters) cut+=model.clusters[id].indices.count;
        } else {
            for (const auto& cluster:model.clusters)
                if (double(cluster.error)<=at && double(cluster.parentError)>at)
                    cut+=cluster.indices.count;
        }
        entry->second=cut/3;
        return entry->second;
    };
    for (const auto& batch:selected.batches) {
        const auto& model=models_[batch.mesh];
        const auto level=std::min<std::size_t>(batch.level,model.levels.size()-1);
        const std::size_t chain=model.levels[level].count/3 +
            (level<model.cardLevels.size()?model.cardLevels[level].count/3:0);
        for (std::uint32_t i=0;i<batch.count;++i) {
            const auto at=batch.first+i;
            if (!keep[at] || hierarchyReplaced[at]) continue;
            std::size_t triangles=chain;
            if (model.clusterReady && pixels[at]>0) {
                const double allowance=world::decor::kLevelPixelError*model.extent()/pixels[at];
                triangles=sourceTriangles(batch.mesh,allowance)+
                    (level<model.cardLevels.size()?model.cardLevels[level].count/3:0);
            }
            demands.push_back({pixels[at],triangles});
        }
    }
    meshStartPixels_=world::decor::meshPixelThreshold(demands);
    collectBudgetMs_=clock.lap();
    std::vector<Instance> sourceDrawn;
    engine::render::GatheredInstances sourceInstances;
    sourceDrawn.reserve(selected.drawn());
    sourceInstances.instances.reserve(selected.drawn());
    sourceInstances.owners.reserve(selected.drawn());
    // The solid source cluster cut and its alpha cards are separate draw
    // representations. Keep one companion batch per model/chain level so the
    // normal draw planner can emit card ranges without re-emitting the DAG.
    std::vector<std::vector<Instance>> clusteredCards(models_.size()*levels_);
    const auto addSourceInstance=[&](const engine::render::LevelBatch& batch,
                                     const engine::render::GatheredInstance& source,
                                     const Instance& instance) {
        if (sourceInstances.batches.empty() || sourceInstances.batches.back().mesh!=batch.mesh ||
            sourceInstances.batches.back().material!=batch.material) {
            sourceInstances.batches.push_back({batch.mesh,batch.material,
                                               std::uint32_t(sourceInstances.instances.size()),0});
        }
        ++sourceInstances.batches.back().count;
        sourceInstances.instances.push_back(source);
        sourceDrawn.push_back(instance);
    };
    for (const auto& batch:selected.batches) {
        const auto& model=models_[batch.mesh];
        for (std::uint32_t i=0;i<batch.count;++i) {
            const auto at=batch.first+i;
            if (!keep[at] || hierarchyReplaced[at]) continue;
            const auto& source=selected.instances[at];
            auto lod=world::decor::objectLod(pixels[at],0,meshStartPixels_);
            lod.coverage*=detail[at]*hierarchyNearCoverage[at];
            const auto views=world::decor::impostorPair(rightAngle,source.yaw);
            Instance instance{{source.position[0],source.position[1],source.position[2]},
                source.scale,source.yaw,source.phase,source.tint,
                model.vegetation?1.0f:0.0f,model.width,model.height,
                float(model.impostor+views.view),lod.mesh,0,lod.coverage,
                float(model.impostor+views.next),views.blend};
            if (lod.mesh>0) {
                if (model.clusterReady) {
                    Instance meshInstance=instance;
                    // Clustered meshes use the last component for geomorph;
                    // the GPU gather overwrites it, while the CPU fallback
                    // must leave the child representation unmorphed. The
                    // card copy below keeps its baked-view blend.
                    meshInstance.viewBlend=0;
                    engine::render::GatheredInstance gathered;
                    std::copy(source.position,source.position+3,gathered.position);
                    gathered.scale=source.scale; gathered.yaw=source.yaw;
                    gathered.tint=source.tint; gathered.phase=source.phase;
                    gathered.variant=source.variant;
                    addSourceInstance(batch,gathered,meshInstance);
                    if (batch.level<model.cardLevels.size() &&
                        model.cardLevels[batch.level].count) {
                        // These are the authored leaf quads from the source SCM:
                        // their indices address model-local position/UV/layer
                        // vertices. They are ordinary alpha-tested surfaces,
                        // not the normalized four-vertex baked billboard.
                        auto cardInstance=meshInstance;
                        clusteredCards[std::size_t(batch.mesh)*levels_+batch.level].push_back(cardInstance);
                    }
                } else {
                    batches[batch.mesh*levels_+batch.level].push_back(instance);
                    ASR_DIAGNOSTIC(++levelInstances_[batch.level];
                        const auto triangles=model.levels[batch.level].count/3;
                        meshTriangles_+=triangles;levelTriangles_[batch.level]+=triangles);
                }
                ASR_DIAGNOSTIC(++meshes_);
            }
            if (lod.mesh<1) { instance.mode=1;batches[cardSlot].push_back(instance);ASR_DIAGNOSTIC(++cards_); }
        }
    }
    // Do not synthesize geometry for terrain pages by stretching the fixed 3x3
    // grove asset. Very-far vegetation is represented by the spatial density
    // regions prepared from the actual hierarchy members (or climate fallback)
    // and consumed by terrain.hlsl.
    // Every instance of the frame in one buffer, in run order, so a draw
    // argument can name its own run by a plain index into it.
    std::vector<Instance> drawn=std::move(sourceDrawn);
    std::vector<engine::render::DrawRun> runs;
    drawn.reserve(selected.drawn()+proxyCards_);
    for (std::size_t i=0;i<batches.size();++i) {
        const auto& instances=batches[i];if (instances.empty()) continue;
        const bool mesh=i<cardSlot;
        const bool shell=i>cardSlot;
        const auto asset=std::uint32_t(mesh?i/levels_
                                           :shell?models_.size()+1+(i-cardSlot-1)
                                                 :models_.size());
        const auto level=std::uint32_t(mesh?i%levels_:0);
        // What the coarsest geometry in this run may cost the finest instance
        // in it. Taken from the LARGEST instance, so nothing in the run is
        // drawn coarser than its own size asked for.
        double allowance=0;
        if (mesh && models_[asset].clusterReady && batchPixels[i]>0)
            allowance=world::decor::kLevelPixelError*models_[asset].extent()/batchPixels[i];
        if (shell) allowance=shellAllowances[i-cardSlot-1];
        runs.push_back({asset,level,std::uint32_t(drawn.size()),
                        std::uint32_t(instances.size()),allowance});
        drawn.insert(drawn.end(),instances.begin(),instances.end());
    }
    for (std::size_t slot=0;slot<clusteredCards.size();++slot) {
        const auto& instances=clusteredCards[slot];
        if (instances.empty()) continue;
        const auto model=std::uint32_t(slot/levels_);
        const auto level=std::uint32_t(slot%levels_);
        runs.push_back({cardGeometryBase_+model,level,std::uint32_t(drawn.size()),
                        std::uint32_t(instances.size()),0});
        drawn.insert(drawn.end(),instances.begin(),instances.end());
    }
    // The mass batch. One instance per resident aggregate, in the same arena as
    // everything else and now in the same geometry table, so a region of four
    // hundred trees is one RUN of the one plan rather than a command of its
    // own. Four hundred instances with a cluster cut each is where the draw
    // records were going.
    std::size_t massRunCount=0;
    const std::size_t runsBeforeMass=runs.size();
    if (!massWeights_.empty()) {
        std::array<std::array<double,4>,5> planes{};
        for (int p=0;p<4;++p)
            for (int axis=0;axis<4;++axis)
                planes[p][axis]=screen.rowW[axis]+(p%2?-1:1)*
                    double(p<2?screen.rowX[axis]:screen.rowY[axis]);
        for (int axis=0;axis<4;++axis) planes[4][axis]=screen.rowW[axis];
        std::array<double,5> norms{};
        for (int p=0;p<5;++p) norms[p]=std::hypot(planes[p][0],planes[p][1],planes[p][2]);
        for (std::size_t slot=0;slot<massSlots_.size();++slot) {
            const auto& mass=massSlots_[slot];
            if (!mass.live || mass.clusters.empty()) continue;
            const auto weight=massWeights_.find(mass.region);
            if (weight==massWeights_.end() || !(weight->second>0)) continue;
            bool visible=true;
            for (int p=0;p<5 && visible;++p)
                visible=planes[p][0]*mass.centre[0]+planes[p][1]*mass.centre[1]+
                        planes[p][2]*mass.centre[2]+planes[p][3]+mass.radius*norms[p]>=0;
            if (!visible) continue;
            double depth=0;
            const double pixelsPerMetre=screen.pixelsPerMetreAt(mass.centre,depth);
            if (!(pixelsPerMetre>0) || !std::isfinite(pixelsPerMetre)) continue;
            // The aggregate is baked at world scale, so its allowance is the
            // pixel error in metres - no instance scale to divide out.
            const double allowance=world::decor::kLevelPixelError/pixelsPerMetre;
            runs.push_back({massGeometryBase_+std::uint32_t(slot),0,
                            std::uint32_t(drawn.size()),1,allowance});
            ++massRunCount;
            drawn.push_back({{mass.origin[0],mass.origin[1],mass.origin[2]},1,0,0,1,0,0,0,0,1,0,
                             weight->second,0,0});
            ASR_DIAGNOSTIC(++massDrawn_);
        }
    }
    const auto sourceAssets=[&] {
        std::vector<engine::render::SourceGeometry> assets(models_.size());
        for (std::size_t model=0;model<models_.size();++model) {
            const auto& source=models_[model];
            assets[model]={source.clusters,&source.clusterIndex,source.clusterBase,
                source.vertexBase,{std::hypot(source.width,source.height)*0.6,
                                   source.height*0.5},0,source.clusterVertexBase};
        }
        return assets;
    }();
    auto regularPlan=engine::render::planDraws(runs,geometry_);
    collectEmitMs_=clock.lap();
    const auto instanceAt=frame.instances->add(std::span<const Instance>(drawn));
    bool gpuSource=false;
    std::vector<engine::MeshRootInstance> gpuRoots;
    std::size_t gpuCandidateCount=0;
    if (sceneGpu && gpuReady_ && gpuMeshRoots_.ready() && !sourceInstances.instances.empty() &&
        engine::render::sourceFitsGpuCapacity(sourceInstances,sourceAssets,
                                              kGpuClusterCapacity,kGpuClusterInstanceCapacity)) {
        gpuSource=true;
        gpuRoots.reserve(sourceInstances.instances.size());
        for (const auto& batch:sourceInstances.batches) {
            if (batch.mesh>=models_.size()) {
                gpuSource=false;
                break;
            }
            const auto& model=models_[batch.mesh];
            if (!model.clusterReady || model.clusters.empty()) {
                gpuSource=false;
                break;
            }
            for (std::uint32_t i=0;i<batch.count && gpuSource;++i) {
                const auto& source=sourceInstances.instances[batch.first+i];
                if (model.clusters.size()>kGpuClusterCapacity ||
                    gpuCandidateCount>kGpuClusterCapacity-model.clusters.size()) {
                    gpuSource=false;
                    break;
                }
                engine::MeshRootInstance root;
                std::copy_n(source.position,3,root.position);
                root.scale=source.scale;
                root.yaw=source.yaw;
                root.clusterFirst=model.gpuRootClusterOffset;
                root.clusterCount=std::uint32_t(model.clusters.size());
                root.bucketBase=model.gpuClusterBase;
                root.outputFirst=std::uint32_t(gpuCandidateCount);
                root.payload=instanceAt+std::uint32_t((batch.first+i)*sizeof(Instance));
                root.familyFirst=model.gpuRootFamilyOffset;
                gpuRoots.push_back(root);
                gpuCandidateCount+=model.clusters.size();
            }
        }
    }
    if (gpuSource && gpuRoots.empty()) gpuSource=false;
    if (gpuSource) {
        engine::ClusterSelection selection;
        std::copy_n(screen.rowX,4,selection.rowX);
        std::copy_n(screen.rowY,4,selection.rowY);
        std::copy_n(screen.rowW,4,selection.rowW);
        std::copy_n(m + 8,4,selection.rowZ);
        selection.pixelError=float(screen.allowance);
        selection.halfWidth=float(screen.scale);
        selection.halfHeight=float(frame.height)*0.5f;
        selection.focal=float(screen.focal);
        selection.hiz=useHiZ?hiz_.buffer():nullptr;
        selection.hizWidth=useHiZ?hiz_.width():0;
        selection.hizHeight=useHiZ?hiz_.height():0;
        selection.hizLevels=useHiZ?hiz_.levels():0;
        gpuSource=gpuMeshRoots_.dispatch(frame,*pipeline_,gpuRoots,selection);
        if (gpuSource)
            gpuClusters_.dispatchGpu(frame,*pipeline_,selection,gpuMeshRoots_.candidates(),
                                     gpuMeshRoots_.candidateCount(),true);
    }
    if (gpuSource) {
        engine::ComputeDispatch gather;
        gather.pipeline=gpuGather_;
        gather.groupsX=static_cast<std::uint32_t>(
            (gpuClusterBuckets_*kGpuClusterInstanceCapacity+63)/64);
        gather.reads={gpuClusters_.visible(),gpuClusters_.arguments(),gpuMorphData_.get(),nullptr};
        gather.instanceReadSlot=3;
        gather.writes={gpuInstances_.get()};
        const auto writeUint=[&](float& destination,std::uint32_t value) {
            std::memcpy(&destination,&value,sizeof(value));
        };
        writeUint(gather.own[0],static_cast<std::uint32_t>(kGpuClusterInstanceCapacity));
        writeUint(gather.own[1],static_cast<std::uint32_t>(gpuClusterBuckets_));
        gather.own[2]=float(screen.allowance);
        gather.own[3]=float(screen.scale);
        gather.own[4]=float(screen.focal);
        std::copy_n(screen.rowW,4,gather.own+8);
        pipeline_->dispatch(std::move(gather));
    }
    // The generic grove proxies this used to feed are gone: a region is now
    // represented by geometry baked from its own members (the mass batch), so
    // there is nothing left for the instance-hierarchy culler to traverse.
    constexpr bool gpuHierarchySource=false;
    const auto sourcePlan=gpuSource ? engine::render::SourceDrawPlan{} :
        engine::render::planSourceDraws(sourceInstances,sourceAssets,screen);
    collectPlanMs_=clock.lap();
#if ASR_ENABLE_DIAGNOSTICS
    const auto totalTriangles=sourcePlan.plan.triangles+regularPlan.triangles;
    meshTriangles_+=sourcePlan.plan.triangles;
#endif
    if (gpuSource && submitDraws) {
        engine::DrawItem gpuItem;
        gpuItem.author=5;
        (wireframe_?wireframeRenderer_:renderer_).material.apply(gpuItem);
        gpuItem.vertex[0]=shared_.vertices.get();
        gpuItem.vertex[1]=gpuInstances_.get();
        gpuItem.vertexStreams=2;
        gpuItem.index=shared_.indices.get();
        gpuItem.indexSize=SDL_GPU_INDEXELEMENTSIZE_32BIT;
        gpuItem.indirect=gpuClusters_.arguments();
        gpuItem.indirectDraws=std::uint32_t(gpuClusterBuckets_);
        queue.push(gpuItem);
    }
    if (gpuHierarchySource) {
        engine::DrawItem hierarchyItem;
        hierarchyItem.author=5;
        (wireframe_?wireframeRenderer_:renderer_).material.apply(hierarchyItem);
        hierarchyItem.vertex[0]=shared_.vertices.get();
        hierarchyItem.vertex[1]=gpuHierarchyInstances_.get();
        hierarchyItem.vertexStreams=2;
        hierarchyItem.index=shared_.indices.get();
        hierarchyItem.indexSize=SDL_GPU_INDEXELEMENTSIZE_32BIT;
        hierarchyItem.indirect=gpuHierarchy_.arguments();
        hierarchyItem.indirectDraws=std::uint32_t(gpuHierarchyBuckets_);
        queue.push(hierarchyItem);
    }
    // One recorded command for everything this frame draws on the processor
    // path: model levels, impostor cards, source-cluster cuts and region
    // aggregates. They are the same material, the same instance arena and - now
    // that the aggregate pool was moved into the shared buffers - the same
    // vertex and index buffer, so what used to be three commands is three
    // ranges of one indirect argument array. A draw is a buffer binding, not a
    // count: the records grow with what is visible, the commands do not.
    if (frame.arguments && submitDraws &&
        (!regularPlan.draws.empty() || (!gpuSource && !sourcePlan.plan.draws.empty()))) {
        std::vector<engine::DrawArguments> arguments;
        arguments.reserve(regularPlan.draws.size()+sourcePlan.plan.draws.size());
        arguments.insert(arguments.end(),regularPlan.draws.begin(),regularPlan.draws.end());
        std::size_t triangles=regularPlan.triangles;
        if (!gpuSource) {
            arguments.insert(arguments.end(),sourcePlan.plan.draws.begin(),
                             sourcePlan.plan.draws.end());
            triangles+=sourcePlan.plan.triangles;
        }
        const auto argumentAt=frame.arguments->add(arguments.data(),arguments.size(),
                                                   sizeof(engine::DrawArguments));
        engine::DrawItem item;
        item.author=5;
        (wireframe_?wireframeRenderer_:renderer_).material.apply(item);
        item.vertex[0]=shared_.vertices.get();
        item.instancesFromArena=true;
        item.vertexOffset[1]=instanceAt;
        item.vertexStreams=2;
        item.index=shared_.indices.get();
        item.indexSize=SDL_GPU_INDEXELEMENTSIZE_32BIT;
        item.indirectFromArena=true;
        item.indirectOffset=argumentAt;
        item.indirectDraws=std::uint32_t(arguments.size());
        item.indirectTriangles=triangles;
        queue.push(item);
    }
    if (!gpuSource && !gpuHierarchySource && regularPlan.draws.empty() &&
        sourcePlan.plan.draws.empty()) return;
    if (std::getenv("ASR_SCENE_DEBUG") && frame.index%120==0) {
        double reach=0;
        for (std::size_t i=0;i<selected.instances.size();++i)
            if (keep[i] && !hierarchyReplaced[i]) {
                const auto& instance=selected.instances[i];
                reach=std::max(reach,std::hypot(instance.position[0]-x_,instance.position[1]-y_));
            }
        // What the aggregates cost in records, planned again here rather than
        // kept: they are a tail of the one plan now, and a frame that is not
        // being watched should not pay to take them apart.
        const std::size_t massRecords=massRunCount
            ?engine::render::planDraws(std::span<const engine::render::DrawRun>(runs)
                                           .subspan(runsBeforeMass),
                                       geometry_).draws.size():0;
        ASR_DIAGNOSTIC(massRecords_=massRecords);
        std::size_t massTierLive[std::size(kMassTiers)]{};
        for (const auto& slot:massSlots_)
            if (slot.live) ++massTierLive[slot.tier];
        std::cerr << "scene-debug selected=" << selected.instances.size()
                  << " regions=" << (placement_?placement_->regions.size():0) << '/' << wantedRegions_.size()
                  << " complete=" << (placement_ && placement_->complete)
                  << " visible-reach-m=" << reach
                  << " mass-drawn=" << massRunCount
                  << " mass-records=" << massRecords
                  << " mass-baking=" << massBaking_.size()
                  << " mass-live=" << massTierLive[0] << '/' << massTierLive[1]
                  << '/' << massTierLive[2]
                  << " mass-wanted=" << massWeights_.size()
                  << " mass-refused=" << massRefused_.size()
                  << " source=" << sourceInstances.instances.size()
                  << " source-batches=" << sourceInstances.batches.size()
                  << " gpu-source=" << gpuSource
                  << " gpu-hierarchy=" << gpuHierarchySource
                  << " gpu-clusters=" << gpuCandidateCount
                  << " source-draws=" << sourcePlan.plan.draws.size()
                  << " source-tri=" << sourcePlan.plan.triangles
                  << " regular-draws=" << regularPlan.draws.size()
                  << " regular-tri=" << regularPlan.triangles << '\n';
        std::cerr << "scene-time frame=" << frame.step*1000 << "ms"
                  << " objects=" << (densityCullMs_+densitySelectMs_+densityHierarchyMs_+
                                     collectHorizonMs_+collectCullMs_+collectSelectMs_+
                                     collectMassMs_+collectHierarchyMs_+collectBudgetMs_+
                                     collectEmitMs_+collectPlanMs_) << "ms"
                  << " density[cull=" << densityCullMs_
                  << " select=" << densitySelectMs_ << " hierarchy=" << densityHierarchyMs_
                  << "] collect[horizon=" << collectHorizonMs_ << " cull=" << collectCullMs_
                  << " select=" << collectSelectMs_ << " mass=" << collectMassMs_
                  << " hierarchy=" << collectHierarchyMs_ << " budget=" << collectBudgetMs_
                  << " emit=" << collectEmitMs_ << " plan=" << collectPlanMs_ << "]\n";
    }
    ASR_DIAGNOSTIC(draws_=std::size_t(gpuSource)+std::size_t(gpuHierarchySource)+
        (!regularPlan.draws.empty())+(!gpuSource&&!sourcePlan.plan.draws.empty());
        indirectDraws_=(gpuSource?gpuClusterBuckets_:0)+(gpuHierarchySource?gpuHierarchyBuckets_:0)+
            regularPlan.draws.size()+(!gpuSource?sourcePlan.plan.draws.size():0);
        clusterDraws_=gpuSource?gpuClusterBuckets_:sourcePlan.plan.fromClusters+regularPlan.fromClusters;
        cardDraws_=regularPlan.fromCards;crownDraws_=regularPlan.fromCrown;triangles_=totalTriangles);
}
} // namespace game
