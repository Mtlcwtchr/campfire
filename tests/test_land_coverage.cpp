#include "framework.hpp"
#include "game/generation/land_coverage.hpp"
#include "game/generation/world_macro.hpp"
#include "game/generation/world_scale_policy.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
using namespace generation;

WorldDescriptor describe(std::int64_t side, std::uint64_t seed = 42,
                         std::uint32_t overviewLimit = 256) {
    WorldScalePolicy policy;
    policy.overviewSideLimit = overviewLimit;
    return policy.describe(WorldDomain(side, side), seed);
}

std::shared_ptr<const MacroOverview> overviewOf(const MacroField& field) {
    return std::make_shared<const MacroOverview>(buildMacroOverview(field));
}

WorldRect around(double x, double y, double halfSide) {
    return {x - halfSide, y - halfSide, x + halfSide, y + halfSide};
}
}

TEST(land_coverage_open_water_is_proven_and_land_is_never_denied) {
    // The one property that can lose content: a rectangle called OpenWater must
    // hold no land at all. Checked against the field itself, not against the
    // overview that produced the answer, and on a dense sub-overview lattice so
    // a point BETWEEN two overview nodes is what does the checking.
    // Four seeds, because whether one particular world has a PROVEN-land cell
    // at one particular cell size is luck, and a test about luck fails for the
    // wrong reason when the composition changes.
    std::size_t water = 0, land = 0, mixed = 0;
    for (const std::uint64_t seed : {1u, 7u, 42u, 128u}) {
        const auto descriptor = describe(207360, seed);
        const MacroField field(descriptor);
        const LandCoverage coverage(overviewOf(field));
        const auto& overview = coverage.overview();
        const double cell = overview.cellWidthMetres;
        for (std::uint32_t row = 0; row < overview.rows; row += 5)
            for (std::uint32_t column = 0; column < overview.columns; column += 5) {
                const WorldRect rect{column * cell, row * overview.cellHeightMetres,
                                     (column + 1) * cell, (row + 1) * overview.cellHeightMetres};
                const auto verdict = coverage.classify(rect);
                water += verdict == Coverage::OpenWater;
                land += verdict == Coverage::Land;
                mixed += verdict == Coverage::Mixed;
                if (verdict == Coverage::Mixed || verdict == Coverage::Unknown) continue;
                for (int sy = 0; sy <= 7; ++sy)
                    for (int sx = 0; sx <= 7; ++sx) {
                        const double h = field.elevationAt(std::lerp(rect.minX, rect.maxX, sx / 7.0),
                                                           std::lerp(rect.minY, rect.maxY, sy / 7.0));
                        if (verdict == Coverage::OpenWater) CHECK(h < coverage.seaLevelMetres());
                        else CHECK(h > coverage.seaLevelMetres());
                    }
            }
    }
    // A classification that answers Mixed everywhere is sound and useless, and
    // one that never answers Mixed has no coast. Both are failures. The bar is
    // what the mask is FOR: most of the world decided, and enough of it proven
    // open water that the terrestrial queues have something to refuse.
    CHECK(water > 0);
    CHECK(land > 0);
    CHECK(mixed > 0);
    CHECK(mixed < water + land);
    CHECK(water * 3 > water + land + mixed);
}

TEST(land_coverage_keeps_an_island_smaller_than_one_overview_cell) {
    // The failure this exists to prevent: an overview samples its corners, sees
    // water, and deletes a stack of rock that sits between them. The bound is
    // taken over the cell rather than at a node, so the cell that contains any
    // land at all cannot come back as open water.
    const auto descriptor = describe(414720);
    const MacroField field(descriptor);
    const LandCoverage coverage(overviewOf(field));
    const auto& overview = coverage.overview();
    std::size_t probed = 0, kept = 0;
    // Walk a fine lattice, four times finer than the overview, and demand that
    // every point of land is inside something the coverage did not call water.
    const double step = std::min(overview.cellWidthMetres, overview.cellHeightMetres) / 4;
    for (double y = step / 2; y < double(descriptor.domain.heightMetres()); y += step * 13)
        for (double x = step / 2; x < double(descriptor.domain.widthMetres()); x += step * 13) {
            if (field.elevationAt(x, y) <= coverage.seaLevelMetres()) continue;
            ++probed;
            kept += coverage.mayHaveLand(around(x, y, 1)) ? 1 : 0;
        }
    CHECK(probed > 100);
    CHECK_EQ(kept, probed);
}

