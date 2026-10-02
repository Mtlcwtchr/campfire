#include "framework.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_map_gen.hpp"

#include <cmath>
#include <filesystem>

namespace {
using namespace generation;

WorldMapParams quick(const WorldLayout& layout) {
    // The generator the editor runs, without the parts that cost minutes and
    // are not what is being checked here: the H64 foundation and the
    // hybrid landforms.
    WorldMapParams p = paramsFor(layout);
    p.hybridTerrain = false;
    p.stagedTerrain = false;
    return p;
}

double landShare(const WorldMapData& w, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    std::int64_t land = 0, all = 0;
    for (std::int32_t y = y0; y < y1; ++y)
        for (std::int32_t x = x0; x < x1; ++x) {
            land += w.at({x, y}).sea ? 0 : 1;
            ++all;
        }
    return all ? double(land) / double(all) : 0.0;
}
} // namespace

TEST(world_regions_are_powers_of_two_and_pages_fit_them_exactly) {
    CHECK_EQ(kMetresPerCell, 512);
    CHECK_EQ(kRegionMetres, 131072);          // the binary 128 km
    CHECK_EQ(kRegionMetres % 512, 0);
    CHECK_EQ(kRegionMetres / 512, 256);       // pages of 512 m to a region side
    for (const auto& size : kWorldSizes) CHECK_EQ(size.cells & (size.cells - 1), 0);
    const WorldLayout layout = emptyLayout(3, 2, 7);
    CHECK_EQ(layout.widthCells(), 768);
    CHECK_EQ(layout.heightCells(), 512);
    CHECK_EQ(layout.regions.size(), std::size_t(6));
    CHECK(!layout.anyGenerated());
}

TEST(world_region_mix_is_one_region_inside_and_a_smooth_blend_at_borders) {
    WorldLayout layout = emptyLayout(2, 2, 11);
    layout.blendMetres = 12288;                // 24 cells either side
    // Deep inside a region it is that region alone.
    const RegionMix inside = regionMixAt(layout, 64, 64);
    CHECK_EQ(inside.count, 1);
    CHECK_EQ(inside.region[0], 0);
    CHECK(std::abs(inside.weight[0] - 1.0f) < 1e-6f);
    // Everywhere the weights sum to one, and they never jump between two
    // neighbouring cells: a border band, not a seam.
    double worstStep = 0;
    for (std::int32_t y = 0; y < 512; y += 3) {
        std::array<float, 4> previous{};
        for (std::int32_t x = 0; x < 512; ++x) {
            const RegionMix mix = regionMixAt(layout, x, y);
            float total = 0;
            std::array<float, 4> byRegion{};
            for (int k = 0; k < mix.count; ++k) {
                total += mix.weight[std::size_t(k)];
                byRegion[std::size_t(mix.region[std::size_t(k)])] += mix.weight[std::size_t(k)];
            }
            CHECK(std::abs(total - 1.0f) < 1e-4f);
            if (x > 0)
                for (std::size_t r = 0; r < 4; ++r)
                    worstStep = std::max(worstStep, double(std::abs(byRegion[r] - previous[r])));
            previous = byRegion;
        }
    }
    CHECK(worstStep < 0.12);
    // Across a border there is a band where both regions have a share.
    bool shared = false;
    for (std::int32_t x = 230; x < 282 && !shared; ++x) shared = regionMixAt(layout, x, 64).count >= 2;
    CHECK(shared);
}

TEST(world_layout_params_and_file_round_trip) {
    WorldLayout layout = emptyLayout(2, 1, 99);
    WorldPreset dry;
    dry.name = "dry";
    dry.params.seaPercent = 40;
    dry.params.erosionPasses = 5;
    dry.params.rainfallPercent = 60;
    layout.at(1, 0).generated = true;
    layout.at(1, 0).settings = regionSettingsFrom(dry, 1234);
    const WorldMapParams p = paramsFor(layout);
    CHECK_EQ(p.width, 512);
    CHECK_EQ(p.height, 256);
    CHECK_EQ(p.seed, 99u);
    CHECK_EQ(p.seaPercent, 40);
    CHECK_EQ(p.erosionPasses, 5);
    CHECK(p.layout != nullptr);

    const auto file = std::filesystem::temp_directory_path() / "asr_world_layout_test.json";
    CHECK(saveWorldLayout(layout, file));
    const auto loaded = loadWorldLayout(file);
    CHECK(loaded.has_value());
    if (loaded) CHECK(*loaded == layout);
    std::filesystem::remove(file);

    // Resizing keeps what still fits where it was.
    resizeLayout(layout, 1, 2);
    CHECK_EQ(layout.regionsX, 1);
    CHECK_EQ(layout.regionsY, 2);
    CHECK(!layout.at(0, 0).generated);
}

TEST(world_empty_regions_are_open_sea_and_generated_ones_have_land) {
    // Two regions side by side: the left one generated as a mostly-land
    // country, the right one left empty.
    WorldLayout layout = emptyLayout(2, 1, 5);
    WorldPreset land;
    land.name = "land";
    land.params.seaPercent = 35;
    layout.at(0, 0).generated = true;
    layout.at(0, 0).settings = regionSettingsFrom(land, 77);
    const WorldMapData world = generateWorldMap(quick(layout));
    CHECK_EQ(world.width, 512);
    // Away from the border band and the map's own edge margin.
    const double generated = landShare(world, 40, 40, 200, 216);
    const double empty = landShare(world, 300, 40, 470, 216);
    CHECK(generated > 0.25);
    CHECK_EQ(empty, 0.0);
}

TEST(world_regions_keep_their_own_share_of_sea) {
    // One region asked for a little sea, the other for a lot: each gets its
    // own share, measured away from the border band.
    WorldLayout layout = emptyLayout(2, 1, 9);
    WorldPreset wet, dry;
    wet.name = "wet"; wet.params.seaPercent = 80;
    dry.name = "dry"; dry.params.seaPercent = 30;
    layout.at(0, 0).generated = true;
    layout.at(0, 0).settings = regionSettingsFrom(dry, 101);
    layout.at(1, 0).generated = true;
    layout.at(1, 0).settings = regionSettingsFrom(wet, 202);
    const WorldMapData world = generateWorldMap(quick(layout));
    const double left = landShare(world, 40, 40, 216, 216);
    const double right = landShare(world, 296, 40, 472, 216);
    CHECK(left > right + 0.2);
}

TEST(world_with_every_region_empty_is_all_sea_and_still_builds) {
    const WorldLayout layout = emptyLayout(1, 1, 3);
    const WorldMapData world = generateWorldMap(quick(layout));
    CHECK_EQ(landShare(world, 0, 0, world.width, world.height), 0.0);
}

TEST(world_layout_builds_through_the_foundation_and_landforms) {
    // The whole generator the editor runs, H64 foundation and hybrid landforms
    // included: a generated region beside an empty one, each with its own
    // erosion, and the empty one stays sea all the way through.
    WorldLayout layout = emptyLayout(2, 1, 21);
    WorldPreset old;
    old.name = "old"; old.params.seaPercent = 40; old.params.erosionPasses = 6;
    layout.at(0, 0).generated = true;
    layout.at(0, 0).settings = regionSettingsFrom(old, 31);
    WorldMapParams p = paramsFor(layout);
    p.terrainReferenceRoot = "/nonexistent";   // analytic landforms, no DEM library
    const WorldMapData world = generateWorldMap(p);
    CHECK(world.terrainFoundation != nullptr);
    CHECK(landShare(world, 40, 40, 200, 216) > 0.25);
    CHECK_EQ(landShare(world, 300, 40, 470, 216), 0.0);
}

