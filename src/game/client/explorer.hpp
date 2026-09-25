#pragma once
// Looking at the world.
//
// A mode of the client that opens the mesh world and nothing else: no
// simulation, no people, no settlements. Its whole job is to let the terrain be
// judged by eye at every scale from a metre to a continent, which until now
// could only be done through pictures dumped by a tool.
//
// It is also the honest half of stage 5 (D113). The game still draws a tile map
// and the simulation still lives on it; this draws the ground the way the rest
// of the world layer describes it - a mesh of triangles with material weights,
// lit by its own normals, with the water where the field says the water is and
// the cliffs where it says the ground breaks. When the simulation moves onto
// continuous coordinates, this is what it will be drawn over.
//
// One world in global metres, sampled at one zoom-selected LOD for the view.
// Runtime chunks are budgeted annuli around a snapshot of the camera focus.
// Workers finish independently; a common radial frontier reveals their data.

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <map>
#include <unordered_map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "game/client/camera.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/environment.hpp"
#include "game/world/inspection.hpp"
#include "game/world/terrain_mesh.hpp"
#include "game/world/climate_field.hpp"
#include "game/world/ring_mesh.hpp"
#include "game/world/tile_mesh.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/weather.hpp"

namespace ui { class Ui; struct Input; }

namespace client {

class Renderer;

// One patch of ground, ready to draw: the mesh, its cliff lines, and the
// screen-space geometry built from them. Kept between frames and rebuilt only
// when the ground it covers changes.
struct TerrainPatch {
    enum class Readiness : std::uint8_t { Proxy, Terrain, Dressing, Gameplay };
    world::TerrainMesh mesh;
    std::shared_ptr<const std::vector<core::WorldPos>> inspectionPositions;
    std::vector<world::CliffSegment> cliffs;
    // Where the water stands over each vertex, worked out once when the patch
    // is built. Asking the field for it while drawing meant walking the
    // drainage of every cell around every vertex of every patch, every frame -
    // which is most of a frame, and the answer never changes.
    std::vector<core::Fixed> waterLevel;
    // And how much of the ground around that vertex is under it, at the spacing
    // this patch was cut at. One at the middle of a river, a half on its bank,
    // nought a footprint clear of it - so a coarse patch shows a stream as a
    // thread of shallow water rather than flooding the valley it runs down.
    std::vector<core::Fixed> waterCover;
    std::vector<std::array<float,4>> waterMotion; // flow xy, river/lake weights
    std::vector<world::RingFoliage> foliage;
    // The most of each material anywhere in the patch. A pass for a material
    // that barely shows here is a pass over every triangle for nothing, and
    // there are six of them.
    std::array<float, world::kMaterialCount> mostOf{};
    bool anyWater = false;
    // Touched when the patch is used, including from a const draw: it is
    // bookkeeping about the cache, not about the ground.
    mutable std::uint64_t lastUsedFrame = 0;
    // When this exact mesh arrived in the cache. Zero means "already settled"
    // (warm-up/resident data) so no stream fade is needed.
    std::uint64_t arrivedFrame = 0;
    // Visual and gameplay readiness are intentionally separate.  A patch can
    // be fully drawable while the CPU is still preparing navigation/entities.
    mutable Readiness readiness = Readiness::Terrain;
    mutable bool gameplayReady = false;
};

// Ground built off the frame.
//
// A patch takes about half a millisecond and a screenful is a hundred of them,
// so building them where the frame can see is fifty milliseconds of stutter
// every time the view moves. They are built on other threads instead and
// collected when they are ready; the frame never waits for one, and what it
// draws in the meantime is whatever coarser ground it already has.
//
// Each worker gets a HeightField of its own. The field remembers the last few
// answers it gave - that is most of why it is fast - and a remembered answer is
// not something two threads can share cheaply.
class PatchWorkshop {
public:
    PatchWorkshop(const generation::WorldMapData& world, std::uint64_t seed);

