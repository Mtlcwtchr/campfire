#include "framework.hpp"
#include "game/generation/world_scale_policy.hpp"
#include "game/world/terrain_source.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <array>
#include <vector>

namespace {
using namespace world::terrain;
using world::streaming::TileKey;

generation::WorldDescriptor describe(std::int64_t side, std::uint64_t seed = 42,
                                     std::uint32_t overviewLimit = 128) {
    generation::WorldScalePolicy policy;
    policy.overviewSideLimit = overviewLimit;
    return policy.describe(generation::WorldDomain(side, side), seed);
}

// Every page a square view around a point wants, at one level.
std::vector<TileKey> around(const ScaleTerrainSource& source, double x, double y,
                            double reach, std::uint8_t level) {
    return source.keysOverlapping(x - reach, y - reach, x + reach, y + reach, level);
}

// The centre of the overview cell with the highest floor, and the one with the
// lowest ceiling. Picking a fixed fraction of the world instead makes a test
// about which seed happened to put a coast there.
std::array<double, 2> cellCentre(const ScaleTerrainSource& source, bool land) {
    const auto& overview = source.land().overview();
    std::size_t best = 0;
    for (std::size_t i = 0; i < overview.cells(); ++i)
        if (land ? overview.floors[i] > overview.floors[best]
                 : overview.ceilings[i] < overview.ceilings[best]) best = i;
    return {(double(best % overview.columns) + 0.5) * overview.cellWidthMetres,
            (double(best / overview.columns) + 0.5) * overview.cellHeightMetres};
}
}

TEST(terrain_source_holds_a_budget_however_far_the_camera_walks) {
    // The property the whole mode exists for. A camera crossing ten thousand
    // kilometres touches far more ground than any budget could hold, and the
    // source has to stay inside one - otherwise this is the world-sized array
    // it replaced, filled in more slowly.
    const std::int64_t side = 10000000;
    constexpr std::size_t kBudget = 32u << 20;
    ScaleTerrainSource source(describe(side), {kBudget, 2});
    const auto before = source.bytes();
    // A line through the land the composition actually put in this world, not
    // through the middle of it: most of a transect across an ocean world is
    // refused, and a walk that never touches ground tests the mask, not the cache.
    const auto start = cellCentre(source, true);
    const auto at = [side, start](int step) {
        return std::array{start[0] + side * (step / 260.0), start[1]};
    };
    std::size_t asked = 0;
    int lastWithGround = -1;
    for (int step = 0; step < 240; ++step) {
        const auto where = at(step);
        for (const auto key : around(source, where[0], where[1], 2000, 0)) {
            if (source.page(key)) lastWithGround = step;
            ++asked;
        }
        CHECK(source.stats().residentBytes <= kBudget);
    }
    CHECK(lastWithGround >= 0);
    const auto stats = source.stats();
    CHECK(asked > 2000);
    CHECK(stats.evicted > 0);                   // the walk really did overflow
    CHECK(source.bytes() <= before + kBudget);
    // And the cache is a cache: the ground just left is still there. Only
    // pages that actually derived can hit - most of a transect across an ocean
    // world is refused, and a refusal is not a cache entry.
    const auto last = at(lastWithGround);
    std::vector<TileKey> derived;
    for (const auto key : around(source, last[0], last[1], 2000, 0))
        if (source.page(key)) derived.push_back(key);
    CHECK(!derived.empty());
    const auto hitsBefore = source.stats().hits;
    for (const auto key : derived) CHECK(bool(source.page(key)));
    CHECK_EQ(source.stats().hits, hitsBefore + derived.size());
}

TEST(terrain_source_costs_the_same_in_a_small_world_and_a_huge_one) {
    // A local view is local work. If a page in a ten-thousand-kilometre world
    // costs more than the same page in a hundred-kilometre one, something is
    // walking the world to answer it.
    std::vector<std::size_t> perPage;
    for (const std::int64_t side : {100000, 1000000, 10000000}) {
        ScaleTerrainSource source(describe(side, 7), {32u << 20, 2});
        const auto centre = cellCentre(source, true);
        for (const auto key : around(source, centre[0], centre[1], 3000, 0)) (void)source.page(key);
        const auto stats = source.stats();
        CHECK(stats.derived > 0);
        // A page costs its own samples and nothing else. How MANY pages a view
        // wants depends on the view; what one costs may not depend on the world.
        perPage.push_back(stats.fieldSamples / stats.derived);
        // And the pages themselves stay inside the budget at every size; the
        // overview is separate and bounded by its own sample limit.
        CHECK(stats.residentBytes <= 32u << 20);
        CHECK(source.land().overview().columns <= 128);
    }
    CHECK_EQ(perPage.front(), perPage.back());
    CHECK_EQ(perPage[1], perPage.back());
}

