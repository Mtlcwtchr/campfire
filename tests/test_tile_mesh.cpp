#include "framework.hpp"

#include <cmath>
#include <map>
#include <set>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/terrain_streaming/page_ground.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/world/tile_mesh.hpp"

namespace {
using core::Fixed;
using namespace world::streaming;

const generation::WorldMapData& country() {
    static const auto world = [] {
        generation::WorldMapParams params;
        params.seed = 11;
        params.width = params.height = 48;
        return generation::generateWorldMap(params);
    }();
    return world;
}
struct Ground {
    HydrologyGraph graph;
    PageStore pages;
    world::HeightField field;
    world::ClimateField climate;
    explicit Ground(const generation::WorldMapData& world)
        : graph(buildHydrologyGraph(world)),
          pages(world, graph, hsimQuantisationFor(world)),
          field(&world, world.seed) {
        climate.raise(world, field);
    }
};
std::pair<std::int64_t, std::int64_t> latticeOf(const world::TerrainVertex& v) {
    return {v.position.x.toInt(), v.position.y.toInt()};
}
} // namespace

TEST(tile_is_named_by_the_ground_it_covers_not_by_where_the_camera_was) {
    // The whole reason for the square tile. An annulus is addressed by an
    // epoch and a ring index, so moving the camera renames every piece of
    // ground and it is built again from nothing. A tile is where it is.
    Ground ground(country());
    const world::TileId id{3, 4, 0};
    const auto first = world::buildTileMesh(ground.field, ground.pages, ground.climate, id);
    const auto again = world::buildTileMesh(ground.field, ground.pages, ground.climate, id);
    CHECK(!first.vertices.empty());
    CHECK_EQ(first.vertices.size(), again.vertices.size());
    CHECK_EQ(world::tileKeyOf(id), world::tileKeyOf({3, 4, 0}));
    CHECK(world::tileKeyOf(id) != world::tileKeyOf({3, 4, 1}));
    CHECK(world::tileKeyOf(id) != world::tileKeyOf({4, 4, 0}));
    // Bit for bit the same ground, however many times it is asked for.
    for (std::size_t i = 0; i < first.vertices.size(); ++i) {
        CHECK_EQ(first.vertices[i].position.x, again.vertices[i].position.x);
        CHECK_EQ(first.vertices[i].height, again.vertices[i].height);
    }
}

TEST(persistent_h64_is_derived_from_h16_at_shared_samples) {
    Ground ground(country());
    PageLattice h16(ground.pages, 2);
    PageLattice h64(ground.pages, 4);
    // Lattice coordinates are in four-metre units; sixteen of them are 64 m.
    for (std::int64_t y = 0; y <= 256; y += 16)
        for (std::int64_t x = 0; x <= 256; x += 16)
            CHECK_EQ(h64.visualHeight(x, y), h16.visualHeight(x, y));
}

TEST(tiles_share_their_border_vertices_exactly) {
    // Nothing is stitched. Two tiles that meet name the same world coordinates
    // on their shared edge and read them from the same page, so they come out
    // identical - a crack there is unrepresentable rather than corrected.
    Ground ground(country());
    const auto west = world::buildTileMesh(ground.field, ground.pages, ground.climate, {3, 4, 0});
    const auto east = world::buildTileMesh(ground.field, ground.pages, ground.climate, {4, 4, 0});
    std::map<std::pair<std::int64_t, std::int64_t>, Fixed> edge;
    const std::int32_t side = world::kTileCells + 1;
    for (std::int32_t row = 0; row < side; ++row) {
        const auto& v = west.vertices[static_cast<std::size_t>(row * side + side - 1)];
        edge[latticeOf(v)] = v.height;
    }
    std::size_t shared = 0;
    for (std::int32_t row = 0; row < side; ++row) {
        const auto& v = east.vertices[static_cast<std::size_t>(row * side)];
        const auto found = edge.find(latticeOf(v));
        CHECK(found != edge.end());
        if (found == edge.end()) continue;
        ++shared;
        CHECK_EQ(found->second, v.height);
    }
    CHECK_EQ(shared, static_cast<std::size_t>(side));
}