TEST(land_coverage_answers_a_whole_world_and_a_metre_at_the_same_cost) {
    // A world-sized query must not walk the overview: the pyramid exists so a
    // ten-thousand-kilometre domain answers from one pair of numbers.
    const auto descriptor = describe(1000000, 7);
    const MacroField field(descriptor);
    const LandCoverage coverage(overviewOf(field));
    const WorldRect whole{0, 0, double(descriptor.domain.widthMetres()),
                          double(descriptor.domain.heightMetres())};
    CHECK_EQ(coverage.classify(whole), Coverage::Mixed);   // a world has both
    // The pyramid has a level for every halving and one cell at the top.
    CHECK(coverage.levels() >= 2);
    CHECK(coverage.bytes() < coverage.overview().bytes());
    // Outside the domain is Unknown, not water: nothing has decided it.
    CHECK_EQ(coverage.classify({-5000, -5000, -1000, -1000}), Coverage::Unknown);
    CHECK_EQ(coverage.classify({whole.maxX + 10, 0, whole.maxX + 20, 10}), Coverage::Unknown);
    // A rectangle that merely overhangs is answered for the part that exists,
    // because every coastal halo overhangs and Unknown would stop the coast.
    CHECK(coverage.classify({-1000, 0, 1000, 1000}) != Coverage::Unknown);
    // A degenerate or reversed rectangle is a caller error, not open water.
    CHECK_EQ(coverage.classify({10, 10, 5, 20}), Coverage::Unknown);
    CHECK_EQ(coverage.classify({std::nan(""), 0, 1, 1}), Coverage::Unknown);
}

TEST(land_coverage_water_demand_keeps_a_coastal_halo_but_drops_dry_interiors) {
    const auto descriptor = describe(276480, 11);
    const MacroField field(descriptor);
    const LandCoverage coverage(overviewOf(field));
    const auto& overview = coverage.overview();
    const double cell = overview.cellWidthMetres;
    std::size_t dry = 0, wet = 0, coastalKept = 0, coasts = 0;
    for (std::uint32_t row = 1; row + 1 < overview.rows; row += 2)
        for (std::uint32_t column = 1; column + 1 < overview.columns; column += 2) {
            const WorldRect rect{column * cell, row * overview.cellHeightMetres,
                                 (column + 1) * cell, (row + 1) * overview.cellHeightMetres};
            const bool water = coverage.mayHaveWater(rect);
            dry += !water;
            wet += water;
            // Anything within the halo of open water must still be offered a
            // water surface, whatever its own cell says.
            if (coverage.classify(rect.grown(LandCoverage::kCoastHaloMetres)) == Coverage::Mixed &&
                coverage.classify(rect) == Coverage::Land) {
                ++coasts;
                coastalKept += water;
            }
        }
    CHECK(dry > 0);   // a continental interior needs no sea
    CHECK(wet > 0);
    CHECK(coasts > 0);
    CHECK_EQ(coastalKept, coasts);
    // A halo of zero still keeps every Mixed rectangle wet.
    CHECK(coverage.mayHaveWater({0, 0, double(descriptor.domain.widthMetres()),
                                 double(descriptor.domain.heightMetres())}, 0));
}

TEST(land_coverage_decides_more_as_its_overview_gets_finer_not_less) {
    // A finer overview is strictly more information, so it may only decide more
    // of the world. This caught a bound that was switched off by an exact
    // floating-point comparison: a rectangle built as (column+1)*cell minus
    // column*cell is a hair wider than `cell`, so every overview whose cell was
    // not a binary fraction quietly lost the sharper of its two bounds, and the
    // proven open water of a small world fell from a half to a tenth with no
    // change to the composition at all.
    double previous = 0;
    for (const std::uint32_t limit : {64u, 128u, 256u, 512u}) {
        const auto descriptor = describe(100000, 42, limit);
        const MacroField field(descriptor);
        const LandCoverage coverage(overviewOf(field));
        const auto& overview = coverage.overview();
        std::size_t decided = 0;
        const double cw = overview.cellWidthMetres, ch = overview.cellHeightMetres;
        for (std::uint32_t row = 0; row < overview.rows; ++row)
            for (std::uint32_t column = 0; column < overview.columns; ++column) {
                const WorldRect rect{column * cw, row * ch, (column + 1) * cw, (row + 1) * ch};
                decided += coverage.classify(rect) != Coverage::Mixed;
            }
        const double share = double(decided) / double(overview.cells());
        CHECK(share >= previous - 0.02);
        previous = share;
    }
    CHECK(previous > 0.55);
}

