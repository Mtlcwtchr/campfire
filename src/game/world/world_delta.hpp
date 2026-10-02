#pragma once
// The persistent delta: everything that has happened to a world.
//
// The world the player walks is three things added together (spec §2):
//
//   authored source + generator  ->  procedural base   (a function; never saved)
//   persistent delta             ->  what happened      (this; always saved)
//   derived caches               ->  how it is drawn    (rebuilt at will)
//
// The base can be regenerated - a new generator version, a layout edited in
// the world editor - and the delta must come through it untouched: the valley
// dug before the game started and the grove cut in its third year are facts
// of the world's history, not of the build that first showed them. So the
// delta is world-space and records DIFFERENCES: height added per sample, a
// generated object named by its stable id and marked gone, an object planted,
// an ecology cell set. Nothing in it is a copy of generated data.
//
// One API for all of history (§12). The editor placing a settlement before the
// game and a villager felling a tree during it make the same records through
// the same calls; `Origin` says which, for tracing, never for behaviour. The
// views the rest of the engine reads - the edit layer the height field adds,
// the object store the scatter filters - are owned here and updated in place,
// so an edit is visible the moment it is made. Writes that reach those views
// by another road are noticed too (both views report every change), and are
// saved all the same: nothing that changed the world can fail to persist
// because it came through the wrong door.
//
// On disk (§7, §13, §17) the delta is one file per 8 km authoring chunk under
// the world root, each a compacted snapshot of the chunk plus the journal of
// ops since, and a manifest that commits them together. A save never stops
// the world: it copies what changed under a short lock and writes it from its
// own thread; the manifest is replaced last and atomically, so an edit that
// spans two files is in both or in neither. A chunk nobody touched is never
// rewritten, and a chunk that cannot be read costs that chunk, not the world.
//
// Height edits are drawn (D149): the snapshot's pages are baked over the
// layer this delta owns, and everything made from the ground - pages, their
// disk cache, meshes, placed trees - checks the layer's revisions itself, so
// the renderer needs nothing from this class. What still stands on the
// generator's ground: water, shadows, the far forest and the simulation.
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "engine/core/geometry.hpp"
#include "engine/world_store/world_root.hpp"
#include "game/world/world_delta_codec.hpp"

namespace world::delta {

// What an edit makes stale downstream (§16), one bit per derived layer.
enum DerivedLayer : std::uint16_t {
    TerrainGeometry = 1u << 0,   // ground meshes and height pages
    Water = 1u << 1,             // water surfaces and drainage, where the ground under them moved
    EcologyField = 1u << 2,      // the ecology cells read from the ground
    Vegetation = 1u << 3,        // placement of trees, bushes and rocks
    NavCollision = 1u << 4,      // path costs and what can be walked on
    Shadows = 1u << 5,           // shadow maps and occluder caches
    Impostors = 1u << 6,         // forest proxies and their hierarchy
};
const char* derivedLayerName(std::uint16_t layer);

struct Invalidation {
    std::uint16_t layers = 0;
    MetreRect area;
    // The edit that caused it; nought for a change that came through another
    // road than this API.
    std::uint64_t sequence = 0;
};

// The dependency graph: what an op makes stale, and how far. Each layer has
// its own reach, and it is the edit's reach, not a region's: a flattened
// house plot costs tens of metres of ground and nav, and a shadow as long as
// the height it changed casts at a low sun - not the region around it.
std::vector<Invalidation> invalidations(const Op& op);

struct LoadReport {
    bool fresh = true;              // there was no delta on disk
    std::size_t chunks = 0, ops = 0, swept = 0;
    std::vector<std::string> damaged;
    std::vector<std::string> warnings;
};

struct SaveReport {
    bool ok = true;
    bool nothingToDo = false;
    std::uint64_t commit = 0;
    std::size_t written = 0, compacted = 0, dropped = 0, swept = 0;
    std::uint64_t bytes = 0;
    std::string error;
};

struct ChunkInfo {
    ChunkKey key;
    std::uint64_t revision = 0, committed = 0;
    std::size_t journal = 0;
    bool stale = false, damaged = false;
    std::string file;
    std::uint64_t fileBytes = 0;
};

struct DeltaOptions {
    // Ops a chunk's journal may hold before a save folds it into the
    // snapshot. The journal makes a save of a busy chunk cheap - the snapshot
    // is written as it was last encoded - and folding keeps replaying it at
    // load cheap.
    std::size_t journalLimit = 256;
    int compression = 3;
};

class WorldDelta {
public:
    using Options = DeltaOptions;

    // In memory, with no root: edits work and nothing is written.
    explicit WorldDelta(std::uint64_t worldSeed, Options options = {});
    // Opened on a root and read now. A root with no manifest is a new world and
    // costs nothing until something is saved. Nothing when the manifest itself
    // cannot be read: then which files are the world is unknown, and running on
    // without them would overwrite it at the first save.
    static std::unique_ptr<WorldDelta> open(engine::world_store::WorldRoot root, std::uint64_t worldSeed,
                                            LoadReport* report = nullptr, Options options = {});
    // Finishes a save that was asked for; never starts one of its own.
    ~WorldDelta();
    WorldDelta(const WorldDelta&) = delete;
    WorldDelta& operator=(const WorldDelta&) = delete;

