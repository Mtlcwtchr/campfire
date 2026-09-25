#pragma once

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
    WorldSnapshot(generation::WorldMapData map, streaming::PageStore::Config config, std::uint64_t version = 0);
    std::uint64_t version() const { return version_; }
    const generation::WorldMapData& worldMap() const { return map_; }
    const ClimateField& climate() const { return climate_; }
    streaming::PageStore& pages() const { return pages_; }
    // HeightField caches are NOT shared: each querying thread gets its own field.
    HeightField field() const { return HeightField(&map_, map_.seed); }
    decor::Scatter scatter(int x, int y, int radiusMetres = decor::kRadius) const;
    decor::Scatter scatter(decor::ScatterBounds bounds) const;
    struct Landmark { std::string name; core::WorldPos where; };
    const std::vector<Landmark>& landmarks() const { return landmarks_; }
    core::WorldPos startingPoint() const;

private:
    const std::uint64_t version_;
    const generation::WorldMapData map_;
    streaming::HydrologyGraph hydrology_;
    ClimateField climate_;
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
    void cancel() { published_.cancel(); }
    Snapshot read() const { return published_.read().value; }
    static streaming::PageStore::Config defaultPageConfig();
private:
    streaming::PageStore::Config config_;
    engine::Publication<WorldSnapshot> published_;
};

} // namespace world
