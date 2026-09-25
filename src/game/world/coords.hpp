#pragma once
// Where things are in a world that has no edges.
//
// There is one coordinate system: metres from the world's origin, held in
// Q32.32 fixed point as core::WorldPos. Everything in this directory is
// addressing on top of that, and nothing below it.
//
// A chunk is a unit of storage, generation, streaming and activation. It is not
// a boundary of the world: no query stops at one, no coordinate is expressed
// relative to one, and nothing is kept in a chunk that could not be recomputed
// from the world coordinates it happens to cover. That is what makes seams
// impossible rather than merely fixed - see height_field.hpp.
//
// On precision, which is the usual reason a big world is cut into sectors:
// Q32.32 holds +-2.1 x 10^9 metres to within a quarter of a nanometre. This
// world is 368 kilometres across, so one global position is exact everywhere in
// it and the simulation needs no sector split at all. Precision loss is a float
// problem, and floats live only in the renderer - which draws relative to the
// camera, so what reaches a float is metres from the view rather than metres
// from the world's corner.

#include <cstdint>
#include <functional>
#include <vector>

#include "engine/core/geometry.hpp"

namespace world {

// The side of a chunk, in metres. Picked against the height lattice (a chunk is
// a whole number of samples across, height_field.hpp) and against how much
// ground is worth generating or dropping at once. Nothing the player can see
// depends on it: at 64 metres a chunk is a third of the old local map, and the
// only thing that changes if it doubles is how much work a stream does at once.
inline constexpr std::int32_t kChunkMetres = 64;

// Integer division that rounds towards minus infinity. The world runs in every
// direction from the origin, and C++ division truncates towards zero - which
// puts everything in the quadrant west and north of the origin one chunk out
// and mirrors the whole map along the axes.
constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

constexpr std::int64_t floorMod(std::int64_t a, std::int64_t b) {
    const std::int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

struct ChunkId {
    std::int32_t x = 0;
    std::int32_t y = 0;

    friend constexpr bool operator==(ChunkId a, ChunkId b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(ChunkId a, ChunkId b) { return !(a == b); }
    // Ordered so a chunk can be a key in a sorted container without a hash.
    friend constexpr bool operator<(ChunkId a, ChunkId b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    }
};

inline ChunkId chunkOf(core::WorldPos p) {
    return {static_cast<std::int32_t>(floorDiv(p.x.toInt(), kChunkMetres)),
            static_cast<std::int32_t>(floorDiv(p.y.toInt(), kChunkMetres))};
}

// The north-west corner of a chunk, in world metres.
inline core::WorldPos chunkOrigin(ChunkId c) {
    return {core::Fixed::fromInt(static_cast<std::int64_t>(c.x) * kChunkMetres),
            core::Fixed::fromInt(static_cast<std::int64_t>(c.y) * kChunkMetres)};
}

inline bool inside(ChunkId c, core::WorldPos p) { return chunkOf(p) == c; }

// Every chunk a rectangle of world touches, corners included. What streaming
// asks for, and what any query wider than one chunk has to walk - a query is
// never allowed to stop at a border.
inline std::vector<ChunkId> chunksOverlapping(core::WorldPos min, core::WorldPos max) {
    const ChunkId first = chunkOf(min);
    const ChunkId last = chunkOf(max);
    std::vector<ChunkId> out;
    out.reserve(static_cast<std::size_t>((last.x - first.x + 1) * (last.y - first.y + 1)));
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) out.push_back({x, y});
    return out;
}

// A chunk and the ring of chunks around it. Anything that reads across a border
// - normals, cliff lines, a navmesh portal, a spatial query near an edge - takes
// this rather than the chunk alone.
inline std::vector<ChunkId> withNeighbours(ChunkId c) {
    std::vector<ChunkId> out;
    out.reserve(9);
    for (std::int32_t dy = -1; dy <= 1; ++dy)
        for (std::int32_t dx = -1; dx <= 1; ++dx) out.push_back({c.x + dx, c.y + dy});
    return out;
}

} // namespace world

template <>
struct std::hash<world::ChunkId> {
    std::size_t operator()(world::ChunkId c) const noexcept {
        return std::hash<std::uint64_t>{}((static_cast<std::uint64_t>(std::uint32_t(c.x)) << 32) |
                                          std::uint32_t(c.y));
    }
};
