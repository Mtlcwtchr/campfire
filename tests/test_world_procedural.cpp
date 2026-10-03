#include "framework.hpp"
#include "engine/world_source/transfer.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_import.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_procedural.hpp"

#include <cmath>
#include <filesystem>
#include <random>

// The procedural way into the world's source (world_procedural.hpp): the
// generator makes the maps an import would bring, a layer at a time.
namespace {
using namespace generation;
namespace ws = engine::world_source;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("campfire_procedural_" + name + "_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

WorldLayout twoRegions() {
    auto layout = emptyLayout(2, 1, 77);
    layout.latitude.fixed = true;
    layout.latitude.northDegrees = 50;
    return layout;
}

ProceduralHeights land() {
    ProceduralHeights dials;
    dials.settings.seaPercent = 45;   // land enough to find
    dials.seed = 4242;
    return dials;
}

ws::WorldExtent extentOf(const WorldLayout& layout) {
    return {double(layout.widthMetres()), double(layout.heightMetres()), 256, 32768};
}

ws::ImportTarget targetOf(const WorldLayout& layout, const SourceRasters& r) {
    ws::ImportTarget target;
    target.world = extentOf(layout);
    target.rect = r.rect();
    for (const auto& [rx, ry] : r.regions) {
        const double m = double(kRegionMetres);
        target.mask.push_back({rx * m, ry * m, (rx + 1) * m, (ry + 1) * m});
    }
    return target;
}

ws::GridRasters gridsOf(SourceRasters r) {
    ws::GridRasters grids;
    if (!r.height.empty()) {
        ws::Image image{std::uint32_t(r.samplesX), std::uint32_t(r.samplesY), 1, 16, {}, std::move(r.height)};
        grids.height = std::move(image);
    }
    if (!r.control.empty()) {
        ws::Image image{std::uint32_t(r.samplesX), std::uint32_t(r.samplesY), 4, 8, std::move(r.control), {}};
        grids.control = std::move(image);
    }
    return grids;
}
} // namespace

// Phase 1 makes heights on the source's grid for the regions asked for, and
// only over their rectangle; the same dials make the same samples.
TEST(world_procedural_heights_are_the_generators_ground_on_the_source_grid) {
    const auto layout = twoRegions();
    std::string why;
    const auto a = generateSourceHeights(layout, {{1, 0}}, land(), 256, &why);
    CHECK(a.has_value());
    if (!a) { std::cerr << why << "\n"; return; }
    CHECK_EQ(a->regionX, 1);
    CHECK_EQ(a->regionsW, 1);
    CHECK_EQ(a->samplesX, std::int64_t(kRegionMetres / 256));
    CHECK_EQ(a->samplesY, std::int64_t(kRegionMetres / 256));
    CHECK_EQ(a->height.size(), std::size_t(a->samplesX * a->samplesY));
    CHECK(a->landSamples > 0);
    CHECK(a->landSamples < a->samplesX * a->samplesY);
    CHECK(a->highestMetres > 50);
    const auto b = generateSourceHeights(layout, {{1, 0}}, land(), 256, &why);
    CHECK(b.has_value());
    CHECK(b && b->height == a->height);
    // Another seed is other ground.
    auto other = land();
    other.seed = 99;
    const auto c = generateSourceHeights(layout, {{1, 0}}, other, 256, &why);
    CHECK(c && c->height != a->height);
}

// Laid into the source, generated heights are imported ground like any
// other: the region holds them, is built from them, and nothing else is.
TEST(world_procedural_heights_become_imported_ground) {
    TempDir dir("heights");
    auto layout = twoRegions();
    std::string why;
    auto rasters = generateSourceHeights(layout, {{0, 0}}, land(), 256, &why);
    CHECK(rasters.has_value());
    if (!rasters) return;
    const auto target = targetOf(layout, *rasters);
    const auto report = ws::importGrids(gridsOf(std::move(*rasters)), dir.path / "source", target, &why);
    CHECK(report.has_value());
    if (!report) { std::cerr << why << "\n"; return; }
    CHECK(report->chunksWritten > 0);
    layout.imported = ImportedSource::open(dir.path / "source");
    CHECK(layout.imported != nullptr);
    CHECK(importedIn(layout, 0, 0));
    CHECK(!importedIn(layout, 1, 0));
    layout.at(0, 0).stage = RegionStage::Primary;
    ComposeReport compose;
    const auto world = generateLayoutWorld(layout, &compose);
    CHECK_EQ(compose.runs, 1);
    std::size_t landCells = 0, wet = 0;
    for (std::int32_t y = 0; y < kCellsPerRegion; ++y)
        for (std::int32_t x = 0; x < kCellsPerRegion; ++x) {
            const auto& c = world.at({x, y});
            landCells += !c.sea;
            wet += !c.sea && c.river;
        }
    CHECK(landCells > 0);
    // Heights alone: no water until it is asked for.
    CHECK_EQ(wet, std::size_t(0));
    // The region beside it took nothing.
    CHECK(world.at({kCellsPerRegion + 128, 128}).sea);
    // Written again unchanged, nothing is rewritten.
    auto again = generateSourceHeights(twoRegions(), {{0, 0}}, land(), 256, &why);
    CHECK(again.has_value());
    if (!again) return;
    const auto second = ws::importGrids(gridsOf(std::move(*again)), dir.path / "source", target, &why);
    CHECK(second.has_value());
    CHECK(second && second->chunksWritten == 0);
}

// Phase 2 works the control maps out from the heights the source holds:
// at rest over the sea, mountains where it is high, and only for regions
// that hold heights.
TEST(world_procedural_controls_follow_the_heights) {
    TempDir dir("controls");
    auto layout = twoRegions();
    std::string why;
    // Nothing to work from yet.
    CHECK(!generateSourceControls(dir.path / "source", layout, {}, ProceduralControls{}, &why).has_value());
    auto rasters = generateSourceHeights(layout, {{0, 0}}, land(), 256, &why);
    CHECK(rasters.has_value());
    if (!rasters) return;
    const std::vector<std::uint16_t> heights = rasters->height;
    const auto target = targetOf(layout, *rasters);
    CHECK(ws::importGrids(gridsOf(std::move(*rasters)), dir.path / "source", target, &why).has_value());

    ProceduralControls dials;
    dials.seed = 7;
    const auto controls = generateSourceControls(dir.path / "source", layout, {}, dials, &why);
    CHECK(controls.has_value());
    if (!controls) { std::cerr << why << "\n"; return; }
    // Only the region with heights.
    CHECK_EQ(controls->regions.size(), std::size_t(1));
    CHECK_EQ(controls->regionX, 0);
    CHECK_EQ(controls->control.size(), heights.size() * 4);
    const auto schema = ws::canonicalSchema(extentOf(layout));
    const auto& heightDesc = schema.rasters[0];
    const auto& controlDesc = schema.rasters[1];
    const std::uint16_t openSea = heightDesc.defaultStored(0);
    double highMountain = 0, lowMountain = 0;
    std::size_t high = 0, low = 0, restAtSea = 0, sea = 0;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        const std::uint8_t* px = &controls->control[i * 4];
        if (heights[i] == openSea) {
            ++sea;
            bool rest = true;
            for (std::size_t c = 0; c < 4; ++c) rest = rest && px[c] == controlDesc.defaultStored(c);
            restAtSea += rest;
            continue;
        }
        const double metres = heightDesc.decode(0, heights[i]);
        if (metres > 800) { highMountain += px[2]; ++high; }
        else if (metres < 100) { lowMountain += px[2]; ++low; }
    }
    CHECK(sea > 0);
    CHECK_EQ(restAtSea, sea);
    if (high > 0 && low > 0) CHECK(highMountain / double(high) > lowMountain / double(low) + 40);
    // Laid in over the heights, they leave the heights as they were.
    const auto ctarget = targetOf(layout, *controls);
    auto grids = gridsOf(*controls);
    const auto report = ws::importGrids(std::move(grids), dir.path / "source", ctarget, &why);
    CHECK(report.has_value());
    if (!report) return;
    for (const auto& [key, layers] : report->changed) CHECK(!layers.count("height"));
    const auto opened = ImportedSource::open(dir.path / "source");
    CHECK(opened != nullptr);
    CHECK(opened && !opened->ground(0, 0)->moisture.empty());
}

// A grid that is not one pixel a sample of its rectangle is refused, not
// stretched.
TEST(world_procedural_grids_must_match_their_rectangle) {
    TempDir dir("mismatch");
    const auto layout = twoRegions();
    ws::ImportTarget target;
    target.world = extentOf(layout);
    target.rect = std::array<double, 4>{0, 0, double(kRegionMetres), double(kRegionMetres)};
    ws::GridRasters grids;
    ws::Image image{100, 100, 1, 16, {}, {}};
    image.allocate();
    grids.height = std::move(image);
    std::string why;
    CHECK(!ws::importGrids(std::move(grids), dir.path / "source", target, &why).has_value());
    CHECK(!why.empty());
}

