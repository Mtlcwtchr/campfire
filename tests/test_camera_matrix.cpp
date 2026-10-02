// The projection, as a matrix, against the projection as it was written out.
//
// The matrix is not a rewrite that "should be equivalent": it is the same
// projection, and the only way to know that is to hold the two side by side over
// a range of the world and compare. The formula below is the one that was in the
// camera and in every shader before the matrix replaced it, kept here on purpose
// - if somebody changes the projection on purpose, this test is where they find
// out what they changed it from.

#include "framework.hpp"

#include <cmath>

#include "game/client/camera.hpp"

namespace {

client::Camera looking() {
    client::Camera cam;
    cam.isometric = true;
    cam.centreX = 1234.5;
    cam.centreY = -678.25;
    cam.focusHeight = 42.75;
    cam.pixelsPerTile = 3.7;
    cam.viewportWidth = 1280;
    cam.viewportHeight = 800;
    return cam;
}

} // namespace

TEST(the_matrix_is_the_projection_that_was_written_out_by_hand) {
    const client::Camera cam = looking();
    const double span = 1.0 / 4321.0, centre = 0.0;
    float m[16];
    cam.viewProjection(m, centre, span);

    double worstScreen = 0, worstDepth = 0;
    int compared = 0;
    for (double wx = -5000; wx <= 5000; wx += 613)
        for (double wy = -5000; wy <= 5000; wy += 517)
            for (double wz = -200; wz <= 1400; wz += 271) {
                // Classic 2:1 dimetric, as it was.
                const double dx = wx - cam.centreX, dy = wy - cam.centreY;
                const double dz = wz - cam.focusHeight;
                const double wasX =
                        (dx - dy) * cam.pixelsPerTile * 0.5 + cam.viewportWidth / 2.0;
                const double wasY = (dx + dy) * cam.pixelsPerTile * 0.25 -
                                    dz * cam.pixelsPerTile * 0.5 + cam.viewportHeight / 2.0;
                // Reversed depth (nearer is larger): one minus the old slab.
                const double wasDepth = 0.5 + ((dx + dy + 2.0 * dz) - centre) * span;

                const double ndcX = m[0] * wx + m[1] * wy + m[2] * wz + m[3];
                const double ndcY = m[4] * wx + m[5] * wy + m[6] * wz + m[7];
                const double ndcZ = m[8] * wx + m[9] * wy + m[10] * wz + m[11];
                worstScreen = std::max(
                        worstScreen,
                        std::abs((ndcX * 0.5 + 0.5) * cam.viewportWidth - wasX));
                worstScreen = std::max(
                        worstScreen, std::abs((0.5 - ndcY * 0.5) * cam.viewportHeight - wasY));
                worstDepth = std::max(worstDepth, std::abs(ndcZ - wasDepth));

                // And the screen position the rest of the client asks for, which
                // now goes through the same numbers.
                float sx = 0, sy = 0;
                cam.worldToScreen3D(wx, wy, wz, sx, sy);
                worstScreen = std::max(worstScreen, std::abs(sx - wasX));
                worstScreen = std::max(worstScreen, std::abs(sy - wasY));
                ++compared;
            }
    CHECK(compared > 500);
    // A hundredth of a pixel: what is left is the matrix being built in doubles
    // and handed over as floats.
    CHECK(worstScreen < 0.01);
    CHECK(worstDepth < 1e-6);
}

TEST(the_flat_projection_is_still_flat) {
    client::Camera cam = looking();
    cam.isometric = false;
    float m[16];
    cam.viewProjection(m, 0, 1.0 / 1000.0);
    // Height moves nothing on the screen in the top-down view - the art is drawn
    // from above - so the height column of the two screen rows is nought.
    CHECK(m[2] == 0.0f);
    CHECK(m[6] == 0.0f);
    // And a metre east is the same number of pixels as a metre south.
    float ax = 0, ay = 0, bx = 0, by = 0;
    cam.worldToScreen3D(0, 0, 0, ax, ay);
    cam.worldToScreen3D(1, 1, 0, bx, by);
    CHECK(std::abs((bx - ax) - (by - ay)) < 0.001);
}