TEST(tile_levels_nest_so_a_morph_has_somewhere_to_walk_to) {
    // A tile of level one covers exactly four of level nought, and every one
    // of its vertices is a vertex of theirs at the same world coordinate. That
    // is what the polar mesh could never offer: there, no vertex of one level
    // stood where a vertex of the level above stood, so a change of level had
    // to be a dissolve instead of a morph.
    Ground ground(country());
    const auto coarse = world::buildTileMesh(ground.field, ground.pages, ground.climate, {1, 1, 1});
    struct There { Fixed height; world::Normal normal; };
    std::map<std::pair<std::int64_t, std::int64_t>, There> byPlace;
    const std::int32_t side = world::kTileCells + 1;
    for (std::int32_t i = 0; i < side * side; ++i) {
        const auto& v = coarse.vertices[static_cast<std::size_t>(i)];
        byPlace[latticeOf(v)] = {v.height, v.normal};
    }
    std::size_t compared = 0, wrongTarget = 0, changedSharedSamples = 0;
    for (const world::TileId fine : {world::TileId{2, 2, 0}, {3, 2, 0}, {2, 3, 0}, {3, 3, 0}}) {
        const auto tile = world::buildTileMesh(ground.field, ground.pages, ground.climate, fine);
        for (std::int32_t i = 0; i < side * side; ++i) {
            const auto& v = tile.vertices[static_cast<std::size_t>(i)];
            const auto found = byPlace.find(latticeOf(v));
            if (found == byPlace.end()) continue;
            ++compared;
            // At a shared vertex the fine tile's morph target is exactly where
            // the coarse tile puts that point. Walk the morph to one and this
            // mesh *is* the coarse mesh there, to the last bit.
            if (v.coarseHeight != found->second.height) ++wrongTarget;
            // The light walks with the shape. A hillside drawn from samples
            // twice as far apart is a different shape and has a different
            // normal; if only the geometry morphed, the ground would slide
            // while the light jumped.
            if (core::abs(v.coarseNormal.z - found->second.normal.z) >
                Fixed::ratio(1, 100))
                ++wrongTarget;
            // Legacy H4 added a residual band. New H64 worlds keep sub-H8
            // detail in materials, so shared geometry samples stay identical.
            if (v.height != found->second.height) ++changedSharedSamples;
        }
    }
    CHECK(compared > 1000);
    CHECK_EQ(wrongTarget, std::size_t(0));
    if (country().terrainFoundation) CHECK_EQ(changedSharedSamples,std::size_t(0));
    else CHECK(changedSharedSamples > 0);

    // H8 refines H16 without adding a new band: their shared samples and
    // morph targets must both match exactly.
    const auto sixteen = world::buildTileMesh(ground.field, ground.pages, ground.climate, {1, 1, 2});
    std::map<std::pair<std::int64_t, std::int64_t>, Fixed> h16;
    for (std::int32_t i = 0; i < side * side; ++i)
        h16[latticeOf(sixteen.vertices[static_cast<std::size_t>(i)])] =
                sixteen.vertices[static_cast<std::size_t>(i)].height;
    std::size_t dataCompared = 0;
    for (const world::TileId child : {world::TileId{2, 2, 1}, {3, 2, 1}, {2, 3, 1}, {3, 3, 1}}) {
        const auto eight = world::buildTileMesh(ground.field, ground.pages, ground.climate, child);
        for (std::int32_t i = 0; i < side * side; ++i) {
            const auto& v = eight.vertices[static_cast<std::size_t>(i)];
            const auto parent = h16.find(latticeOf(v));
            if (parent == h16.end()) continue;
            ++dataCompared;
            CHECK_EQ(v.coarseHeight, parent->second);
            CHECK_EQ(v.height, parent->second);
        }
    }
    CHECK(dataCompared > 1000);
}

TEST(tile_carries_a_skirt_so_a_seam_shows_ground_rather_than_sky) {
    Ground ground(country());
    const auto tile = world::buildTileMesh(ground.field, ground.pages, ground.climate, {3, 4, 0});
    const std::int32_t side = world::kTileCells + 1;
    const auto grid = static_cast<std::size_t>(side) * side;
    CHECK(tile.vertices.size() > grid);
    // Every skirt vertex sits under a border vertex, at the same place.
    std::size_t under = 0;
    for (std::size_t i = grid; i < tile.vertices.size(); ++i) {
        const auto& hem = tile.vertices[i];
        for (std::size_t j = 0; j < grid; ++j)
            if (tile.vertices[j].position.x == hem.position.x &&
                tile.vertices[j].position.y == hem.position.y &&
                tile.vertices[j].height > hem.height) {
                ++under;
                break;
            }
    }
    CHECK_EQ(under, tile.vertices.size() - grid);
}

TEST(shared_grid_indices_match_every_tile_including_its_skirt) {
    Ground ground(country());
    const auto tile = world::buildTileMesh(ground.field, ground.pages, ground.climate, {3, 4, 2});
    const auto grid = world::terrain::makeGridTopology(world::kTileCells);
    CHECK_EQ(grid.vertices.size(), tile.vertices.size());
    CHECK_EQ(grid.indices.size(), tile.indices.size());
    for (std::size_t i = 0; i < grid.indices.size(); ++i)
        CHECK_EQ(static_cast<std::uint32_t>(grid.indices[i]), tile.indices[i]);
}
