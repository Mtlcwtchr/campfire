// Walking the world: polygons of walkable ground, portals between them, and
// the path that comes out.
//
// The checks that matter are the ones a grid would fail: that a polygon covers
// ground rather than a cell, that a path crosses a chunk border without
// noticing, and that the line a body walks bends round corners instead of
// visiting the middle of everything it passes.

#include "framework.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <vector>

#include "engine/core/rng.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/navmesh.hpp"
#include "game/world/terrain_mesh.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/page_ground.hpp"

#include <map>
#include <memory>

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

// The baked pages this country's navigation is read from.
//
// A navmesh no longer asks a field to work a corner out; it reads the page
// the terrain itself is stored in, so the two cannot disagree. A chunk is
// sixty-four metres and a page five hundred and twelve, so one page answers
// for a chunk - and for the sixty-four around it, which is why they are kept.
struct Pages {
    const generation::WorldMapData& world = country();
    world::streaming::HydrologyGraph graph = world::streaming::buildHydrologyGraph(world);
    world::streaming::BaseTileBaker baker{world, graph,
                                          world::streaming::hsimQuantisationFor(world)};
    mutable std::map<std::pair<std::int32_t, std::int32_t>,
                     std::unique_ptr<world::streaming::PageGround>>
            baked;

    const world::streaming::PageGround& of(world::ChunkId chunk) const {
        const auto key = world::streaming::tileAt(world::chunkOrigin(chunk));
        auto& slot = baked[{key.x, key.y}];
        if (!slot)
            slot = std::make_unique<world::streaming::PageGround>(baker.bakePage(key));
        return *slot;
    }
};

const Pages& pages() {
    static const Pages only;
    return only;
}

// A patch of country with its navigation built: three chunks by three, so that
// a path across it has borders to cross and a middle chunk to be surrounded.
world::NavMesh built(world::ChunkId centre,
                     const std::vector<world::Obstacle>& obstacles = {}) {
    world::NavMesh nav;
    for (std::int32_t y = centre.y - 1; y <= centre.y + 1; ++y)
        for (std::int32_t x = centre.x - 1; x <= centre.x + 1; ++x)
            nav.build(pages().of({x, y}), {x, y}, obstacles);
    return nav;
}

WorldPos middleOf(const world::NavPoly& p) { return p.centre(); }


// A chunk with ground worth testing on, found rather than assumed.
//
// Read off the coarse map, which is what knows where the country keeps its
// mountains and its flat land: hunting for them by sampling the field would be
// a million queries over a world fifty kilometres across, and naming a chunk
// outright is a test that fails the day the map changes scale - and fails as
// "the navmesh is broken" rather than as "you are standing in the sea".
world::ChunkId groundWhere(const world::HeightField& field, bool wantBroken) {
    (void)field;
    const generation::WorldMapData& map = country();
    core::TilePos best{map.width / 2, map.height / 2};
    std::int32_t bestScore = -1;
    for (std::int32_t y = 2; y < map.height - 2; ++y) {
        for (std::int32_t x = 2; x < map.width - 2; ++x) {
            std::int32_t low = 999, high = -999;
            bool anySea = false;
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const generation::WorldCell& c = map.at({x + dx, y + dy});
                    if (c.sea) anySea = true;
                    low = std::min<std::int32_t>(low, c.elevation);
                    high = std::max<std::int32_t>(high, c.elevation);
                }
            if (anySea) continue;
            const std::int32_t relief = high - low;
            const std::int32_t score = wantBroken ? relief : 200 - relief * 20;
            if (score > bestScore) {
                bestScore = score;
                best = {x, y};
            }
        }
    }
    const core::Fixed metres = core::Fixed::fromInt(generation::kMetresPerCell);
    return world::chunkOf({core::Fixed::fromInt(best.x) * metres + metres / core::Fixed::fromInt(2),
                           core::Fixed::fromInt(best.y) * metres + metres / core::Fixed::fromInt(2)});
}

} // namespace

