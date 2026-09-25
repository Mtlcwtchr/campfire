#include "framework.hpp"

#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "game/generation/plate_field.hpp"

namespace {
using namespace generation;

PlateFieldSettings world(double side, int wanted) {
    PlateFieldSettings settings;
    // The spacing IS the plate count: a region of side S holds about
    // (S / spacing)^2 of them.
    settings.plateMetres = side / std::sqrt(double(wanted));
    // Below half the spacing, or there is no plate interior at all: the
    // furthest any point can be from a boundary is about half of it.
    settings.marginMetres = settings.plateMetres * 0.30;
    settings.shelfMetres = settings.plateMetres * 0.10;
    settings.warpMetres = settings.plateMetres * 0.18;
    settings.warpBroadMetres = settings.plateMetres * 2.1;
    settings.warpFineMetres = settings.plateMetres * 0.5;
    return settings;
}

std::set<std::uint64_t> platesOver(const PlateField& field, double side, int steps = 64) {
    std::set<std::uint64_t> seen;
    for (int j = 0; j < steps; ++j)
        for (int i = 0; i < steps; ++i)
            seen.insert(field.at(side * (i + 0.5) / steps, side * (j + 0.5) / steps).plate);
    return seen;
}
}

TEST(plate_field_puts_the_asked_for_number_of_plates_in_a_world) {
    // The old model had a plate count and world-sized arrays to hold it. This
    // has a spacing, and the count follows from how much world is looked at -
    // which is the only formulation an unbounded world can have.
    for (const int wanted : {6, 8, 16, 40}) {
        const double side = 400000;
        const PlateField field(1234, world(side, wanted));
        const auto seen = platesOver(field, side);
        CHECK(seen.size() >= std::size_t(wanted) / 2);
        CHECK(seen.size() <= std::size_t(wanted) * 3);
    }
    // And twice the world at the same spacing has about four times the plates,
    // because that is what area means.
    const PlateField field(99, world(400000, 8));
    const auto small = platesOver(field, 400000).size();
    const auto large = platesOver(field, 800000, 128).size();
    CHECK(large > small * 2);
    CHECK(large < small * 6);
}

TEST(plate_field_is_the_same_field_wherever_it_is_asked_from) {
    // Nothing is stored, so the only thing that makes two chunks agree is that
    // they compute the same answer. Walk the same points in two different
    // orders, at two different strides, from two different objects.
    const PlateField first(77, world(300000, 8));
    const PlateField again(77, world(300000, 8));
    for (int i = 0; i < 400; ++i) {
        const double x = -140000 + i * 913.7, y = 220000 - i * 1171.3;
        const auto a = first.at(x, y);
        const auto b = again.at(x, y);
        CHECK_EQ(a.plate, b.plate);
        CHECK_EQ(a.neighbour, b.neighbour);
        CHECK_EQ(a.boundaryMetres, b.boundaryMetres);
        CHECK_EQ(a.stress, b.stress);
        CHECK_EQ(int(a.margin), int(b.margin));
    }
    // A different seed is a different world, not the same one shifted.
    const PlateField other(78, world(300000, 8));
    std::size_t differing = 0;
    for (int i = 0; i < 400; ++i) {
        const double x = -140000 + i * 913.7, y = 220000 - i * 1171.3;
        differing += other.at(x, y).plate != first.at(x, y).plate;
    }
    CHECK(differing > 300);
}

TEST(plate_field_margins_are_torn_rather_than_straight) {
    // A coastline that runs dead straight for eighty kilometres is a diagram.
    // Walk across a boundary along many parallel lines and measure where it
    // crosses: a Voronoi bisector gives a straight line, a warped one does not.
    //
    // Over two hundred kilometres of lanes, not sixty - the broad warp works at
    // the scale of a plate, and a stretch short against it is nearly straight
    // whatever the warp is doing.
    const PlateField field(2024, world(400000, 8));
    std::vector<double> crossings;
    for (int lane = 0; lane < 40; ++lane) {
        const double y = 40000 + lane * 5000.0;
        std::uint64_t previous = 0;
        for (int step = 0; step <= 900; ++step) {
            const double x = 20000 + step * 400.0;
            const auto sample = field.at(x, y);
            if (previous && sample.plate != previous) { crossings.push_back(x); break; }
            previous = sample.plate;
        }
    }
    CHECK(crossings.size() > 20);
    // What is left after fitting a straight line through where it crossed.
    const auto n = double(crossings.size());
    double sumI = 0, sumX = 0, sumII = 0, sumIX = 0;
    for (std::size_t i = 0; i < crossings.size(); ++i) {
        sumI += double(i);
        sumX += crossings[i];
        sumII += double(i) * double(i);
        sumIX += double(i) * crossings[i];
    }
    const double slope = (n * sumIX - sumI * sumX) / std::max(1e-9, n * sumII - sumI * sumI);
    const double intercept = (sumX - slope * sumI) / n;
    double residual = 0;
    for (std::size_t i = 0; i < crossings.size(); ++i) {
        const double d = crossings[i] - (intercept + slope * double(i));
        residual += d * d;
    }
    // Kilometres off a straight line, not metres.
    CHECK(std::sqrt(residual / n) > 1500.0);
}

