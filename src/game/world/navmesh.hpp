#pragma once
// Where a body can walk, and how it gets there.
//
// Navigation is its own geometry. The terrain mesh says what the ground looks
// like; this says what can be stood on and walked across, and the two are not
// the same shape and are not meant to be: a hundred metres of level field is
// one navigation polygon and eight hundred terrain triangles, and a scree slope
// is the other way round.
//
// The pieces, in the order they are used:
//
//   polygons   convex areas of walkable ground. Built per chunk, addressed
//              globally, and merged as large as the ground allows - so a path
//              across open country is three polygons rather than a thousand
//              tiles.
//   portals    the shared edge between two polygons, kept as a segment rather
//              than a point. Two polygons in different chunks share a portal
//              exactly like two in the same one; that is what makes a path
//              cross a border without noticing it.
//   regions    the connected part of one chunk a polygon belongs to. The coarse
//              graph a long path is planned on before any polygon is looked at.
//   the path   A* over the polygon graph gives a corridor of polygons; the
//              funnel then pulls a string through it. What comes out is a line
//              that hugs the corners it passes, not a staircase through the
//              middles of things - which is the whole reason a navmesh is worth
//              having over a grid.

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"
#include "game/world/terrain_streaming/page_ground.hpp"

namespace world {

// The shared edge between two polygons, seen from the polygon that owns it:
// `left` and `right` are its ends in that order, which is what the funnel needs
// to know which way it is being squeezed.
struct Portal {
    std::uint32_t to = 0;
    core::WorldPos left, right;
};

// A convex piece of walkable ground.
//
// Rectangles, for now: they come out of merging the walkable ground of a chunk
// and they are exactly convex, exactly comparable, and exactly cheap. Nothing
// outside this file knows that - a path is planned over portals and pulled
// through a funnel, neither of which asks what shape a polygon is - so the day
// the ground wants triangles or a general convex hull, this is the only struct
// that changes.
struct NavPoly {
    std::uint32_t id = 0;
    // The polygon itself: convex, three corners or more, wound the same way for
    // every polygon in the mesh.
    //
    // It was a rectangle, and a rectangle is what makes a navmesh a tile grid
    // wearing a different name: an area of any other shape comes out as a
    // staircase of thin boxes, ten shapes where three would do, and every step
    // of that staircase is walkable ground the mesh threw away or water it took
    // in. Nothing outside this file reads the shape - a path is planned over
    // portals and pulled through a funnel, and neither asks what a polygon
    // looks like - which is what makes this change possible at all.
    std::vector<core::WorldPos> points;
    core::WorldPos min, max;      // the box round it, for rejecting fast
    core::Fixed height;           // the ground in the middle of it
    ChunkId chunk;
    std::uint32_t region = 0;     // connected part of its chunk
    // How this ground is got over. Polygons of different kinds are never merged
    // together, so the edge of a scree slope is a polygon boundary and a path
    // can decide to go round it.
    Travel travel = Travel::Walk;
    std::vector<Portal> links;

    // Inside the box first, because that rejects nearly everything for two
    // comparisons; then inside the polygon, which for a convex one is "on the
    // same side of every edge".
    bool contains(core::WorldPos p) const;
    core::WorldPos centre() const;
};

// What a metre of each kind of ground costs to cross, against a metre of level
// walking. A body will go three times as far round to keep off a scramble and
// eight times as far to keep off a climb - and will take the climb rather than
// walk to the far end of a range, which is the whole point of having it (D117).
core::Fixed travelCost(Travel t);

struct Path {
    // The line a body actually walks: world points, corner to corner. The first
    // is where it started and the last is where it was going.
    std::vector<core::WorldPos> points;
    // The polygons it passes through, in order. Kept because a follower wants
    // to know when it has left one and entered the next without testing every
    // polygon in the world.
    std::vector<std::uint32_t> corridor;
    core::Fixed length;
    // What it costs to walk, which is not its length: a short scramble can cost
    // more than the long way round, and the planner chose knowing that.
    core::Fixed cost;
    // The hardest ground on it. What a caller warns about, or refuses with.
    Travel worst = Travel::Walk;
};

// Anything that blocks the ground: a building's footprint, a wall, a pen. Given
// to the builder rather than discovered by it, because what is built is the
// simulation's business and the navmesh's job is only to know it is there.
struct Obstacle {
    core::WorldPos min, max;
};

class NavMesh {
public:
    // Builds one chunk from the field, minus whatever stands on it, and relinks
    // it to its neighbours. Rebuilding a chunk relinks the ring around it, so a
    // wall raised on a border cannot leave a portal pointing at ground that is
    // no longer walkable.
    void build(const streaming::PageGround& ground, ChunkId chunk, const std::vector<Obstacle>& obstacles);
    void drop(ChunkId chunk);
    bool has(ChunkId chunk) const { return byChunk_.count(chunk) != 0; }

    const NavPoly* polyAt(core::WorldPos p) const;
    const NavPoly* poly(std::uint32_t id) const;
    std::size_t polyCount() const { return polys_.size(); }
    const std::vector<NavPoly>& polys() const { return polys_; }

    // The nearest point a body could stand, to somewhere it may not be able to -
    // what a caller uses when the player clicks the water or the sky.
    std::optional<core::WorldPos> nearestWalkable(core::WorldPos p, core::Fixed within) const;

    // A path, or nothing when there is no way. Two levels: for anything longer
    // than a few chunks the regions are searched first and the polygon search
    // is kept inside the corridor that came out, which is what keeps a walk
    // across the world from opening every polygon between here and there.
    // `hardest` is the worst ground this traveller will take, and it is how a
    // cart differs from a person. Both use a ford, and slowly - that is what a
    // ford is - but a cart does not swim, and finding that out after the path
    // has been planned is finding it out too late, so the planner is told
    // rather than the answer being filtered afterwards.
    //
    // Climb is the default because it was the hardest passable ground before
    // water had a depth, so a caller that does not care gets exactly what it
    // got before.
    std::optional<Path> findPath(core::WorldPos from, core::WorldPos to,
                                 Travel hardest = Travel::Climb) const;

    // The funnel on its own, over a corridor somebody else chose. Exposed
    // because it is worth testing against the corridor it was pulled through.
    static std::vector<core::WorldPos> pullString(const std::vector<Portal>& gates,
                                                  core::WorldPos from, core::WorldPos to);

    // Whether a body could walk straight from one point to the other - every
    // step of the way over ground some polygon covers. What straightening a
    // path is built on, and what a caller asks before ordering somebody to walk
    // somewhere in a straight line.
    // Whether a body could walk straight from one point to the other, and what
    // that would cost. Nothing at all when the line leaves the navmesh.
    bool clearBetween(core::WorldPos from, core::WorldPos to) const;
    std::optional<core::Fixed> costBetween(core::WorldPos from, core::WorldPos to) const;

private:
    struct Region {
        std::uint32_t id = 0;
        ChunkId chunk;
        std::vector<std::uint32_t> neighbours;
    };

    void relink(ChunkId chunk);
    void linkPair(ChunkId a, ChunkId b);
    void rebuildRegions();
    std::vector<std::uint32_t> regionCorridor(std::uint32_t from, std::uint32_t to) const;

    std::vector<NavPoly> polys_;
    std::unordered_map<ChunkId, std::vector<std::uint32_t>> byChunk_;
    std::vector<Region> regions_;
    // Ids the last drop left behind, reused before the vector grows.
    std::vector<std::uint32_t> spare_;
};

} // namespace world