TEST(walkable_ground_is_covered_by_polygons_and_nothing_else_is) {
    const world::HeightField field(&country(), 5);
    const world::ChunkId chunk = groundWhere(field, false);
    world::NavMesh nav;
    nav.build(pages().of(chunk), chunk, {});
    CHECK(nav.polyCount() > 0);

    // Every polygon stands on ground a body can get over, and says correctly
    // which way it has to be got over. No two polygons claim the same ground.
    core::Rng rng(555, 1);
    for (const world::NavPoly& poly : nav.polys()) {
        if (poly.max.x.raw <= poly.min.x.raw) continue;
        CHECK(poly.travel != world::Travel::None);
        for (int i = 0; i < 6; ++i) {
            const Fixed x = poly.min.x + (poly.max.x - poly.min.x) * Fixed::ratio(rng.below(90) + 5, 100);
            const Fixed y = poly.min.y + (poly.max.y - poly.min.y) * Fixed::ratio(rng.below(90) + 5, 100);
            CHECK(field.travelAt({x, y}) != world::Travel::None);
        }
    }
    for (const world::NavPoly& a : nav.polys()) {
        if (a.max.x.raw <= a.min.x.raw) continue;
        for (const world::NavPoly& b : nav.polys()) {
            if (b.id == a.id || b.max.x.raw <= b.min.x.raw) continue;
            const bool apart = b.min.x.raw >= a.max.x.raw || b.max.x.raw <= a.min.x.raw ||
                               b.min.y.raw >= a.max.y.raw || b.max.y.raw <= a.min.y.raw;
            CHECK(apart);
        }
    }

    // And the polygons are not all one size. A navmesh that came out uniform
    // would be the tile grid again under another name: open ground has to merge
    // into something bigger than the lattice it was built from.
    Fixed widest = core::kZero;
    for (const world::NavPoly& p : nav.polys())
        if ((p.max.x - p.min.x).raw > widest.raw) widest = p.max.x - p.min.x;
    CHECK(widest.raw > Fixed::fromInt(world::kSampleMetres * 2).raw);
}

