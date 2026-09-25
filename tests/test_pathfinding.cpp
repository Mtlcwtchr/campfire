#include "framework.hpp"
#include <iostream>
#include <cmath>
#include "support.hpp"

#include "game/client/wall_tiling.hpp"
#include "game/simulation/pathfinder.hpp"
#include "game/simulation/world.hpp"

namespace {

sim::TileMap openMap(std::int32_t w, std::int32_t h) {
    sim::TileMap m;
    m.resize(w, h);
    for (std::int32_t y = 0; y < h; ++y)
        for (std::int32_t x = 0; x < w; ++x) {
            m.at({x, y}).terrain = sim::Terrain::Grass;
            m.setBlocked({x, y}, false);
        }
    return m;
}

} // namespace

TEST(a_straight_path_is_exactly_the_hex_distance) {
    // On a tile grid every neighbour is one step, so an unobstructed path is
    // always as long as the distance and never longer. There is no diagonal to
    // correct for - which is the reason for the grid.
    auto m = openMap(24, 24);
    const core::TilePos from{4, 4};
    for (const core::TilePos to : {core::TilePos{9, 4}, core::TilePos{4, 12},
                                   core::TilePos{11, 13}, core::TilePos{1, 15}}) {
        const auto r = sim::findPath(m, from, to);
        CHECK(r.found);
        CHECK_EQ(r.tiles.size(), std::size_t(core::tileDistance(from, to)));
        CHECK((r.tiles.back() == to));
    }
}

TEST(every_step_of_a_path_is_a_real_neighbour) {
    auto m = openMap(24, 24);
    for (std::int32_t i = 0; i < 24; ++i) m.setBlocked({12, i}, i != 9);
    const auto r = sim::findPath(m, {3, 3}, {20, 18});
    CHECK(r.found);

    core::TilePos previous{3, 3};
    for (core::TilePos step : r.tiles) {
        CHECK_EQ(core::tileDistance(previous, step), 1);
        previous = step;
    }
}

TEST(path_goes_around_a_wall) {
    auto m = openMap(20, 20);
    for (std::int32_t y = 0; y < 19; ++y) m.setBlocked({10, y}, true);
    const auto r = sim::findPath(m, {5, 10}, {15, 10});
    CHECK(r.found);
    CHECK(r.tiles.size() > 10);                    // had to detour around the gap
    CHECK((r.tiles.back() == core::TilePos{15, 10}));
}

TEST(path_reports_failure_when_walled_in) {
    auto m = openMap(20, 20);
    for (std::int32_t y = 0; y < 20; ++y) m.setBlocked({10, y}, true);
    const auto r = sim::findPath(m, {5, 10}, {15, 10});
    CHECK(!r.found);
}

TEST(adjacent_path_reaches_a_blocked_target) {
    auto m = openMap(20, 20);
    m.setBlocked({10, 10}, true);                  // a tree
    const auto r = sim::findPathAdjacent(m, {2, 2}, {10, 10});
    CHECK(r.found);
    CHECK((core::chebyshev(r.tiles.back(), core::TilePos{10, 10}) == 1));
}

TEST(neighbours_are_symmetric_and_all_one_step_away) {
    // The whole grid rests on this: if a tile's neighbour does not have it back
    // as a neighbour, paths become one-way and the planner starts lying.
    for (std::int32_t y = -3; y <= 3; ++y) {
        for (std::int32_t x = -3; x <= 3; ++x) {
            const core::TilePos p{x, y};
            for (int d = 0; d < core::kNeighbourCount; ++d) {
                const core::TilePos n = core::neighbour(p, d);
                CHECK_EQ(core::tileDistance(p, n), 1);
                bool mutual = false;
                for (int e = 0; e < core::kNeighbourCount; ++e)
                    if (core::neighbour(n, e) == p) mutual = true;
                CHECK(mutual);
            }
            // The four cardinals are the even directions, which is what lets an
            // area outline be drawn along shared edges only.
            for (int d : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour(p, d);
                CHECK((n.x == p.x || n.y == p.y));
                CHECK(!core::isDiagonal(d));
            }
        }
    }
}

