#pragma once
#include <mutex>

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "engine/core/publication.hpp"
#include "game/world/climate_field.hpp"
#include "game/world/scene_scatter.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace world {

// Immutable world data plus a thread-safe, logically mutable page cache.
// Readers retain a lease. Workers must be drained before releasing their lease.
class WorldSnapshot {
public:
    // `edits` is the world's persistent object/ecology delta when it has one
    // (world_delta.hpp): it outlives this snapshot, so a rebuilt base keeps
    // what was cut and planted in it. Without one the snapshot keeps its own,
    // which lasts as long as it does. `heights` is the same delta's ground:
    // every field this snapshot hands out, and every page it bakes, stands on
    // it. Without one the ground is the generator's alone.
    WorldSnapshot(generation::WorldMapData map, streaming::PageStore::Config config, std::uint64_t version = 0,
                  std::shared_ptr<ecology::Store> edits = {}, std::shared_ptr<const EditLayer> heights = {});
    std::uint64_t version() const { return version_; }
    const generation::WorldMapData& worldMap() const { return map_; }
    const ClimateField& climate() const { return climate_; }
    ecology::Store& ecology() const { return *ecology_; }
    const std::shared_ptr<ecology::Store>& ecologyStore() const { return ecology_; }
    // What people dug into this world, or null. Anything made from the ground
    // remembers `edits()->revision()` from before it read, and is stale when
    // `edits()->revisionIn(what it read)` has passed that.
    const EditLayer* edits() const { return heights_.get(); }
    streaming::PageStore& pages() const { return pages_; }
    // HeightField caches are NOT shared: each querying thread gets its own field.
    // The macro layer's whole-map pass is made once for the snapshot and every
    // field shares it: it was made afresh for each, and the forest, the
    // shadows and the page workers ask for fields all the time.
    HeightField field() const {
        std::call_once(macroOnce_, [&] { macroResolved_ = HeightField(&map_, map_.seed).macro().resolved(); });
        HeightField result(&map_, map_.seed, macroResolved_);
        result.edits(heights_.get());
        return result;
    }
    decor::Scatter scatter(int x, int y, int radiusMetres = decor::kRadius) const;
    decor::Scatter scatter(decor::ScatterBounds bounds) const;
    decor::Scatter scatter(decor::ScatterBounds bounds, const ecology::Delta& delta) const;
    decor::Scatter scatter(decor::ScatterBounds bounds, const ecology::Delta& delta, HeightField& query) const;
    ecology::Cell ecologyAt(double x, double y, HeightField& field, const ecology::Delta& delta) const;
    struct Landmark { std::string name; core::WorldPos where; };
    const std::vector<Landmark>& landmarks() const { return landmarks_; }
    core::WorldPos startingPoint() const;
    // What raising this snapshot cost, in milliseconds: the drainage graph,
    // the climate, the landmarks. The prebake that follows is on its own
    // clock (pages().prebakeProgress()).
    struct Timings { double graph = 0, climate = 0, landmarks = 0; };
    const Timings& timings() const { return timings_; }

private:
    const std::chrono::steady_clock::time_point began_ = std::chrono::steady_clock::now();
    Timings timings_;
    const std::uint64_t version_;
    const generation::WorldMapData map_;
    mutable std::once_flag macroOnce_;
    mutable std::shared_ptr<const MacroWorld::Resolved> macroResolved_;
    // Registered where every HeightField over map_ looks for it
    // (sharedHydrologyGraph), and held for as long as the snapshot lives: a
    // graph held only by the fields that asked for it was built afresh by
    // each field that came after the last one went - three times over while
    // a world was raised.
    std::shared_ptr<const streaming::HydrologyGraph> hydrology_;
    ClimateField climate_;
    std::shared_ptr<ecology::Store> ecology_;
    std::shared_ptr<const EditLayer> heights_;   // before pages_: its bakers read it
    mutable streaming::PageStore pages_; // destroyed first, joins its workers
    std::vector<Landmark> landmarks_;
    void findLandmarks();
};

// Requests, completion and reads may cross threads. Only the latest request
// may publish; readers retain old worlds independently of the producer.
// No callbacks into a view, no SDL, GPU resources or camera in world generation.
class WorldBuilder {
public:
    using Snapshot = std::shared_ptr<const WorldSnapshot>;
    using Ticket = engine::Publication<WorldSnapshot>::Ticket;
    explicit WorldBuilder(streaming::PageStore::Config config = defaultPageConfig())
        : config_(std::move(config)) {}
    void build(const generation::WorldMapParams& params);
    void publish(generation::WorldMapData map);
    Ticket request() { return published_.request(); }
    bool complete(Ticket ticket, generation::WorldMapData map);
    // The two halves of complete(), for a world that should be shown only once
    // it is ready: raised (its drainage, climate and the prebake of its pages
    // begun) without being published - null if a newer world was asked for
    // meanwhile - and then published, which fails the same way. Between the
    // two the world that is showing goes on being the one read.
    std::shared_ptr<const WorldSnapshot> raise(Ticket ticket, generation::WorldMapData map);
    bool publish(Ticket ticket, std::shared_ptr<const WorldSnapshot> raised);
    bool current(Ticket ticket) const { return published_.current(ticket); }
    void cancel() { published_.cancel(); }
    Snapshot read() const { return published_.read().value; }
    // The persistent delta's object store and ground, handed to every snapshot
    // built from now on; attaching nothing detaches them, and each snapshot
    // keeps its own objects and the generator's ground again. Attach them for
    // the world they belong to and detach them before building a different
    // one - a delta is the history of one world, not of any.
    void attach(std::shared_ptr<ecology::Store> edits, std::shared_ptr<const EditLayer> heights = {}) {
        edits_ = std::move(edits);
        heights_ = std::move(heights);
    }
    static streaming::PageStore::Config defaultPageConfig();
private:
    streaming::PageStore::Config config_;
    std::shared_ptr<ecology::Store> edits_;
    std::shared_ptr<const EditLayer> heights_;
    engine::Publication<WorldSnapshot> published_;
};

} // namespace world
