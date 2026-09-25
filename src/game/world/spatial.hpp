#pragma once
// Finding things in the world.
//
// Three structures, because there are three questions and they have different
// shapes:
//
//   StaticGeometry  the ground, the cliffs and everything built - triangles
//                   that hardly ever move. A bounding-volume hierarchy over
//                   them: point, box and ray queries, exact.
//   EntityIndex     people, animals, carts - a few thousand things that move
//                   every tick. A spatial hash, because rebuilding a tree for
//                   them every tick is the classic way to lose the frame.
//   LookupGrid      a coarse regular grid of remembered answers. A cache and a
//                   hint, never the truth: anything that needs to be right
//                   confirms against the geometry.
//
// A tree for moving things and a hash for still ones are both wrong, and both
// are easy to write by accident, so they are separate types with separate names
// rather than one "spatial index" with a flag.
//
// None of these is a boundary. A query names a region of the world, and the
// index walks whatever chunks that region touches: a person standing a metre
// inside one chunk sees the barn a metre inside the next.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"

namespace world {

// An axis-aligned box in world metres, with a height range. Flat queries leave
// the height range open, which is what most of the game asks for.
struct Bounds {
    core::WorldPos min{core::Fixed::fromInt(1) * core::Fixed::fromInt(1 << 20),
                       core::Fixed::fromInt(1) * core::Fixed::fromInt(1 << 20)};
    core::WorldPos max{core::Fixed::fromInt(-(1 << 20)), core::Fixed::fromInt(-(1 << 20))};
    core::Fixed low = core::Fixed::fromInt(1 << 20);
    core::Fixed high = core::Fixed::fromInt(-(1 << 20));

    static Bounds around(core::WorldPos centre, core::Fixed radius);
    void add(core::WorldPos p, core::Fixed z);
    void add(const Bounds& other);
    bool overlaps(const Bounds& other) const;
    bool containsFlat(core::WorldPos p) const;
    bool valid() const { return max.x.raw >= min.x.raw && max.y.raw >= min.y.raw; }
    core::Fixed spanX() const { return max.x - min.x; }
    core::Fixed spanY() const { return max.y - min.y; }
};

// A triangle of the world, in world coordinates. `id` is whatever the owner
// wants to find again - a terrain triangle index, a navmesh polygon, a wall -
// and `kind` says which of those it is, so one structure holds all the static
// geometry rather than one per sort of thing.
enum class Surface : std::uint8_t { Ground, Cliff, Built, Water };

struct Triangle {
    core::WorldPos a, b, c;
    core::Fixed za, zb, zc;
    std::uint32_t id = 0;
    Surface surface = Surface::Ground;

    Bounds bounds() const;
    // Whether a point is inside the triangle seen from above, and if so how
    // high the triangle is there. Flat containment plus the plane's height is
    // what "the ground under this point" means on a surface world.
    bool coversFlat(core::WorldPos p) const;
    core::Fixed heightAt(core::WorldPos p) const;
};

struct RayHit {
    std::uint32_t id = 0;
    Surface surface = Surface::Ground;
    core::WorldPos where;
    core::Fixed height;
    core::Fixed distance;      // along the ray, in metres
};

// The tree over one chunk's worth of triangles. Bounding volumes rather than a
// KD-tree: a triangle is a volume, not a point, and a KD-tree either splits it
// across cells or grows the cells until they overlap anyway - a BVH says that
// out loud and keeps each triangle in exactly one leaf.
class TriangleTree {
public:
    void build(std::vector<Triangle> triangles);
    bool empty() const { return triangles_.empty(); }
    const std::vector<Triangle>& triangles() const { return triangles_; }
    const Bounds& bounds() const { return bounds_; }

    // Every triangle covering this point, seen from above. More than one when
    // ground runs under a bridge or a cliff face hangs over its own foot.
    void coveringFlat(core::WorldPos p, std::vector<const Triangle*>& out) const;
    void overlapping(const Bounds& box, std::vector<const Triangle*>& out) const;
    // The first triangle a ray meets. `from`/`to` are world points with their
    // own heights: picking with the mouse and line of sight are the same query.
    std::optional<RayHit> raycast(core::WorldPos from, core::Fixed fromZ, core::WorldPos to,
                                  core::Fixed toZ) const;

private:
    struct Node {
        Bounds box;
        std::uint32_t first = 0, count = 0;   // leaf: a run of triangles
        std::uint32_t left = 0, right = 0;    // branch: children, or zero
        bool leaf() const { return count > 0; }
    };
    std::uint32_t buildNode(std::uint32_t first, std::uint32_t count, int depth);

    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    Bounds bounds_;
};

// Every chunk's tree, and the routing that makes a query not stop at a border.
class StaticGeometry {
public:
    void set(ChunkId chunk, std::vector<Triangle> triangles);
    void clear(ChunkId chunk);
    bool has(ChunkId chunk) const { return trees_.count(chunk) != 0; }
    std::size_t chunkCount() const { return trees_.size(); }

