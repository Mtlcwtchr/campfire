#include "framework.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/hydrology_cache.hpp"

namespace {
using core::Fixed;
using core::TilePos;
using namespace world::streaming;

struct BasinWorld {
    generation::WorldMapData map;
    std::shared_ptr<generation::TerrainFoundation> foundation =
            std::make_shared<generation::TerrainFoundation>();

    BasinWorld() {
        map.width = map.height = 10;
        map.cells.resize(100);
        for (auto& cell : map.cells) {
            cell.elevation = 10;
            cell.moisture = 160;
            cell.temperature = 130;
        }
        map.lakeRegionField.assign(100, -1);
        map.lakeLevelField.assign(100, 0);
        map.lakeDepthField.assign(100, 0);
        map.lakeRegionField[44] = 44;
        map.lakeLevelField[44] = 5; // 45 m above sea level
        map.lakeDepthField[44] = 3;
        map.cells[44].elevation = 5;
        foundation->columns = foundation->rows =
                (10 * generation::kMetresPerCell + foundation->step - 1) / foundation->step + 1;
        for (auto& plane : foundation->heightDm)
            plane.assign(std::size_t(foundation->columns) * foundation->rows, 900);
        // The depression extends beyond the proposed macro lake into page x=6.
        for (int y = 36; y <= 40; ++y)
            for (int x = 36; x <= 48; ++x) height(x, y) = 200;
        height(37, 38) = 100;
        map.terrainFoundation = foundation;
    }

    std::int32_t& height(int x, int y) {
        return foundation->heightDm[static_cast<std::size_t>(generation::TerrainStage::Slopes)]
                [std::size_t(y) * foundation->columns + x];
    }

    void river(TilePos from, TilePos to) {
        auto& cell = map.at(from);
        cell.river = true;
        cell.drainSize = cell.riverSize = 8;
        for (int d = 0; d < core::kNeighbourCount; ++d)
            if (core::neighbour(from, d) == to)
                cell.riverOut = cell.drainOut = static_cast<std::int8_t>(d);
    }
};

core::WorldPos position(int x, int y) {
    return {Fixed::fromInt(x * 64), Fixed::fromInt(y * 64)};
}

bool contains(const WaterBody& body, TilePos sample) {
    return std::find(body.basinSamples.begin(), body.basinSamples.end(), sample) !=
           body.basinSamples.end();
}

CarvedSample sample(const GraphCarver& carver, const BasinWorld& world, core::WorldPos at) {
    const auto ground = world.foundation->sample(at.x, at.y, generation::TerrainStage::Slopes);
    return carver.carve(at, ground, core::kZero);
}
} // namespace

TEST(natural_basin_uses_slopes_without_sculpting_a_mask_or_rim) {
    BasinWorld world;
    const auto graph = buildHydrologyGraph(world.map);
    CHECK(validateHydrologyGraph(graph));
    CHECK_EQ(graph.waterBodies.size(), std::size_t(2));
    const auto& lake = graph.waterBodies.at(1);
    CHECK_EQ(lake.level, Fixed::fromInt(45));
    CHECK_EQ(lake.basinStep, 64);
    CHECK(contains(lake, {48, 38}));
    const GraphCarver carver(graph, {position(34, 34), position(50, 42)}, 0);
    for (int x = 34 * 64; x <= 50 * 64; x += 4) {
        const core::WorldPos at{Fixed::fromInt(x), position(0, 38).y};
        const auto ground = world.foundation->sample(at.x, at.y, generation::TerrainStage::Slopes);
        const auto water = sample(carver, world, at);
        CHECK_EQ(water.floor, ground);
        CHECK_EQ(water.wet, ground < lake.level);
        if (water.wet) {
            CHECK_EQ(water.body, lake.id);
            CHECK_EQ(water.surface, lake.level);
        }
    }
}