TEST(a_tile_and_its_pixel_centre_round_trip) {
    // A click has to land on the tile it looks like it landed on.
    for (std::int32_t y = -6; y <= 6; ++y)
        for (std::int32_t x = -6; x <= 6; ++x) {
            const core::TilePos t{x, y};
            CHECK((core::toTile(core::tileCentre(t)) == t));
        }
}

TEST(a_disc_holds_the_right_number_of_tiles) {
    // "Within n steps" on a square grid where a diagonal is a step means a
    // square block, and the ring at exactly n steps is its border.
    for (std::int32_t radius = 0; radius <= 4; ++radius) {
        const auto disc = core::tilesWithin({7, 9}, radius);
        const std::size_t span = std::size_t(2 * radius + 1);
        CHECK_EQ(disc.size(), span * span);
        for (core::TilePos t : disc) CHECK(core::tileDistance({7, 9}, t) <= radius);
    }
    for (std::int32_t radius = 1; radius <= 4; ++radius) {
        const auto ring = core::tileRing({7, 9}, radius);
        CHECK_EQ(ring.size(), std::size_t(8 * radius));
        for (core::TilePos t : ring) CHECK_EQ(core::tileDistance({7, 9}, t), radius);
        // No tile twice: the four sides must each stop short of the next corner.
        for (std::size_t i = 0; i < ring.size(); ++i)
            for (std::size_t j = i + 1; j < ring.size(); ++j) CHECK(ring[i] != ring[j]);
    }
}

TEST(pathfinding_is_deterministic) {
    auto m = openMap(40, 40);
    for (std::int32_t i = 0; i < 30; ++i) m.setBlocked({i % 37 + 1, (i * 7) % 37 + 1}, true);
    const auto a = sim::findPath(m, {0, 0}, {39, 39});
    const auto b = sim::findPath(m, {0, 0}, {39, 39});
    CHECK_EQ(a.found, b.found);
    CHECK_EQ(a.tiles.size(), b.tiles.size());
    for (std::size_t i = 0; i < a.tiles.size(); ++i) CHECK(a.tiles[i] == b.tiles[i]);
}

TEST(each_neighbour_direction_has_the_edge_that_faces_it) {
    // The renderer draws an area outline by stroking, for each tile on the
    // border, only the edges facing outward. That needs a table from neighbour
    // direction to tile edge, and a wrong table draws an outline that looks
    // almost right - so it is checked against the geometry instead of by eye.
    // A diagonal neighbour shares only a corner and so has no edge at all.
    //
    // This mirrors kDirectionToEdge in renderer.cpp.
    static constexpr int kDirectionToEdge[8]{1, -1, 0, -1, 3, -1, 2, -1};
    static constexpr float kCornerX[4]{-1.0f, +1.0f, +1.0f, -1.0f};
    static constexpr float kCornerY[4]{-1.0f, -1.0f, +1.0f, +1.0f};

    const core::TilePos tile{4, 7};
    const auto centre = core::tileCentre(tile);

    for (int direction = 0; direction < core::kNeighbourCount; ++direction) {
        if (core::isDiagonal(direction)) {
            CHECK_EQ(kDirectionToEdge[direction], -1);
            continue;
        }
        const core::TilePos next = core::neighbour(tile, direction);
        const auto towards = core::tileCentre(next);
        const double dx = towards.x.toDouble() - centre.x.toDouble();
        const double dy = towards.y.toDouble() - centre.y.toDouble();

        // The edge whose midpoint lies furthest along the direction of the
        // neighbour is the shared one.
        int best = -1;
        double bestProjection = -1e9;
        for (int edge = 0; edge < 4; ++edge) {
            const int i = edge, j = (edge + 1) % 4;
            const double mx = (kCornerX[i] + kCornerX[j]) * 0.5;
            const double my = (kCornerY[i] + kCornerY[j]) * 0.5;
            const double projection = mx * dx + my * dy;
            if (projection > bestProjection) { bestProjection = projection; best = edge; }
        }
        CHECK_EQ(best, kDirectionToEdge[direction]);
    }
}

