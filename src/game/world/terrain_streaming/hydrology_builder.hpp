#pragma once

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

} // namespace world::streaming