TEST(natural_basin_does_not_fill_a_disconnected_lower_depression) {
    BasinWorld world;
    world.height(50, 38) = 50; // lower, but separated by the dry x=49 ridge
    const auto graph = buildHydrologyGraph(world.map);
    const auto& lake = graph.waterBodies.at(1);
    CHECK(contains(lake, {48, 38}));
    CHECK(!contains(lake, {50, 38}));
    const GraphCarver carver(graph, {position(34, 34), position(52, 42)}, 0);
    CHECK(sample(carver, world, position(48, 38)).wet);
    CHECK(!sample(carver, world, position(50, 38)).wet);
}

TEST(natural_basin_does_not_cross_a_dry_diagonal_saddle) {
    BasinWorld world;
    world.height(49, 41) = 200; // touches (48,40) only diagonally
    const auto graph = buildHydrologyGraph(world.map);
    const auto& lake = graph.waterBodies.at(1);
    CHECK(contains(lake, {48, 40}));
    CHECK(!contains(lake, {49, 41}));
    // Even the middle of the diagonal stands above the lake's head.
    CHECK(world.foundation->sample(Fixed::fromInt(48 * 64 + 32),
            Fixed::fromInt(40 * 64 + 32), generation::TerrainStage::Slopes) > lake.level);
    const GraphCarver carver(graph, {position(34, 34), position(52, 43)}, 0);
    CHECK(!sample(carver, world, position(49, 41)).wet);
}

TEST(natural_basin_spills_at_the_connected_sill_and_preserves_river_heads) {
    BasinWorld world;
    // Escape to the bounded search edge at 30 m, below the proposed 45 m head.
    for (int x = 49; x < world.foundation->columns; ++x) world.height(x, 38) = 300;
    world.river({3, 4}, {4, 4});
    world.river({4, 4}, {5, 4});
    world.river({5, 4}, {6, 4});
    const auto graph = buildHydrologyGraph(world.map);
    CHECK(validateHydrologyGraph(graph));
    const auto& lake = graph.waterBodies.at(1);
    CHECK_EQ(lake.level, Fixed::fromInt(30));
    CHECK(!lake.inlets.empty());
    CHECK(!lake.outlets.empty());
    CHECK(!contains(lake, {49, 38})); // the sill is dry at exactly the head
    for (const auto& node : graph.nodes)
        if (node.waterBody == lake.id) CHECK_EQ(node.surface, lake.level);
    for (const auto& segment : graph.segments) {
        if (segment.sourceWaterBody == lake.id) CHECK_EQ(segment.course.front().surface, lake.level);
        if (segment.destinationWaterBody == lake.id) CHECK_EQ(segment.course.back().surface, lake.level);
        for (std::size_t i = 1; i < segment.course.size(); ++i)
            CHECK(segment.course[i].surface <= segment.course[i - 1].surface);
    }
}

TEST(natural_basin_open_slope_holds_no_standing_water) {
    BasinWorld world;
    auto& plane = world.foundation->heightDm[static_cast<std::size_t>(generation::TerrainStage::Slopes)];
    std::fill(plane.begin(), plane.end(), 200);
    const auto graph = buildHydrologyGraph(world.map);
    const auto& lake = graph.waterBodies.at(1);
    CHECK_EQ(lake.level, Fixed::fromInt(20));
    CHECK(lake.basinSamples.empty());
    for (const auto& page : graph.spatialPages)
        CHECK(!std::binary_search(page.waterBodies.begin(), page.waterBodies.end(), lake.id));
    const GraphCarver carver(graph, {position(34, 34), position(52, 43)}, 0);
    // An empty natural basin must not silently fall back to its old macro mask.
    CHECK(!carver.carve(position(38, 38), Fixed::fromInt(10), core::kZero).wet);
}

TEST(natural_basin_spatial_index_covers_expansion_and_shore_support) {
    BasinWorld world;
    const auto graph = buildHydrologyGraph(world.map);
    const auto& lake = graph.waterBodies.at(1);
    for (const auto node : lake.basinSamples) {
        // Bilinear support extends one foundation step in all directions.
        for (const int dy : {-63, 0, 63}) for (const int dx : {-63, 0, 63}) {
            const auto at = position(node.x, node.y);
            const auto* page = findHydrologySpatialPage(graph,
                    tileAt({at.x + Fixed::fromInt(dx), at.y + Fixed::fromInt(dy)}));
            CHECK(page != nullptr);
            if (page) CHECK(std::binary_search(page->waterBodies.begin(), page->waterBodies.end(), lake.id));
        }
    }
}

