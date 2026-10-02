#include "framework.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_brush.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_sketch.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/hydrology_cache.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>

// The authoring pipeline (doc/authoring_pipeline_2026-09-30.md): a region made
// by hand is worked out only as far as it has been decided.
namespace {
using namespace generation;

// An island painted into region (rx, 0) of a world two regions across, as the
// editor paints one: the region made a sketch by the first land put into it.
void paintIsland(WorldLayout& layout, std::int32_t rx, double lean = 0.3) {
    beginSketch(layout, rx, 0);
    Brush paint;
    paint.tool = BrushTool::Paint;
    paint.value = 900;
    paint.strength = 1.0f;
    paint.hardness = 0.7f;
    paint.radius = legalRadius(LayerId::Continents, layout, 22000);
    const double cx = (rx + 0.5) * double(kRegionMetres), cy = 0.5 * double(kRegionMetres);
    for (double d = -26000; d <= 26000; d += 4000) dab(layout, LayerId::Continents, paint, cx + d, cy + d * lean, nullptr);
}

WorldLayout island(std::uint64_t seed) {
    auto layout = emptyLayout(2, 1, seed);
    layout.latitude.fixed = true;
    layout.latitude.northDegrees = 50;
    paintIsland(layout, 0);
    return layout;
}

struct Census {
    std::size_t land = 0, coast = 0, rivers = 0, lakes = 0, notAlpine = 0;
    std::int32_t highest = 0;   // decimetres, the foundation's highest point in the region
};
Census census(const WorldMapData& m, std::int32_t rx) {
    Census c;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = rx * kCellsPerRegion; x < (rx + 1) * kCellsPerRegion; ++x) {
            const WorldCell& cell = m.at({x, y});
            const std::size_t i = std::size_t(y) * std::size_t(m.width) + std::size_t(x);
            if (cell.river) ++c.rivers;
            if (m.lakeRegionField[i] >= 0) ++c.lakes;
            if (cell.sea) continue;
            ++c.land;
            if (cell.climate != Climate::Alpine) ++c.notAlpine;
            for (int dir : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour({x, y}, dir);
                if (m.inBounds(n) && m.at(n).sea) { ++c.coast; break; }
            }
        }
    if (const auto* f = m.terrainFoundation.get()) {
        const int per = int(kRegionMetres / f->step);
        for (int j = 0; j < std::min(per, f->rows); ++j)
            for (int i = rx * per; i < std::min((rx + 1) * per, f->columns); ++i)
                c.highest = std::max(c.highest, f->heightDm[4][std::size_t(j) * std::size_t(f->columns) + std::size_t(i)]);
    }
    return c;
}
} // namespace

TEST(world_stage_sketch_is_drawn_and_never_computed) {
    auto layout = island(9101);
    CHECK(layout.at(0, 0).stage == RegionStage::Sketch);
    CHECK(authored(layout, 0, 0));
    CHECK(!authored(layout, 1, 0));   // nothing painted there: still Full, open sea
    ComposeReport report;
    const auto world = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.runs, 0);
    CHECK_EQ(report.sketchRegions, 1);
    CHECK_EQ(census(world, 0).land, std::size_t(0));

    // Drawn instead: a top where the paint is land, walls along its shore,
    // all of it inside the sketched region.
    const auto mesh = sketchMesh(layout);
    CHECK(!mesh.empty());
    CHECK_EQ(mesh.sketchRegions, std::size_t(1));
    std::size_t tops = 0, walls = 0;
    for (const auto& v : mesh.vertices) {
        CHECK(v.x >= 0.0f && v.x <= float(kRegionMetres) + 4096.0f);
        CHECK(v.y >= 0.0f && v.y <= float(kRegionMetres) + 4096.0f);
        if (v.top > 0.5f && v.value > mesh.shore) ++tops;
        if (v.top < 0.5f) ++walls;
    }
    CHECK(tops > 100);
    CHECK(walls > 20);

    // Pinned, it is not a sketch any more.
    CHECK_EQ(raiseStage(layout, {}, RegionStage::Primary), 1);
    CHECK(sketchMesh(layout).empty());
    // Never down: asking for a sketch again moves nothing.
    CHECK_EQ(raiseStage(layout, {}, RegionStage::Sketch), 0);
}

