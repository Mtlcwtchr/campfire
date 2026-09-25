#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "game/generation/erosion_field.hpp"

namespace {
using namespace generation;

// A break in the ground: one height on the left, another on the right. This is
// what a talus cut is for - a lip, a crag, the top of a face.
HeightAt breakOf(double metres) {
    return [metres](double x, double) { return x < 0 ? metres : 0.0; };
}

// A uniform ramp, for the case the cut cannot fix and must not pretend to.
HeightAt ramp(double gradient, double topMetres = 600) {
    return [gradient, topMetres](double x, double) {
        if (x <= 0) return topMetres;
        return std::max(0.0, topMetres - x * gradient);
    };
}

// The steepest slope between neighbouring samples of a surface, at one step.
double steepestOf(const HeightAt& height, double step, double fromX, double toX, double y) {
    double worst = 0;
    for (double x = fromX; x + step <= toX; x += step)
        worst = std::max(worst, std::abs(height(x + step, y) - height(x, y)) / step);
    return worst;
}
}

TEST(erosion_lays_a_break_back_to_the_angle_at_every_step_it_is_asked_at) {
    // The bug this replaces. The old thermal pass compared a height difference
    // against a constant forty metres, which is an angle only once you know the
    // spacing: at sixty-four metres it happened to mean thirty-two degrees, at
    // four it would mean eighty-four, and so the stage did nothing at all in a
    // world sampled finer than it was written for.
    //
    // An angle means the same thing everywhere, and so does what it can reach:
    // a break up to tan(talus) * rings * step tall is laid back exactly to the
    // angle, which is twelve metres at a four-metre step and a hundred and
    // thirty at a sixty-four-metre one. Each level of detail lays back what it
    // can see.
    ErosionSettings settings;
    settings.talusDegrees = 34;
    settings.gullyMetres = 0;          // the cut on its own
    settings.talusRings = 3;
    const ErosionField field(1, settings);
    const double talus = std::tan(34 * 3.14159265358979323846 / 180.0);

    for (const double step : {4.0, 16.0, 64.0, 256.0}) {
        const double reach = talus * settings.talusRings * step;
        const auto face = breakOf(reach * 0.8);
        const auto eroded = [&](double x, double y) {
            return face(x, y) + field.at(face, x, y, step).geometryMetres;
        };
        CHECK(steepestOf(face, step, -12 * step, 12 * step, 0) > talus * 2);
        CHECK(steepestOf(eroded, step, -12 * step, 12 * step, 0) <= talus * 1.02);
    }
}

TEST(erosion_cannot_lay_back_a_face_longer_than_its_reach_and_does_not_pretend_to) {
    // Stated rather than hidden. A six-hundred-metre cliff needs a
    // nine-hundred-metre apron, and no operator that looks a few steps around
    // can build one - nor can the real thing, which is why talus aprons are
    // finite and the faces above them are not.
    //
    // Two ways of reaching further both failed, and both failed silently:
    // anchors on a fixed world lattice jump as the query slides across it and
    // the envelope jumped with them, at seventeen times the angle; and a
    // pyramid of spacings from the step up to a fixed reach jumps for the same
    // reason at every level of it.
    ErosionSettings settings;
    settings.talusDegrees = 34;
    settings.gullyMetres = 0;
    const ErosionField field(1, settings);
    const double talus = std::tan(34 * 3.14159265358979323846 / 180.0);

    const auto face = ramp(1.9, 600);   // three hundred metres long, far past reach
    const double step = 8;
    const auto eroded = [&](double x, double y) {
        return face(x, y) + field.at(face, x, y, step).geometryMetres;
    };
    // In the middle of a uniform face the cut is a CONSTANT - every point is
    // above the cone from its downhill neighbour by the same amount - so the
    // ground is lowered and its angle is exactly what it was. That is the
    // honest description of what a local operator does to a slope longer than
    // it can see, and it is worth measuring rather than assuming: a test that
    // checked the height was untouched would fail, and for the wrong reason.
    const double lowered = face(160, 0) - eroded(160, 0);
    CHECK(lowered > 10.0);
    CHECK(std::abs((face(200, 0) - eroded(200, 0)) - lowered) < 0.5);
    CHECK(std::abs(steepestOf(eroded, step, 40, 280, 0) - 1.9) < 0.05);
    CHECK(steepestOf(eroded, step, 40, 280, 0) > talus * 2);
    // And no jumps anywhere: whatever it does or does not do, it is continuous.
    double worst = 0;
    for (double x = -200; x < 600; x += 1.0)
        worst = std::max(worst, std::abs(eroded(x + 1, 0) - eroded(x, 0)));
    CHECK(worst < 2.5);
}

TEST(erosion_leaves_a_slope_that_is_already_gentle_alone) {
    // A cut that fired everywhere would flatten the world. Only what is past
    // repose is cut back, which is what turns a face into a facet and leaves a
    // crest a crest.
    ErosionSettings settings;
    settings.talusDegrees = 34;
    settings.gullyMetres = 0;          // the talus cut alone, for this one
    const ErosionField field(2, settings);
    const auto gentle = ramp(0.35);   // about nineteen degrees
    for (const double step : {4.0, 32.0, 128.0})
        for (double x = 20; x < 600; x += step) {
            const auto sample = field.at(gentle, x, 0, step);
            CHECK(std::abs(sample.geometryMetres) < 0.01);
            CHECK_EQ(sample.talusCut, 0.0);
        }
}