TEST(natural_basin_samples_agree_across_page_windows_and_sampling_strides) {
    BasinWorld world;
    const auto graph = buildHydrologyGraph(world.map);
    const GraphCarver whole(graph, {position(32, 32), position(56, 48)}, 0);
    const GraphCarver left(graph, {{Fixed::fromInt(2560), Fixed::fromInt(2048)},
                                  {Fixed::fromInt(3072), Fixed::fromInt(2560)}}, 8);
    const GraphCarver right(graph, {{Fixed::fromInt(3072), Fixed::fromInt(2048)},
                                   {Fixed::fromInt(3584), Fixed::fromInt(2560)}}, 8);
    for (const int stride : {4, 16, 64})
        for (int y = 36 * 64; y <= 41 * 64; y += stride)
            for (int dx = -8; dx <= 8; dx += 4) {
                const core::WorldPos at{Fixed::fromInt(3072 + dx), Fixed::fromInt(y)};
                const auto expected = sample(whole, world, at);
                for (const auto* window : {&left, &right}) {
                    const auto actual = sample(*window, world, at);
                    CHECK_EQ(actual.floor, expected.floor);
                    CHECK_EQ(actual.surface, expected.surface);
                    CHECK_EQ(actual.body, expected.body);
                    CHECK_EQ(actual.wet, expected.wet);
                }
            }
}

TEST(natural_basin_cache_payload_is_deterministic_and_round_trips) {
    BasinWorld world;
    const auto graph = buildHydrologyGraph(world.map);
    std::vector<std::uint8_t> payload, rebuilt;
    CHECK(encodeHydrologyGraphPayload(graph, payload));
    CHECK(encodeHydrologyGraphPayload(buildHydrologyGraph(world.map), rebuilt));
    CHECK_EQ(payload, rebuilt);
    HydrologyCacheHeader header;
    header.worldSeed = graph.worldSeed;
    header.sourceFingerprint = graph.sourceFingerprint;
    header.macroWidth = graph.macroWidth;
    header.macroHeight = graph.macroHeight;
    header.macroCellMetres = graph.macroCellMetres;
    header.nodeCount = static_cast<std::uint32_t>(graph.nodes.size());
    header.segmentCount = static_cast<std::uint32_t>(graph.segments.size());
    header.waterBodyCount = static_cast<std::uint32_t>(graph.waterBodies.size());
    header.spatialPageCount = static_cast<std::uint32_t>(graph.spatialPages.size());
    HydrologyGraph decoded;
    CHECK(decodeHydrologyGraphPayload(header, payload, decoded));
    CHECK_EQ(decoded.waterBodies.size(), graph.waterBodies.size());
    if (decoded.waterBodies.size() != graph.waterBodies.size()) return;
    for (std::size_t i = 0; i < graph.waterBodies.size(); ++i) {
        CHECK_EQ(decoded.waterBodies[i].basinStep, graph.waterBodies[i].basinStep);
        CHECK_EQ(decoded.waterBodies[i].basinSamples, graph.waterBodies[i].basinSamples);
        CHECK_EQ(decoded.waterBodies[i].level, graph.waterBodies[i].level);
    }
    CHECK(encodeHydrologyGraphPayload(decoded, rebuilt));
    CHECK_EQ(payload, rebuilt);
}

TEST(natural_basin_local_index_preserves_legacy_lake_shore_support) {
    BasinWorld world;
    world.map.terrainFoundation.reset();
    const auto graph = buildHydrologyGraph(world.map);
    const GraphCarver whole(graph, {position(20, 20), position(56, 56)}, 0);
    // The original macro footprint begins at 2160 m, on page 4. Its
    // interpolated shore already reaches page 3, starting at 1890 m.
    const GraphCarver shore(graph, {{Fixed::fromInt(1536), Fixed::fromInt(2048)},
                                   {Fixed::fromInt(2048), Fixed::fromInt(2560)}}, 0);
    for (int x = 1892; x < 2048; x += 4) {
        const core::WorldPos at{Fixed::fromInt(x), Fixed::fromInt(2430)};
        const auto expected = whole.carve(at, Fixed::fromInt(40), core::kZero);
        const auto actual = shore.carve(at, Fixed::fromInt(40), core::kZero);
        CHECK(expected.wet);
        CHECK_EQ(actual.floor, expected.floor);
        CHECK_EQ(actual.surface, expected.surface);
        CHECK_EQ(actual.wet, expected.wet);
        CHECK_EQ(actual.body, expected.body);
    }
}

