#pragma once
// Turning a set of cells into a mesh.
//
// The navmesh knows which cells of a chunk a body can cross; this turns that
// into polygons. It is here rather than inside navmesh.cpp because it is pure
// geometry over integers - no field, no world, no chunks - and that means it can
// be tested on masks drawn by hand: a square, a diagonal, a ring with a hole in
// it. Every geometry bug this kind of code has is visible in a five-by-five
// mask and invisible in a continent.
//
// Integers throughout, and the same integers on every machine. The mesh decides
// where people can walk, so it is part of the simulation: two machines that
// triangulate the same ground differently are two machines that send a person
// two different ways (rule 2).

#include <array>
#include <cstdint>
#include <vector>

namespace world::shape {

// A corner of the cell lattice, in cells. The mask is `side` by `side` cells, so
// its corners run from 0 to side inclusive.
struct Lattice {
    std::int32_t x = 0, y = 0;
    friend bool operator==(Lattice a, Lattice b) { return a.x == b.x && a.y == b.y; }
    friend bool operator!=(Lattice a, Lattice b) { return !(a == b); }
};

// Two cells that touch only at a corner are two cells with no way between them.
//
// A boundary through such a pinch has two equally good ways to turn, and either
// choice makes a loop that touches itself - which nothing downstream can
// triangulate. Worse, if the mesh did join them, a body would be told it can
// walk through a gap of no width at all. So one of the two is dropped, always
// the same one, before anything else looks at the mask.
void unpinch(std::vector<char>& mask, std::int32_t side);

// The boundary of the marked cells, as closed loops on the lattice of their
// corners. The ground is on the same side of every loop, so an outer boundary
// and the boundary of a hole come out wound opposite ways: outer loops enclose
// a negative area by twiceAreaOf, holes a positive one.
std::vector<std::vector<Lattice>> loopsOf(const std::vector<char>& mask, std::int32_t side);

// Twice the signed area of a loop. Sign says which way it is wound.
std::int64_t twiceAreaOf(const std::vector<Lattice>& loop);

// Corners taken off a loop while the line stays within `tolerance` cells of
// where it was - and while every edge it leaves behind still runs over marked
// cells only.
//
// The second half is the whole difficulty. A staircase along a diagonal shore
// should become one diagonal edge, and that edge cuts a little walkable ground
// off the polygon - which is a loss of nothing much. Cut the other way, across
// a notch, and the polygon takes in ground the body cannot cross: on a shore
// that is a polygon over water, and a body sent across it walks into a river.
// So every edge the simplification proposes is rasterised back onto the mask
// and kept only if the cells under it are all marked.
void simplify(std::vector<Lattice>& loop, const std::vector<char>& mask, std::int32_t side,
              std::int32_t tolerance);

// The loops of one region, as triangles. The first loop is the outer boundary,
// the rest are holes.
std::vector<std::array<Lattice, 3>> triangulate(const std::vector<Lattice>& outer,
                                                const std::vector<std::vector<Lattice>>& holes);

// Triangles joined back into convex polygons, as far as they will go: fewer
// polygons is fewer nodes for a path to be planned over, and a plain has no
// business being a thousand triangles.
std::vector<std::vector<Lattice>> mergeConvex(const std::vector<std::array<Lattice, 3>>& triangles,
                                              std::size_t mostCorners = 6);

} // namespace world::shape
