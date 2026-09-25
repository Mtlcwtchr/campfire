#pragma once
// The pages a worker can ask for by key.
//
// Between "a page can be baked" and "a reader can have one" there has to be
// something that owns them: a bake is tens of milliseconds and a ring of
// terrain asks for the same page from several jobs at once, so baking per
// request would spend most of a frame's worth of worker time re-deriving
// ground that is already in memory.
//
// Worker-owned, like everything else on this side of the boundary. The frame
// thread may ask what is already resident, which never blocks and never
// bakes; anything that might bake belongs to a worker.
//
// Residency is a bounded least-recently-used set. A page is thirty kilobytes
// of ground and a page of water, and the camera walks: holding every page a
// session has ever touched would grow without limit for no gain, while
// holding too few turns a pan into a re-bake of ground that was on screen a
// moment ago.

#include <cstddef>
#include <filesystem>
#include <initializer_list>
#include <list>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>

#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"
#include "game/world/terrain_streaming/worker_pool.hpp"
#include "game/world/terrain_lod.hpp"

namespace generation { struct WorldMapData; }

namespace world::streaming {

class BakedPageCache;

class PageStore {
public:
    struct Config {
        // How much memory the resident pages may take, in bytes.
        //
        // It was a count, and a count is the wrong unit because a page is not
        // a fixed size: at four metres to the sample it is 444 KB and at two
        // hundred and fifty-six it is 1.8. So a budget of a couple of hundred
        // pages was eighty-five megabytes of fine ground and a third of a
        // megabyte of coarse - and a coarse view needs *more* pages, not
        // fewer, because the page stays five hundred and twelve metres while
        // the view reaches for kilometres. Measured on a view seventeen
        // kilometres across: 4410 pages are wanted, which is eight megabytes
        // at level six and thirteen at level five. Counted in pages they did
        // not fit and every sample evicted the one before it.
        //
        // A hundred and twenty-eight megabytes holds any view at any level -
        // the widest coarse disc, and the sixty fine pages a close view reads.
        std::size_t residentBytes = 128u << 20;
        std::uint16_t padding = kDefaultPaddingSamples;
        // Zero selects half the hardware threads. A lower explicit limit is
        // useful for tests/tools; it never increases the machine-wide budget.
        unsigned workerCount = 0;
        // Empty disables disk persistence (CPU tools remain side-effect free).
        // Only H16/H64 are persisted; I/O runs on the existing terrain workers.
        std::filesystem::path diskCacheRoot;
    };

    // What a page costs to keep, near enough to budget by: the arrays it owns.
    [[nodiscard]] static std::size_t footprintOf(const BakedPage& page);

    // How far apart a level's samples are. The key carries the level and the
    // level carries the spacing, so one store answers for the whole view: a
    // ring reads its own level and the page it gets is a strict subsample of
    // the fine one under it, which is what lets the morph between two rings
    // be a morph rather than a dissolve.
    //
    // A page stays 512 m square at every level. Ring or frustum distance
    // decides which level is asked for, never how the ground is divided.
    static constexpr std::int32_t sampleMetresAtLevel(std::uint8_t level) {
        return level <= kFinestLevels ? (kSampleMetres << level) : 0;
    }
    // 4 m up to 512 m, which is the coarsest spacing a page can still hold
    // two samples of.
    static constexpr std::uint8_t kFinestLevels = 7;

    struct Stats {
        std::size_t resident = 0;
        std::size_t residentBytes = 0;
        std::size_t hits = 0;      // already in memory
        std::size_t baked = 0;
        std::size_t evicted = 0;
        // Two workers that ask for one page at the same moment may both bake
        // it. The waste is bounded and the answer is identical, which is why
        // this is counted rather than prevented with a lock held across a
        // bake - a bake is tens of milliseconds and no other page may wait on
        // it.
        std::size_t rebaked = 0;
        std::size_t evaluatedSamples = 0, reusedSamples = 0;
        std::size_t diskLoaded = 0, diskSaved = 0, diskMisses = 0, diskErrors = 0;
    };

    PageStore(const generation::WorldMapData& world, const HydrologyGraph& graph,
              HsimQuantisation quantisation, Config config);
    // The defaults, spelled out of line because a default argument cannot see
    // Config's own initialisers from inside this class.
    PageStore(const generation::WorldMapData& world, const HydrologyGraph& graph,
              HsimQuantisation quantisation);
    ~PageStore();

    // Lazily created on the owner thread, before registering work. All terrain
    // consumers of this store share these workers and must detach before it dies.
    TerrainWorkerPool& workerPool();

