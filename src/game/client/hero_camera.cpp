#include "game/client/hero_camera.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace client {

void HeroCamera::take(Camera& camera, const Ground& ground, double facingOffset) {
    double x = camera.centreX, y = camera.centreY;
    if (camera.mode == Camera::Mode::Free) {
        x -= std::cos(camera.yaw) * 4.0;
        y -= std::sin(camera.yaw) * 4.0;
    }
    controller_.place(x, y, ground(x, y), camera.yaw + std::numbers::pi + facingOffset);
    camera.setMode(Camera::Mode::Orbit);
    camera.pitch = 0.32;
    camera.heightOffset = 0;
    active_ = true;
}

void HeroCamera::release(Camera& camera) {
    active_ = false;
    camera.heightOffset = 0;
    // Back to a view of the country around where the character stood.
    if (camera.mode == Camera::Mode::Orbit)
        camera.pixelsPerTile = camera.viewportHeight / (std::tan(camera.verticalFov * 0.5) * 60.0);
}

void HeroCamera::cycleView(Camera& camera) {
    if (camera.mode == Camera::Mode::Orbit) camera.setMode(Camera::Mode::Free);
    else if (camera.mode == Camera::Mode::Free) {
        camera.setMode(Camera::Mode::Map);
        camera.pitch = kDefaultCameraPitch;
        camera.pixelsPerTile = mapZoom;
    } else {
        mapZoom = camera.pixelsPerTile;
        camera.setMode(Camera::Mode::Orbit);
        camera.pitch = 0.32;
    }
}

void HeroCamera::update(Camera& camera, const Input& input, double seconds, const Ground& ground) {
    if (!active_) return;
    if (wantsMouse(camera)) {
        camera.orbit(input.mouseDx * 0.0035, input.mouseDy * 0.0035);
        if (camera.mode == Camera::Mode::Orbit) camera.pitch = std::clamp(camera.pitch, -0.35, 1.35);
    }
    if (camera.mode == Camera::Mode::Orbit && input.wheel != 0)
        distance = std::clamp(distance * std::pow(0.88, double(input.wheel)), 1.8, 14.0);
    if (camera.mode == Camera::Mode::Map && input.wheel != 0)
        camera.zoomAt(std::pow(1.15, double(input.wheel)), camera.viewportWidth / 2, camera.viewportHeight / 2);
    CharacterController::Input in;
    if (input.keys) {
        const bool* k = input.keys;
        in.forward = double(k[SDL_SCANCODE_W] || k[SDL_SCANCODE_UP]) - double(k[SDL_SCANCODE_S] || k[SDL_SCANCODE_DOWN]);
        in.right = double(k[SDL_SCANCODE_D] || k[SDL_SCANCODE_RIGHT]) - double(k[SDL_SCANCODE_A] || k[SDL_SCANCODE_LEFT]);
        in.sprint = k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT];
        in.walk = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT];
    }
    if (input.autoRun) {
        in.forward = 1;
        in.sprint = input.autoRunPace > 1.5;
        in.walk = input.autoRunPace < 0.75;
    }
    in.jump = input.jump;
    in.cameraYaw = camera.yaw;
    controller_.update(in, seconds, ground);
}

void HeroCamera::follow(Camera& camera, const Ground& ground, const game::CharacterModel* model) {
    if (!active_) return;
    const auto& hero = controller_;
    if (camera.mode == Camera::Mode::Orbit) {
        camera.pixelsPerTile = camera.viewportHeight / (std::tan(camera.verticalFov * 0.5) * distance);
        // Over the right shoulder, as the genre does: the character a little
        // left of the middle, the way ahead clear of him.
        const double shoulder = std::min(0.55, distance * 0.12);
        camera.centreX = hero.x() + std::sin(camera.yaw) * shoulder;
        camera.centreY = hero.y() - std::cos(camera.yaw) * shoulder;
        camera.focusHeight = hero.z() + CharacterController::kShoulderHeight;
        // The eye out of the ground: a slope behind pushes it up, not under.
        const auto eye = camera.eyePosition();
        const double floor = ground(eye[0], eye[1]) + 0.35;
        if (eye[2] < floor) camera.pitch = std::min(1.35, camera.pitch + (floor - eye[2]) / distance);
    } else if (camera.mode == Camera::Mode::Free) {
        // At the eyes, a little in front of the face, so the inside of the
        // hood is never in view (the head itself is not drawn).
        const double eye = model ? model->eyeHeight() : CharacterController::kEyeHeight;
        const double ahead = model ? model->eyeForward() : 0.12;
        camera.centreX = hero.x() + std::cos(hero.facing()) * ahead;
        camera.centreY = hero.y() + std::sin(hero.facing()) * ahead;
        camera.focusHeight = hero.z() + eye;
    } else {
        camera.centreX = hero.x();
        camera.centreY = hero.y();
        camera.focusHeight = hero.z();
    }
}

game::CharacterPass::State HeroCamera::state(const Camera& camera, const Ground& ground, double seconds) const {
    game::CharacterPass::State s;
    s.visible = active_;
    s.firstPerson = camera.mode == Camera::Mode::Free;
    s.x = controller_.x(); s.y = controller_.y(); s.z = controller_.z();
    s.yaw = controller_.facing();
    s.gait = controller_.gait();
    s.lookX = -std::cos(camera.yaw) * std::cos(camera.pitch);
    s.lookY = -std::sin(camera.yaw) * std::cos(camera.pitch);
    s.lookZ = -std::sin(camera.pitch);
    s.ground = ground;
    s.seconds = seconds;
    return s;
}

} // namespace client