TEST(erosion_hands_a_gully_to_the_shader_when_the_step_cannot_hold_it) {
    // The rule from the plan, in one function. A gully two hundred metres
    // across is shape at four metres and a picture at two hundred and fifty,
    // and what changes with the step is WHICH of the two - never how much
    // erosion there is.
    ErosionSettings settings;
    settings.gullyScaleMetres = 240;
    settings.gullyMetres = 40;
    const ErosionField field(3, settings);
    const auto face = ramp(0.85, 1400);

    double fineShape = 0, fineShading = 0, coarseShape = 0, coarseShading = 0;
    for (double x = 200; x < 1200; x += 37.0) {
        const auto fine = field.at(face, x, x * 0.5, 4.0);
        const auto coarse = field.at(face, x, x * 0.5, 200.0);
        fineShape += std::abs(fine.geometryMetres) - fine.talusCut;
        fineShading += fine.shadingMetres;
        coarseShape += std::abs(coarse.geometryMetres) - coarse.talusCut;
        coarseShading += coarse.shadingMetres;
        CHECK(fine.shadingScaleMetres == settings.gullyScaleMetres);
    }
    // At a fine step the gullies are geometry and nothing is owed.
    CHECK(fineShape > 0);
    CHECK(fineShading < fineShape * 0.05);
    // At a coarse one they are owed and not cut.
    CHECK(coarseShading > 0);
    CHECK(coarseShape < coarseShading * 0.05);
    // And nothing is lost in the crossing: what one step draws the other owes.
    CHECK(std::abs((fineShape + fineShading) - (coarseShape + coarseShading)) <
          (fineShape + fineShading) * 0.35);
}

TEST(erosion_runs_its_gullies_down_the_slope_not_across_the_world) {
    // Gullies laid out in world coordinates are a plaid, and a plaid is the
    // giveaway that nothing is running anywhere. They follow the fall line.
    ErosionSettings settings;
    settings.gullyMetres = 40;
    settings.gullyScaleMetres = 200;
    const ErosionField field(4, settings);

    // The same cliff, twice: falling east, and falling north. If the pattern
    // were in world coordinates the two would give the same numbers at
    // mirrored points; because it follows the slope, they do not.
    const auto east = ramp(0.9, 1400);
    const HeightAt north = [](double, double y) {
        return std::max(0.0, 1400 - std::max(0.0, y) * 0.9);
    };
    std::size_t differing = 0, looked = 0;
    for (double t = 240; t < 1100; t += 23.0) {
        const double a = field.at(east, t, 300, 6.0).geometryMetres;
        const double b = field.at(north, 300, t, 6.0).geometryMetres;
        ++looked;
        differing += std::abs(a - b) > 0.5;
    }
    CHECK(looked > 30);
    CHECK(differing * 3 > looked * 2);
}

TEST(erosion_does_nothing_to_a_plain) {
    // A plain has no gullies in it, and a talus slope under a hillock is not a
    // talus slope. Relief has to be there before anything happens.
    const ErosionField field(5, {});
    const HeightAt flat = [](double, double) { return 120.0; };
    const HeightAt gentle = [](double x, double y) {
        return 120.0 + std::sin(x / 900.0) * 4.0 + std::cos(y / 700.0) * 3.0;
    };
    for (double x = 0; x < 3000; x += 71.0) {
        CHECK_EQ(field.at(flat, x, x, 8.0).geometryMetres, 0.0);
        CHECK_EQ(field.at(flat, x, x, 8.0).shadingMetres, 0.0);
        CHECK(std::abs(field.at(gentle, x, -x, 8.0).geometryMetres) < 0.01);
        CHECK_EQ(field.at(gentle, x, -x, 8.0).shadingMetres, 0.0);
    }
}

TEST(erosion_is_the_same_erosion_twice_and_refuses_nonsense) {
    const ErosionField field(6, {});
    const auto face = ramp(1.2, 900);
    for (double x = 10; x < 700; x += 29.0) {
        const auto a = field.at(face, x, 40, 12.0);
        const auto b = field.at(face, x, 40, 12.0);
        CHECK_EQ(a.geometryMetres, b.geometryMetres);
        CHECK_EQ(a.shadingMetres, b.shadingMetres);
    }
    const double bad = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(field.at(face, bad, 0, 8.0).geometryMetres, 0.0);
    CHECK_EQ(field.at(face, 0, 0, 0.0).geometryMetres, 0.0);
    CHECK_EQ(field.at({}, 0, 0, 8.0).geometryMetres, 0.0);
    const HeightAt broken = [](double, double) {
        return std::numeric_limits<double>::quiet_NaN();
    };
    CHECK_EQ(field.at(broken, 100, 100, 8.0).geometryMetres, 0.0);

    // Settings that ask for nothing are taken to their bounds rather than
    // dividing by them.
    ErosionSettings silly;
    silly.talusDegrees = 0;
    silly.talusRings = 99;
    silly.gullyScaleMetres = 0;
    const ErosionField guarded(7, silly);
    CHECK(guarded.settings().talusDegrees > 0);
    CHECK(guarded.settings().talusRings <= 6);
    CHECK(guarded.settings().gullyScaleMetres > 0);
    CHECK(std::isfinite(guarded.at(face, 100, 100, 8.0).geometryMetres));
}