    // Worker-only: bakes on a miss. The world and graph must outlive the store.
    [[nodiscard]] std::shared_ptr<const BakedPage> page(TileKey key);
    // Frame-safe: whatever is already resident, or nothing. Never bakes.
    [[nodiscard]] std::shared_ptr<const BakedPage> resident(TileKey key) const;
    // Every page a rectangle of world touches, in row-major order. What a ring
    // or a frustum turns into before it can be read.
    [[nodiscard]] std::vector<TileKey> keysOverlapping(core::WorldRect area,
                                                       std::uint8_t level = 0) const;
    // Conservative, with one 64 m cell of halo for filters, normals and coast.
    [[nodiscard]] bool containsLand(TileKey key) const;
    [[nodiscard]] const terrain::LandMask64& landMask() const { return landMask_; }
    [[nodiscard]] const HydrologyGraph& graph() const { return graph_; }

    // Bake the land pages (plus a filtering halo) at the explicitly requested
    // data levels once and keep them. Geometry-only levels are not listed.
    //
    // A coarse page is small, but there are about ten thousand page positions
    // in the default world because a page stays five hundred and twelve metres.
    // Pure ocean is deliberately absent; the sea surface covers it.
    //
    // Levels not listed stay on demand. H8 and H4 are runtime refinement
    // levels while H16 and H64 are persistent; geometry-only H32/H128/H256
    // have no pages of their own.
    //
    // Pinned rather than kept: these are not a working set and must not be
    // evicted by the ground under the camera, which arrives later and is much
    // larger.
    // Started, not done: it returns at once and the world fills behind it.
    //
    // The frame never waits, but the entire shared worker budget is reserved
    // for this phase. Publish H64 first, then H16; only after all requested
    // levels finish may streaming, detail, meshes and inspection start.
    // A temporarily empty level queue is not completion: its in-flight pages
    // must finish and publish before the pool is released.
    void prebakeInBackground(std::initializer_list<std::uint8_t> levels);
    struct Progress {
        std::size_t done = 0, total = 0;
        bool running = false;
        // The finest level published so far, or kNothingPinned while the
        // coarsest one is still baking. A caller that only needs the coarse
        // world asks this rather than waiting for `running` to go false.
        int finestPinned = kNothingPinned;
        // done counts attempts; published is immutable and actually accessible
        // to readers. A completed-but-unpublished level is not resident yet.
        std::size_t published = 0, failed = 0;
    };
    static constexpr int kNothingPinned = 99;
    [[nodiscard]] Progress prebakeProgress() const;
    [[nodiscard]] std::size_t pinnedBytes() const;

    [[nodiscard]] Stats stats() const;
    void clear();
    // Drop every baked page whose ground overlaps this rectangle, so the next
    // ask rebakes it.
    //
    // What makes an edit show up. A brush writes a difference into the edit
    // layer, and the height field adds it - but a page already baked holds the
    // heights as they were, and the streaming has no reason to doubt it. This
    // is that reason. Returns how many were dropped, which is what a tool
    // reports and a test counts.
    std::size_t forget(core::WorldRect area);

private:
    void keepLocked(TileKey key, std::shared_ptr<const BakedPage> page);
    // This store's baker for the calling thread, made once.
    //
    // Owned here rather than kept in a thread_local keyed on the world: a
    // baker holds references to the world *and* the graph, and a second store
    // over the same world with a different graph would have been handed a
    // baker pointing at a graph that had gone. The store's lifetime bounds
    // its bakers, which is the only honest bound for them.
    [[nodiscard]] BaseTileBaker& bakerForThisThread() const;

    const generation::WorldMapData& world_;
    const HydrologyGraph& graph_;
    HsimQuantisation quantisation_;
    Config config_;
    terrain::LandMask64 landMask_;
    std::unique_ptr<BakedPageCache> diskCache_;

    mutable std::mutex guard_;
    // The order pages were last asked for, oldest first, with the map holding
    // an iterator into it so a hit is a splice rather than a search.
    mutable std::list<TileKey> order_;
    struct Entry {
        std::shared_ptr<const BakedPage> page;
        std::list<TileKey>::iterator age;
    };
    mutable std::unordered_map<TileKey, Entry> pages_;
    // Baked once for the whole world and never evicted.
    //
    // Published as one immutable map rather than filled in place: a reader
    // takes it without a lock, and a half-filled map is never visible to
    // anyone. The bake builds its own and swaps it in when it is whole.
    using Pinned = std::unordered_map<TileKey, std::shared_ptr<const BakedPage>>;
    std::shared_ptr<const Pinned> pinned_ = std::make_shared<const Pinned>();
    std::atomic<std::size_t> pinnedBytes_{0};
    std::atomic<std::size_t> prebakeDone_{0}, prebakeTotal_{0};
    std::atomic<std::size_t> prebakeFailed_{0};
    std::atomic<int> finestPinned_{kNothingPinned};
    std::atomic<bool> prebakeRunning_{false}, prebakeStop_{false};
    std::unique_ptr<TerrainWorkerPool> workerPool_;
    TerrainWorkerPool::Handle prebake_;
    mutable Stats stats_;
    mutable std::size_t bytes_ = 0;

    mutable std::mutex bakerGuard_;
    mutable std::unordered_map<std::thread::id, std::unique_ptr<BaseTileBaker>> bakers_;
};

} // namespace world::streaming