    [[nodiscard]] std::uint64_t worldSeed() const { return worldSeed_; }
    [[nodiscard]] const std::shared_ptr<ecology::Store>& objects() const { return objects_; }
    [[nodiscard]] const std::shared_ptr<EditLayer>& heights() const { return heights_; }
    [[nodiscard]] const std::optional<engine::world_store::WorldRoot>& root() const { return root_; }
    // The layout this delta sits over, recorded in the manifest at the next save.
    void source(std::string relativeLayout, std::uint64_t layoutHash);

    // ---- the one edit API (§12) ----
    // A brush stroke, resolved against `ground` (which must include what this
    // delta already holds) and recorded as the samples it changed. Returns how
    // many it changed.
    std::size_t brush(const Brush& brush, const GroundAt& ground, core::WorldPos at, double seconds,
                      Origin origin);
    // Any op. Returns the sequence it was recorded under, or nought when it
    // changed nothing (a tree removed twice) and so was not recorded.
    std::uint64_t apply(Op op);
    bool removeObject(std::uint64_t id, double x, double y, Origin origin);
    // A removed generated object stands again (undo of removeObject). False
    // when it was not removed.
    bool restoreObject(std::uint64_t id, double x, double y, Origin origin);
    // Returns the new object's id.
    std::uint64_t plant(const ecology::Added& object, Origin origin);
    bool clearPage(double x, double y, std::uint32_t models, Origin origin);
    void setEcology(double x, double y, const ecology::Cell& cell, Origin origin);

    // ---- versions (§9.3, §17) ----
    // A worker reads a chunk at a revision and commits only if it is still at
    // it: anything built from an older one describes a world that has moved.
    struct Token {
        ChunkKey key;
        std::uint64_t revision = 0;
    };
    [[nodiscard]] std::uint64_t revision(const ChunkKey& chunk) const;
    [[nodiscard]] Token token(const ChunkKey& chunk) const { return {chunk.within(kFileLevel), revision(chunk)}; }
    [[nodiscard]] bool current(const Token& token) const { return revision(token.key) == token.revision; }

    // ---- dependency (§16) ----
    // What has gone stale since the last call, oldest first.
    std::vector<Invalidation> drainInvalidations();

    // ---- persistence (§13, §17) ----
    [[nodiscard]] bool dirty() const;
    // Commits now, on the calling thread.
    SaveReport save();
    // Returns at once; the save runs on the delta's own thread, and one asked
    // for while another runs is folded into the next.
    void saveInBackground();
    // Waits for any save asked for to finish, and says how the last one went.
    SaveReport waitForSave();
    // The next save rewrites every chunk that has a file as a fresh snapshot,
    // journal folded in. That is a change of those chunks as far as tokens go:
    // their revisions move, because their files do.
    void compact();

    [[nodiscard]] std::vector<ChunkInfo> chunks() const;
    [[nodiscard]] std::vector<Op> journal(const ChunkKey& chunk) const;
    [[nodiscard]] std::uint64_t nextSequence() const;
    // Everything the delta holds - heights and objects - as one number. Two
    // deltas with the same content have the same hash, however they got it.
    [[nodiscard]] std::uint64_t contentHash() const;

    // Fault injection: stop saves after the chunk files and before the
    // manifest, which is the worst moment for a crash.
    void failBeforeCommitForTesting(bool fail) { failBeforeCommit_ = fail; }

private:
    struct Record {
        std::uint64_t revision = 0, committed = 0;
        std::string file;
        std::uint64_t fileBytes = 0, payloadHash = 0;
        std::vector<Block> snapshot;     // as last encoded
        std::vector<Op> journal;         // since then
        bool stale = false;              // changed around the journal: must be re-snapshotted
        bool damaged = false;            // its file could not be read; kept aside before it is replaced
        std::uint64_t foreignObjects = 0, foreignHeights = 0;   // the latest such change, per view
    };

    void watch();
    void drainForeignLocked() const;
    std::uint64_t applyLocked(Op op);
    SaveReport saveNow();
    void saverLoop();

    const std::uint64_t worldSeed_;
    Options options_;
    std::optional<engine::world_store::WorldRoot> root_;
    std::shared_ptr<ecology::Store> objects_ = std::make_shared<ecology::Store>();
    std::shared_ptr<EditLayer> heights_ = std::make_shared<EditLayer>();

    // Records, journals, sequence and invalidations. Mutable because catching
    // up with what the views reported is bookkeeping, not a change: a const
    // question about a chunk's revision has to see every write already made.
    mutable std::mutex mutex_;
    mutable std::map<ChunkKey, Record> records_;
    std::uint64_t nextSequence_ = 1, commit_ = 0;
    std::string source_;
    std::uint64_t sourceHash_ = 0;
    bool forceCompact_ = false;
    mutable std::vector<Invalidation> invalidations_;

    // Changes the views reported that did not come through this API. A leaf
    // lock: the views call in while holding their own, so nothing may be
    // called out to while it is held.
    mutable std::mutex foreignMutex_;
    mutable std::map<std::pair<ecology::Key, int>, std::uint64_t> foreign_;   // (page or block, -1 or height level) -> revision

    std::mutex saveMutex_;       // one save at a time
    std::atomic<bool> failBeforeCommit_{false};

    std::mutex saverMutex_;
    std::condition_variable saverWake_, saverIdle_;
    bool saveRequested_ = false, saving_ = false, stopping_ = false;
    SaveReport lastSave_;
    std::thread saver_;
};

} // namespace world::delta


