#include "game/client/character_controller.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include "engine/animation/procedural.hpp"

namespace client {

void CharacterController::place(double x, double y, double ground, double facing) {
    x_ = x; y_ = y; z_ = ground; facing_ = facing;
    vx_ = vy_ = vz_ = speed_ = turn_ = 0;
    groundSpeed_ = acceleration_ = slope_ = impact_ = phase_ = time_ = 0;
    grounded_ = true;
    jumped_ = jumpHeld_ = false;
}

void CharacterController::update(const Input& input, double seconds, const Ground& ground) {
    const double dt = std::clamp(seconds, 0.0, 0.1);
    time_ += dt;
    jumped_ = false;
    impact_ *= std::exp(-dt * 8.0);
    const double beforeX = x_, beforeY = y_, beforeSpeed = groundSpeed_;
    const bool jumpPressed = input.jump && !jumpHeld_;
    jumpHeld_ = input.jump;
    // The wanted direction on the ground, in the camera's frame.
    const double c = std::cos(input.cameraYaw), s = std::sin(input.cameraYaw);
    double wx = -c * input.forward + s * input.right;
    double wy = -s * input.forward - c * input.right;
    const double length = std::hypot(wx, wy);
    const double top = input.sprint ? kSprint : input.walk ? kWalk : kJog;
    double target = 0;
    if (length > 1e-6) { wx /= length; wy /= length; target = top; }

    // Speed: up in about a third of a second, down a little faster; in the
    // air nothing changes but gravity.
    if (grounded_) {
        const double rate = target > speed_ ? 14.0 : 18.0;
        speed_ += std::clamp(target - speed_, -rate * dt, rate * dt);
        // Turning: towards where the keys point, faster at a walk than at a
        // sprint, never instant.
        double wanted = facing_;
        if (target > 0) wanted = std::atan2(wy, wx);
        double delta = std::remainder(wanted - facing_, 2 * std::numbers::pi);
        const double maxTurn = (speed_ > kJog ? 7.0 : 11.0) * dt;
        const double step = std::clamp(delta, -maxTurn, maxTurn);
        facing_ = std::remainder(facing_ + step, 2 * std::numbers::pi);
        turn_ += (step / std::max(dt, 1e-4) - turn_) * std::min(1.0, dt * 8.0);
        // The body goes the way it faces (it is turning into the keys), so a
        // reversal is a quick turn, not a moonwalk.
        vx_ = std::cos(facing_) * speed_;
        vy_ = std::sin(facing_) * speed_;
        if (jumpPressed) { vz_ = 4.6; grounded_ = false; jumped_ = true; }
    }
    const double nx = x_ + vx_ * dt, ny = y_ + vy_ * dt;
    // Too steep to climb: the move is refused, the speed kept for the turn.
    const double here = ground(x_, y_), there = ground(nx, ny);
    const double rise = there - here, run = std::hypot(nx - x_, ny - y_);
    if (!(grounded_ && run > 1e-6 && rise / run > 1.2)) { x_ = nx; y_ = ny; }
    const double floor = ground(x_, y_);
    if (grounded_) {
        // Down a slope the feet stay on it; off a ledge more than a stride
        // high the character falls.
        if (z_ - floor > 0.6) { grounded_ = false; vz_ = 0; }
        else z_ = floor;
    }
    if (!grounded_) {
        vz_ -= 9.81 * dt;
        z_ += vz_ * dt;
        if (z_ <= floor) { impact_ = std::max(impact_, -vz_); z_ = floor; vz_ = 0; grounded_ = true; }
    }
    const double travelled = std::hypot(x_ - beforeX, y_ - beforeY);
    const double actualSpeed = dt > 1e-6 ? travelled / dt : 0;
    groundSpeed_ += (actualSpeed - groundSpeed_) * (1 - std::exp(-dt * 18.0));
    const double acceleration = dt > 1e-6 ? (groundSpeed_ - beforeSpeed) / dt : 0;
    acceleration_ += (acceleration - acceleration_) * (1 - std::exp(-dt * 10.0));
    const double grade = travelled > 1e-5 ? (floor - here) / travelled : 0;
    slope_ += (std::clamp(grade, -1.0, 1.0) - slope_) * (1 - std::exp(-dt * 8.0));
    // A blocked move advances no stride. The same style distances are used
    // by the procedural gait, including walk/jog/sprint transitions.
    if (grounded_) phase_ = std::fmod(phase_ + 2 * std::numbers::pi * travelled /
        engine::animation::gaitStride(float(groundSpeed_)), 2 * std::numbers::pi);
}

} // namespace client
