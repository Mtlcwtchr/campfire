// Finding things in the world: the tree over static geometry, the index over
// moving bodies, and the grid of remembered answers.
//
// An acceleration structure has one job - to give the same answer as looking at
// everything, only faster - so it is tested against looking at everything.
// Anything else measures the structure against itself.

#include "framework.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

#include "engine/core/rng.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/spatial.hpp"
#include "game/world/terrain_mesh.hpp"

namespace {

using core::Fixed;
using core::WorldPos;

const generation::WorldMapData& country() {
    static generation::WorldMapData world = [] {
        generation::WorldMapParams params;
        params.seed = 4242;
        params.width = params.height = 96;
        // The dials pinned, not taken from the defaults.
        //
        // These tests are about geometry and about getting from one place to
        // another, not about how much of a world is sea - and when the default
        // moved to the share this world has (D141), the flattest landlocked
        // cell on this seed came out a patch hemmed in by water and four
        // pathfinding tests stopped finding a way at all. A test that wants a
        // continent under it has to ask for one.
        params.seaPercent = 62;
        params.erosionPasses = 2;
        return generation::generateWorldMap(params);
    }();
    return world;
}

// Somewhere with ground on it, found rather than named: where the land is
// depends on the world's scale, and a test that names a chunk fails as "the
// tree is broken" the day the map grows.
world::ChunkId someGround(const world::HeightField& field) {
    (void)field;
    const generation::WorldMapData& map = country();
    core::TilePos best{map.width / 2, map.height / 2};
    std::int32_t bestScore = -1;
    for (std::int32_t y = 2; y < map.height - 2; ++y)
        for (std::int32_t x = 2; x < map.width - 2; ++x) {
            std::int32_t dry = 0;
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                for (std::int32_t dx = -1; dx <= 1; ++dx)
                    if (!map.at({x + dx, y + dy}).sea) ++dry;
            if (dry > bestScore) { bestScore = dry; best = {x, y}; }
        }
    const core::Fixed metres = core::Fixed::fromInt(generation::kMetresPerCell);
    return world::chunkOf({core::Fixed::fromInt(best.x) * metres + metres / core::Fixed::fromInt(2),
                           core::Fixed::fromInt(best.y) * metres + metres / core::Fixed::fromInt(2)});
}

// The same block of world every test works over: four chunks, so that anything
// asked near their shared corner has to cross a border to be answered.
world::StaticGeometry loadedBlock(const world::HeightField& field, world::ChunkId at) {
    world::StaticGeometry geometry;
    for (world::ChunkId c : {at, world::ChunkId{at.x + 1, at.y}, world::ChunkId{at.x, at.y + 1},
                             world::ChunkId{at.x + 1, at.y + 1}})
        geometry.set(c, world::trianglesOf(world::buildChunkMesh(field, c)));
    return geometry;
}

WorldPos at(double x, double y) {
    return {Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)};
}

} // namespace

TEST(the_tree_answers_what_looking_at_everything_answers) {
    const world::HeightField field(&country(), 5);
    const world::TerrainMesh mesh = world::buildChunkMesh(field, someGround(field));
    const std::vector<world::Triangle> all = world::trianglesOf(mesh);
    world::TriangleTree tree;
    tree.build(all);

    core::Rng rng(9001, 1);
    const WorldPos corner = world::chunkOrigin(someGround(field));
    int checked = 0;
    for (int i = 0; i < 300; ++i) {
        const WorldPos p{corner.x + Fixed::ratio(rng.below(world::kChunkMetres * 64), 64),
                         corner.y + Fixed::ratio(rng.below(world::kChunkMetres * 64), 64)};

        std::vector<std::uint32_t> byTree;
        std::vector<const world::Triangle*> found;
        tree.coveringFlat(p, found);
        for (const world::Triangle* t : found) byTree.push_back(t->id);

        std::vector<std::uint32_t> byHand;
        for (const world::Triangle& t : all)
            if (t.coversFlat(p)) byHand.push_back(t.id);

        std::sort(byTree.begin(), byTree.end());
        std::sort(byHand.begin(), byHand.end());
        CHECK(byTree == byHand);
        // Somewhere inside the chunk, every point is on the ground.
        CHECK(!byHand.empty());
        ++checked;
    }
    CHECK(checked == 300);
}