namespace {
// The basin world with a shore standing only five metres over the lake, and
// a detail layer cutting thirty metres into all of it - the gullies every real
// hillside has, and deeper than the shore stands.
struct GulliedShore : BasinWorld {
    GulliedShore() {
        for (auto& h : foundation->heightDm[static_cast<std::size_t>(generation::TerrainStage::Slopes)])
            if (h == 900) h = 500;
    }
    CarvedSample at(const GraphCarver& carver, int x, int y) const {
        const core::WorldPos p{Fixed::fromInt(x), Fixed::fromInt(y)};
        return carver.carve(p, foundation->sample(p.x, p.y, generation::TerrainStage::Slopes),
                            Fixed::fromInt(-30));
    }
};
} // namespace

TEST(natural_basin_water_never_ends_over_lower_ground) {
    // A lake's water ends where the ground comes up through its level, and
    // nowhere else. It used to end on the edge of the flooded lattice as well,
    // over whatever the gullies had cut there: a wall of water standing thirty
    // metres over the hillside, which is a lake hanging in the air.
    GulliedShore world;
    const auto graph = buildHydrologyGraph(world.map);
    const auto& lake = graph.waterBodies.at(1);
    CHECK_EQ(lake.level, Fixed::fromInt(45));
    const GraphCarver carver(graph, {position(32, 32), position(54, 46)}, 0);
    std::size_t shore = 0;
    for (int y = 34 * 64; y <= 44 * 64; y += 4)
        for (int x = 32 * 64; x < 54 * 64; x += 4) {
            const auto here = world.at(carver, x, y);
            for (const auto& there : {world.at(carver, x + 4, y), world.at(carver, x, y + 4)}) {
                if (here.wet == there.wet) continue;
                const auto& wet = here.wet ? here : there;
                const auto& dry = here.wet ? there : here;
                ++shore;
                CHECK_EQ(wet.body, lake.id);
                CHECK(dry.floor >= wet.surface - Fixed::ratio(1, 2));
            }
        }
    CHECK(shore > 0);
    // And the gullies are still there, inside the lake and out on the hill.
    CHECK(world.at(carver, 40 * 64, 38 * 64).floor < Fixed::fromInt(0));
    CHECK(world.at(carver, 40 * 64, 45 * 64 + 32).floor < Fixed::fromInt(25));
}

TEST(natural_basin_shore_fill_agrees_across_page_windows) {
    GulliedShore world;
    const auto graph = buildHydrologyGraph(world.map);
    const GraphCarver whole(graph, {position(32, 32), position(56, 48)}, 0);
    const GraphCarver left(graph, {{Fixed::fromInt(2560), Fixed::fromInt(2048)},
                                  {Fixed::fromInt(3072), Fixed::fromInt(2560)}}, 8);
    const GraphCarver right(graph, {{Fixed::fromInt(3072), Fixed::fromInt(2048)},
                                   {Fixed::fromInt(3584), Fixed::fromInt(2560)}}, 8);
    for (int y = 34 * 64; y <= 43 * 64; y += 4)
        for (int dx = -8; dx <= 8; dx += 4) {
            const auto expected = world.at(whole, 3072 + dx, y);
            for (const auto* window : {&left, &right}) {
                const auto actual = world.at(*window, 3072 + dx, y);
                CHECK_EQ(actual.floor, expected.floor);
                CHECK_EQ(actual.surface, expected.surface);
                CHECK_EQ(actual.wet, expected.wet);
                CHECK_EQ(actual.body, expected.body);
            }
        }
}
