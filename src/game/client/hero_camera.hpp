#pragma once
// The player's character and the camera that goes with it, for any view that
// shows the world (the game client, the explorer): taking it and letting it
// go, the third person / first person / map cycle, the keys and the mouse,
// the camera following, and what the renderer is told (CharacterPass::State).
//
//   third person   orbit over the right shoulder, the mouse turns it, the
//                  wheel sets the distance; the eye kept out of the ground
//   first person   at the head's eyes, a little ahead of the face; the body
//                  drawn without its head
//   map            the orthographic map over the character
//
// Keyboard: WASD relative to the camera, Shift sprint, Alt walk, Space jump.
#include <functional>

#include "game/client/camera.hpp"
#include "game/client/character_controller.hpp"
#include "game/render/passes/character_pass.hpp"

namespace client {

class HeroCamera {
public:
    using Ground = std::function<double(double, double)>;

    [[nodiscard]] bool active() const { return active_; }
    // Puts the character on the ground where the camera looks, facing away
    // from it, and the camera behind it.
    void take(Camera& camera, const Ground& ground, double facingOffset = 0);
    void release(Camera& camera);
    // Third person -> first person -> map -> third person.
    void cycleView(Camera& camera);
    // Whether the mouse should be captured (the views that look with it).
    [[nodiscard]] bool wantsMouse(const Camera& camera) const { return active_ && camera.mode != Camera::Mode::Map; }

    struct Input {
        const bool* keys = nullptr;   // SDL scancode state, or null
        float mouseDx = 0, mouseDy = 0;
        float wheel = 0;
        bool jump = false;
        bool autoRun = false;          // forward without keys (shots, tours)
        double autoRunPace = 1;        // 0.5 walk, 1 jog, 2 sprint
    };
    void update(Camera& camera, const Input& input, double seconds, const Ground& ground);
    // After everything else has moved the camera: it goes to the character.
    void follow(Camera& camera, const Ground& ground, const game::CharacterModel* model);
    [[nodiscard]] game::CharacterPass::State state(const Camera& camera, const Ground& ground, double seconds) const;

    CharacterController& controller() { return controller_; }
    double distance = 4.2;     // third person, metres
    double mapZoom = 8.0;      // pixels a metre on the map

private:
    CharacterController controller_;
    bool active_ = false;
};

} // namespace client