TEST(a_box_query_returns_the_same_triangles_by_tree_and_by_hand) {
    const world::HeightField field(&country(), 5);
    const std::vector<world::Triangle> all =
            world::trianglesOf(world::buildChunkMesh(field, someGround(field)));
    world::TriangleTree tree;
    tree.build(all);

    core::Rng rng(4004, 1);
    const WorldPos corner = world::chunkOrigin(someGround(field));
    for (int i = 0; i < 80; ++i) {
        world::Bounds box;
        const WorldPos p{corner.x + Fixed::fromInt(rng.below(world::kChunkMetres)),
                         corner.y + Fixed::fromInt(rng.below(world::kChunkMetres))};
        const Fixed size = Fixed::fromInt(1 + rng.below(12));
        box.add(p, Fixed::fromInt(-1000));
        box.add({p.x + size, p.y + size}, Fixed::fromInt(1000));

        std::vector<std::uint32_t> byTree, byHand;
        std::vector<const world::Triangle*> found;
        tree.overlapping(box, found);
        for (const world::Triangle* t : found) byTree.push_back(t->id);
        for (const world::Triangle& t : all)
            if (t.bounds().overlaps(box)) byHand.push_back(t.id);
        std::sort(byTree.begin(), byTree.end());
        std::sort(byHand.begin(), byHand.end());
        CHECK(byTree == byHand);
    }
}

TEST(the_ground_under_a_point_is_the_ground_the_field_describes) {
    // The tree is over the mesh, and the mesh is cut from the field, so asking
    // the geometry and asking the field have to give the same answer at a
    // lattice point and a close one between them. This is the "confirm against
    // the real geometry" path that the lookup grid is explicitly not.
    const world::HeightField field(&country(), 5);
    const world::ChunkId block = someGround(field);
    const world::StaticGeometry geometry = loadedBlock(field, block);

    const WorldPos corner = world::chunkOrigin({block.x + 1, block.y + 1});
    CHECK(geometry.groundHeight(corner).has_value());
    if (!geometry.groundHeight(corner)) return;
    CHECK(geometry.groundHeight(corner)->raw == field.heightAt(corner).raw);

    core::Rng rng(77, 1);
    Fixed worst = core::kZero;
    for (int i = 0; i < 200; ++i) {
        const WorldPos p{corner.x + Fixed::ratio(rng.below(4000), 100),
                         corner.y + Fixed::ratio(rng.below(4000), 100)};
        const std::optional<Fixed> ground = geometry.groundHeight(p);
        CHECK(ground.has_value());
        if (!ground) continue;
        // A triangulated surface and a bilinear field differ inside a cell -
        // the triangle is a plane through three of its corners, the field
        // curves through four - but only by a little, and never by a step.
        const Fixed gap = Fixed::fromRaw(std::abs(ground->raw - field.heightAt(p).raw));
        if (gap.raw > worst.raw) worst = gap;
    }
    CHECK(worst.raw < Fixed::fromInt(2).raw);
}

TEST(a_query_does_not_stop_at_a_chunk_border) {
    // The thing chunked worlds get wrong: a box drawn across a border, or a
    // point on it, coming back with only what one chunk knew about.
    const world::HeightField field(&country(), 5);
    const world::ChunkId block = someGround(field);
    const world::StaticGeometry geometry = loadedBlock(field, block);

    // The corner where all four chunks meet.
    const WorldPos meeting = world::chunkOrigin({block.x + 1, block.y + 1});
    world::Bounds box;
    box.add({meeting.x - Fixed::fromInt(6), meeting.y - Fixed::fromInt(6)}, Fixed::fromInt(-999));
    box.add({meeting.x + Fixed::fromInt(6), meeting.y + Fixed::fromInt(6)}, Fixed::fromInt(999));

    const std::vector<const world::Triangle*> found = geometry.overlapping(box);
    CHECK(!found.empty());

    // Triangles from all four chunks are in the answer. Counted by which side
    // of the meeting point their first corner falls on.
    bool northWest = false, northEast = false, southWest = false, southEast = false;
    for (const world::Triangle* t : found) {
        const bool west = t->a.x.raw < meeting.x.raw;
        const bool north = t->a.y.raw < meeting.y.raw;
        if (west && north) northWest = true;
        if (!west && north) northEast = true;
        if (west && !north) southWest = true;
        if (!west && !north) southEast = true;
    }
    CHECK(northWest && northEast && southWest && southEast);

    // And the ground is continuous across the border: stepping over it a
    // centimetre at a time never falls off the world or jumps.
    std::optional<Fixed> previous;
    for (int i = -200; i <= 200; ++i) {
        const WorldPos p{meeting.x + Fixed::ratio(i, 100), meeting.y + Fixed::ratio(3, 10)};
        const std::optional<Fixed> here = geometry.groundHeight(p);
        CHECK(here.has_value());
        if (here && previous) CHECK(std::abs(here->raw - previous->raw) < Fixed::fromInt(1).raw);
        previous = here;
    }
}

