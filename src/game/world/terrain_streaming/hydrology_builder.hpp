#pragma once

#include <filesystem>
#include <memory>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"

namespace world::streaming {

// Pure extraction from a completed WorldMapData. It never mutates the map,
// runs no terrain sampling and has no renderer/main-thread dependency.
//
// The graph contains every course that stands in water the year round, which
// is the set MacroWorld resolves over the whole map: the coarse map's own
// `WorldCell::river` lines plus every catchment above its cell's own stream
// threshold, and everything downstream of those. Minor dry drainage stays in
// the macro map for valley carving; it does not become a water body merely
// because a camera reaches it.
//
// `WorldCell::river` alone is deliberately not the rule. It is drawn above a
// threshold that scales with the map, so on a continent it marks the trunk
// rivers and leaves every stream out - a graph of it would still leave the
// runtime deriving most of the world's water per query.
// A map whose width, height and cell count disagree gives back an empty graph.
// A map whose drainage runs in a circle throws, from the same check that
// rejects it everywhere else: it is not a world that can be routed.
HydrologyGraph buildHydrologyGraph(const generation::WorldMapData& world);

// The one graph a world has, built when it is first asked for and shared by
// everyone who asks after that.
//
// Extraction is three to fourteen milliseconds and the result is tens of
// thousands of reach points; ten workers each building their own would spend
// the time ten times and hold ten copies of an answer that is a pure function
// of the map. Held weakly, so a world that goes takes its graph with it.
//
// Deterministic: the same map gives the same graph, so sharing one changes
// nothing but the cost.
std::shared_ptr<const HydrologyGraph> sharedHydrologyGraph(
        const generation::WorldMapData& world);
// The same, and read from (or written to) a cache under `cacheRoot` when it
// is not empty: the graph of a map opened before comes off the disk.
std::shared_ptr<const HydrologyGraph> sharedHydrologyGraph(
        const generation::WorldMapData& world, const std::filesystem::path& cacheRoot);
// The water is fitted to the ground in groups that read nothing of each other,
// and a group whose inputs are what they were at its last fit takes that fit
// back rather than being fitted again (hydrology_builder.cpp, FitGroups): an
// edit to one island refits that island's water, not the continent's. The
// graph is the same bit for bit either way. What the last build did, and a
// way to make the next one fit everything.
struct HydrologyFitStats {
    std::size_t groups = 0, fitted = 0;
    // What counts as a stream and how wide the widest river is are read off
    // the whole map (one valley in three carries water; widths go against the
    // map's largest flow). When an edit moves them every group's water is a
    // different width, and every group is fitted again - rightly.
    std::uint8_t streamFlow = 0, largestFlow = 0;
};
HydrologyFitStats lastHydrologyFits();
void forgetHydrologyFits();

// What a graph built from this map would carry as its sourceFingerprint.
std::uint64_t hydrologySourceFingerprint(const generation::WorldMapData& world);

} // namespace world::streaming