    // Whether anyone is going to draw the cliff lines.
    //
    // Cutting them is a third of what a patch costs, and the answer for the
    // renderer that draws the world on the card is no - it has passes for
    // ground, grass and water and none for cliffs. Work whose output nobody
    // reads is the most expensive kind, because it does not look like a cost.
    void cutCliffs(bool wanted) { cliffs_ = wanted; }
    ~PatchWorkshop();

    struct Order {
        world::TileId tile;
        double urgency = 0;      // smaller is sooner: distance from the view
    };
    struct Finished {
        world::TileId tile;
        world::TerrainMesh mesh;
        std::shared_ptr<const std::vector<core::WorldPos>> inspectionPositions;
        std::vector<world::CliffSegment> cliffs;
        std::vector<core::Fixed> waterLevel;
        std::vector<core::Fixed> waterCover;
        std::vector<std::array<float,4>> waterMotion;
        std::vector<world::RingFoliage> foliage;
        // Worked out here rather than when the frame takes delivery: it is a
        // walk over every vertex, and doing it on the main thread turned the
        // hand-over itself into the bottleneck - patches finished faster than
        // they could be accepted, and the view outran its own ground.
        std::array<float, world::kMaterialCount> mostOf{};
        bool anyWater = false;
    };

    // What to build next, replacing whatever was asked for before: the view has
    // moved, and ground nobody is looking at any more is not worth finishing.
    void wants(std::vector<Order> orders);
    std::vector<Finished> collect();
    std::size_t waiting() const;
    struct WorkerStats { std::size_t workers, busy, peakBusy, queued, ready; };
    WorkerStats workerStats() const;
    // Same worker pool/HeightFields as terrain; separate bounded completion queue.
    void inspectionWants(std::shared_ptr<const world::InspectionSnapshot> state,
                         std::vector<world::InspectionOrder> orders);
    std::vector<world::InspectionResult> collectInspection(std::size_t limit = 2);
    WorkerStats inspectionStats() const;
    [[nodiscard]] world::streaming::PageStore::Progress prebakeProgress() const {
        return pages_.prebakeProgress();
    }
    const world::ClimateField& climate() const { return climate_; }
    world::streaming::PageStore& pages() { return pages_; }
    [[nodiscard]] bool containsLand(world::TileId tile) const;
    void debugArtificialDelay(std::chrono::milliseconds delay);
    void debugFreeze(bool frozen);
    std::chrono::milliseconds debugArtificialDelay() const;
    bool debugFrozen() const;

private:
    // How many finished patches may be held for the frame to take in. Twice
    // what a frame takes, so a worker is never idle waiting for the next
    // collection, and no more - see the wait in work().
    static constexpr std::size_t kMostWaiting = 8;

    bool work(std::size_t index, bool inspectionOnly);
    void notifyWorkers();

    const generation::WorldMapData& world_;
    std::uint64_t seed_;
    // The hydrology and the pages every worker reads its ground from. Built
    // once and shared: the graph is immutable and the store is its own lock,
    // so a page baked for one ring is there for the next.
    //
    // Declared before the workers, because a worker reads them the moment it
    // starts.
    world::streaming::HydrologyGraph hydrology_;
    world::streaming::HsimQuantisation quantisation_;
    world::streaming::PageStore pages_;
    world::streaming::TerrainWorkerPool& pool_;
    world::streaming::TerrainWorkerPool::Handle source_, inspectionSource_;
    // One climate for the world, raised beside the coarse pages.
    world::ClimateField climate_;
    std::vector<world::HeightField> fields_;
    std::vector<world::TileSampleCache> samples_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<Order> queue_;
    std::vector<Finished> done_;
    std::shared_ptr<const world::InspectionSnapshot> inspectionState_;
    std::vector<world::InspectionOrder> inspectionQueue_;
    std::vector<world::InspectionResult> inspectionDone_;
    std::set<std::uint64_t> inspectionWanted_;
    std::set<std::pair<std::uint64_t, std::uint64_t>> inspectionInFlight_;
    std::size_t inspectionBusy_ = 0, inspectionPeak_ = 0;
    std::set<std::int64_t> inFlight_;
    std::uint64_t epoch_ = 0;
    // Every tile the view still wants, by name. A worker is stopped when the
    // ground it is cutting has left the view - not because the camera moved.
    std::set<std::int64_t> live_;
    std::size_t busy_ = 0;
    std::size_t peakBusy_ = 0;
    bool closing_ = false;
    bool cliffs_ = true;
    std::size_t dropped_ = 0;
    std::atomic<std::int64_t> artificialDelayMs_{0};
    std::atomic<bool> frozen_{false};
};

class Explorer {
public:
    Explorer(const generation::WorldMapData& world, std::uint64_t seed);

