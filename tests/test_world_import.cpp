#include "framework.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/world_source/transfer.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_import.hpp"
#include "game/generation/world_layout.hpp"

#include <cmath>
#include <filesystem>
#include <random>

// An imported skeleton (world_import.hpp) is the ground of the region it is in.
namespace {
using namespace generation;
namespace ws = engine::world_source;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("campfire_import_" + name + "_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// A cone of an island, `peak` metres at the middle of the picture, down to
// the sea 40 km out. Written as a picture is read (LooseImages): grey 10 and
// darker is sea, land rises from a metre at the coast to 2000 m at white.
bool writeCone(const fs::path& file, double peak) {
    constexpr std::uint32_t n = 512;   // one region at 256 m
    ws::Image image{n, n, 1, 16, {}, {}};
    image.allocate();
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            const double r = std::hypot((x + 0.5) * 256.0 - 65536.0, (y + 0.5) * 256.0 - 65536.0);
            const double metres = r < 40000 ? peak * (1.0 - r / 40000.0) + 2.0 : -100.0;
            const double grey = metres <= 0 ? 4.0 : 10.0 + (metres - 1.0) / 1999.0 * 245.0;
            image.set(x, y, 0, std::uint16_t(std::lround(grey / 255.0 * 65535.0)));
        }
    return ws::writePng(file, image);
}

WorldLayout importedLayout(const fs::path& dir, double peak) {
    auto layout = emptyLayout(2, 1, 77);
    layout.latitude.fixed = true;
    layout.latitude.northDegrees = 50;
    ws::ImportTarget target;
    target.world = ws::WorldExtent{2.0 * kRegionMetres, double(kRegionMetres), 256, 32768};
    target.rect = std::array<double, 4>{0, 0, double(kRegionMetres), double(kRegionMetres)};
    target.mask = {*target.rect};
    ws::LooseImages images;
    images.height = dir / "cone.png";
    images.seaGrey = 10;
    images.highMetres = 2000;
    std::string why;
    if (!writeCone(images.height, peak) || !ws::importImages(images, dir / "source", target, &why))
        std::cerr << "import: " << why << "\n";
    layout.imported = ImportedSource::open(dir / "source");
    layout.at(0, 0).stage = RegionStage::Primary;
    return layout;
}

double foundationMetres(const WorldMapData& world, double x, double y) {
    return double(world.terrainFoundation->sample(core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y),
                                                 TerrainStage::Slopes).toDouble());
}
} // namespace

TEST(world_import_skeleton_is_the_ground_of_its_region) {
    TempDir dir("ground");
    const auto layout = importedLayout(dir.path, 1200);
    CHECK(layout.imported != nullptr);
    CHECK(importedIn(layout, 0, 0));
    CHECK(!importedIn(layout, 1, 0));
    ComposeReport report;
    const auto world = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.runs, 1);
    CHECK(world.terrainFoundation != nullptr);
    const double c = double(kRegionMetres) / 2;
    // The peak and the slope where the picture put them, within what the
    // weathering takes off a gentle cone.
    CHECK(std::abs(foundationMetres(world, c, c) - 1202) < 60);
    CHECK(std::abs(foundationMetres(world, c + 20000, c) - 602) < 40);
    // The coast is the skeleton's: land 30 km out, sea 50 km out and in the
    // region beside it.
    CHECK(!world.at({int((c + 30000) / kMetresPerCell), int(c / kMetresPerCell)}).sea);
    CHECK(world.at({int((c + 50000) / kMetresPerCell), int(c / kMetresPerCell)}).sea);
    CHECK(foundationMetres(world, c + 50000, c) < 0);
    CHECK(world.at({kCellsPerRegion + 128, 128}).sea);
}

TEST(world_import_run_is_made_again_only_when_its_skeleton_changes) {
    TempDir dir("again");
    auto layout = importedLayout(dir.path, 1200);
    ComposeReport first, second, third;
    generateLayoutWorld(layout, &first);
    // Opened again, nothing changed: the run is the cached one.
    layout.imported = ImportedSource::open(dir.path / "source");
    generateLayoutWorld(layout, &second);
    CHECK_EQ(second.cachedRuns, 1);
    // A taller cone: made again, and taller.
    layout = importedLayout(dir.path, 1600);
    const auto world = generateLayoutWorld(layout, &third);
    CHECK_EQ(third.cachedRuns, 0);
    const double c = double(kRegionMetres) / 2;
    CHECK(std::abs(foundationMetres(world, c, c) - 1602) < 80);
}

// An import is heights and masks: no drainage is baked by opening it, and the
// region it went into carries no river, no lake and no drainage until it is
// taken to those stages.
TEST(world_import_brings_heights_alone) {
    TempDir dir("heights");
    const auto layout = importedLayout(dir.path, 1200);
    CHECK(layout.imported != nullptr);
    CHECK(!layout.imported->drained());
    CHECK(!fs::exists(ImportedSource::bakedRootOf(dir.path / "source")));
    CHECK(!importDrained(layout));
    const auto world = generateLayoutWorld(layout);
    std::size_t land = 0, wet = 0, draining = 0;
    for (std::size_t i = 0; i < world.cells.size(); ++i) {
        const auto& c = world.cells[i];
        if (c.sea) continue;
        ++land;
        wet += c.river || world.lakeRegionField[i] >= 0;
        draining += c.drainOut >= 0 || world.flowDirectionField[i] >= 0;
    }
    CHECK(land > 0);
    CHECK_EQ(wet, std::size_t(0));
    CHECK_EQ(draining, std::size_t(0));
}

// The drainage is a stage of its own: asked for, the drained height is baked
// beside the source, a region taken to it is built on it, and its run is not
// the heights-alone one.
TEST(world_import_drainage_is_baked_when_asked_for) {
    TempDir dir("drain");
    auto layout = importedLayout(dir.path, 1200);
    const auto raw = layout.imported->ground(0, 0, false);
    // Not baked yet: asking for the drained ground gives the heights as drawn.
    CHECK_EQ(layout.imported->ground(0, 0, true)->key, raw->key);
    layout.at(0, 0).stage = RegionStage::Relief;
    CHECK(importDrained(layout));
    layout.imported = openImported(dir.path / "source", layout);
    CHECK(layout.imported->drained());
    CHECK(fs::exists(ImportedSource::bakedRootOf(dir.path / "source")));
    CHECK(layout.imported->ground(0, 0, true)->key != layout.imported->ground(0, 0, false)->key);
    CHECK_EQ(layout.imported->ground(0, 0, false)->key, raw->key);
    ComposeReport report;
    const auto world = generateLayoutWorld(layout, &report);
    CHECK_EQ(report.runs, 1);
    CHECK_EQ(report.cachedRuns, 0);
    // Drainage, and still no water.
    std::size_t rivers = 0;
    for (const auto& c : world.cells) rivers += !c.sea && c.river;
    CHECK_EQ(rivers, std::size_t(0));
}