TEST(terrain_source_refuses_open_water_before_it_derives_anything) {
    // The land mask has to stop the work, not decorate it. A view over an
    // ocean must leave the sample counter alone, and a view over a coast must
    // not - if both derive everything the mask is a comment.
    ScaleTerrainSource source(describe(1000000, 3), {32u << 20, 2});
    const auto ocean = cellCentre(source, false), land = cellCentre(source, true);

    for (const auto key : around(source, ocean[0], ocean[1], 2000, 0)) (void)source.page(key);
    const auto afterOcean = source.stats();
    CHECK(afterOcean.refusedOpenWater > 0);
    CHECK_EQ(afterOcean.derived, std::size_t(0));
    CHECK_EQ(afterOcean.fieldSamples, std::size_t(0));

    for (const auto key : around(source, land[0], land[1], 2000, 0)) (void)source.page(key);
    const auto afterLand = source.stats();
    CHECK(afterLand.derived > 0);
    CHECK(afterLand.fieldSamples > 0);
    // A refused page is absent, not an empty page pretending to be ground.
    for (const auto key : around(source, ocean[0], ocean[1], 2000, 0))
        if (!source.mayHaveLand(key)) CHECK(!source.page(key));
}

TEST(terrain_source_pages_are_the_same_whatever_asked_for_them_and_when) {
    // Two workers, two orders, one world. A page that depended on what had
    // been asked before would make two machines disagree about the ground.
    const auto descriptor = describe(276480, 11);
    ScaleTerrainSource first(descriptor, {32u << 20, 2});
    ScaleTerrainSource second(descriptor, {4u << 20, 2});   // small enough to evict
    const double centre = 276480 * 0.5;
    auto keys = around(first, centre, centre, 6000, 0);
    std::vector<std::shared_ptr<const SurfacePage>> forward;
    for (const auto key : keys) forward.push_back(first.page(key));
    std::vector<std::shared_ptr<const SurfacePage>> backward;
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) backward.push_back(second.page(*it));
    std::reverse(backward.begin(), backward.end());
    CHECK_EQ(forward.size(), backward.size());
    std::size_t compared = 0;
    for (std::size_t i = 0; i < forward.size(); ++i) {
        CHECK_EQ(bool(forward[i]), bool(backward[i]));
        if (!forward[i]) continue;
        CHECK_EQ(forward[i]->bed, backward[i]->bed);
        CHECK_EQ(forward[i]->head, backward[i]->head);
        CHECK_EQ(forward[i]->step, backward[i]->step);
        ++compared;
    }
    CHECK(compared > 8);
    // Eviction is not a different answer either: ask again after the cache has
    // certainly turned over.
    const auto again = second.page(keys.front());
    if (forward.front()) CHECK_EQ(again->bed, forward.front()->bed);
}

TEST(terrain_source_is_safe_and_identical_under_concurrent_workers) {
    const auto descriptor = describe(207360, 5);
    ScaleTerrainSource source(descriptor, {32u << 20, 2});
    const double centre = 207360 * 0.5;
    const auto keys = around(source, centre, centre, 5000, 0);
    const auto run = [&](std::size_t offset) {
        std::vector<std::vector<float>> beds;
        for (std::size_t i = 0; i < keys.size(); ++i) {
            const auto page = source.page(keys[(i + offset) % keys.size()]);
            beds.push_back(page ? page->bed : std::vector<float>{});
        }
        return beds;
    };
    auto a = std::async(std::launch::async, run, 0);
    auto b = std::async(std::launch::async, run, keys.size() / 3);
    auto c = std::async(std::launch::async, run, keys.size() / 2);
    const auto first = a.get(), shifted = b.get(), other = c.get();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        CHECK_EQ(first[i], shifted[(i + keys.size() - keys.size() / 3) % keys.size()]);
        CHECK_EQ(first[i], other[(i + keys.size() - keys.size() / 2) % keys.size()]);
    }
    const auto stats = source.stats();
    CHECK_EQ(stats.resident + stats.evicted, stats.derived);
}