    // A different world, in the same explorer.
    //
    // Rebuilt rather than reseated: the workshop is ten threads each holding a
    // view of the old country, and the cheapest way to be certain none of them
    // is still cutting it is to let them finish and go. Everything cached is
    // dropped with it - a chunk keeps its number from world to world, so ground
    // held over would be the old country under the new one's name.
    //
    // The renderer's own cache of uploaded meshes is not this object's to clear;
    // whoever calls this clears it (see ExploreView::rebuild).
    void restart(const generation::WorldMapData& world, std::uint64_t seed);

    // Where the camera starts: the played site if the world has one, or the
    // middle of the largest land mass.
    core::WorldPos startingPoint() const;

    // Compatibility entry point. Streaming starts asynchronously once the
    // actual camera zoom and viewport are known; no square proxies are built.
    void warmUp(core::WorldPos around);
    // Passed straight to the workshop; see PatchWorkshop::cutCliffs.
    void cutCliffs(bool wanted) { workshop_->cutCliffs(wanted); cliffs_ = wanted; }
    // The coarsest level this world has, and the level from which patches are
    // never dropped - which is the same level.
    //
    // Worked out from the size of the world rather than fixed at eight, which is
    // what it was when a world was eleven hundred kilometres across. On a world
    // of fifty a patch of level eight covers sixteen kilometres, level ten
    // sixty-five and level twelve two hundred and sixty - one patch bigger than
    // the whole world, which is not a level of detail but a joke.
    //
    // The rule is that the world is about thirteen chunks across at the coarsest
    // level, which comes out at six for the two small sizes and seven for the
    // two large ones - a widest view of a hundred to two hundred patches, three
    // to six megabytes on the card, kept for ever. Coarser than that and the
    // view is a dozen patches of very little; finer and the widest zoom pays
    // thousands of draw calls for detail smaller than a pixel (measured: level
    // four on the largest world is ten thousand patches and three hundred
    // megabytes).
    std::int32_t coarsest() const { return coarsest_; }
    void update(const Camera& camera, double seconds);
    void useGpuPages() { gpuPages_ = true; workshop_->wants({}); }
    world::streaming::PageStore& pages() { return workshop_->pages(); }
    const generation::WorldMapData& worldMap() const { return *world_; }
    double terrainRadius(const Camera& camera) const { return viewRadius(camera, levelFor(camera.pixelsPerTile)); }
    bool streamingFrozen() const { return workshop_->debugFrozen(); }
    void gpuProgress(int level, double radius, std::size_t drawn, std::size_t pending,
                     std::size_t coarse, std::size_t blank) {
        lastLevel_ = level; streamRadius_ = radius; gpuDrawn_ = drawn;
        gpuPending_ = pending; gpuPrepared_ = true;
        lastWanted_ = drawn + blank; missing_ = blank;
        blankShare_ = lastWanted_ ? double(blank) / lastWanted_ : 0;
        coarseShare_ = drawn ? double(coarse) / drawn : 0;
    }
    // Resolves streaming/LOD for this view without drawing it. GPU and legacy
    // SDL renderers consume the same patch list, so world generation remains one
    // deterministic CPU pipeline while presentation can change independently.
    // One patch of the view, and whether it is the level the view asked for.
    //
    // A patch that is not is a stand-in: the ground is there, but sampled more
    // coarsely than the camera wants, and it is only on screen because the sharp
    // one has not been cut yet. Which it is decides how it is drawn, and that is
    // not a detail - see the terrain pass.
    struct Visible {
        const TerrainPatch* patch;
        // Whether this is the level the view asked for. For the counters and
        // the overlay only.
        bool exact;
        // Whether it is only there to have something behind the real ground.
        // Coarser than wanted; drawn first with the depth buffer untouched, so
        // anything sharp lands on top of it. Ground finer than wanted is not a
        // backdrop - it is better than what was asked for and is drawn as
        // ground.
        bool backdrop;
        // Runtime morph and opacity for stream blending. The same patch can be
        // drawn with different values across frames without being rebuilt.
        float morph = 0;
        float alpha = 1;
        // Shared radial frontier, and (for the retained snapshot) the window
        // replaced by the new snapshot. Passed unchanged to all GPU passes.
        std::array<float, 16> draw{};
    };
    const std::vector<Visible>& prepareVisible(const Camera& camera);
    void draw(SDL_Renderer* sdl, Renderer& art, const Camera& camera);
    // The lines of text the overlay shows: where we are, what is under the
    // cursor, and what the streaming is doing.
    std::vector<std::string> status(const Camera& camera, int mouseX, int mouseY,
                                  const world::weather::Snapshot* weather = nullptr) const;