TEST(world_stage_primary_is_bare_ground_with_a_ragged_coast) {
    auto layout = island(9102);
    raiseStage(layout, {}, RegionStage::Primary);
    ComposeReport report;
    const auto primary = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.runs, 1);
    const Census p = census(primary, 0);
    CHECK(p.land > 1000);
    // Nothing past the stage: no water, no climate of its own, no peoples,
    // and no mountains the person did not paint.
    CHECK_EQ(p.rivers, std::size_t(0));
    CHECK_EQ(p.lakes, std::size_t(0));
    CHECK_EQ(p.notAlpine, std::size_t(0));
    CHECK(primary.sites.empty());
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion; ++x)
            CHECK_EQ(primary.upliftField[std::size_t(y) * std::size_t(primary.width) + std::size_t(x)], 0);
    CHECK(primary.terrainFoundation != nullptr);

    // The coast is the paint's, torn: near the painted shore the land goes
    // its own way - bays into the paint, headlands and islets out of it -
    // and deep inside the paint or far out at sea it never does.
    const auto painted = layerCells(layout.layer(LayerId::Continents), primary.width, primary.height);
    std::size_t nearShore = 0, torn = 0, farWrong = 0;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(primary.width) + std::size_t(x);
            const double paint = painted.over(i, 0.0);
            const bool land = !primary.cells[i].sea;
            if (std::abs(paint - 512.0) < 200.0) {
                ++nearShore;
                torn += land != (paint > 512.0);
            } else if (paint < 60.0 && land) {
                // Far: further from any painted land than the coast is ever
                // moved (world_map_gen.cpp, coastWarp: a third of the bays'
                // width and half of the inlets').
                constexpr std::int32_t reach = 20;
                bool paintedNear = false;
                for (std::int32_t dy = -reach; dy <= reach && !paintedNear; ++dy)
                    for (std::int32_t dx = -reach; dx <= reach && !paintedNear; ++dx) {
                        const std::int32_t nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= primary.width || ny >= primary.height) continue;
                        paintedNear = painted.over(std::size_t(ny) * std::size_t(primary.width) + std::size_t(nx), 0.0) > 512.0;
                    }
                farWrong += paintedNear ? 0 : 1;
            }
        }
    std::cout << "near the painted shore " << nearShore << " cells, " << torn << " torn from it; " << farWrong
              << " land cells far out at sea\n";
    CHECK(torn * 10 > nearShore);
    CHECK_EQ(farWrong, std::size_t(0));
}

TEST(world_stage_relief_grows_the_painted_range_and_keeps_no_water) {
    auto layout = island(9103);
    raiseStage(layout, {}, RegionStage::Primary);
    const Census before = census(generateLayoutWorld(layout), 0);
    // A range painted down the island: the relief stage grows it.
    Brush range;
    range.tool = BrushTool::Paint;
    range.value = 800;
    range.strength = 1.0f;
    range.hardness = 0.6f;
    range.radius = legalRadius(LayerId::Ranges, layout, 6000);
    const double c = 0.5 * double(kRegionMetres);
    for (double d = -16000; d <= 16000; d += 2000) dab(layout, LayerId::Ranges, range, c + d, c + d * 0.3, nullptr);
    CHECK_EQ(raiseStage(layout, {{0, 0}}, RegionStage::Relief), 1);
    ComposeReport report;
    const auto relief = generateLayoutWorld(layout, &report);
    const Census after = census(relief, 0);
    std::cout << "highest, dm: pinned " << before.highest << ", relief " << after.highest << "\n";
    CHECK(after.highest > before.highest + 1000);   // a hundred metres more, at least
    CHECK_EQ(after.rivers, std::size_t(0));
    CHECK_EQ(after.lakes, std::size_t(0));
    CHECK(after.notAlpine > 0);       // its climate is worked out now

    // And the water is the last stage: rivers run on the same island.
    CHECK_EQ(raiseStage(layout, {{0, 0}}, RegionStage::Water), 1);
    const Census water = census(generateLayoutWorld(layout), 0);
    std::cout << "rivers with water: " << water.rivers << "\n";
    CHECK(water.rivers > 0);
}

TEST(world_runs_whose_inputs_did_not_change_are_taken_as_they_were) {
    auto layout = emptyLayout(2, 1, 9104);
    layout.latitude.fixed = true;
    layout.latitude.northDegrees = 40;
    paintIsland(layout, 0);
    paintIsland(layout, 1, -0.4);
    raiseStage(layout, {}, RegionStage::Primary);
    ComposeReport first;
    const auto a = generateLayoutWorld(layout, &first);
    CHECK_EQ(first.runs, 2);
    ComposeReport again;
    const auto b = generateLayoutWorld(layout, &again);
    CHECK_EQ(again.cachedRuns, 2);
    for (std::int32_t y = 0; y < a.height; ++y)
        for (std::int32_t x = 0; x < a.width; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(a.width) + std::size_t(x);
            if (a.cells[i].elevation != b.cells[i].elevation || a.cells[i].sea != b.cells[i].sea) {
                CHECK(false);
                y = a.height;
                break;
            }
        }

    // A stroke in the second region: only its run is made again.
    Brush more;
    more.tool = BrushTool::Paint;
    more.value = 900;
    more.strength = 1.0f;
    more.hardness = 0.7f;
    more.radius = legalRadius(LayerId::Continents, layout, 12000);
    dab(layout, LayerId::Continents, more, 1.8 * double(kRegionMetres), 0.8 * double(kRegionMetres), nullptr);
    ComposeReport edited;
    const auto c = generateLayoutWorld(layout, &edited);
    CHECK_EQ(edited.runs, 2);
    CHECK_EQ(edited.cachedRuns, 1);
    // The first region's own cells, away from the border band, are the same.
    std::size_t differ = 0;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion - 64; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(a.width) + std::size_t(x);
            differ += a.cells[i].elevation != c.cells[i].elevation || a.cells[i].sea != c.cells[i].sea;
        }
    CHECK_EQ(differ, std::size_t(0));
}