TEST(a_path_crosses_a_chunk_border_without_noticing) {
    const world::HeightField field(&country(), 5);
    const world::ChunkId centre = groundWhere(field, false);
    const world::NavMesh nav = built(centre);

    // Two points in different chunks, both on walkable ground.
    const WorldPos west = nav.nearestWalkable(
                                 {world::chunkOrigin({centre.x - 1, centre.y}).x + Fixed::fromInt(20),
                                  world::chunkOrigin({centre.x - 1, centre.y}).y + Fixed::fromInt(32)},
                                 Fixed::fromInt(60))
                                  .value_or(WorldPos{});
    const WorldPos east = nav.nearestWalkable(
                                 {world::chunkOrigin({centre.x + 1, centre.y}).x + Fixed::fromInt(44),
                                  world::chunkOrigin({centre.x + 1, centre.y}).y + Fixed::fromInt(32)},
                                 Fixed::fromInt(60))
                                  .value_or(WorldPos{});
    CHECK(nav.polyAt(west) != nullptr);
    CHECK(nav.polyAt(east) != nullptr);
    if (!nav.polyAt(west) || !nav.polyAt(east)) return;

    const std::optional<world::Path> path = nav.findPath(west, east);
    CHECK(path.has_value());
    if (!path) return;

    // It went through polygons of more than one chunk - the point of the whole
    // exercise - and every step of the line it drew stands on walkable ground.
    std::vector<world::ChunkId> chunks;
    for (std::uint32_t id : path->corridor) {
        const world::NavPoly* p = nav.poly(id);
        CHECK(p != nullptr);
        if (p && std::find(chunks.begin(), chunks.end(), p->chunk) == chunks.end())
            chunks.push_back(p->chunk);
    }
    CHECK(chunks.size() >= 3);

    CHECK(path->points.size() >= 2);
    CHECK(path->points.front().x.raw == west.x.raw);
    CHECK(path->points.back().x.raw == east.x.raw);
    for (std::size_t i = 0; i + 1 < path->points.size(); ++i) {
        const WorldPos a = path->points[i], b = path->points[i + 1];
        const Fixed span = core::hypot(b.x - a.x, b.y - a.y);
        const int steps = static_cast<int>(span.toInt()) + 2;
        for (int s = 0; s <= steps; ++s) {
            const Fixed t = Fixed::ratio(s, steps);
            const WorldPos on{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
            CHECK(nav.polyAt(on) != nullptr);
        }
    }
}

TEST(the_path_is_a_line_and_not_a_tour_of_polygon_centres) {
    const world::HeightField field(&country(), 5);
    const world::ChunkId centre = groundWhere(field, false);
    const world::NavMesh nav = built(centre);

    const WorldPos from = nav.nearestWalkable(
                                 {world::chunkOrigin({centre.x - 1, centre.y - 1}).x + Fixed::fromInt(30),
                                  world::chunkOrigin({centre.x - 1, centre.y - 1}).y + Fixed::fromInt(30)},
                                 Fixed::fromInt(70))
                                  .value_or(WorldPos{});
    const WorldPos to = nav.nearestWalkable(
                               {world::chunkOrigin({centre.x + 1, centre.y + 1}).x + Fixed::fromInt(34),
                                world::chunkOrigin({centre.x + 1, centre.y + 1}).y + Fixed::fromInt(34)},
                               Fixed::fromInt(70))
                                .value_or(WorldPos{});
    const std::optional<world::Path> path = nav.findPath(from, to);
    CHECK(path.has_value());
    if (!path) return;

    // The same corridor walked through the middles of its polygons, which is
    // what a grid path or an unpulled corridor gives you.
    Fixed throughCentres = core::kZero;
    WorldPos at = from;
    for (std::uint32_t id : path->corridor) {
        const world::NavPoly* p = nav.poly(id);
        if (!p) continue;
        throughCentres += core::hypot(middleOf(*p).x - at.x, middleOf(*p).y - at.y);
        at = middleOf(*p);
    }
    throughCentres += core::hypot(to.x - at.x, to.y - at.y);

    const Fixed straight = core::hypot(to.x - from.x, to.y - from.y);
    // Between the two: never shorter than the straight line, and shorter than
    // the tour of the middles. On open ground it is very nearly the straight
    // line, which is the thing that cannot be said of any grid path.
    CHECK(path->length.raw >= straight.raw - Fixed::ratio(1, 100).raw);
    CHECK(path->length.raw <= throughCentres.raw);

    // And the string really was pulled: a corridor of many polygons comes out
    // as a handful of corners, not a point per polygon.
    if (path->corridor.size() > 6) CHECK(path->points.size() < path->corridor.size());
}

TEST(a_wall_closes_the_way_and_taking_it_down_opens_it) {
    // The navmesh has to answer to what has been built, and it has to answer
    // again when that changes - including when the change lands on a chunk
    // border, where a stale portal would leave a path walking through a wall.
    const world::HeightField field(&country(), 5);
    const world::ChunkId chunk = groundWhere(field, false);
    const WorldPos corner = world::chunkOrigin(chunk);

    world::NavMesh open;
    open.build(pages().of(chunk), chunk, {});
    const WorldPos north{corner.x + Fixed::fromInt(32), corner.y + Fixed::fromInt(8)};
    const WorldPos south{corner.x + Fixed::fromInt(32), corner.y + Fixed::fromInt(56)};
    if (!open.polyAt(north) || !open.polyAt(south)) {
        std::cerr << "    this chunk is not open enough to wall in half of it\n";
        CHECK(open.polyAt(north) != nullptr);
        return;
    }
    CHECK(open.findPath(north, south).has_value());

    // A wall right across the chunk.
    std::vector<world::Obstacle> wall;
    wall.push_back({{corner.x, corner.y + Fixed::fromInt(28)},
                    {corner.x + Fixed::fromInt(world::kChunkMetres), corner.y + Fixed::fromInt(36)}});
    world::NavMesh walled;
    walled.build(pages().of(chunk), chunk, wall);
    CHECK(walled.polyAt(north) != nullptr);
    CHECK(walled.polyAt(south) != nullptr);
    if (walled.polyAt(north) && walled.polyAt(south))
        CHECK(!walled.findPath(north, south).has_value());

    // A gate in it: eight metres of the wall taken out, and the way is open
    // again - and the path goes through the gap rather than through the wall.
    std::vector<world::Obstacle> withGate;
    withGate.push_back({{corner.x, corner.y + Fixed::fromInt(28)},
                        {corner.x + Fixed::fromInt(28), corner.y + Fixed::fromInt(36)}});
    withGate.push_back({{corner.x + Fixed::fromInt(36), corner.y + Fixed::fromInt(28)},
                        {corner.x + Fixed::fromInt(world::kChunkMetres),
                         corner.y + Fixed::fromInt(36)}});
    world::NavMesh gated;
    gated.build(pages().of(chunk), chunk, withGate);
    const std::optional<world::Path> through = gated.findPath(north, south);
    CHECK(through.has_value());
    if (!through) return;
    // Where the line crosses the wall it has to be in the gap. Checked on the
    // line rather than on its corners: a funnel bends only where it must, so a
    // path straight through an open gate has no corner in the gateway at all.
    const Fixed wallMiddle = corner.y + Fixed::fromInt(32);
    bool crossed = false, inTheGap = false;
    for (std::size_t i = 0; i + 1 < through->points.size(); ++i) {
        const WorldPos a = through->points[i], b = through->points[i + 1];
        if ((a.y.raw <= wallMiddle.raw) == (b.y.raw <= wallMiddle.raw)) continue;
        crossed = true;
        const Fixed t = (wallMiddle - a.y) / (b.y - a.y);
        const Fixed x = a.x + (b.x - a.x) * t;
        inTheGap = x.raw >= (corner.x + Fixed::fromInt(28)).raw &&
                   x.raw <= (corner.x + Fixed::fromInt(36)).raw;
    }
    CHECK(crossed);
    CHECK(inTheGap);
}

TEST(the_same_ground_gives_the_same_path_every_time) {
    const world::HeightField field(&country(), 5);
    const world::ChunkId centre = groundWhere(field, false);
    const world::NavMesh first = built(centre);
    const world::NavMesh second = built(centre);

    const WorldPos from =
            first.nearestWalkable(world::chunkOrigin({centre.x - 1, centre.y - 1}), Fixed::fromInt(90))
                    .value_or(WorldPos{});
    const WorldPos to =
            first.nearestWalkable(world::chunkOrigin({centre.x + 1, centre.y + 1}), Fixed::fromInt(90))
                    .value_or(WorldPos{});
    const std::optional<world::Path> a = first.findPath(from, to);
    const std::optional<world::Path> b = second.findPath(from, to);
    CHECK(a.has_value() == b.has_value());
    if (!a || !b) return;
    CHECK(a->points.size() == b->points.size());
    CHECK(a->length.raw == b->length.raw);
    for (std::size_t i = 0; i < a->points.size() && i < b->points.size(); ++i) {
        CHECK(a->points[i].x.raw == b->points[i].x.raw);
        CHECK(a->points[i].y.raw == b->points[i].y.raw);
    }
}

TEST(the_funnel_cuts_the_corner_it_is_given) {
    // The funnel on its own, over a corridor with a bend in it, where the right
    // answer can be worked out by hand: two gates that step sideways, so the
    // straight line from start to finish does not pass through both. The string
    // has to bend exactly once, at the corner that squeezed it.
    const auto p = [](int x, int y) {
        return WorldPos{Fixed::fromInt(x), Fixed::fromInt(y)};
    };

    // Straight ahead, then a gate that has moved off the line: walking from
    // (0,0) to (30,0), the second gate only opens between y=3 and y=9, so the
    // string has to bend round its near end at (20,3) and nowhere else.
    std::vector<world::Portal> gates;
    gates.push_back({1, p(10, -5), p(10, 5)});
    gates.push_back({2, p(20, 3), p(20, 9)});
    const std::vector<WorldPos> line = world::NavMesh::pullString(gates, p(0, 0), p(30, 0));

    CHECK(line.size() == 3);
    CHECK(line.front().x.raw == p(0, 0).x.raw && line.front().y.raw == p(0, 0).y.raw);
    CHECK(line.back().x.raw == p(30, 0).x.raw && line.back().y.raw == p(30, 0).y.raw);
    if (line.size() == 3) {
        CHECK(line[1].x.raw == Fixed::fromInt(20).raw);
        CHECK(line[1].y.raw == Fixed::fromInt(3).raw);
    }

    // And when the straight line does pass through every gate, the string does
    // not bend at all: two points, start to finish.
    std::vector<world::Portal> open;
    open.push_back({1, p(10, -5), p(10, 5)});
    open.push_back({2, p(20, -5), p(20, 5)});
    const std::vector<WorldPos> straight = world::NavMesh::pullString(open, p(0, 0), p(30, 0));
    CHECK(straight.size() == 2);
}

TEST(navigation_geometry_is_not_terrain_geometry) {
    // The two meshes describe the same ground and are not the same shape. If
    // they ever become the same shape, the separation has quietly been lost and
    // navigation is back to being a picture of the terrain.
    const world::HeightField field(&country(), 5);
    const world::ChunkId chunk = groundWhere(field, false);
    world::NavMesh nav;
    nav.build(pages().of(chunk), chunk, {});

    std::size_t navPieces = 0;
    for (const world::NavPoly& p : nav.polys())
        if (p.max.x.raw > p.min.x.raw) ++navPieces;

    const world::TerrainMesh terrain = world::buildChunkMesh(field, chunk);
    CHECK(navPieces > 0);
    CHECK(terrain.triangleCount() > navPieces * 2);
}

TEST(a_range_is_crossed_by_climbing_it_or_by_going_round) {
    // The point of having more than one way to get over ground: a body will
    // walk round a scramble it can afford to walk round, and take the climb
    // when the alternative is the far end of a mountain range.
    const world::HeightField field(&country(), 5);
    const world::ChunkId rough = groundWhere(field, true);
    const world::NavMesh nav = built(rough);

    // The navmesh knows the range is there: not everything in a mountain chunk
    // is level going, and none of it is missing.
    int walk = 0, scramble = 0, climb = 0;
    for (const world::NavPoly& p : nav.polys()) {
        if (p.max.x.raw <= p.min.x.raw) continue;
        if (p.travel == world::Travel::Walk) ++walk;
        else if (p.travel == world::Travel::Scramble) ++scramble;
        else if (p.travel == world::Travel::Climb) ++climb;
    }
    if (scramble + climb == 0)
        std::cerr << "    the roughest ground in this country is level; nothing was proved\n";
    CHECK(scramble + climb > 0);
    // There may be no level going at all in the steepest chunk of a range -
    // that is what a range is - so this is reported rather than required.
    if (walk == 0) std::cerr << "    no level ground in this chunk at all: it is all mountain\n";

    // Steep ground costs more than its length. A path that crosses any of it
    // pays for it, so its cost is above its length; a path over level ground
    // costs exactly its length.
    const WorldPos corner = world::chunkOrigin(rough);
    const WorldPos from = nav.nearestWalkable({corner.x + Fixed::fromInt(6),
                                               corner.y + Fixed::fromInt(6)},
                                              Fixed::fromInt(90))
                                  .value_or(WorldPos{});
    const WorldPos to = nav.nearestWalkable(
                               {corner.x + Fixed::fromInt(world::kChunkMetres - 6),
                                corner.y + Fixed::fromInt(world::kChunkMetres - 6)},
                               Fixed::fromInt(90))
                                .value_or(WorldPos{});
    const std::optional<world::Path> path = nav.findPath(from, to);
    CHECK(path.has_value());
    if (!path) return;
    CHECK(path->cost.raw >= path->length.raw);
    if (path->worst != world::Travel::Walk) CHECK(path->cost.raw > path->length.raw);

    // And the classes really are ordered: crossing a climb costs more than
    // crossing the same distance of scramble, which costs more than walking it.
    CHECK(world::travelCost(world::Travel::Walk).raw <
          world::travelCost(world::Travel::Scramble).raw);
    CHECK(world::travelCost(world::Travel::Scramble).raw <
          world::travelCost(world::Travel::Climb).raw);
}

TEST(a_body_goes_round_rough_ground_it_can_afford_to_go_round) {
    // A wall of scramble with a level way round it: the path has to take the
    // way round, and it has to be longer than the straight line to prove it.
    const world::HeightField field(&country(), 5);
    const world::ChunkId flat = groundWhere(field, false);
    const WorldPos corner = world::chunkOrigin(flat);

    // Nothing steep here to begin with, so anything the path avoids is what
    // this test put in its way.
    world::NavMesh open;
    open.build(pages().of(flat), flat, {});
    const WorldPos start{corner.x + Fixed::fromInt(32), corner.y + Fixed::fromInt(6)};
    const WorldPos finish{corner.x + Fixed::fromInt(32), corner.y + Fixed::fromInt(58)};
    const std::optional<world::Path> straight = open.findPath(start, finish);
    CHECK(straight.has_value());
    if (!straight) return;

    // A bar of obstacle across the middle with a gap at one side: the same
    // shape a scramble would have, and the cheapest thing to test it with.
    std::vector<world::Obstacle> bar;
    bar.push_back({{corner.x + Fixed::fromInt(8), corner.y + Fixed::fromInt(28)},
                   {corner.x + Fixed::fromInt(world::kChunkMetres), corner.y + Fixed::fromInt(36)}});
    world::NavMesh barred;
    barred.build(pages().of(flat), flat, bar);
    const std::optional<world::Path> round = barred.findPath(start, finish);
    CHECK(round.has_value());
    if (!round) return;
    CHECK(round->length.raw > straight->length.raw);
    // It went round the western end, where the gap is.
    bool wentWest = false;
    for (const WorldPos& p : round->points)
        if (p.x.raw < (corner.x + Fixed::fromInt(10)).raw) wentWest = true;
    CHECK(wentWest);
}