    // Somewhere worth looking at, for the keys that jump about the world.
    struct Landmark {
        std::string name;
        core::WorldPos where;
    };
    const std::vector<Landmark>& landmarks() const { return landmarks_; }

    const world::HeightField& field() const { return field_; }
    std::uint64_t worldRevision() const { return worldRevision_; }
    void inspectionWants(std::shared_ptr<const world::InspectionSnapshot> state,
                         std::vector<world::InspectionOrder> orders) {
        workshop_->inspectionWants(std::move(state), std::move(orders));
    }
    std::vector<world::InspectionResult> collectInspection() { return workshop_->collectInspection(); }
    PatchWorkshop::WorkerStats inspectionStats() const { return workshop_->inspectionStats(); }
    world::SoilState& soil() { return soil_; }
    const world::SoilState& soil() const { return soil_; }

    // Whether the last frame drew ground it did not have yet. A picture asked
    // for while this is true is a picture of a half-built world.
    bool stillFillingIn() const { return gpuPages_ ? gpuPending_ > 0 : missing_ > 0; }
    // Loaded meshes can still be behind the animated reveal frontier.
    bool fullyRevealed() const {
        // Every ring of the view has ground behind it. There is nothing else to
        // wait for: the frontier was a curtain travelling over ground that was
        // already there, and asking whether it had finished travelling is what
        // made a finished view report itself unfinished.
        return (gpuPages_ ? gpuPrepared_ : wanted_.count() > 0) && !stillFillingIn();
    }
    // How much of the last frame had no ground at all behind it, and how much
    // was standing in for a sharper level that had not arrived. The first has to
    // be nought for the thing to be playable; the second is what "loading"
    // honestly looks like.
    double blankShare() const { return blankShare_; }
    double coarseShare() const { return coarseShare_; }
    std::int32_t lastLevel() const { return lastLevel_; }
    std::size_t lastWanted() const { return lastWanted_; }
    double drawMillis() const { return drawMillis_; }
    std::size_t arrived() const { return builtThisFrame_; }
    std::size_t onOrder() const { return workshop_->waiting(); }
    // How much of the foundation - the whole world at the coarsest level - is
    // cut. It only ever grows.
    std::size_t coarseHeld() const {
        std::size_t many = 0;
        for (const auto& [key, patch] : patches_)
            if (patch.mesh.lod >= coarsest_) ++many;
        return many;
    }
    PatchWorkshop::WorkerStats workerStats() const { return workshop_->workerStats(); }
    // How much of the coarse world has been baked, for the loading line.
    world::streaming::PageStore::Progress prebakeProgress() const {
        return workshop_->prebakeProgress();
    }
    // The world's weather, raised once with the map. The card holds it as
    // three pictures; nothing else in the frame reads it.
    const world::ClimateField& climate() const { return workshop_->climate(); }
    double streamRadius() const { return streamRadius_; }
    std::size_t held() const { return patches_.size(); }
    // Pieces of the view with nothing at all behind them, at any level.
    std::size_t blankPatches() const { return missing_; }
    // And which pieces those are, at the level the view asked for.
    //
    // Not "coarser than wanted", which is a different and much more common
    // thing: these are chunks the gather could find no shape for at any level
    // at all. What the screen-space mask is drawn from.
    //
    // With how high the ground under them stands, off the coarse map. There is
    // no mesh here to ask - that is what makes the chunk unfilled - and putting
    // its square on the screen at the height the camera happens to be focused
    // on is wrong by however far the country is from that: at a wide zoom the
    // mask then lands on ground that is perfectly well drawn, somewhere else
    // entirely. The coarse map knows the answer to within a cell, which is
    // closer than a mask this soft can tell.
    struct Unfilled {
        world::ChunkId chunk;
        float low = 0, high = 0;   // metres, off the coarse map
    };
    const std::vector<Unfilled>& unfilled() const { return unfilled_; }
    std::size_t droppedLastFrame() const { return dropped_; }
    struct StreamRecord {
        enum class State : std::uint8_t {
            Unloaded,
            ProxyRequested,
            ProxyReady,
            TerrainRequested,
            TerrainReady,
            DressingRequested,
            DressingReady,
            GameplayRequested,
            GameplayReady,
        };
        std::int32_t lod = 0;
        world::ChunkId chunk;
        TerrainPatch::Readiness state = TerrainPatch::Readiness::Proxy;
        State phase = State::Unloaded;
        std::uint64_t lastRequested = 0;
        bool requested = false;
        bool gameplayReady = false;
        bool activationQueued = false;
    };
    struct GameplayRequest {
        std::int32_t lod = 0;
        world::ChunkId chunk;
    };
    std::size_t streamRecords() const { return streamRecords_.size(); }
    const StreamRecord* streamRecord(std::int32_t lod, world::ChunkId chunk) const;
    std::vector<GameplayRequest> takeGameplayRequests();

