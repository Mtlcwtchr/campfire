#include "framework.hpp"

#include <cmath>

#include "engine/render/systems/horizon_cull.hpp"

namespace {
using namespace engine::render;

// A ridge a hundred metres out and forty metres above the eye, filling one
// quarter of the sky - the far side of a valley, which is what this is for.
Horizon ridgeAt(double distance, double height, double fromAngle, double toAngle) {
    Horizon horizon;
    horizon.clear();
    horizon.eyeX = 0;
    horizon.eyeY = 0;
    horizon.eyeZ = 0;
    for (int bin = 0; bin < Horizon::kBins; ++bin) {
        const double angle = (double(bin) + 0.5) / Horizon::kBins * 6.283185307179586 -
                             3.141592653589793;
        if (angle < fromAngle || angle > toAngle) continue;
        horizon.raise(std::cos(angle) * distance, std::sin(angle) * distance, height);
    }
    return horizon;
}
}

TEST(horizon_hides_what_the_ground_stands_in_front_of) {
    // The ridge is a hundred metres away and forty up, so its skyline is at a
    // slope of 0.4. Everything past it and below that line is behind it.
    const auto horizon = ridgeAt(100, 40, -0.4, 0.4);

    // Five hundred metres out along the ridge's bearing, and well under its
    // skyline: a tree there is behind a hill and nobody can see it.
    CHECK(horizon.hides(500, 0, 60, 7));
    // The same distance, but standing high enough to break the skyline.
    CHECK(!horizon.hides(500, 0, 260, 7));
    // In FRONT of the ridge, and low. The ridge cannot hide what is nearer
    // than itself, however low that is - which is the case a horizon that
    // forgot to record its own distance gets wrong.
    CHECK(!horizon.hides(60, 0, -10, 7));
    // Off to the side, where nothing was recorded at all.
    CHECK(!horizon.hides(0, 500, -50, 7));
}

TEST(horizon_keeps_anything_it_is_not_certain_about) {
    // The whole design: wrong in one direction only. A thing is culled when
    // EVERY bin its width touches buries it, so one gap in the ridge saves it.
    Horizon horizon = ridgeAt(100, 40, -0.4, 0.4);
    const int middle = Horizon::binOf(1, 0);
    horizon.rise[middle] = -1e9f;   // a notch in the skyline, straight ahead
    horizon.reach[middle] = 0.0f;
    CHECK(!horizon.hides(500, 0, 60, 7));

    // An empty horizon hides nothing at all, which is what a frame with no
    // ground resident yet has to do.
    Horizon nothing;
    nothing.clear();
    CHECK(!nothing.hides(500, 0, -100, 7));
    CHECK(!nothing.active);

    // A sphere large enough to straddle the ridge's edge is kept: its width
    // reaches bins that do not bury it.
    const auto narrow = ridgeAt(100, 40, -0.02, 0.02);
    CHECK(!narrow.hides(500, 0, 60, 40));
}

TEST(horizon_finds_the_eye_in_a_view_projection) {
    // The same basis camera.cpp builds: right, up and forward orthonormal, with
    // the translation folded in as minus their dot with the eye.
    const double eye[3]{1200, -430, 95};
    const double right[3]{0, -1, 0}, up[3]{0, 0, 1}, forward[3]{1, 0, 0};
    const double focal = 1.7;
    float rowX[4], rowY[4], rowW[4];
    for (int i = 0; i < 3; ++i) {
        rowX[i] = float(right[i] * focal);
        rowY[i] = float(up[i] * focal);
        rowW[i] = float(forward[i]);
    }
    rowX[3] = rowY[3] = rowW[3] = 0;
    for (int i = 0; i < 3; ++i) {
        rowX[3] -= float(rowX[i] * eye[i]);
        rowY[3] -= float(rowY[i] * eye[i]);
        rowW[3] -= float(rowW[i] * eye[i]);
    }
    double found[3]{};
    CHECK(eyeFrom(rowX, rowY, rowW, found));
    for (int i = 0; i < 3; ++i) CHECK(std::abs(found[i] - eye[i]) < 0.5);

    // An orthographic frame has no single centre of projection, and saying so
    // is better than returning a point at infinity for the horizon to use.
    float flat[4]{0, 0, 0, 1};
    CHECK(!eyeFrom(rowX, rowY, flat, found));
}