TEST(a_diagonal_step_does_not_cut_a_corner) {
    // A wall with a corner in it has to be a wall. Two blocked tiles meeting at
    // a corner must not leave a diagonal gap a body can slip through.
    auto m = openMap(9, 9);
    m.setBlocked({4, 3}, true);
    m.setBlocked({3, 4}, true);
    const auto r = sim::findPath(m, {4, 4}, {3, 3});
    CHECK(r.found);
    // The direct diagonal is one step; going round is more.
    CHECK(r.tiles.size() > 1);
}

TEST(a_turn_moves_a_tile_card_the_other_way_from_the_clock) {
    // The renderer turns a ground card by shifting which corner samples which
    // texture corner: corner i takes texture corner (i + turn) % 4, with corners
    // ordered north-west, north-east, south-east, south-west.
    //
    // That is a counter-clockwise turn, and getting it backwards is exactly the
    // mistake that put a bite of water into the middle of a shore tile instead
    // of along its edge. The shore cards are drawn with the water at the north,
    // so one turn has to put it at the west.
    constexpr int kCorners = 4;
    // Where each corner is, in units of half a tile.
    constexpr float kCornerX[kCorners]{-1.0f, +1.0f, +1.0f, -1.0f};
    constexpr float kCornerY[kCorners]{-1.0f, -1.0f, +1.0f, +1.0f};

    // The texture's northern half, sampled at the corners the renderer would use.
    for (int turn = 0; turn < kCorners; ++turn) {
        // Take the mid-point of the texture's north edge - texture corners 0 and
        // 1 - and find where those corners land on the tile after the turn.
        float sx = 0.0f, sy = 0.0f;
        for (int textureCorner : {0, 1}) {
            // Which screen corner samples this texture corner: i such that
            // (i + turn) % 4 == textureCorner.
            const int screenCorner = ((textureCorner - turn) % kCorners + kCorners) % kCorners;
            sx += kCornerX[screenCorner] * 0.5f;
            sy += kCornerY[screenCorner] * 0.5f;
        }
        // turn 0 north, 1 west, 2 south, 3 east.
        const float wantX[kCorners]{0.0f, -1.0f, 0.0f, +1.0f};
        const float wantY[kCorners]{-1.0f, 0.0f, +1.0f, 0.0f};
        if (std::abs(sx - wantX[turn]) > 0.01f || std::abs(sy - wantY[turn]) > 0.01f)
            std::cerr << "    turn " << turn << " puts the card's north at " << sx << "," << sy
                      << " and it should be at " << wantX[turn] << "," << wantY[turn] << "\n";
        CHECK(std::abs(sx - wantX[turn]) < 0.01f);
        CHECK(std::abs(sy - wantY[turn]) < 0.01f);
    }
}

TEST(a_wall_is_drawn_for_the_wall_beside_it) {
    using namespace client;

    // A run east to west is the card drawn across the view; a run north to south
    // is the one drawn receding. Before this, every tile of a citadel wall got
    // the same front-on segment, so the sides of the ring read as loose posts.
    CHECK(wallVariant(kWallEast | kWallWest).empty());
    CHECK_EQ(wallVariant(kWallNorth | kWallSouth), std::string("_side"));

    // A stub points along its run rather than changing shape as the wall goes
    // up, and a tile on its own takes the straight card.
    CHECK(wallVariant(kWallEast).empty());
    CHECK_EQ(wallVariant(kWallNorth), std::string("_side"));
    CHECK(wallVariant(0).empty());

    // A turn keeps the run going: the receding card, with the straight from the
    // next tile butting into its side. The bend the sheet provides covers two
    // tiles of wall, and shrunk into one it left a notch at every corner.
    CHECK_EQ(wallVariant(kWallNorth | kWallEast), std::string("_side"));
    CHECK_EQ(wallVariant(kWallSouth | kWallWest), std::string("_side"));
    CHECK_EQ(wallVariant(kWallNorth | kWallEast | kWallSouth), std::string("_side"));
    CHECK_EQ(wallVariant(kWallNorth | kWallEast | kWallSouth | kWallWest), std::string("_side"));
}