    // Gameplay preparation is owned by the simulation side.  It can publish
    // readiness without touching visual residency or delaying rendering.
    void markGameplayRequested(std::int32_t lod, world::ChunkId chunk);
    void markGameplayReady(std::int32_t lod, world::ChunkId chunk, bool ready = true);
    void setStreamingDebugDelay(std::chrono::milliseconds delay) { workshop_->debugArtificialDelay(delay); }
    void setStreamingFrozen(bool frozen);
    bool teleportedLastUpdate() const { return teleportedLastUpdate_; }

    // How coarse the ground is drawn at this zoom. Public because the overlay
    // says it out loud: it is the one number that explains what is on screen.
    // How coarse the ground is drawn at this zoom, never coarser than this
    // world has levels for.
    std::int32_t levelFor(double pixelsPerTile) const;

    // The same question answered as a real number rather than as a step.
    //
    // levelFor asks "how many doublings until a sample is small enough" and
    // counts them; this solves for the doublings instead, so it says where
    // between two levels the zoom is standing. It is what the morph runs along:
    // a level is not entered at a boundary, it is walked into.
    static double levelExactFor(double pixelsPerTile);
    double lastLevelExact() const { return lastLevelExact_; }
    double predictedCameraX() const { return predictedX_; }
    double predictedCameraY() const { return predictedY_; }
    // How far a patch built at `lod` has walked towards the level above it, at
    // the zoom the view was last prepared for. Nought where a level has just
    // been entered, one where the next one is about to take over - so a patch
    // finer than the view asked for is fully morphed and matches the level
    // around it exactly, and a coarse stand-in stays as it was built.
    float morphOf(std::int32_t lod) const;

private:
    bool gpuPages_ = false;
    bool gpuPrepared_ = false;
    std::size_t gpuPending_ = 0;
    std::size_t gpuDrawn_ = 0;
    float streamBlendOf(const TerrainPatch& patch) const;
    // Whether this ground is already had: in the cache, or finished and waiting
    // to be taken into it.
    bool held(world::TileId tile) const;
    double viewRadius(const Camera& camera, std::int32_t lod) const;

