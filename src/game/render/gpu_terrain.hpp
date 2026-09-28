#pragma once
// The active GPU terrain path: pages + a quadtree cut + one immutable grid.
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "engine/render/frame.hpp"
#include "engine/render/systems/terrain_gather.hpp"
#include "game/render/climate_textures.hpp"
#include "game/render/height_page_atlas.hpp"
#include "game/render/height_page_stream.hpp"
#include "game/world/terrain_plan.hpp"
#include "game/world/world_system.hpp"
#include "game/client/camera.hpp"

namespace game {

class GpuTerrain {
public:
    using Key = world::streaming::TileKey;
    using Plan = world::terrain::TerrainPlan;
    using Block = Plan::Block;
    explicit GpuTerrain(std::shared_ptr<world::WorldPreparation> preparation);
    ~GpuTerrain() { reset(); }
    bool ensure(engine::Device& device);
    void update(engine::Frame& frame, const client::Camera& camera);
    // Drains workers before releasing resources borrowed from the snapshot.
    void reset();
    const std::vector<Block>& drawing() const {
        static const std::vector<Block> empty;
        return plan_ && !tableDirty_ ? drawing_ : empty;
    }
    std::vector<SDL_GPUTextureSamplerBinding> bindings() const;
    std::array<float, 16> parameters(const Block& block) const;

    // The cut as the passes want it: gathered once per frame, ordered, grouped
    // by level and measured against the screen. Four passes used to walk
    // `drawing()` and re-derive the same numbers; this is the one walk.
    struct Drawn {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        std::uint32_t waterIndices = 0;     // the surface alone, with no skirt
        const world::terrain::AdaptiveMesh* mesh = nullptr;
        std::array<float, 16> parameters{};
    };
    const engine::render::GatheredTerrain& gathered() const { return gathered_; }
    // Indexed by a patch's `source`, which is the only thing the gather carries
    // about where a square's resources live.
    const Drawn& drawn(std::uint32_t source) const { return resolved_[source]; }
    // Cached surface + actual projection: also valid when free-flight zoom is unchanged.
    [[nodiscard]] double vegetationPixelsPerMetre(const engine::Frame& frame,double x,double y) const;
    SDL_GPUBuffer* vertices() const { return vertices_.get(); }
    SDL_GPUBuffer* indices() const { return indices_.get(); }
    std::uint32_t indexCount() const { return skirts_ ? indexCount_ : surfaceIndexCount_; }
    SDL_GPUBuffer* vertices(const Block& block) const;
    SDL_GPUBuffer* indices(const Block& block) const;
    std::uint32_t indexCount(const Block& block, bool water = false) const;
    // Diagnostic only: isolate seam walls without changing height data or LOD.
    void skirts(bool enabled) { skirts_ = enabled; }
    bool skirts() const { return skirts_; }
    void cycleGrid() { grid_ = (grid_ + 1) % 3; }
    int grid() const { return grid_; } // 0 off, 1 height samples, 2 geometry cells
    void stage(generation::TerrainStage value) { requestedStage_=value; }
    bool finalStage() const {
        return requestedStage_==generation::TerrainStage::Final && stageTo_==requestedStage_ && stageAmount_>=1;
    }
    std::size_t missing() const { return missing_; }
    std::size_t coarse() const { return coarse_; }
    int target() const { return target_; }
    double radius() const { return radius_; }
    const world::terrain::StreamingProgress& progress() const { return progress_; }
    bool settled() const { return settled_; }
    void frozen(bool value) { frozen_ = value; }
private:
    // Stored page levels are 0/1/2/4; the four atlas layers are 0/1/2/3.
    static int dataset(int dataLevel) { return dataLevel == 4 ? 3 : dataLevel; }
    void protect(Key key, std::uint64_t serial);
    void retire(std::uint64_t completed);
    bool publishTable(engine::Device& device);
    bool accept(engine::Device& device, std::shared_ptr<const Plan> plan);

    std::shared_ptr<world::WorldPreparation> preparation_;
    world::WorldBuilder::Snapshot world_;
    bool frozen_ = false, settled_ = false;
    world::terrain::TerrainConfig config_;
public:
    // Read once where a pipeline is built, which is why it is not a live value.
    [[nodiscard]] const world::terrain::TerrainConfig& config() const { return config_; }
private:
    world::terrain::TerrainConfigWatch configWatch_;
    int gridCells_ = 0, activeH8AtlasSide_ = 0;
    std::array<int, world::terrain::kGeometryLevels> activeChunkMetres_{};
    std::unique_ptr<HeightPageStream> stream_;
    std::shared_ptr<const Plan> plan_;
    std::shared_ptr<const Plan> preparingStage_;
    generation::TerrainStage requestedStage_=generation::TerrainStage::Final;
    generation::TerrainStage stageFrom_=generation::TerrainStage::Final,stageTo_=generation::TerrainStage::Final;
    float stageAmount_=1;
    bool stageStarted_=true;
    float displayAmount_ = 1;
    std::vector<Block> drawing_;
    engine::render::GatheredTerrain gathered_;
    std::vector<Drawn> resolved_;
    void gather(const engine::Frame& frame);
public:
    // The ground that is actually resident, which is the only honest occluder:
    // it is the same heights the terrain pass is about to draw.
    [[nodiscard]] std::shared_ptr<const world::terrain::TerrainResidency> residency() const {
        return residency_;
    }
private:
    std::shared_ptr<const world::terrain::TerrainResidency> residency_;
    std::array<std::unique_ptr<HeightPageAtlas>, 4> atlases_;
    ClimateTextures climate_;
    engine::Texture table_;
    engine::Sampler tableSampler_;
    engine::Buffer vertices_, indices_;
    struct MeshBuffers {
        std::shared_ptr<const world::terrain::AdaptiveMesh> source;
        engine::Buffer vertices, indices;
        std::uint64_t lastSerial = 0;
    };
    std::unordered_map<const world::terrain::AdaptiveMesh*, MeshBuffers> meshes_;
    std::uint32_t indexCount_ = 0, tableWidth_ = 0, tableHeight_ = 0;
    std::uint32_t surfaceIndexCount_ = 0;
    bool skirts_ = true;
    int grid_ = 0;
    std::uint64_t revision_ = 0, serial_ = 0, lastDrawSerial_ = 0;
    struct Lease {
        std::uint64_t serial;
        std::unordered_set<Key> keys;
        std::shared_ptr<const Plan> plan;
    };
    std::deque<Lease> leases_;
    std::unordered_set<Key> persistent_, known_;
    std::vector<PackedHeightPage> staged_;
    float low_ = 0, range_ = 1;
    std::size_t missing_ = 0, coarse_ = 0;
    int target_ = -1;
    double radius_ = 0, lastX_ = 0, lastY_ = 0;
    double planTime_ = 0;
    bool haveCamera_ = false, tableDirty_ = true, restartPlan_ = false;
    world::terrain::TerrainView lastView_;
    world::terrain::StreamingProgress progress_;
    std::size_t persistentTotal_ = 0, gpuBytes_ = 0;
    std::size_t fineResident_ = 0, h8Resident_ = 0;
};

} // namespace game