TEST(world_stages_round_trip_and_old_files_read_as_full) {
    auto layout = island(9105);
    paintIsland(layout, 1);
    raiseStage(layout, {{1, 0}}, RegionStage::Relief);
    const auto file = std::filesystem::temp_directory_path() / "asr_world_stages.json";
    CHECK(saveWorldLayout(layout, file));
    const auto loaded = loadWorldLayout(file);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK(*loaded == layout);
        CHECK(loaded->at(0, 0).stage == RegionStage::Sketch);
        CHECK(loaded->at(1, 0).stage == RegionStage::Relief);
    }
    std::filesystem::remove(file);
    CHECK(stageNamed("water").value_or(RegionStage::Full) == RegionStage::Water);
    CHECK(!stageNamed("nonsense").has_value());
    // A region painted under the old rule is not made a sketch by more paint.
    auto old = emptyLayout(1, 1, 3);
    Brush paint;
    paint.tool = BrushTool::Paint;
    paint.value = 900;
    paint.strength = 1.0f;
    paint.radius = legalRadius(LayerId::Continents, old, 10000);
    dab(old, LayerId::Continents, paint, 60000, 60000, nullptr);
    beginSketch(old, 0, 0);
    CHECK(old.at(0, 0).stage == RegionStage::Full);
}


TEST(world_water_of_an_island_redrawn_is_fitted_again_and_nothing_else_is) {
    // Two islands, one in each region, with their water.
    auto layout = island(9107);
    paintIsland(layout, 1, -0.2);
    raiseStage(layout, {}, RegionStage::Water);
    const auto before = generateLayoutWorld(layout);
    world::streaming::forgetHydrologyFits();
    (void)world::streaming::buildHydrologyGraph(before);
    const auto first = world::streaming::lastHydrologyFits();
    CHECK(first.groups >= 2);
    CHECK_EQ(first.fitted, first.groups);

    // The second one redrawn: only its water is fitted again...
    Brush paint;
    paint.tool = BrushTool::Paint;
    paint.value = 900;
    paint.strength = 1.0f;
    paint.hardness = 0.7f;
    paint.radius = legalRadius(LayerId::Continents, layout, 16000);
    dab(layout, LayerId::Continents, paint, 1.62 * double(kRegionMetres), 0.44 * double(kRegionMetres), nullptr);
    const auto after = generateLayoutWorld(layout);
    const auto warm = world::streaming::buildHydrologyGraph(after);
    const auto second = world::streaming::lastHydrologyFits();
    std::cout << "water groups " << first.groups << " then " << second.groups << ", fitted again " << second.fitted
              << "; streams from " << int(first.streamFlow) << "/" << int(second.streamFlow) << ", widest "
              << int(first.largestFlow) << "/" << int(second.largestFlow) << "\n";
    CHECK(second.fitted > 0);
    // The first island's water is taken back as it was - unless the redrawn
    // one moved what the whole map calls a stream, which widens every river.
    if (first.streamFlow == second.streamFlow && first.largestFlow == second.largestFlow)
        CHECK(second.fitted < second.groups);

    // ... and the graph is the one fitting everything gives, byte for byte.
    world::streaming::forgetHydrologyFits();
    const auto cold = world::streaming::buildHydrologyGraph(after);
    CHECK_EQ(world::streaming::lastHydrologyFits().fitted, second.groups);
    std::vector<std::uint8_t> a, b;
    CHECK(bool(world::streaming::encodeHydrologyGraphPayload(warm, a)));
    CHECK(bool(world::streaming::encodeHydrologyGraphPayload(cold, b)));
    CHECK(a == b);
}

