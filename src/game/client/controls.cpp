#include "game/client/controls.hpp"

#include <algorithm>
#include <cmath>

namespace client {
namespace {

// How the camera moves. Metres a second at the reference zoom, and how quickly a
// push decays once the player lets go: enough that a flick carries and a stop
// settles inside a breath.
constexpr double kKeyboardSpeed = 26.0;
constexpr double kEdgeSpeed = 22.0;
constexpr int kEdgeMargin = 12;
constexpr double kDamping = 12.0;
constexpr double kZoomStep = 1.16;

// How big a thing is to a click, in tiles. A person is not their tile: they are
// a body about half a tile wide standing on it, and a click within that is a
// click on them.
constexpr float kPersonHalfWidth = 0.42f;
constexpr float kPersonHeight = 1.35f;
constexpr float kAnimalHalfWidth = 0.5f;
constexpr float kAnimalHeight = 0.9f;
constexpr float kStackHalf = 0.4f;
constexpr float kNodeHalf = 0.5f;

ui::Rect boxAround(const Camera& camera, double worldX, double worldY, float halfWidth,
                   float height) {
    float sx = 0, sy = 0;
    camera.worldToScreen(worldX, worldY, sx, sy);
    const float w = halfWidth * 2 * static_cast<float>(camera.pixelsPerTile);
    const float h = height * static_cast<float>(camera.pixelsPerTile);
    return {sx - w * 0.5f, sy - h, w, h};
}

} // namespace

void CameraControl::update(Camera& camera, const ui::Input& input, const bool* keys, double seconds,
                           bool pointerOverUi) {
    const double dt = std::clamp(seconds, 0.0, 0.1);
    if (pointerOverUi) {
        dragging = false;
        orbiting = false;
    }
    if (camera.perspective()) {
        velocityX = velocityY = 0;
        if (input.rightDown && !pointerOverUi) {
            if (orbiting) camera.orbit((input.mouseX - dragFromX) * 0.006,
                                      (input.mouseY - dragFromY) * 0.006);
            dragFromX = input.mouseX; dragFromY = input.mouseY;
            orbiting = true;
        } else orbiting = false;
        if (keys && !pointerOverUi) {
            const double side = double(keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) -
                                double(keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
            const double forward = double(keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) -
                                   double(keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
            const double up = double(keys[SDL_SCANCODE_E] || keys[SDL_SCANCODE_PAGEUP]) -
                              double(keys[SDL_SCANCODE_Q] || keys[SDL_SCANCODE_PAGEDOWN]);
            const bool flying = camera.mode == Camera::Mode::Free;
            const double cp = flying ? std::cos(camera.pitch) : 1.0;
            double dx = std::sin(camera.yaw) * side - std::cos(camera.yaw) * cp * forward;
            double dy = -std::cos(camera.yaw) * side - std::sin(camera.yaw) * cp * forward;
            double dz = up - (flying ? std::sin(camera.pitch) * forward : 0.0);
            const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double speed = flying ? camera.flightSpeed : std::max(10.0, camera.orbitDistance() * 0.5);
            const double boost = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] ? 4.0 : 1.0;
            if (length > 0) {
                const double distance = speed * boost * dt / length;
                camera.pan(dx * distance, dy * distance);
                camera.focusHeight += dz * distance;
                if (!flying) camera.heightOffset += dz * distance;
            }
            if (keys[SDL_SCANCODE_HOME]) {
                camera.yaw = kDefaultCameraYaw; camera.pitch = kDefaultCameraPitch;
                camera.heightOffset = 0;
            }
        }
        dragging = false;
        if (input.wheel && !pointerOverUi)
            camera.zoomAt(std::pow(kZoomStep, input.wheel), int(input.mouseX), int(input.mouseY));
        return;
    }
    // Everything is in world metres a second, scaled by how far out the camera
    // is: pushed back over a country, a keypress should cross country.
    const double scale = std::max(1.0, 34.0 / std::max(0.05, camera.pixelsPerTile));
    const auto pushInView = [&](double screenX, double screenY, double speed) {
        double worldX = screenX;
        double worldY = screenY;
        if (camera.isometric) {
            float m[16];
            camera.viewProjection(m, 0, 1);
            const double x = screenX / std::max(1, camera.viewportWidth);
            const double y = -screenY / std::max(1, camera.viewportHeight);
            const double determinant = double(m[0]) * m[5] - double(m[1]) * m[4];
            worldX = (x * m[5] - y * m[1]) / determinant;
            worldY = (y * m[0] - x * m[4]) / determinant;
        }
        const double length = std::hypot(worldX, worldY);
        if (length <= 0.0) return;
        velocityX += worldX / length * speed * scale * dt * 60.0;
        velocityY += worldY / length * speed * scale * dt * 60.0;
    };

    double pushX = 0, pushY = 0;
    if (keys != nullptr) {
        if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) pushY -= 1;
        if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]) pushY += 1;
        if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]) pushX -= 1;
        if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) pushX += 1;
    }
    if (pushX != 0 || pushY != 0) {
        pushInView(pushX, pushY, kKeyboardSpeed);
    }

    // The edge of the screen, unless the pointer is on the interface: a HUD that
    // runs to the bottom of the window would otherwise scroll the map whenever
    // somebody reached for it.
    //
    // And only while the pointer is actually in the window. Outside it, SDL
    // still reports a position, and a position outside the window is past the
    // edge margin by definition - so a window sitting in the background with the
    // cursor anywhere else on the desktop scrolls itself, for ever, at fifteen
    // metres a frame. Every screenshot this project has taken was of a slowly
    // moving camera because of it.
    const bool pointerInside = input.mouseX >= 0 && input.mouseY >= 0 &&
                               input.mouseX < camera.viewportWidth &&
                               input.mouseY < camera.viewportHeight;
    if (edgePan && pointerInside && !pointerOverUi && !dragging) {
        double edgeX = 0, edgeY = 0;
        if (input.mouseX <= kEdgeMargin) edgeX = -1;
        else if (input.mouseX >= camera.viewportWidth - kEdgeMargin) edgeX = 1;
        if (input.mouseY <= kEdgeMargin) edgeY = -1;
        else if (input.mouseY >= camera.viewportHeight - kEdgeMargin) edgeY = 1;
        pushInView(edgeX, edgeY, kEdgeSpeed);
    }

    // Dragging the map with the middle button: the ground stays under the
    // pointer, which is the only way a drag ever feels right.
    if (input.rightDown && !pointerOverUi) {
        if (!orbiting) {
            orbiting = true;
            dragFromX = input.mouseX;
            dragFromY = input.mouseY;
        } else {
            camera.orbit((input.mouseX - dragFromX) * 0.006,
                         (input.mouseY - dragFromY) * 0.006);
            dragFromX = input.mouseX;
            dragFromY = input.mouseY;
        }
        velocityX = velocityY = 0;
    } else {
        orbiting = false;
    }

    if (input.middleDown && !pointerOverUi && !orbiting) {
        double wx = 0, wy = 0;
        camera.worldOfScreen(static_cast<int>(input.mouseX), static_cast<int>(input.mouseY), wx, wy);
        if (!dragging) {
            dragging = true;
            dragFromX = wx;
            dragFromY = wy;
            velocityX = velocityY = 0;
        } else {
            camera.pan(dragFromX - wx, dragFromY - wy);
        }
    } else {
        dragging = false;
    }

    camera.pan(velocityX * dt, velocityY * dt);
    const double decay = std::exp(-kDamping * dt);
    velocityX *= decay;
    velocityY *= decay;
    if (std::abs(velocityX) < 0.001) velocityX = 0;
    if (std::abs(velocityY) < 0.001) velocityY = 0;

    if (input.wheel != 0 && !pointerOverUi) {
        const double factor = input.wheel > 0 ? kZoomStep : 1.0 / kZoomStep;
        camera.zoomAt(factor, static_cast<int>(input.mouseX), static_cast<int>(input.mouseY));
    }
    if (keys != nullptr && !pointerOverUi) {
        const double previousOffset = camera.heightOffset;
        if (keys[SDL_SCANCODE_PAGEUP] || keys[SDL_SCANCODE_E]) camera.heightOffset += 80.0 * dt;
        if (keys[SDL_SCANCODE_PAGEDOWN] || keys[SDL_SCANCODE_Q]) camera.heightOffset -= 80.0 * dt;
        camera.heightOffset = std::clamp(camera.heightOffset, -10000.0, 10000.0);
        camera.focusHeight += camera.heightOffset - previousOffset;
        if (keys[SDL_SCANCODE_HOME]) {
            camera.focusHeight -= camera.heightOffset;
            camera.heightOffset = 0;
            camera.yaw = kDefaultCameraYaw;
            camera.pitch = kDefaultCameraPitch;
        }
    }
}