TEST(land_coverage_and_its_overview_do_not_grow_with_the_world) {
    // Bounded means bounded: the whole point of the overview is that a world a
    // hundred times wider does not cost a hundred times the memory, and that
    // the coarse start moves instead.
    std::vector<std::size_t> footprints;
    std::vector<std::uint32_t> steps;
    for (const std::int64_t side : {100000, 1000000, 10000000}) {
        const auto descriptor = describe(side, 1, 256);
        const MacroField field(descriptor);
        const auto overview = overviewOf(field);
        const LandCoverage coverage(overview);
        footprints.push_back(overview->bytes() + coverage.bytes());
        steps.push_back(descriptor.coarseStepMetres);
        CHECK(overview->columns <= 256);
        CHECK(overview->rows <= 256);
    }
    CHECK_EQ(footprints.front(), footprints.back());
    // The coarse source step is what absorbs the extra width, and it may only
    // grow. A ten-thousand-kilometre world cannot start at 256 m.
    CHECK(std::is_sorted(steps.begin(), steps.end()));
    CHECK(steps.back() > steps.front());
}

TEST(land_coverage_is_the_same_answer_whatever_order_it_is_asked_in) {
    // The source may not depend on the camera, the worker count or what has
    // been cached, so two coverages built from one descriptor must agree
    // everywhere - including on rectangles asked in the opposite order.
    const auto descriptor = describe(207360, 128);
    const MacroField field(descriptor);
    const LandCoverage first(overviewOf(field)), second(overviewOf(field));
    const double side = double(descriptor.domain.widthMetres());
    std::vector<WorldRect> rects;
    for (int i = 0; i < 40; ++i) {
        const double x = side * (i * 0.023 + 0.01), y = side * (1 - i * 0.021 - 0.02);
        rects.push_back(around(x, y, 200 + i * 900));
    }
    std::vector<Coverage> forward, backward;
    for (const auto& rect : rects) forward.push_back(first.classify(rect));
    for (auto it = rects.rbegin(); it != rects.rend(); ++it) backward.push_back(second.classify(*it));
    std::reverse(backward.begin(), backward.end());
    CHECK_EQ(forward, backward);
    // And a sub-rectangle can never be less decided than the one holding it.
    for (const auto& rect : rects) {
        const auto whole = first.classify(rect);
        const WorldRect half{rect.minX, rect.minY, (rect.minX + rect.maxX) / 2,
                             (rect.minY + rect.maxY) / 2};
        const auto part = first.classify(half);
        if (whole == Coverage::OpenWater) CHECK_EQ(part, Coverage::OpenWater);
        if (whole == Coverage::Land) CHECK_EQ(part, Coverage::Land);
    }
}

