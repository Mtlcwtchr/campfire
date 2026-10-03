#pragma once
#include <atomic>
#include <future>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "game/world/world_builder.hpp"

namespace world {
struct ScenePlacementSnapshot {
    std::uint64_t worldVersion = 0, version = 0;
    std::uint64_t objectsVersion = 0; // changes only when object values change
    decor::ScatterBounds bounds;
    std::vector<decor::ScatterBounds> regions; // only regions actually resident in this snapshot
    using Revisions=std::unordered_map<decor::ScatterBounds,std::uint64_t,decor::ScatterBoundsHash>;
    Revisions regionRevisions;
    bool complete = false; // all admitted regions, not necessarily the whole visible world
    bool capacityLimited = false;
    decor::Scatter scatter; // Object::id remains seed/position-derived, not request-derived.
    // The same objects by region, `parts[i]` the scatter of `regions[i]`,
    // shared with the placement's cache (nothing copied). What the renderer
    // reads: a region whose part is the same pointer as last time is the same
    // objects, and nothing downstream has to look at it again. `scatter` holds
    // the merged list only when the placement merges (Limits::merged); its
    // counts (sampled, populations) are filled either way.
    std::vector<std::shared_ptr<const decor::Scatter>> parts;
    // Every object of every part in id order, merged on request (tools and
    // tests; the renderer reads the parts).
    [[nodiscard]] std::vector<decor::Object> mergedObjects() const;
    [[nodiscard]] std::size_t objectCount() const {
        std::size_t n = 0;
        for (const auto& p : parts) n += p ? p->objects.size() : 0;
        return n;
    }
};

class ScenePlacement {
public:
    struct Limits {
        // The warm fringe is what makes coming back free: a region's scatter
        // is a few kilobytes, re-sampling it is milliseconds of height/ecology
        // work. 128 of them were spent by one turn of the camera.
        std::size_t regions=512, warmRegions=2048;
        std::size_t startsPerUpdate=32, regionsPerJob=8;
        std::size_t collectsPerUpdate=8, publishesPerUpdate=8;
        // A region that leaves the view stays resident this many updates
        // (culling hides it anyway). Every membership change is a full
        // republication of the object list and a regroup downstream; turning
        // the camera used to cause one per frame. An explicitly empty demand
        // still clears at once. 0 = no lingering (strict admitted set).
        std::uint64_t lingerUpdates=0;
        // While the admitted set is still filling, publish at most this often
        // (updates) instead of every update that admitted a region.
        std::uint64_t publishEvery=1;
        // Whether a snapshot also carries every object in one list, in id
        // order (tools, tests). Off, only `parts`.
        bool merged=true;
        // What the game's streaming uses (tests keep the strict defaults).
        static Limits streaming() {
            Limits l;
            // The whole view in orbit, not a few chunks: past the mesh line
            // every tree is an impostor, so four times the regions is mostly
            // instance records, not triangles.
            l.regions=2048;
            l.warmRegions=4096;
            l.lingerUpdates=240;
            l.publishEvery=6;
            l.publishesPerUpdate=32;
            // The renderer reads the parts; the merged list of every object
            // in view, rebuilt on each publication, is not needed there.
            l.merged=false;
            return l;
        }
    };
    struct Stats {
        std::size_t requested=0, admitted=0, cached=0, resident=0, inFlight=0;
        std::size_t started=0, collected=0, published=0;
        bool limited=false;
    };
    explicit ScenePlacement(WorldBuilder::Snapshot world) : ScenePlacement(std::move(world),Limits{}) {}
    ScenePlacement(WorldBuilder::Snapshot world, Limits limits);
    ~ScenePlacement();
    // Owner-thread requests; immutable snapshots are safe to retain across updates.
    // Rectangle compatibility API publishes only once the entire request is ready.
    void update(decor::ScatterBounds bounds, bool detailWanted);
    // Priority-ordered, aligned ownership regions; only the bounded input prefix
    // is admitted. Duplicate entries are harmless; deferred tails are not queued.
    void updateRegions(std::span<const decor::ScatterBounds> regions, bool enabled);
    std::shared_ptr<const ScenePlacementSnapshot> read() const { return published_.read().value; }
    bool busy() const { return !jobs_.empty() || missing_ != 0; }
    const std::string& error() const { return error_; }
    const Limits& limits() const { return limits_; }
    const Stats& stats() const { return stats_; }
    // The world lease the regions are generated from. Other bounded consumers
    // (the far forest hierarchy) generate from the same immutable snapshot.
    const WorldBuilder::Snapshot& world() const { return world_; }
    // Bounded by the machine, never by how much of the world is visible.
    static unsigned workerLimit();
private:
    using Publication = engine::Publication<ScenePlacementSnapshot>;
    using Region = decor::ScatterBounds;
    using RegionHash=decor::ScatterBoundsHash;
    // `stale`: the ground under it has been dug since it was placed. It is
    // still shown - trees a metre off for a moment beat trees gone - and is
    // placed again as if it were missing.
    struct Entry { std::shared_ptr<const decor::Scatter> scatter; std::uint64_t used=0; bool stale=false; };
    using Completed = std::vector<std::pair<Region,std::shared_ptr<const decor::Scatter>>>;
    struct Job {
        std::future<Completed> work;
        std::vector<Region> regions;
        Completed ready;
        std::size_t next=0;
        std::shared_ptr<std::atomic_bool> cancel;
    };
    void updateDemand(std::vector<Region> regions, Region bounds, bool enabled, bool incremental,
                      bool limited=false);
    void collectFinishedJobs();
    void startJobs();
    // Whether nothing has been dug under a region since it was placed.
    [[nodiscard]] bool groundCurrent(const Region& region, const decor::Scatter& scatter) const;
    WorldBuilder::Snapshot world_;
    Limits limits_;
    Stats stats_;
    Publication published_;
    std::vector<Region> order_;
    std::unordered_set<Region,RegionHash> wanted_;
    std::unordered_set<Region,RegionHash> resident_;
    std::unordered_map<Region,Entry,RegionHash> cache_;
    Region bounds_;
    bool enabled_=false, incremental_=false, dirty_=false, limited_=false;
    std::size_t missing_=0;
    std::size_t stale_=0;   // wanted regions shown over ground dug since
    std::uint64_t clock_=0;
    std::uint64_t lastPublish_=0;
    std::uint64_t ecologyRevision_=0;
    std::uint64_t biomesGeneration_ = 0;
    std::uint64_t groundRevision_=0;
    std::vector<Job> jobs_;
    std::string error_;
};
} // namespace world
