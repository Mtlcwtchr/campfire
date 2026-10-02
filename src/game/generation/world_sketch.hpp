#pragma once
// The sketch of a coast: what the continents layer says of the regions still
// in the Sketch stage (world_layout.hpp, RegionStage), as geometry the
// renderer extrudes (game::SketchPass).
//
// Painting a coastline is deciding where the land will be, not asking for it
// to be made, so nothing is generated for a sketch: the paint is read texel by
// texel and drawn as a slab standing out of the sea - a flat top wherever the
// paint is over the shore value and a wall along the line where it crosses it.
// A stroke costs the time it takes to rebuild this (a few milliseconds for a
// continent) and nothing else; the world is only worked out when the sketch is
// pinned.
//
// The top is the paint's own lattice (a vertex at every texel centre of the
// tiles over sketch regions, with the paint's value on it): the renderer
// clips it at the shore value, so the coast follows the paint between texels
// rather than their squares. The walls are the same line, found by marching
// squares over that lattice, so top and wall meet.
#include <cstdint>
#include <vector>

#include "game/generation/world_layout.hpp"

namespace generation {

struct SketchMesh {
    // x, y in world metres; `top` 1 for the slab's top and a wall's upper
    // edge, 0 for a wall's foot at the sea; `value` the paint's shape value
    // (0..1023), the top drawn only where it is over `shore`.
    struct Vertex {
        float x = 0, y = 0, top = 1, value = 0;
    };
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    float shore = 512.0f;
    // What it was made from, so a caller can tell whether it is stale.
    std::size_t sketchRegions = 0;
    bool empty() const { return indices.empty(); }
};

// Every sketch region's paint. Empty when no region is a sketch.
SketchMesh sketchMesh(const WorldLayout& layout);

} // namespace generation

