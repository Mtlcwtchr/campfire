#pragma once
// The active GPU terrain path: pages + a quadtree cut + one immutable grid.
#include "engine/render/geometry/range_pool.hpp"
#include <cstdlib>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "engine/render/frame.hpp"
#include "engine/render/systems/terrain_gather.hpp"
#include "game/render/biome_textures.hpp"
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
    // The terrain categories (engine/biomes): the category plane and the
    // biome table, for a fragment stage after its own textures.
    std::vector<SDL_GPUTextureSamplerBinding> biomeBindings() const { return biomes_.bindings(); }
    std::array<float, 16> parameters(const Block& block) const;
    // The same layout for a draw that is no square of the cut - the open-sea
    // sheet - and reads the page atlas at whatever level is resident: the
    // table size and the height window, no tile, no morph, no grid overlay.
    std::array<float, 16> sheetParameters() const {
        return {0, 2, 0, 4, 0, 0, low_, range_, 0, 0, 0, 14,
                float(tableWidth_), float(tableHeight_), 0, 1};
    }

    // The cut as the passes want it: gathered once per frame, ordered, grouped
    // by level and measured against the screen. Four passes used to walk
    // `drawing()` and re-derive the same numbers; this is the one walk.
    struct Drawn {
        SDL_GPUBuffer* vertices = nullptr;
        SDL_GPUBuffer* indices = nullptr;
        // Pooled: where this square's vertices start in `vertices` (bytes) and
        // its first index in `indices`.
        std::uint32_t vertexOffset = 0, firstIndex = 0;
        std::uint32_t waterIndices = 0;     // the surface alone, with no skirt
        // The real height range, unlike the plan's conservative bounds: the
        // highest bed in the square, and whether any water in it stands above
        // sea level. A square wholly under the sea with none has nothing for
        // page water to draw (the sea is the sheet's), and one deep enough
        // has no ground anyone can see.
        float highBed = 0;
        bool inland = true;
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
    // How far from the camera the ground is kept, in metres (TerrainView::
    // window): the draw distance and a margin. Nought keeps the whole world.
    void window(double metres) { window_ = metres; }
    bool skirts() const { return skirts_; }
    void cycleGrid() { grid_ = (grid_ + 1) % 3; }
    int grid() const { return grid_; } // 0 off, 1 height samples, 2 geometry cells
    void stage(generation::TerrainStage value) { requestedStage_=value; }
    bool finalStage() const {
        return requestedStage_==generation::TerrainStage::Final && stageTo_==requestedStage_ && stageAmount_>=1;
    }
    std::size_t missing() const { return missing_; }
    // Pages on the card and in the table.
    std::size_t resident() const { return known_.size(); }
    std::size_t coarse() const { return coarse_; }
    // Resident pages drawn over ground that has been dug since, and how many
    // such pages have been written over with their new ground so far.
    std::size_t stale() const { return stale_.size(); }
    std::size_t refreshed() const { return refreshed_; }
    int target() const { return target_; }
    double radius() const { return radius_; }
    const world::terrain::StreamingProgress& progress() const { return progress_; }
    bool settled() const { return settled_; }
    // A plan has been made for this world: "nothing missing" means something.
    bool planned() const { return plan_ != nullptr; }
    void frozen(bool value) { frozen_ = value; }
private:
    // Stored page levels are 0/1/2/4; the four atlas layers are 0/1/2/3.
    // The atlas a page level is held in: H4, H8, H16, and H64 for H64 and
    // everything coarser (their pages have one shape).
    static int atlasOf(int dataLevel) { return dataLevel >= 4 ? 3 : dataLevel; }
    // The dataset a shader names it by: 0..3 as the atlases, 4 for H256, 5 for
    // H1024 (terrain_pages.hlsli, page_levels.hlsli). The table holds 4 and 5
    // in one layer, H1024 from half its width.
    static int dataset(int dataLevel) {
        return dataLevel == 8 ? 5 : dataLevel >= 5 ? 4 : dataLevel == 4 ? 3 : dataLevel;
    }
    void protect(Key key, std::uint64_t serial);
    void retire(std::uint64_t completed);
    bool publishTable(engine::Device& device);
    bool accept(engine::Device& device, std::shared_ptr<const Plan> plan);
    // What was dug since the last look (edit_layer.hpp): every resident page
    // whose reach it touches is marked stale.
    void observeGround();

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
    // Where each resident page's samples are in its atlas, a layer per
    // dataset (H4, H8, H16, H64, H256, H1024 - page_levels.hlsli).
    //
    // Wrapped: a page is at its coordinates modulo the side, and the texel says
    // which page it holds, so a page the table has no room for reads as absent
    // rather than as a neighbour's ground. It was a grid of the whole world at
    // 512 m for every dataset - 4004 x 4004 x 5 x 16 bytes, 1.3 GB, on a world
    // 2000 km a side, zeroed and uploaded whole whenever a page arrived. The
    // side covers 262 km of H64, a thousand of H256 and four thousand of
    // H1024: more than any of them is ever wanted over.
    static constexpr std::uint32_t kTableLayers = 6;
    static constexpr std::uint32_t kTableSide = 512;
    // What the table holds, as uploaded, and which rows of it a change touched.
    std::vector<std::array<float, 4>> tableMirror_;
    std::unordered_map<std::uint64_t, Key> tableOwner_;   // texel -> the page written there
    bool tableUploaded_ = false;
    ClimateTextures climate_;
    BiomeTextures biomes_;
    engine::Texture table_;
    engine::Sampler tableSampler_;
    engine::Buffer vertices_, indices_;
    // Mesh vertices and indices live in pooled pages (engine::RangePool), not
    // in two buffers of their own: a mesh coming or going - and every
    // per-plan copy of one (seams, display transition) - is a range taken or
    // given back, not a buffer created and released. A copy has the same
    // index list as the mesh it was copied from (terrain_seams.cpp copies the
    // whole mesh and changes vertices only), so it draws with that mesh's
    // indices and uploads its vertices alone.
    struct MeshBuffers {
        std::shared_ptr<const world::terrain::AdaptiveMesh> source;
        engine::RangePool::Range vertices;
        const world::terrain::AdaptiveMesh* indexOwner = nullptr;   // key into indexRanges_
        float highBed = 0;       // the highest ground in the square
        bool inland = false;     // any water standing above sea level
        std::uint64_t lastSerial = 0;
    };
    std::unordered_map<const world::terrain::AdaptiveMesh*, MeshBuffers> meshes_;
    struct IndexBuffers {
        std::shared_ptr<const world::terrain::AdaptiveMesh> source;  // the mesh the list was taken from
        engine::RangePool::Range range;
        std::size_t users = 0;
    };
    std::unordered_map<const world::terrain::AdaptiveMesh*, IndexBuffers> indexRanges_;
    // ASR_TERRAIN_POOL_PAGE=0 (diagnostic): every mesh a page of its own -
    // one buffer per mesh, as before the pool - for A/B pictures.
    static std::uint64_t poolPage(std::uint64_t ordinary) {
        const char* value = std::getenv("ASR_TERRAIN_POOL_PAGE");
        return value && std::atoll(value) == 0 ? 256 : ordinary;
    }
    engine::RangePool vertexPool_{poolPage(16u << 20), 256}, indexPool_{poolPage(8u << 20), 256};
    // ASR_TERRAIN_KEEP_COPIES=1 (diagnostic): spent per-plan copies stay in
    // the cache as they did before, for A/B pictures.
    bool keepCopies_ = std::getenv("ASR_TERRAIN_KEEP_COPIES") != nullptr;
    std::vector<engine::Buffer> vertexPages_, indexPages_;
    // Drops a mesh's ranges (and its index list once no copy uses it).
    void forget(const world::terrain::AdaptiveMesh* key);
    // Places one mesh in the pools; false leaves nothing behind.
    bool place(engine::Device& device, engine::Device::Uploader& upload,
               const std::shared_ptr<const world::terrain::AdaptiveMesh>& mesh, MeshBuffers& into);
    std::size_t pooledUploads_ = 0, sharedIndexLists_ = 0, spentCopies_ = 0;
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
    // Resident pages the ground under them has moved since they were
    // uploaded. They are still drawn - their replacements stream in and are
    // written over them in place - so a stroke never opens a hole.
    std::unordered_set<Key> stale_;
    std::uint64_t groundSeen_ = 0;
    std::size_t refreshed_ = 0;
    bool restream_ = false;
    std::vector<PackedHeightPage> staged_;
    float low_ = 0, range_ = 1;
    std::size_t missing_ = 0, coarse_ = 0;
    int target_ = -1;
    double radius_ = 0, lastX_ = 0, lastY_ = 0;
    double planTime_ = 0;
    bool haveCamera_ = false, tableDirty_ = true, restartPlan_ = false;
    bool lostPages_ = false;
    // Every page of the residency snapshot the plan being built was handed:
    // held from its request to its collection, so the atlas cannot let go of
    // a page that plan will stand on. Without this the plan came back with a
    // pin that was gone, was refused, and the cut was asked for again.
    std::vector<Key> inflight_;
    bool requestPlan(const world::terrain::TerrainView& view);
    void releaseInflight();
    double velocityX_ = 0, velocityY_ = 0;   // the eye's, eased, metres a second   // the last refused plan stood on pages no longer resident
    world::terrain::TerrainView lastView_;
    world::terrain::StreamingProgress progress_;
    std::size_t persistentTotal_ = 0, gpuBytes_ = 0;
    double window_ = 0;
    std::size_t fineResident_ = 0, h8Resident_ = 0;
};

} // namespace game