    // Keep current and retained snapshots, not a permanent square LOD pyramid.
    void raiseFoundation(const generation::WorldMapData& world);
    void forgetOldPatches();
    void findLandmarks();

    world::HeightField field_;
    world::SoilState soil_;
    const generation::WorldMapData* world_;
    std::uint64_t worldRevision_ = 1;

    // Owned ring data by (LOD, epoch/index key); never addressed by ChunkId.
    // Keyed by the tile's own name, which is where it is in the world.
    std::unordered_map<std::int64_t, TerrainPatch> patches_;
    double streamRadius_ = 0;
    std::vector<Landmark> landmarks_;
    bool cliffs_ = true;
    std::size_t dropped_ = 0;
    std::unique_ptr<PatchWorkshop> workshop_;

    // LOD switch dead-zone around integer boundaries, in LOD units.
    static constexpr double kLodHysteresis = 0.18;
    // Frames to fade an exact patch in from its coarse parent.
    static constexpr std::uint64_t kStreamBlendFrames = 20;
    // Frames an exact patch stays staged before it can replace the stand-in.
    static constexpr std::uint64_t kStreamHysteresisFrames = 8;

    // How much new ground may reach the card in one frame, in bytes.
    //
    // It was a count - two patches a frame - and a count is the wrong unit for
    // a bandwidth. A patch is whatever size its ring happens to be, so two of
    // them is anything from a few kilobytes to a megabyte, and the number had
    // to be small enough for the largest case. Measured on a settled view:
    // two hundred and eighty patches at two a frame is a hundred and forty
    // frames before the last of them can be drawn, and the view took three
    // hundred and seventy-two frames to finish. That is the loading.
    //
    // Twenty-four megabytes is about forty patches of a near view, and it is
    // what a modern card takes in a frame without noticing. The bound stays,
    // because a backlog arriving at once is a real stall; it is the unit that
    // was wrong.
    static constexpr std::size_t kUploadBytesPerFrame = 24u << 20;
    // What a patch costs on the card: the GPU vertex is thirty-three floats
    // and an index is four bytes. Close enough to budget by - the collector
    // owns the real layout, and a byte or two either way does not change how
    // many patches fit in twenty-four megabytes.
    static constexpr std::size_t kGpuVertexBytes = 33 * 4;
    static constexpr std::size_t kGpuIndexBytes = 4;
    static std::size_t uploadBytesOf(const world::TerrainMesh& mesh) {
        return mesh.vertices.size() * kGpuVertexBytes + mesh.indices.size() * kGpuIndexBytes;
    }
    std::vector<PatchWorkshop::Finished> arriving_;