TEST(a_ray_finds_the_nearest_thing_it_crosses) {
    const world::HeightField field(&country(), 5);
    const world::ChunkId block = someGround(field);
    const world::StaticGeometry geometry = loadedBlock(field, block);
    const WorldPos corner = world::chunkOrigin(someGround(field));

    // Straight down from well above the ground: the classic pick. It has to hit,
    // and it has to hit at the height the ground is.
    core::Rng rng(1234, 1);
    int hits = 0;
    for (int i = 0; i < 60; ++i) {
        const WorldPos p{corner.x + Fixed::fromInt(4 + rng.below(100)),
                         corner.y + Fixed::fromInt(4 + rng.below(100))};
        const std::optional<Fixed> ground = geometry.groundHeight(p);
        if (!ground) continue;
        const std::optional<world::RayHit> hit =
                geometry.raycast(p, *ground + Fixed::fromInt(300), p, *ground - Fixed::fromInt(300));
        CHECK(hit.has_value());
        if (!hit) continue;
        ++hits;
        CHECK(std::abs(hit->height.raw - ground->raw) < Fixed::ratio(1, 10).raw);
    }
    CHECK(hits > 50);

    // A slanted ray hits the first triangle along it, not merely some triangle:
    // compared against every triangle the block holds.
    const WorldPos from{corner.x + Fixed::fromInt(10), corner.y + Fixed::fromInt(10)};
    const WorldPos to{corner.x + Fixed::fromInt(70), corner.y + Fixed::fromInt(52)};
    const Fixed ground = geometry.groundHeight(from).value_or(core::kZero);
    const std::optional<world::RayHit> hit =
            geometry.raycast(from, ground + Fixed::fromInt(40), to, ground - Fixed::fromInt(40));
    CHECK(hit.has_value());
    if (hit) {
        // Whatever it hit, the point it names is on the ground there.
        const std::optional<Fixed> under = geometry.groundHeight(hit->where);
        CHECK(under.has_value());
        if (under) CHECK(std::abs(under->raw - hit->height.raw) < Fixed::fromInt(1).raw);
    }
}

