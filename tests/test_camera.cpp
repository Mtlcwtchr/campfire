#include "framework.hpp"

#include <cmath>

#include "game/client/camera.hpp"

namespace {

bool near(double a, double b, double epsilon = 0.001) {
    return std::abs(a - b) <= epsilon;
}

} // namespace

TEST(camera_top_down_projection_is_unchanged) {
    client::Camera camera;
    camera.viewportWidth = 800;
    camera.viewportHeight = 600;
    camera.centreX = 10;
    camera.centreY = 20;
    camera.pixelsPerTile = 40;

    float sx = 0, sy = 0;
    camera.worldToScreen3D(12, 17, 900, sx, sy);
    CHECK(near(sx, 480));
    CHECK(near(sy, 180));

    double wx = 0, wy = 0;
    camera.worldOfScreen(480, 180, wx, wy);
    CHECK(near(wx, 12));
    CHECK(near(wy, 17));
}

TEST(camera_isometric_projection_uses_height_and_is_invertible) {
    client::Camera camera;
    camera.viewportWidth = 800;
    camera.viewportHeight = 600;
    camera.centreX = 10;
    camera.centreY = 20;
    camera.focusHeight = 100;
    camera.pixelsPerTile = 40;
    camera.isometric = true;

    float sx = 0, sy = 0;
    camera.worldToScreen3D(12, 20, 103, sx, sy);
    CHECK(near(sx, 440));
    CHECK(near(sy, 260));

    double wx = 0, wy = 0;
    camera.worldOfScreenAtHeight(440, 260, 103, wx, wy);
    CHECK(near(wx, 12));
    CHECK(near(wy, 20));
}

TEST(camera_isometric_ground_plane_round_trips_and_zoom_stays_anchored) {
    client::Camera camera;
    camera.viewportWidth = 1000;
    camera.viewportHeight = 700;
    camera.centreX = 120;
    camera.centreY = 80;
    camera.focusHeight = 35;
    camera.pixelsPerTile = 20;
    camera.isometric = true;

    double beforeX = 0, beforeY = 0;
    camera.worldOfScreen(740, 210, beforeX, beforeY);
    camera.zoomAt(1.6, 740, 210);
    double afterX = 0, afterY = 0;
    camera.worldOfScreen(740, 210, afterX, afterY);

    CHECK(near(beforeX, afterX));
    CHECK(near(beforeY, afterY));
    CHECK(near(camera.tileRadiusX(), 16));
    CHECK(near(camera.tileRadiusY(), 8));
}

