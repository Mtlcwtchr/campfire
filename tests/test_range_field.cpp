#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/generation/field_noise.hpp"
#include "game/generation/range_field.hpp"

namespace {
using namespace generation;

PlateFieldSettings plateWorld(double side, int wanted) {
    PlateFieldSettings settings;
    settings.plateMetres = side / std::sqrt(double(wanted));
    settings.marginMetres = settings.plateMetres * 0.30;
    settings.shelfMetres = settings.plateMetres * 0.10;
    settings.warpMetres = settings.plateMetres * 0.18;
    settings.warpBroadMetres = settings.plateMetres * 2.1;
    settings.warpFineMetres = settings.plateMetres * 0.5;
    return settings;
}

// The cross-section of the range that owns the highest point on a line, walked
// outwards from the crest.
struct Crossing {
    double crestX = 0, crestY = 0, peak = 0;
    std::vector<double> profile;   // height at 0, 5%, 10% ... of the half width
};

Crossing acrossARange(const RangeField& field, double y, double fromX, double toX) {
    Crossing found;
    for (int step = 0; step <= 4000; ++step) {
        const double x = fromX + (toX - fromX) * step / 4000;
        const auto sample = field.at(x, y);
        if (sample.metres > found.peak) {
            found.peak = sample.metres;
            found.crestX = x;
            found.crestY = y;
        }
    }
    return found;
}
}

TEST(range_field_section_is_triangular_rather_than_a_dome) {
    // The single most visible thing about the mountains in this world: `q * q`
    // has zero slope at the summit, and zero slope at a summit is a loaf. A
    // real range is steepest just below its crest.
    //
    // Measured as the ratio of the slope near the crest to the slope at the
    // middle of the flank. A cone gives one; a dome gives nearly nothing.
    const PlateField plates(4242, plateWorld(400000, 8));
    const RangeField field(4242, plates);
    const double half = field.settings().halfWidthMetres;

    std::size_t measured = 0;
    double worstRatio = 10;
    for (int lane = 0; lane < 24; ++lane) {
        const double y = 20000 + lane * 15000.0;
        const auto crest = acrossARange(field, y, 20000, 380000);
        if (crest.peak < 300) continue;
        // Walk out perpendicular-ish - along x, which crosses the range - and
        // take the slope just off the crest and at mid-flank.
        const auto height = [&](double at) { return field.at(crest.crestX + at, y).metres; };
        const double step = half * 0.04;
        const double nearCrest = std::abs(height(step) - height(0.0)) / step;
        const double midFlank = std::abs(height(half * 0.52) - height(half * 0.48)) / (half * 0.04);
        if (midFlank <= 0) continue;
        worstRatio = std::min(worstRatio, nearCrest / midFlank);
        ++measured;
    }
    CHECK(measured > 8);
    // A dome would give well under a tenth here. The section is linear with
    // only its foot eased, so the two slopes are within a factor of about two.
    CHECK(worstRatio > 0.35);
}

TEST(range_field_has_peaks_and_passes_along_its_length_not_a_wall) {
    // A fill gives a swell with no line in it; a line with no noise along it
    // gives a wall. What a range has is peaks, and passes between them.
    const PlateField plates(77, plateWorld(500000, 8));
    const RangeField field(77, plates);

    // Walk along a crest by following the highest point of each cross-section.
    std::vector<double> summits;
    for (int lane = 0; lane < 160; ++lane) {
        const double y = 30000 + lane * 2200.0;
        const auto crest = acrossARange(field, y, 30000, 470000);
        if (crest.peak > 0) summits.push_back(crest.peak);
    }
    CHECK(summits.size() > 100);

    double highest = 0, total = 0;
    for (const double h : summits) { highest = std::max(highest, h); total += h; }
    const double mean = total / double(summits.size());
    CHECK(highest > 800);
    // Peaks well above the average, which a wall does not have.
    CHECK(highest > mean * 1.6);

    // And real passes: local minima along the crest, with the lowest of them
    // well under the highest peak. A wall has neither.
    double lowest = highest;
    std::size_t dips = 0;
    for (std::size_t i = 1; i + 1 < summits.size(); ++i) {
        lowest = std::min(lowest, summits[i]);
        if (summits[i] < summits[i - 1] && summits[i] < summits[i + 1]) ++dips;
    }
    CHECK(dips > 15);
    CHECK(lowest < highest * 0.5);
}

TEST(range_field_crest_wanders_off_the_bisector) {
    // A Voronoi bisector is a diagram, and a range that follows one reads as a
    // diagram however good the section on it is. The crest line has to leave it.
    const PlateField plates(1005, plateWorld(400000, 8));
    const RangeField field(1005, plates);
    std::size_t moved = 0, looked = 0;
    double furthest = 0;
    for (int j = 0; j < 60; ++j)
        for (int i = 0; i < 60; ++i) {
            const double x = 400000.0 * i / 60, y = 400000.0 * j / 60;
            const auto plate = plates.at(x, y);
            const auto range = field.at(x, y);
            if (plate.margin == Margin::None || plate.stress <= 0) continue;
            ++looked;
            const double shift = std::abs(range.crestMetres - plate.boundaryMetres);
            furthest = std::max(furthest, shift);
            moved += shift > 500;
        }
    CHECK(looked > 500);
    CHECK(moved * 2 > looked);          // most of it is off the bisector
    CHECK(furthest > 4000);             // kilometres, not metres
}