TEST(camera_rotated_map_projection_and_height_inverse_agree) {
    auto cam = looking();
    float beforeX, beforeY, afterX, afterY;
    cam.worldToScreen3D(1300, -600, 80, beforeX, beforeY);
    cam.orbit(0.8, 0.3);
    cam.worldToScreen3D(1300, -600, 80, afterX, afterY);
    CHECK(std::hypot(afterX - beforeX, afterY - beforeY) > 10);
    for (double yaw : {-2.8, 0.4, 2.5})
        for (double pitch : {0.15, 0.8, 1.35}) {
            cam.yaw = yaw; cam.pitch = pitch;
            for (int x : {20, 640, 1260}) for (int y : {20, 400, 780}) {
                double wx, wy;
                cam.worldOfScreenAtHeight(x, y, 120.0, wx, wy);
                float sx, sy;
                cam.worldToScreen3D(wx, wy, 120.0, sx, sy);
                CHECK(std::abs(sx - x) < 0.01);
                CHECK(std::abs(sy - y) < 0.01);
            }
        }
}

TEST(camera_perspective_rays_project_back_at_all_orientations) {
    auto cam = looking();
    cam.centreX = 52000; cam.centreY = 35000; cam.focusHeight = 1500;
    for (auto mode : {client::Camera::Mode::Orbit, client::Camera::Mode::Free}) {
        cam.mode = mode;
        for (double yaw : {-2.8, 0.4, 2.5})
            for (double pitch : {-1.2, 0.0, 1.2}) {
                cam.yaw = yaw; cam.pitch = pitch;
                for (int x : {20, 640, 1260}) for (int y : {20, 400, 780}) {
                    const auto ray = cam.screenRay(x, y);
                    float sx, sy;
                    cam.worldToScreen3D(ray.origin[0] + ray.direction[0] * 1000,
                                        ray.origin[1] + ray.direction[1] * 1000,
                                        ray.origin[2] + ray.direction[2] * 1000, sx, sy);
                    CHECK(std::isfinite(sx) && std::isfinite(sy));
                    CHECK(std::abs(sx - x) < 0.05);
                    CHECK(std::abs(sy - y) < 0.05);
                }
            }
    }
}

TEST(camera_perspective_depth_is_one_to_zero_and_scales_with_distance) {
    client::Camera cam;
    cam.mode = client::Camera::Mode::Free;
    cam.yaw = cam.pitch = 0;
    cam.nearPlane = 1; cam.farPlane = 10000;
    float m[16];
    cam.viewProjection(m, 42, 0.001); // orthographic depth slab must be ignored
    const auto depth = [&](double distance) { return (-distance * m[8] + m[11]) / (-distance * m[12] + m[15]); };
    // Reversed: one at the near plane, nought at the far one, near / distance between.
    CHECK(std::abs(depth(1) - 1) < 1e-6);
    CHECK(std::abs(depth(10000)) < 1e-6);
    CHECK(depth(0.5) > 1);
    CHECK(depth(20000) < 0);
    // Relative precision kept out to the far plane: two points a metre apart
    // at five kilometres are many float steps apart.
    const float a = float(depth(5000)), b = float(depth(5001));
    CHECK(a > b);
    CHECK(a - b > 1000.0f * (a - std::nextafter(a, 0.0f)));
    float nearX, nearY, farX, farY;
    cam.worldToScreen3D(-100, -10, 0, nearX, nearY);
    cam.worldToScreen3D(-200, -10, 0, farX, farY);
    CHECK(std::abs((nearX - cam.viewportWidth * 0.5) / (farX - cam.viewportWidth * 0.5) - 2) < 0.001);
}

