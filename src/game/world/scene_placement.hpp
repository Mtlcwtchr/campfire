#pragma once
#include <future>
#include <map>
#include <set>
#include "game/world/world_builder.hpp"

namespace world {
struct ScenePlacementSnapshot {
    std::uint64_t worldVersion = 0, version = 0;
    decor::ScatterBounds bounds;
    std::vector<decor::ScatterBounds> regions; // only regions actually resident in this snapshot
    bool complete = false;
    decor::Scatter scatter; // Object::id remains seed/position-derived, not request-derived.
};

class ScenePlacement {
public:
    explicit ScenePlacement(WorldBuilder::Snapshot world) : world_(std::move(world)) {}
    ~ScenePlacement();
    // Owner-thread requests; immutable snapshots are safe to retain across updates.
    // Rectangle compatibility API publishes only once the entire request is ready.
    void update(decor::ScatterBounds bounds, bool detailWanted);
    // Priority-ordered, aligned ownership regions; publishes incremental coverage.
    void updateRegions(std::span<const decor::ScatterBounds> regions, bool enabled);
    std::shared_ptr<const ScenePlacementSnapshot> read() const { return published_.read().value; }
    bool busy() const { return !jobs_.empty() || missing_ != 0; }
    const std::string& error() const { return error_; }
    // Bounded by the machine, never by how much of the world is visible.
    static unsigned workerLimit();
private:
    using Publication = engine::Publication<ScenePlacementSnapshot>;
    using Region = decor::ScatterBounds;
    struct Entry { std::shared_ptr<const decor::Scatter> scatter; std::uint64_t used=0; };
    using Completed = std::vector<std::pair<Region,std::shared_ptr<const decor::Scatter>>>;
    struct Job { std::future<Completed> work; std::vector<Region> regions; };
    void updateDemand(std::vector<Region> regions, Region bounds, bool enabled, bool incremental);
    void collectFinishedJobs();
    void startJobs();
    WorldBuilder::Snapshot world_;
    Publication published_;
    std::vector<Region> order_;
    std::set<Region> wanted_;
    std::map<Region,Entry> cache_;
    Region bounds_;
    bool enabled_=false, incremental_=false, dirty_=false;
    std::size_t missing_=0;
    std::uint64_t clock_=0;
    std::vector<Job> jobs_;
    std::string error_;
};
} // namespace world
