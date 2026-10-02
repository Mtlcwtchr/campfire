#include "framework.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_brush.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_layout.hpp"

#include <filesystem>

namespace {
using namespace generation;

WorldPreset preset() {
    WorldPreset p;
    p.name = "test";
    return p;
}

// Region (rx, ry) of two maps, cell for cell and foundation point for point.
bool sameRegion(const WorldMapData& a, const WorldMapData& b, std::int32_t rx, std::int32_t ry) {
    for (std::int32_t y = ry * kCellsPerRegion; y < (ry + 1) * kCellsPerRegion; ++y)
        for (std::int32_t x = rx * kCellsPerRegion; x < (rx + 1) * kCellsPerRegion; ++x) {
            const WorldCell& ca = a.at({x, y});
            const WorldCell& cb = b.at({x, y});
            if (ca.elevation != cb.elevation || ca.sea != cb.sea || ca.river != cb.river || ca.climate != cb.climate ||
                ca.temperature != cb.temperature || ca.moisture != cb.moisture)
                return false;
        }
    const auto& fa = *a.terrainFoundation;
    const auto& fb = *b.terrainFoundation;
    if (fa.step != fb.step) return false;
    const int per = int(kRegionMetres / fa.step);
    for (int j = ry * per; j < (ry + 1) * per; ++j)
        for (int i = rx * per; i < (rx + 1) * per; ++i)
            for (std::size_t s = 0; s < fa.heightDm.size(); ++s)
                if (fa.heightDm[s][std::size_t(j) * fa.columns + i] != fb.heightDm[s][std::size_t(j) * fb.columns + i])
                    return false;
    return true;
}
} // namespace

TEST(world_generations_round_trip_and_old_files_read_as_one_run) {
    auto layout = emptyLayout(2, 1, 5);
    generateAll(layout, preset());
    CHECK(isWholeWorld(layout));
    CHECK_EQ(layout.generations.size(), std::size_t(1));
    generateRegions(layout, {{1, 0}}, layout.at(0, 0).settings, 77, true);
    CHECK(!isWholeWorld(layout));
    CHECK(layout.latitude.fixed);                     // a run of its own is a place on a planet
    CHECK_EQ(layout.at(1, 0).source, 1);
    CHECK_EQ(layout.generations[1].w, 1);
    const auto file = std::filesystem::temp_directory_path() / "asr_world_generations.json";
    CHECK(saveWorldLayout(layout, file));
    const auto loaded = loadWorldLayout(file);
    CHECK(loaded.has_value());
    if (loaded) CHECK(*loaded == layout);
    std::filesystem::remove(file);

    // Made before generations: every generated region is the one run the
    // generator made, and the file names nothing more.
    auto old = emptyLayout(2, 2, 9);
    generateAll(old, preset());
    old.generations.clear();
    for (auto& r : old.regions) r.source = -1;
    CHECK(saveWorldLayout(old, file));
    const auto read = loadWorldLayout(file);
    CHECK(read.has_value());
    if (read) CHECK(isWholeWorld(*read));
    std::filesystem::remove(file);

    // Cleared, a region is taken out of every run that made it, and a run
    // nothing is made from any more is dropped.
    clearRegions(layout, {{1, 0}});
    CHECK_EQ(layout.generations.size(), std::size_t(1));
    CHECK(!layout.at(1, 0).generated);
    CHECK(!layout.generations[0].at(1, 0).generated);
}

TEST(world_that_grows_keeps_what_it_had_bit_for_bit) {
    auto layout = emptyLayout(1, 1, 4242);
    generateAll(layout, preset());
    const WorldMapData one = generateLayoutWorld(layout);

    // A region added beside it is empty: nothing is run for it, and the first
    // region is what it was.
    resizeLayout(layout, 2, 1);
    ComposeReport report;
    const WorldMapData two = generateLayoutWorld(layout, &report);
    CHECK(!report.wholeWorld);
    CHECK_EQ(report.runs, 1);
    CHECK_EQ(report.seaRegions, 1);
    CHECK(sameRegion(one, two, 0, 0));
    CHECK(two.at({400, 128}).sea);

    // Made on its own terms, the new region does not touch the old one either.
    generateRegions(layout, {{1, 0}}, layout.at(0, 0).settings, 99, true);
    const WorldMapData three = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.runs, 2);
    CHECK(sameRegion(one, three, 0, 0));
    double land = 0;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = kCellsPerRegion; x < 2 * kCellsPerRegion; ++x) land += three.at({x, y}).sea ? 0 : 1;
    CHECK(land > 1000);

    // And a coast painted across the border joins the two: land through the
    // seam, the seam closed over it, the rivers of that land worked out again.
    Brush paint;
    paint.tool = BrushTool::Paint;
    paint.value = 900;
    paint.strength = 1.0f;
    paint.hardness = 0.8f;
    paint.radius = legalRadius(LayerId::Continents, layout, 12000);
    const double border = double(kRegionMetres);
    for (double x = border - 40000; x <= border + 40000; x += 4000)
        dab(layout, LayerId::Continents, paint, x, border * 0.5, nullptr);
    const WorldMapData four = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.seams, 1);
    CHECK(report.landmasses >= 1);
    for (std::int32_t x = 250; x < 262; ++x) CHECK(!four.at({x, 128}).sea);
}

TEST(world_that_grows_west_and_north_keeps_its_ground_where_it_went) {
    auto layout = emptyLayout(1, 1, 4242);
    generateAll(layout, preset());
    const WorldMapData one = generateLayoutWorld(layout);
    // A region added on the west and one on the north: the old region is now
    // (1, 1), and it is the same ground - latitude and all.
    CHECK(reshapeLayout(layout, 1, 0, 1, 0));
    CHECK_EQ(layout.regionsX, 2);
    CHECK_EQ(layout.regionsY, 2);
    const WorldMapData grown = generateLayoutWorld(layout);
    bool same = true;
    for (std::int32_t y = 0; y < kCellsPerRegion && same; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion && same; ++x) {
            const WorldCell& a = one.at({x, y});
            const WorldCell& b = grown.at({x + kCellsPerRegion, y + kCellsPerRegion});
            same = a.elevation == b.elevation && a.sea == b.sea && a.river == b.river && a.climate == b.climate &&
                   a.temperature == b.temperature && a.moisture == b.moisture;
        }
    CHECK(same);
    CHECK(grown.at({40, 40}).sea);
    // And taken away again, it is where it was.
    CHECK(reshapeLayout(layout, -1, 0, -1, 0));
    const WorldMapData back = generateLayoutWorld(layout);
    CHECK(sameRegion(one, back, 0, 0));
}