    std::vector<const Triangle*> coveringFlat(core::WorldPos p) const;
    std::vector<const Triangle*> overlapping(const Bounds& box) const;
    std::optional<RayHit> raycast(core::WorldPos from, core::Fixed fromZ, core::WorldPos to,
                                  core::Fixed toZ) const;
    // The ground under a point, from the geometry itself rather than from the
    // field: what a caller uses when a hint is not good enough.
    std::optional<core::Fixed> groundHeight(core::WorldPos p) const;

private:
    std::unordered_map<ChunkId, TriangleTree> trees_;
};

// Things that move. A uniform grid of buckets keyed by cell, which is the one
// structure that survives everything in it moving every tick: an update is two
// hash lookups, and nothing is rebuilt.
class EntityIndex {
public:
    // The side of a bucket, in metres. Around the distance most queries ask
    // about, so a query touches four buckets rather than four hundred.
    static constexpr std::int32_t kBucketMetres = 8;

    void insert(std::uint32_t id, core::WorldPos at);
    void move(std::uint32_t id, core::WorldPos to);
    void remove(std::uint32_t id);
    void clear();
    std::size_t size() const { return where_.size(); }
    bool has(std::uint32_t id) const { return where_.count(id) != 0; }

    // Everything within a radius, and everything in a box. Both walk whatever
    // buckets the region touches, so neither has an edge.
    std::vector<std::uint32_t> near(core::WorldPos centre, core::Fixed radius) const;
    std::vector<std::uint32_t> inBox(const Bounds& box) const;
    std::optional<core::WorldPos> positionOf(std::uint32_t id) const;

private:
    struct Cell {
        std::int32_t x = 0, y = 0;
        friend bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }
    };
    struct CellHash {
        std::size_t operator()(Cell c) const noexcept {
            return std::hash<std::uint64_t>{}((std::uint64_t(std::uint32_t(c.x)) << 32) |
                                              std::uint32_t(c.y));
        }
    };
    static Cell cellOf(core::WorldPos p);

    std::unordered_map<Cell, std::vector<std::uint32_t>, CellHash> buckets_;
    std::unordered_map<std::uint32_t, core::WorldPos> where_;
};

// Answers worth keeping, on a coarse regular grid.
//
// This is the only grid left in the world, and it is a cache. It never decides
// anything: it remembers what the geometry said, it says "I do not know" the
// moment anything near it changes, and a caller that needs to be right asks the
// geometry. Written down here because a cache that is allowed to be believed
// stops being a cache and becomes the tile map again.
class LookupGrid {
public:
    static constexpr std::int32_t kCellMetres = 8;

    struct Answer {
        core::Fixed height;
        std::uint32_t region = 0;     // navmesh region this cell mostly belongs to
        bool walkable = false;
    };

    std::optional<Answer> hint(core::WorldPos p) const;
    void remember(core::WorldPos p, Answer answer);
    // Everything this region touched is unknown again. What dirty propagation
    // calls when a wall goes up, a chunk is rebuilt, or a cliff is cut.
    void forget(const Bounds& box);
    void forgetAll();
    std::size_t remembered() const { return answers_.size(); }

private:
    struct Cell {
        std::int32_t x = 0, y = 0;
        friend bool operator==(Cell a, Cell b) { return a.x == b.x && a.y == b.y; }
    };
    struct CellHash {
        std::size_t operator()(Cell c) const noexcept {
            return std::hash<std::uint64_t>{}((std::uint64_t(std::uint32_t(c.x)) << 32) |
                                              std::uint32_t(c.y));
        }
    };
    static Cell cellOf(core::WorldPos p);

    std::unordered_map<Cell, Answer, CellHash> answers_;
};

// The triangles of a chunk's terrain mesh, ready for the tree. Kept here rather
// than in the mesh so that terrain, cliffs and buildings all arrive at the
// index as the same kind of thing.
struct TerrainMesh;
std::vector<Triangle> trianglesOf(const TerrainMesh& mesh, Surface surface = Surface::Ground);

} // namespace world
