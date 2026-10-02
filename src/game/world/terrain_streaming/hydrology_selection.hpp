#pragma once
// Choosing a piece of the water to edit, with what depends on it.
//
// A river is not a thing on its own: the brooks that run into it are part of
// what it is, and a lake is the rivers that fill it. So picking a reach picks
// everything UPSTREAM of it - its tributaries, their tributaries, the lakes
// they pass through and whatever feeds those - and nothing downstream: a
// brook picked is that brook and its springs, not the river it ends in.
//
// What is picked is also what an edit has to be answered for: its bounds and
// macro cells are the only ground a change to it can move (the rebuild after
// the edit is local to them, and to the land around them).
#include <cstdint>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"

namespace world::streaming {

struct WaterSelection {
    std::vector<RiverId> segments;           // ascending
    std::vector<WaterBodyId> waterBodies;    // ascending; the ocean never
    core::WorldRect bounds{};                 // the water and the ground it shapes
    std::vector<std::int32_t> macroCells;     // ascending, unique
    bool empty() const { return segments.empty() && waterBodies.empty(); }
};

// A reach and everything that drains into it.
WaterSelection upstreamOfSegment(const HydrologyGraph& graph, RiverId segment);
// A lake and everything that drains into it. The ocean selects only itself
// (the whole world drains into it, and editing "the sea and all its rivers"
// is not an edit anybody means).
WaterSelection upstreamOfWaterBody(const HydrologyGraph& graph, WaterBodyId body);

// What is under a point: the nearest reach whose water or valley reaches it,
// or the lake whose footprint holds it; nothing (0, 0) if neither. A reach is
// preferred to the lake it runs into only where it is nearer than the shore.
struct WaterPick {
    RiverId segment = kInvalidRiverId;
    WaterBodyId body = kInvalidWaterBodyId;
};
WaterPick pickWater(const HydrologyGraph& graph, core::WorldPos at, core::Fixed slack = core::Fixed::fromInt(40));

// Whichever of the two it picked, with its dependencies.
WaterSelection selectWater(const HydrologyGraph& graph, core::WorldPos at);

} // namespace world::streaming