TEST(terrain_source_pyramid_covers_a_horizon_in_pages_rather_than_in_millions) {
    // The second way to hold a world-sized array is to ask for a world-sized
    // number of small pages. A coarse level has to cover more world, not need
    // more of itself.
    ScaleTerrainSource source(describe(10000000), {32u << 20, 2});
    CHECK_EQ(SourcePageLayout::extentMetres(0), std::int64_t(512));   // matches the fine storage
    for (std::uint8_t level = 1; level < SourcePageLayout::kLevels; ++level)
        CHECK_EQ(SourcePageLayout::extentMetres(level), SourcePageLayout::extentMetres(level - 1) * 2);
    // A horizon a hundred kilometres across, at the spacing that horizon can
    // actually show: a few hundred pages, not a few million.
    const auto coarse = around(source, 5000000, 5000000, 50000, ScaleTerrainSource::levelFor(200));
    CHECK(!coarse.empty());
    CHECK(coarse.size() < 500);
    // Asking for a whole world in 512 m pages is the failure this avoids -
    // nineteen thousand pages a side - and the source refuses to enumerate it
    // rather than handing back a list nobody could use.
    CHECK(source.keysOverlapping(0, 0, 10000000, 10000000, 0).empty());
    // The same world at a spacing that can actually show it is a few thousand.
    const auto whole = source.keysOverlapping(0, 0, 10000000, 10000000,
                                              ScaleTerrainSource::levelFor(8192));
    CHECK(!whole.empty());
    CHECK(whole.size() < 200);
    // A level is chosen by how far apart the samples need to be, and a coarser
    // request may never come back finer.
    std::uint8_t previous = 0;
    for (const double metres : {1.0, 4.0, 5.0, 64.0, 500.0, 100000.0}) {
        const auto level = ScaleTerrainSource::levelFor(metres);
        CHECK(level >= previous);
        CHECK(SourcePageLayout::stepMetres(level) >= std::min(metres, 8192.0));
        previous = level;
    }
}

TEST(terrain_source_neighbouring_pages_agree_on_the_ground_they_share) {
    // Pages overlap by their padding, and the overlap has to be the same
    // ground: a seam here is a seam in every mesh built from them.
    ScaleTerrainSource source(describe(207360, 17), {32u << 20, 2});
    const auto centre = cellCentre(source, true);
    const auto keys = around(source, centre[0], centre[1], 1500, 0);
    std::size_t checked = 0;
    for (const auto key : keys) {
        const auto left = source.page(key);
        const auto right = source.page({key.x + 1, key.y, key.level});
        if (!left || !right) continue;
        const int width = left->side, padding = left->padding;
        const int interior = width - 1 - 2 * padding;   // samples of shared world
        for (int row = padding; row < width - padding; ++row) {
            // The right page's column `padding` is the left page's column
            // `padding + interior`: the same world point, twice.
            const float a = left->bed[std::size_t(row) * width + padding + interior];
            const float b = right->bed[std::size_t(row) * width + padding];
            CHECK_EQ(a, b);
            ++checked;
        }
    }
    CHECK(checked > 100);
}

TEST(terrain_source_water_surface_is_the_sea_and_says_nothing_about_lakes) {
    // Honest about what this stage knows. Ocean gets a surface at sea level,
    // dry ground gets the sentinel the planner already reads, and no inland
    // water is invented that the hydrology stage would then have to unlearn.
    ScaleTerrainSource source(describe(276480, 23), {32u << 20, 2});
    const auto centre = cellCentre(source, true);
    std::size_t wet = 0, dry = 0;
    for (const auto key : around(source, centre[0], centre[1], 8000, 0)) {
        const auto page = source.page(key);
        if (!page) continue;
        for (std::size_t i = 0; i < page->bed.size(); ++i) {
            if (page->bed[i] < 0) {
                CHECK_EQ(page->head[i], 0.0f);
                ++wet;
            } else {
                CHECK(page->head[i] < page->bed[i]);
                ++dry;
            }
        }
    }
    CHECK(wet + dry > 1000);
}