std::vector<Collider> collidersOnScreen(const sim::World& w, const Camera& camera) {
    std::vector<Collider> out;
    const ui::Rect screen{0, 0, static_cast<float>(camera.viewportWidth),
                          static_cast<float>(camera.viewportHeight)};
    const auto keep = [&](Collider c) {
        if (c.box.right() < screen.x || c.box.x > screen.right()) return;
        if (c.box.bottom() < screen.y || c.box.y > screen.bottom()) return;
        out.push_back(c);
    };

    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        Collider c;
        c.box = boxAround(camera, p.pos.x.toDouble(), p.pos.y.toDouble(), kPersonHalfWidth,
                          kPersonHeight);
        c.what.kind = SelectionKind::Person;
        c.what.person = p.id;
        c.what.tile = p.tile;
        c.depth = c.box.bottom() + 2000;      // bodies are in front of the ground
        keep(c);
    }
    for (const auto& a : w.animals()) {
        if (!a.alive) continue;
        Collider c;
        c.box = boxAround(camera, a.pos.x.toDouble(), a.pos.y.toDouble(), kAnimalHalfWidth,
                          kAnimalHeight);
        c.what.kind = SelectionKind::Animal;
        c.what.animal = a.id;
        c.what.tile = a.tile;
        c.depth = c.box.bottom() + 1800;
        keep(c);
    }
    for (const auto& s : w.stacks()) {
        if (!s.alive || s.count <= 0) continue;
        if (s.where != sim::StackWhere::Ground && s.where != sim::StackWhere::InBuilding) continue;
        const core::TilePos at = s.where == sim::StackWhere::InBuilding
                                         ? w.building(s.building).origin
                                         : s.tile;
        Collider c;
        c.box = boxAround(camera, at.x, at.y + 0.3, kStackHalf, kStackHalf * 2);
        c.what.kind = SelectionKind::Stack;
        c.what.stack = s.id;
        c.what.stackGeneration = s.generation;
        c.what.tile = at;
        c.depth = c.box.bottom() + 1400;
        keep(c);
    }
    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        const auto& def = w.db().building(b.def);
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        camera.worldToScreen(b.origin.x - 0.5, b.origin.y - 0.5, x0, y0);
        camera.worldToScreen(b.origin.x + def.footprintWidth - 0.5,
                             b.origin.y + def.footprintDepth - 0.5, x1, y1);
        Collider c;
        c.box = {x0, y0, x1 - x0, y1 - y0};
        c.what.kind = SelectionKind::Building;
        c.what.building = b.id;
        c.what.tile = b.origin;
        c.depth = c.box.bottom() + 1000;
        keep(c);
    }
    for (const auto& n : w.nodes()) {
        if (!n.alive) continue;
        Collider c;
        c.box = boxAround(camera, n.tile.x, n.tile.y + 0.3, kNodeHalf, kNodeHalf * 2.2f);
        c.what.kind = SelectionKind::Resource;
        c.what.node = n.id;
        c.what.tile = n.tile;
        c.depth = c.box.bottom() + 600;
        keep(c);
    }
    // Front to back.
    std::sort(out.begin(), out.end(), [](const Collider& a, const Collider& b) {
        return a.depth > b.depth;
    });
    return out;
}

