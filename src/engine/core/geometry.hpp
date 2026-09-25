#pragma once
// Square grid geometry (see doc/DECISIONS.md D2, D20 and D44).
//
//   Pos2<T>  - a pair of whatever the coordinate is made of.
//   TilePos  - Pos2<int32>: an integer address on the map. Zones, buildings,
//              stockpiles, paths - everything the simulation still thinks in.
//   WorldPos - Pos2<Fixed>: Q32.32 continuous metres. Where a body actually
//              stands this tick.
//   Pos3<T>  - the same with a height, for the ground and everything on it.
//
// The two named ones are aliases rather than types of their own, because they
// are the same idea in different arithmetic and every operator written twice is
// an operator that can disagree with itself. Naming them after what they are
// used for is worth keeping - a function taking a TilePos says more than one
// taking a Pos2<int32> - but the geometry belongs to the template.
//
// Tiles are square and one metre on a side. TilePos.x is a column, TilePos.y is
// a row, and the centre of tile (x, y) is at world (x, y), so the tile spans
// half a metre either way. The map is a rectangle and the tile store is a flat
// array.
//
// This started out as a tile grid, for the usual reason: on a tile every one of
// the six neighbours is exactly one step away, while on a square grid a diagonal
// is 1.41 times a cardinal. It was not worth it. Nothing in this game reads a
// tile - not the fields, not the walls, not the houses, and least of all the art,
// which is drawn on square cards. What the tile grid did instead was make every
// area a rosette, every wall a zigzag, and every ground texture a lattice of
// identical medallions that had to be rotated per tile to hide the seams. The
// diagonal correction is one line in the pathfinder; the tile was a tax on
// everything else.

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/core/fixed.hpp"

namespace core {

// One tile is one metre on a side.
inline constexpr Fixed kTileSize = kOne;

template <class T>
struct Pos2 {
    T x{};
    T y{};

    friend constexpr bool operator==(Pos2 a, Pos2 b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(Pos2 a, Pos2 b) { return !(a == b); }
    friend constexpr Pos2 operator+(Pos2 a, Pos2 b) { return {a.x + b.x, a.y + b.y}; }
    friend constexpr Pos2 operator-(Pos2 a, Pos2 b) { return {a.x - b.x, a.y - b.y}; }
};

template <class T>
struct Pos3 {
    T x{};
    T y{};
    T z{};

    friend constexpr bool operator==(Pos3 a, Pos3 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }
    friend constexpr bool operator!=(Pos3 a, Pos3 b) { return !(a == b); }
    friend constexpr Pos3 operator+(Pos3 a, Pos3 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z};
    }
    friend constexpr Pos3 operator-(Pos3 a, Pos3 b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }
    constexpr Pos2<T> flat() const { return {x, y}; }
};

// An integer address on the map. The word "tile" is a leftover from when the
// ground was a grid of them and the name is kept only because five hundred call
// sites read better for it; nothing about this is a tile any more.
using TilePos = Pos2<std::int32_t>;
// Continuous metres, where a body actually stands.
using WorldPos = Pos2<Fixed>;
// The same with a height: a point on the ground, or above it.
using WorldPos3 = Pos3<Fixed>;

// Axis-aligned rectangle in world metres. The seamless terrain, zone outlines
// and building footprints speak this language directly; the tile grid is only a
// cache layered on top.
struct WorldRect {
    WorldPos min;
    WorldPos max;