    // What the view wants at one level: the square tiles that cover it.
    //
    // No epoch and no centre. A tile is named by the ground it covers, so the
    // set changes as the camera moves but its members do not: the ones that
    // were wanted before and are wanted still are the same tiles, already
    // built, and nothing is rebuilt for having moved.
    struct TileView {
        // Whether it names anything at all. Without it a default-constructed
        // view is not empty but a single tile at the origin of level nought -
        // which is a real tile, and an unset backdrop drew it under a view of
        // the same level, so one patch was drawn twice.
        bool set = false;
        std::int32_t lod = 0;
        std::int32_t firstX = 0, firstY = 0, lastX = 0, lastY = 0;
        [[nodiscard]] bool empty() const { return !set || lastX < firstX || lastY < firstY; }
        [[nodiscard]] std::size_t count() const {
            return empty() ? 0
                           : static_cast<std::size_t>(lastX - firstX + 1) *
                                     static_cast<std::size_t>(lastY - firstY + 1);
        }
    };
    TileView wanted_;
    // The view as it was at the level before this one.
    //
    // Walking up the levels finds a substitute when the camera comes in - the
    // coarser tile covering a place is one tile, and it is usually already
    // there. Going out there is no such thing: one coarse place is four finer
    // tiles and then sixteen, so what covers the screen while the new level
    // arrives is simply the old level, kept and drawn until it is not needed.
    TileView previous_;
    // The ground that is always there.
    //
    // A coarse disc around the camera at the coarsest level this world has,
    // never dropped. It is what a ring refines *onto*: without it a view being
    // built has open sea behind it wherever no ring has arrived, and moving is
    // a redraw from nothing rather than a sharpening of what is on screen.
    //
    // Only planned once the coarse world is baked. Before that its pages would
    // be baked on demand and it would be the most expensive thing in the queue
    // rather than the cheapest - measured, it took a seven second view to
    // fifty-three.
    TileView backdrop_;
    std::uint64_t nextRingEpoch_ = 1;
    double frameSeconds_ = 1.0 / 60.0;
    double focusQuietSeconds_ = 0;
    std::uint64_t preparedFrame_ = ~std::uint64_t(0);
    std::pair<double, double> worldHeightBounds_{};
    Camera footprintCamera_;
    double footprintRadius_ = 0;
    bool haveFootprint_ = false;

    std::uint64_t frame_ = 0;
    std::size_t builtThisFrame_ = 0;
    std::size_t missing_ = 0;
    double blankShare_ = 0;
    double coarseShare_ = 0;
    // The finest level the whole world is held at, and so the finest a
    // backdrop can be without baking anything.
    static constexpr std::int32_t kFinestHeldLevel = 2;   // 4 m << 2 == 16 m
    std::int32_t coarsest_ = 6;
    // The whole world at the coarsest level, built once and never dropped.
    //
    // Everything else in this cache is a tenancy: built when the eye comes
    // near, forgotten a few seconds after it leaves. The foundation is not.
    // It is the answer to "what is here" for every square metre of the world,
    // so a place is never blank and a refinement is always a refinement of
    // something rather than a first draw. On the default map it is thirteen
    // tiles a side - a hundred and sixty-nine meshes, a hundred and thirty
    // megabytes - and its pages are pinned already, so it costs the cutting
    // and nothing else.
    TileView foundation_;
    std::int32_t lastLevel_ = 0;
    double lastLevelExact_ = 0;
    std::size_t lastWanted_ = 0;
    std::size_t drawnPatches_ = 0;
    std::size_t drawnTriangles_ = 0;
    double buildMillis_ = 0;
    double drawMillis_ = 0;
    // Presentation-only camera velocity used to order work ahead of motion.
    double cameraVelocityX_ = 0;
    double cameraVelocityY_ = 0;
    double predictedX_ = 0;
    double predictedY_ = 0;
    double lastCameraX_ = 0;
    double lastCameraY_ = 0;
    bool haveCameraSample_ = false;
    bool teleportedLastUpdate_ = false;
    std::vector<Visible> visible_;
    std::vector<Unfilled> unfilled_;
    std::map<std::pair<std::int32_t, std::int64_t>, StreamRecord> streamRecords_;
    std::vector<GameplayRequest> gameplayRequests_;

    // Scratch, kept between frames so the vectors stop growing.
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
    static constexpr std::size_t kFoliageVariants = 6;
    std::array<std::vector<SDL_Vertex>, kFoliageVariants> foliageVertices_;
    std::array<std::vector<int>, kFoliageVariants> foliageIndices_;
};

// Opens the world, shows it, and returns when the window is closed. When
// `shotPath` is given it draws one frame into that file and returns instead -
// which is how the terrain gets reviewed without a person at the keyboard.
int runExplorer(SDL_Window* window, SDL_Renderer* sdl, Renderer& art, Camera& camera,
                std::uint64_t seed, std::int32_t worldCells, const std::string& shotPath,
                double startZoom = 0, const std::string& startAt = {}, bool measuring = false,
                bool closeUp = false);

} // namespace client