TEST(range_field_builds_what_the_margin_is_and_digs_what_it_is_not) {
    // The margin kind decides the sign and the size; the axis decides the
    // shape. A trench is not a mountain with a minus in front of it by
    // accident - it is the other side of the same boundary.
    const PlateField plates(31337, plateWorld(600000, 10));
    const RangeField field(31337, plates);
    double highestCollision = 0, deepestTrench = 0, highestArc = 0, deepestRift = 0;
    for (int j = 0; j < 200; ++j)
        for (int i = 0; i < 200; ++i) {
            const double x = 600000.0 * i / 200, y = 600000.0 * j / 200;
            const auto plate = plates.at(x, y);
            const auto range = field.at(x, y);
            CHECK(std::isfinite(range.metres));
            switch (plate.margin) {
                case Margin::Collision:
                    highestCollision = std::max(highestCollision, range.metres);
                    CHECK(range.metres >= 0);
                    break;
                case Margin::Trench:
                    deepestTrench = std::min(deepestTrench, range.metres);
                    CHECK(range.metres <= 0);
                    break;
                case Margin::Arc: highestArc = std::max(highestArc, range.metres); break;
                case Margin::Rift:
                    if (!plate.oceanic || !plate.neighbourOceanic)
                        deepestRift = std::min(deepestRift, range.metres);
                    break;
                default: break;
            }
        }
    CHECK(highestCollision > 1200);
    CHECK(deepestTrench < -1200);
    CHECK(highestArc > 200);
    CHECK(deepestRift < -100);
    // Two continents crumple higher than an island arc, which is the whole
    // reason the kinds are told apart.
    CHECK(highestCollision > highestArc * 1.5);
}

TEST(range_field_leaves_most_of_the_world_as_plain) {
    // A range that faded to a few hundred metres everywhere is a world with no
    // plains in it, which is the failure mode every "add noise to it" terrain
    // has. Measured over a whole world rather than asserted.
    //
    // Note what the number is NOT: a margin reaches most of the map - with
    // eight plates and margins three tenths of the spacing, only an eighth of
    // the world is beyond any of them. What keeps the plains is that the stress
    // falls off as the SQUARE of the reach, so at half a margin's width it is
    // already down to a quarter.
    const PlateField plates(9, plateWorld(400000, 8));
    const RangeField field(9, plates);
    std::size_t plain = 0, beyond = 0, total = 0;
    for (int j = 0; j < 120; ++j)
        for (int i = 0; i < 120; ++i) {
            const double x = 400000.0 * i / 120, y = 400000.0 * j / 120;
            const double metres = field.at(x, y).metres;
            ++total;
            plain += std::abs(metres) < 150;
            if (plates.at(x, y).stress <= 0) {
                CHECK_EQ(metres, 0.0);   // beyond every margin, exactly nothing
                ++beyond;
            }
        }
    CHECK(beyond > 1000);
    CHECK(plain * 4 > total);   // most of a world is not mountain
}

TEST(range_field_is_the_same_range_wherever_it_is_asked_from) {
    const PlateField plates(555, plateWorld(300000, 8));
    const RangeField first(555, plates);
    const RangeField again(555, plates);
    for (int i = 0; i < 500; ++i) {
        const double x = -90000 + i * 719.3, y = 160000 - i * 883.1;
        CHECK_EQ(first.at(x, y).metres, again.at(x, y).metres);
        CHECK_EQ(first.at(x, y).crestMetres, again.at(x, y).crestMetres);
    }
    // And refuses what is not a number rather than producing one.
    const double bad = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(first.at(bad, 0).metres, 0.0);
    CHECK_EQ(first.at(0, bad).flank, 0.0);
}

TEST(ridged_noise_creases_where_summed_noise_would_round) {
    // The property the whole look rests on. A ridged field is bottom-heavy:
    // broad low ground with thin high ridges through it. Summed noise is
    // symmetric about its middle, which is what makes a range of it a row of
    // domes.
    // Over hundreds of lattice cells, not a handful: with forty corners in the
    // sample the mean of a uniform field is itself uncertain by a tenth, and a
    // test that measured that would be measuring its own sample size.
    std::vector<double> ridged, summed;
    for (int j = 0; j < 300; ++j)
        for (int i = 0; i < 300; ++i) {
            const double x = i * 1900.0, y = j * 1900.0;
            ridged.push_back(noise::ridged(11, x, y, 9000, 5));
            summed.push_back(noise::value(11, x, y, 9000) * 0.5 + 0.5);
        }
    const auto middleOf = [](std::vector<double> v) {
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    const auto meanOf = [](const std::vector<double>& v) {
        double total = 0;
        for (const double x : v) total += x;
        return total / double(v.size());
    };
    // Bottom-heavy: the median sits below the mean, and well below the middle
    // of the range. Summed noise does neither.
    CHECK(middleOf(ridged) < meanOf(ridged));
    CHECK(middleOf(ridged) < 0.45);
    CHECK(std::abs(middleOf(summed) - 0.5) < 0.05);
    for (const double v : ridged) CHECK(v >= 0 && v <= 1);
}