TEST(plate_field_knows_how_far_the_boundary_is_in_metres) {
    // What the flood fill it replaces never had. Its reach was counted in macro
    // cells, so the width of a mountain belt followed the resolution of the map
    // rather than the geology.
    const PlateField field(5, world(400000, 8));
    double lowest = 1e30;
    double previous = -1;
    std::size_t falling = 0, rising = 0;
    for (int step = 0; step <= 800; ++step) {
        const double x = 30000 + step * 300.0;
        const auto sample = field.at(x, 150000);
        CHECK(sample.boundaryMetres >= 0);
        CHECK(std::isfinite(sample.boundaryMetres));
        lowest = std::min(lowest, sample.boundaryMetres);
        if (previous >= 0) (sample.boundaryMetres < previous ? falling : rising) += 1;
        previous = sample.boundaryMetres;
    }
    CHECK(lowest < 4000.0);          // the walk really does cross one
    CHECK(falling > 100);
    CHECK(rising > 100);

    // The margin is held below four tenths of the spacing precisely so that
    // there IS an interior: the furthest any point can be from a boundary is
    // about half of it, and a margin wider than that reaches everywhere.
    CHECK(field.settings().marginMetres < field.settings().plateMetres * 0.5);
    std::size_t quiet = 0, loud = 0;
    double far = 0;
    for (int j = 0; j < 90; ++j)
        for (int i = 0; i < 90; ++i) {
            const auto sample = field.at(400000.0 * i / 90, 400000.0 * j / 90);
            far = std::max(far, sample.boundaryMetres);
            if (sample.boundaryMetres > field.settings().marginMetres) {
                CHECK_EQ(sample.stress, 0.0);
                ++quiet;
            } else if (sample.boundaryMetres < field.settings().marginMetres * 0.15) {
                loud += sample.stress > 0.05;
            }
        }
    CHECK(quiet > 500);
    CHECK(loud > 100);
    CHECK(far > field.settings().marginMetres);
}

TEST(plate_field_tells_a_collision_from_a_trench_from_a_rift) {
    // The margin kind is what decides the SHAPE the stress takes; the stress
    // only says how much. Over a whole world every kind should appear, and each
    // one only where its own crust pair and its own drift put it.
    const PlateField field(31337, world(600000, 10));
    std::map<int, std::size_t> kinds;
    for (int j = 0; j < 120; ++j)
        for (int i = 0; i < 120; ++i) {
            const auto sample = field.at(600000.0 * i / 120, 600000.0 * j / 120);
            if (sample.boundaryMetres > sample.stress * 0 + 12000) continue;   // near a margin only
            kinds[int(sample.margin)] += 1;
            switch (sample.margin) {
                case Margin::Collision:
                    CHECK(!sample.oceanic);
                    CHECK(!sample.neighbourOceanic);
                    CHECK(sample.closing > 0);
                    break;
                case Margin::Arc:
                    CHECK(sample.oceanic);
                    CHECK(sample.neighbourOceanic);
                    CHECK(sample.closing > 0);
                    break;
                case Margin::Cordillera:
                    CHECK(!sample.oceanic);
                    CHECK(sample.neighbourOceanic);
                    break;
                case Margin::Trench:
                    CHECK(sample.oceanic);
                    CHECK(!sample.neighbourOceanic);
                    break;
                case Margin::Rift: CHECK(sample.closing < 0); break;
                default: break;
            }
        }
    CHECK(kinds.size() >= 4);
    // A cordillera and the trench in front of it are the same boundary seen
    // from its two sides, so neither can appear without the other.
    CHECK((kinds.count(int(Margin::Cordillera)) > 0) == (kinds.count(int(Margin::Trench)) > 0));
}

TEST(plate_field_softens_the_crust_step_over_a_shelf) {
    // A step taken straight off the plate map is a cliff along the whole
    // margin, in the shape of the margin - which is what made the old world
    // look like tiles laid on a floor before its blur was added.
    //
    // The shelf is held well below the margin for a reason the numbers found:
    // the step blends towards the NEIGHBOURING plate's crust, and which plate
    // is the neighbour changes abruptly along the medial axis between three of
    // them. At a shelf of a third of the spacing the crust jumped by a quarter
    // at every such line - a cliff in the middle of open water.
    const PlateField field(808, world(400000, 8));
    CHECK(field.settings().shelfMetres < field.settings().marginMetres);

    bool sawLand = false, sawOcean = false;
    double steepest = 0;
    for (int lane = 0; lane < 24; ++lane) {
        const double y = 15000 + lane * 16000.0;
        double previous = -1;
        for (int step = 0; step <= 1200; ++step) {
            const double x = 10000 + step * 320.0;
            const auto sample = field.at(x, y);
            CHECK(sample.crust >= 0);
            CHECK(sample.crust <= 1);
            sawLand = sawLand || sample.crust > 0.9;
            sawOcean = sawOcean || sample.crust < 0.1;
            if (previous >= 0)
                steepest = std::max(steepest, std::abs(sample.crust - previous) / 320.0);
            previous = sample.crust;
        }
    }
    CHECK(sawLand);
    CHECK(sawOcean);
    // Per metre, over two dozen lines: a shelf kilometres wide, not a cliff and
    // not a quarter-height step where a third plate takes over as the
    // neighbour.
    CHECK(steepest < 1.0 / 4000.0);
}

TEST(plate_field_refuses_a_position_that_is_not_a_number) {
    const PlateField field(1, world(400000, 8));
    const double bad = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(field.at(bad, 0).plate, std::uint64_t(0));
    CHECK_EQ(field.at(0, bad).stress, 0.0);
    CHECK_EQ(int(field.at(bad, bad).margin), int(Margin::None));
    // And settings that ask for nothing are taken to their floors rather than
    // dividing by them.
    PlateFieldSettings broken;
    broken.plateMetres = 0;
    broken.marginMetres = -5;
    const PlateField guarded(1, broken);
    CHECK(guarded.settings().plateMetres > 0);
    CHECK(guarded.settings().marginMetres > 0);
    CHECK(std::isfinite(guarded.at(1000, 1000).boundaryMetres));
}