Selection pickAt(const sim::World& w, const Camera& camera, float screenX, float screenY,
                 const Selection& current) {
    const std::vector<Collider> colliders = collidersOnScreen(w, camera);
    std::vector<Selection> under;
    for (const auto& c : colliders)
        if (c.box.contains(screenX, screenY)) under.push_back(c.what);

    // The ground is always the last thing under the pointer: a click never comes
    // back empty, because a tile has terrain, soil and whatever areas cover it.
    Selection ground;
    ground.kind = SelectionKind::Tile;
    ground.tile = camera.screenToTile(static_cast<int>(screenX), static_cast<int>(screenY));
    under.push_back(ground);

    // Clicking the same place again walks down the pile.
    for (std::size_t i = 0; i < under.size(); ++i)
        if (under[i] == current) return under[(i + 1) % under.size()];
    return under.front();
}

std::vector<Selection> pickInBox(const sim::World& w, const Camera& camera, const ui::Rect& box) {
    std::vector<Selection> out;
    for (const auto& c : collidersOnScreen(w, camera)) {
        if (c.what.kind != SelectionKind::Person) continue;
        const bool overlaps = c.box.x < box.right() && c.box.right() > box.x &&
                              c.box.y < box.bottom() && c.box.bottom() > box.y;
        if (overlaps) out.push_back(c.what);
    }
    return out;
}

} // namespace client
