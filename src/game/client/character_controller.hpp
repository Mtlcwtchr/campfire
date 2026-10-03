#pragma once
// The player's character on the ground: WASD relative to where the camera
// looks, a jog by default, a sprint on Shift, a walk on Alt, a jump on Space.
//
// Kinematic, not physical: the feet follow the ground under them (the height
// field the explorer already asks for the camera's focus), the body turns
// towards where it is going at a bounded rate, and speed comes up and goes
// down over a fraction of a second. That last is what reads as weight - a
// character at full speed the frame a key goes down is a cursor.
//
// The gait (CharacterModel::Gait) comes out of the same numbers: the stride
// phase advances by distance covered over a stride length that grows with
// speed, so the feet do not skate.
//
// Mouse-driven movement on the map (click to walk there) wants a path over
// the ground, and a path wants a navigation mesh baked per chunk; that is
// planned (doc/plan_character_2026-10-02.md), not here.
#include <functional>

#include "game/render/character_model.hpp"

namespace client {

class CharacterController {
public:
    struct Input {
        double forward = 0, right = 0;   // -1..1 each, from the keys
        bool sprint = false, walk = false, jump = false;
        double cameraYaw = 0;            // Camera::yaw: the camera looks along -(cos, sin)
    };
    using Ground = std::function<double(double, double)>;

    void place(double x, double y, double ground, double facing);
    void update(const Input& input, double seconds, const Ground& ground);

    [[nodiscard]] double x() const { return x_; }
    [[nodiscard]] double y() const { return y_; }
    [[nodiscard]] double z() const { return z_; }
    [[nodiscard]] double facing() const { return facing_; }
    [[nodiscard]] double speed() const { return speed_; }
    [[nodiscard]] bool grounded() const { return grounded_; }
    [[nodiscard]] game::CharacterModel::Gait gait() const {
        return {phase_, grounded_ ? groundSpeed_ : 0.0, turn_, time_, vz_, grounded_, jumped_, acceleration_, slope_, impact_};
    }

    static constexpr double kWalk = 1.6, kJog = 4.2, kSprint = 7.0;   // metres a second
    static constexpr double kEyeHeight = 1.68;                         // first person
    static constexpr double kShoulderHeight = 1.55;                    // third person aims here

private:
    double x_ = 0, y_ = 0, z_ = 0, facing_ = 0;
    double vx_ = 0, vy_ = 0, vz_ = 0;
    double speed_ = 0, phase_ = 0, turn_ = 0, time_ = 0;
    double groundSpeed_ = 0, acceleration_ = 0, slope_ = 0, impact_ = 0;
    bool grounded_ = true, jumped_ = false;
    bool jumpHeld_ = false;
};

} // namespace client