namespace {
// Water painted on the water layer (WaterPaint): a disc, or a line of dabs.
void paintWater(WorldLayout& layout, float kind, double x0, double y0, double x1, double y1, double radius) {
    Brush water;
    water.tool = BrushTool::Paint;
    water.value = kind;
    water.strength = 1.0f;
    water.hardness = 0.9f;
    water.radius = legalRadius(LayerId::Water, layout, radius);
    const double length = std::hypot(x1 - x0, y1 - y0);
    const int steps = std::max(1, int(length / 400.0));
    for (int s = 0; s <= steps; ++s) {
        const double t = double(s) / steps;
        dab(layout, LayerId::Water, water, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, nullptr);
    }
}
// Cells of region 0 whose water paint is `kind`, and how many of them are land
// cells with a lake or a river on them.
struct Painted { std::size_t cells = 0, lakes = 0, rivers = 0; };
Painted underPaint(const WorldMapData& m, WaterPaint kind) {
    Painted p;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion; ++x) {
            const std::size_t i = std::size_t(y) * std::size_t(m.width) + std::size_t(x);
            if (static_cast<WaterPaint>(m.waterPaintField[i]) != kind || m.cells[i].sea) continue;
            ++p.cells;
            p.lakes += m.lakeRegionField[i] >= 0;
            p.rivers += m.cells[i].river;
        }
    return p;
}
} // namespace

TEST(world_water_paint_puts_a_lake_a_river_and_dry_ground_where_they_were_painted) {
    auto layout = island(9110);
    raiseStage(layout, {}, RegionStage::Water);
    const double c = 0.5 * double(kRegionMetres);
    // A lake in the middle of the island, a river course from its east end
    // towards the coast, and the west end painted dry.
    paintWater(layout, 1000.0f, c - 2000, c - 600, c - 2000, c - 600, 2500);
    paintWater(layout, 500.0f, c + 4000, c + 1200, c + 22000, c + 6600, 600);
    paintWater(layout, 0.0f, c - 26000, c - 7800, c - 12000, c - 3600, 6000);
    const auto m = generateLayoutWorld(layout);
    const auto lake = underPaint(m, WaterPaint::Lake);
    const auto course = underPaint(m, WaterPaint::Course);
    const auto dry = underPaint(m, WaterPaint::Dry);
    std::cout << "painted lake " << lake.lakes << "/" << lake.cells << " under water; course " << course.rivers << "/"
              << course.cells << " river; dry " << dry.lakes << " lake + " << dry.rivers << " river of " << dry.cells
              << "\n";
    CHECK(lake.cells > 10);
    CHECK(lake.lakes * 2 > lake.cells);          // the basin holds a lake under most of the paint
    CHECK(course.cells > 10);
    CHECK(course.rivers * 2 > course.cells);     // and the course carries a river along most of it
    CHECK(dry.cells > 50);
    CHECK_EQ(dry.lakes + dry.rivers, std::size_t(0));

    // The drainage graph keeps the same: water on the course, none on the dry ground.
    const auto graph = world::streaming::buildHydrologyGraph(m);
    std::size_t courseCells = 0, dryCells = 0;
    for (const auto& segment : graph.segments)
        for (const std::int32_t cell : segment.macroCells) {
            const auto kind = static_cast<WaterPaint>(m.waterPaintField[std::size_t(cell)]);
            courseCells += kind == WaterPaint::Course;
            dryCells += kind == WaterPaint::Dry;
        }
    CHECK(courseCells > 0);
    CHECK_EQ(dryCells, std::size_t(0));
}

TEST(world_water_dials_make_more_or_fewer_rivers_and_lakes) {
    auto layout = island(9111);
    raiseStage(layout, {}, RegionStage::Water);
    const Census plain = census(generateLayoutWorld(layout), 0);
    layout.authoring.rivers = 2.5f;
    const auto wetter = generateLayoutWorld(layout);
    const Census more = census(wetter, 0);
    layout.authoring.rivers = 0.4f;
    layout.authoring.lakes = 0.0f;
    const Census fewer = census(generateLayoutWorld(layout), 0);
    std::cout << "rivers: plain " << plain.rivers << ", more " << more.rivers << ", fewer " << fewer.rivers
              << "; lakes plain " << plain.lakes << ", none " << fewer.lakes << "\n";
    CHECK(more.rivers > plain.rivers);
    CHECK(fewer.rivers < plain.rivers);
    CHECK_EQ(fewer.lakes, std::size_t(0));
    CHECK(wetter.riverShare > 2.0f);
    // The dials round-trip with the layout.
    const auto file = std::filesystem::temp_directory_path() / "asr_world_water_dials.json";
    CHECK(saveWorldLayout(layout, file));
    const auto loaded = loadWorldLayout(file);
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK_EQ(loaded->authoring.lakes, 0.0f);
        CHECK(std::abs(loaded->authoring.rivers - 0.4f) < 1e-4f);
        CHECK(*loaded == layout);
    }
    std::filesystem::remove(file);
}