TEST(macro_composition_grows_in_regions_rather_than_in_feature_size) {
    // Two to four large systems across a hundred kilometres, tens to hundreds
    // across ten thousand. What must NOT happen is the second world being the
    // first one stretched, so the belt is measured as well as counted.
    const auto systems = [](std::int64_t side) {
        std::size_t total = 0;
        for (const std::uint64_t seed : {3u, 7u, 11u, 42u}) {
            const MacroField field(describe(side, seed, 256));
            for (const double v : {0.23, 0.51, 0.79}) {
                bool inside = false;
                for (int i = 0; i <= 2000; ++i) {
                    const bool now = field.at(side * (i / 2000.0), side * v).uplift > 0.62;
                    total += now && !inside;
                    inside = now;
                }
            }
        }
        return total;
    };
    const auto small = systems(100000), large = systems(10000000);
    CHECK(small >= 4);            // twelve transects across four small worlds
    CHECK(large > small * 4);     // and far more of them in the large one

    const MacroField tiny(describe(100000, 3, 256)), huge(describe(10000000, 3, 256));
    // A hundredfold world with a fivefold belt: a mountain range is a mountain
    // range at any map size. Sizes of things are not multiplied by world scale.
    const double growth = huge.beltWavelengthMetres() / tiny.beltWavelengthMetres();
    CHECK(growth > 3 && growth < 8);
    // The belt is a smaller and smaller share of the world, which is what
    // "more regional hierarchy" means in a number.
    CHECK(huge.beltWavelengthMetres() / 10000000.0 < tiny.beltWavelengthMetres() / 100000.0 / 10);
    // The relief a range carries is a physical constant, so the only part of
    // the ceiling allowed to differ between the two is the continental
    // platform, which a small world is not wide enough to raise.
    CHECK(std::abs(tiny.elevationCeiling() - huge.elevationCeiling()) <=
          MacroField::kInteriorHeight + 1);
    CHECK(tiny.elevationCeiling() <= huge.elevationCeiling());
    // And the mountains themselves are mountains at both sizes: the highest
    // ground a transect finds is within a factor of two, not a factor of a
    // hundred, however much wider the map got.
    const auto peak = [](const MacroField& field, double side) {
        double best = 0;
        for (const double v : {0.19, 0.44, 0.67, 0.88})
            for (int i = 0; i <= 3000; ++i)
                best = std::max(best, double(field.elevationAt(side * (i / 3000.0), side * v)));
        return best;
    };
    const double low = peak(tiny, 100000), high = peak(huge, 10000000);
    CHECK(low > 500 && high > 500);
    CHECK(std::max(low, high) < std::min(low, high) * 2);
}

TEST(macro_overview_is_the_same_bits_however_many_threads_built_it) {
    // The overview is built in parallel bands. Two machines with different core
    // counts have to agree about a world down to the bit, so the bands may not
    // be allowed to change a single float - and the bound a cell carries is
    // what the land mask trusts, so a one-ulp difference is a different world.
    const auto descriptor = describe(276480, 17, 64);
    const MacroField field(descriptor);
    const auto first = buildMacroOverview(field);
    for (int repeat = 0; repeat < 3; ++repeat) {
        const auto again = buildMacroOverview(field);
        CHECK_EQ(again.elevation, first.elevation);
        CHECK_EQ(again.floors, first.floors);
        CHECK_EQ(again.ceilings, first.ceilings);
        CHECK_EQ(again.continent, first.continent);
        CHECK_EQ(again.uplift, first.uplift);
    }
    // Every cell's bound has to hold its own centre sample, or the pyramid is
    // summarising something the field never said.
    for (std::size_t i = 0; i < first.cells(); ++i) {
        CHECK(first.floors[i] <= first.elevation[i]);
        CHECK(first.ceilings[i] >= first.elevation[i]);
    }
}

TEST(macro_field_lipschitz_bound_is_not_beaten_by_the_field_itself) {
    // If this bound is wrong the island test above proves nothing, so it is
    // measured directly: no pair of nearby points may differ by more than the
    // bound says, at several separations and in both axes.
    const auto descriptor = describe(207360, 99);
    const MacroField field(descriptor);
    const double bound = field.elevationLipschitz();
    CHECK(bound > 0);
    double worst = 0;
    for (int i = 0; i < 4000; ++i) {
        const double x = 12345.0 * i * 0.37, y = 7654.0 * i * 0.53;
        const double wx = std::fmod(x, double(descriptor.domain.widthMetres()));
        const double wy = std::fmod(y, double(descriptor.domain.heightMetres()));
        for (const double step : {1.0, 37.0, 613.0}) {
            const double here = field.elevationAt(wx, wy);
            worst = std::max(worst, std::abs(field.elevationAt(wx + step, wy) - here) / step);
            worst = std::max(worst, std::abs(field.elevationAt(wx, wy + step) - here) / step);
        }
    }
    CHECK(worst <= bound);
    // And it must not be absurd: a bound a thousand times the real slope turns
    // every rectangle Mixed and buys nothing.
    CHECK(worst * 200 > bound);
}