TEST(camera_orbit_free_switch_preserves_eye_and_free_zoom_changes_only_speed) {
    auto cam = looking();
    cam.setMode(client::Camera::Mode::Orbit);
    const auto eye = cam.eyePosition();
    cam.setMode(client::Camera::Mode::Free);
    CHECK_EQ(cam.eyePosition(), eye);
    const double zoom = cam.pixelsPerTile, speed = cam.flightSpeed;
    cam.zoomAt(2.0, 640, 400);
    CHECK_EQ(cam.pixelsPerTile, zoom);
    CHECK_EQ(cam.flightSpeed, speed * 2);
    cam.clampTo(1, 1);
    CHECK_EQ(cam.eyePosition(), eye);
    cam.setMode(client::Camera::Mode::Orbit);
    const auto restored = cam.eyePosition();
    for (int i = 0; i < 3; ++i) CHECK(std::abs(restored[i] - eye[i]) < 1e-8);
    cam.setMode(client::Camera::Mode::Free);
    cam.orbit(0.4, -1.4);
    cam.setMode(client::Camera::Mode::Orbit);
    const auto fromSky = cam.eyePosition();
    for (int i = 0; i < 3; ++i) CHECK(std::abs(fromSky[i] - eye[i]) < 1e-8);
}

TEST(camera_ground_bounds_contain_the_full_height_slab_in_rotated_views) {
    auto cam=looking();
    for (double yaw:{-2.8,0.4,2.5}) for (double pitch:{0.15,0.8,1.35}) {
        cam.yaw=yaw;cam.pitch=pitch;
        const auto box=cam.visibleGroundBounds(-50,700);
        CHECK_EQ(box,cam.visibleGroundBounds(700,-50));
        for (int x:{0,320,1280}) for (int y:{0,600,800}) for (double z:{-50.0,100.0,700.0}) {
            double wx,wy;
            cam.worldOfScreenAtHeight(x,y,z,wx,wy);
            CHECK(wx>=box[0]-1e-6 && wx<=box[2]+1e-6);
            CHECK(wy>=box[1]-1e-6 && wy<=box[3]+1e-6);
        }
    }
}

TEST(camera_ground_bounds_clip_the_frustum_not_just_corner_rays) {
    auto cam=looking();
    cam.centreX=cam.centreY=0;cam.focusHeight=100;
    cam.nearPlane=1;cam.farPlane=4000;
    std::size_t checked=0;
    for (auto mode:{client::Camera::Mode::Free,client::Camera::Mode::Orbit})
        for (double pitch:{-0.15,0.0,0.15,1.0}) for (double yaw:{0.0,1.7}) {
            cam.mode=mode;cam.pitch=pitch;cam.yaw=yaw;
            const auto box=cam.visibleGroundBounds(0,200);
            for (int y=0;y<=800;y+=20) for (int x=0;x<=1280;x+=80)
                for (double z:{0.0,100.0,200.0}) {
                    const auto ray=cam.screenRay(x,y);
                    if (std::abs(ray.direction[2])<1e-9) continue;
                    const double t=(z-ray.origin[2])/ray.direction[2];
                    if (t<cam.nearPlane || t>cam.farPlane) continue;
                    const double wx=ray.origin[0]+t*ray.direction[0];
                    const double wy=ray.origin[1]+t*ray.direction[1];
                    CHECK(wx>=box[0]-1e-6 && wx<=box[2]+1e-6);
                    CHECK(wy>=box[1]-1e-6 && wy<=box[3]+1e-6);
                    ++checked;
                }
        }
    CHECK(checked>1000);
    cam.mode=client::Camera::Mode::Free;cam.yaw=cam.pitch=0;
    const auto horizon=cam.visibleGroundBounds(0,0);
    CHECK(std::abs(horizon[0]+cam.farPlane)<1e-6);
    CHECK(horizon[2]<0); // no artificial camera-centre fallback for sky corners
}