    constexpr bool contains(WorldPos p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
    }
    constexpr bool overlaps(const WorldRect& other) const {
        return !(other.max.x < min.x || other.min.x > max.x ||
                 other.max.y < min.y || other.min.y > max.y);
    }
    constexpr bool valid() const { return max.x >= min.x && max.y >= min.y; }
    constexpr WorldPos centre() const { return {(min.x + max.x) / 2, (min.y + max.y) / 2}; }
};

// Steps between two tiles when a diagonal counts as one step: the Chebyshev
// distance. This is the step count a path actually takes and therefore an
// admissible A* heuristic.
constexpr std::int32_t tileDistance(TilePos a, TilePos b) {
    const std::int32_t dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const std::int32_t dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    return dx > dy ? dx : dy;
}

// The same thing under its own name. Every call site that says "chebyshev" means
// this, and on a square grid the name is finally the truth.
constexpr std::int32_t chebyshev(TilePos a, TilePos b) { return tileDistance(a, b); }

// The eight neighbours, in a fixed order so iteration is deterministic: east,
// then counter-clockwise. The four cardinals are the even indices, which is what
// lets an area outline be drawn along shared edges only.
inline constexpr std::array<TilePos, 8> kNeighbourOffsets{{
    {+1, 0}, {+1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, +1}, {0, +1}, {+1, +1},
}};
inline constexpr int kNeighbourCount = 8;
inline constexpr std::array<int, 4> kCardinalDirections{0, 2, 4, 6};

// Takes a position it no longer needs, so that call sites written for the tile
// grid's row parity keep reading the same.
constexpr const std::array<TilePos, 8>& neighbourOffsets(TilePos) { return kNeighbourOffsets; }

constexpr TilePos neighbour(TilePos p, int direction) {
    const TilePos d = kNeighbourOffsets[static_cast<std::size_t>(direction) % 8];
    return {p.x + d.x, p.y + d.y};
}

// Whether a step in this direction crosses a corner rather than an edge.
constexpr bool isDiagonal(int direction) { return (direction % 2) != 0; }

// Every tile within `radius` steps, including the centre - a square block, since
// that is what "within n steps" means when a diagonal is a step. Used for
// building footprints, blast radii and "near the hearth" queries.
std::vector<TilePos> tilesWithin(TilePos centre, std::int32_t radius);

// Just the ring at exactly `radius` steps. Growing a settlement outward walks
// these one after another.
std::vector<TilePos> tileRing(TilePos centre, std::int32_t radius);

inline Fixed distance(WorldPos a, WorldPos b) { return hypot(b.x - a.x, b.y - a.y); }

// Centre of a tile in metres.
inline WorldPos tileCentre(TilePos t) { return {Fixed::fromInt(t.x), Fixed::fromInt(t.y)}; }

// The square metre of ground this cache cell covers.
inline WorldRect tileBounds(TilePos t) {
    constexpr Fixed half = Fixed::ratio(1, 2);
    const WorldPos c = tileCentre(t);
    return {{c.x - half, c.y - half}, {c.x + half, c.y + half}};
}

// Which tile a point falls in.
inline TilePos toTile(WorldPos p) {
    return {static_cast<std::int32_t>(p.x.roundToInt()), static_cast<std::int32_t>(p.y.roundToInt())};
}

// Axis-aligned rectangle in tile coordinates. A bounding box for iteration and
// view culling, and nothing more.
struct TileRect {
    TilePos min;
    TilePos max;

    constexpr bool contains(TilePos p) const {
        return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y;
    }
    constexpr std::int32_t width() const { return max.x - min.x; }
    constexpr std::int32_t height() const { return max.y - min.y; }
    constexpr std::int64_t area() const { return std::int64_t(width()) * height(); }
};

// A deterministic 16-gon approximation of a disc, suitable for drawing or area
// queries without introducing floating point into the simulation.
std::vector<WorldPos> discOutline(WorldPos centre, Fixed radius);

// World-space polygon queries. The polygon is assumed simple; zone strokes are
// unions/differences of such simple shapes.
bool pointInPolygon(const std::vector<WorldPos>& polygon, WorldPos p);
bool segmentIntersectsRect(WorldPos a, WorldPos b, const WorldRect& rect);
bool polygonIntersectsRect(const std::vector<WorldPos>& polygon, const WorldRect& rect);

} // namespace core

template <>
struct std::hash<core::TilePos> {
    std::size_t operator()(core::TilePos p) const noexcept {
        std::uint64_t k = (std::uint64_t(std::uint32_t(p.x)) << 32) | std::uint32_t(p.y);
        k ^= k >> 33; k *= 0xff51afd7ed558ccdULL;
        k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ULL;
        k ^= k >> 33;
        return static_cast<std::size_t>(k);
    }
};