TEST(moving_bodies_are_found_where_they_now_are) {
    world::EntityIndex index;
    core::Rng rng(2468, 1);
    std::vector<WorldPos> truth;
    const int count = 400;
    for (int i = 0; i < count; ++i) {
        const WorldPos p{Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100),
                         Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100)};
        truth.push_back(p);
        index.insert(static_cast<std::uint32_t>(i), p);
    }
    CHECK(index.size() == static_cast<std::size_t>(count));

    const auto agrees = [&](WorldPos centre, Fixed radius) {
        std::vector<std::uint32_t> byHand;
        for (int i = 0; i < count; ++i) {
            const Fixed dx = truth[i].x - centre.x, dy = truth[i].y - centre.y;
            if ((dx * dx + dy * dy).raw <= (radius * radius).raw)
                byHand.push_back(static_cast<std::uint32_t>(i));
        }
        std::sort(byHand.begin(), byHand.end());
        return index.near(centre, radius) == byHand;
    };

    for (int i = 0; i < 50; ++i) {
        const WorldPos centre{Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100),
                              Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100)};
        CHECK(agrees(centre, Fixed::fromInt(1 + rng.below(30))));
    }

    // Now move everything, many times, including across bucket boundaries and
    // across the world origin - where a truncating division would file a body
    // in the wrong bucket and it would vanish from every query.
    for (int round = 0; round < 20; ++round) {
        for (int i = 0; i < count; ++i) {
            const WorldPos to{truth[i].x + Fixed::ratio(static_cast<std::int64_t>(rng.below(1600)) - 800, 100),
                              truth[i].y + Fixed::ratio(static_cast<std::int64_t>(rng.below(1600)) - 800, 100)};
            truth[i] = to;
            index.move(static_cast<std::uint32_t>(i), to);
        }
        CHECK(index.size() == static_cast<std::size_t>(count));
    }
    for (int i = 0; i < 50; ++i) {
        const WorldPos centre{Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100),
                              Fixed::ratio(static_cast<std::int64_t>(rng.below(40000)) - 20000, 100)};
        CHECK(agrees(centre, Fixed::fromInt(1 + rng.below(30))));
    }

    // Everything is still findable by name, and removal removes exactly one.
    for (int i = 0; i < count; ++i)
        CHECK(index.positionOf(static_cast<std::uint32_t>(i)).has_value());
    index.remove(7);
    CHECK(!index.has(7));
    CHECK(index.size() == static_cast<std::size_t>(count - 1));
    const std::vector<std::uint32_t> after = index.near(truth[7], Fixed::ratio(1, 100));
    CHECK(std::find(after.begin(), after.end(), 7u) == after.end());
}

TEST(the_lookup_grid_admits_when_it_does_not_know) {
    world::LookupGrid grid;
    const WorldPos p = at(103.5, -47.25);
    CHECK(!grid.hint(p).has_value());

    grid.remember(p, {Fixed::fromInt(12), 3, true});
    const std::optional<world::LookupGrid::Answer> hint = grid.hint(p);
    CHECK(hint.has_value());
    if (hint) {
        CHECK(hint->height.raw == Fixed::fromInt(12).raw);
        CHECK(hint->region == 3);
        CHECK(hint->walkable);
    }

    // Anywhere else in the same eight-metre cell gets the same answer - that is
    // what makes it a cache and not the truth, and why a caller that needs to
    // be right asks the geometry instead.
    CHECK(grid.hint(at(102.0, -46.0)).has_value());
    // And a point in the next cell along is a different question entirely.
    CHECK(!grid.hint(at(112.0, -46.0)).has_value());

    // And a change to the world unremembers it, along with the ring around it,
    // because an answer is about the ground near a point rather than at it.
    world::Bounds changed;
    changed.add(at(150.0, -47.0), core::kZero);
    changed.add(at(152.0, -45.0), core::kZero);
    grid.forget(changed);
    CHECK(grid.hint(p).has_value());          // far enough away to survive

    world::Bounds here;
    here.add(at(103.0, -48.0), core::kZero);
    here.add(at(104.0, -47.0), core::kZero);
    grid.forget(here);
    CHECK(!grid.hint(p).has_value());
    CHECK(grid.remembered() == 0);
}

TEST(the_tree_is_the_same_tree_however_the_triangles_arrived) {
    // A structure whose shape depends on input order cannot be compared between
    // two runs, and this one is built from a mesh that may be regenerated in a
    // different order after a load.
    const world::HeightField field(&country(), 5);
    std::vector<world::Triangle> all =
            world::trianglesOf(world::buildChunkMesh(field, someGround(field)));

    world::TriangleTree straight;
    straight.build(all);

    std::vector<world::Triangle> shuffled = all;
    core::Rng rng(31415, 1);
    for (std::size_t i = shuffled.size(); i > 1; --i)
        std::swap(shuffled[i - 1], shuffled[rng.below(static_cast<std::uint32_t>(i))]);
    world::TriangleTree jumbled;
    jumbled.build(shuffled);

    CHECK(straight.triangles().size() == jumbled.triangles().size());
    for (std::size_t i = 0; i < straight.triangles().size(); ++i)
        CHECK(straight.triangles()[i].id == jumbled.triangles()[i].id);
}
