#pragma once
// Camera control and picking: the two things that make a strategy game feel like
// one rather than like a diagram you can click.
//
// The camera is driven the way every game of this kind drives it - the edges of
// the screen, a dragged mouse, the keyboard, and the wheel zooming towards what
// is under the pointer - and it carries momentum, so a flick keeps going and a
// stop settles rather than snapping.
//
// Picking is done against colliders: every thing on screen has a box, the boxes
// are sorted front to back, and a click takes the front one. A drag takes every
// person inside the rectangle. Neither asks the tile grid anything, because what
// the player aims at is the body they can see, not the square it stands on.

#include <vector>

#include "game/client/camera.hpp"
#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"
#include "engine/ui/ui.hpp"

namespace client {

// What the camera is doing between frames.
struct CameraControl {
    double velocityX = 0, velocityY = 0;
    bool dragging = false;
    bool orbiting = false;
    double dragFromX = 0, dragFromY = 0;   // world position under the pointer when the drag began
    bool edgePan = true;
    bool relativeLook = false;
    double lookDeltaX = 0, lookDeltaY = 0;

    // One frame of it. `keys` is SDL's keyboard state; `seconds` the frame time.
    void update(Camera& camera, const ui::Input& input, const bool* keys, double seconds,
                bool pointerOverUi);
};

// A thing on screen and the box you can hit it in. Depth decides what is in
// front: bigger is nearer the viewer, which here means further down the screen.
struct Collider {
    ui::Rect box;
    Selection what;
    float depth = 0;
};

// Everything visible, with its box. Built fresh each time it is needed: a click
// happens once and a drag ends once, so there is nothing to cache and nothing to
// keep in step with the world.
std::vector<Collider> collidersOnScreen(const sim::World& w, const Camera& camera);

// The thing under the pointer, front-most first. Clicking the same spot again
// walks down through whatever is stacked there, so a pawn standing on a pile
// never hides the pile.
Selection pickAt(const sim::World& w, const Camera& camera, float screenX, float screenY,
                 const Selection& current);

// Everything of one kind inside a dragged rectangle. People first: a box drag in
// a game like this means "these people", and picking up the ground they are
// standing on with them would make the box useless.
std::vector<Selection> pickInBox(const sim::World& w, const Camera& camera, const ui::Rect& box);

} // namespace client
