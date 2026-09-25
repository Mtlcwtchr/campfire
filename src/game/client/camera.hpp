#pragma once
// Pan and zoom. The camera is client state and never touches the simulation
// (GDD 11: the core does not know a renderer exists).
//
// It works in world metres rather than tile indices, because bodies stand
// between tiles and sprites are placed by their real height. Everything the
// camera is asked to place goes through a world position first.

#include "engine/core/geometry.hpp"

#include <array>
#include <cmath>
#include <numbers>

namespace client {

// Pixels per metre. A tile is one metre across, so this is also its screen width.
inline constexpr double kDefaultPixelsPerTile = 34.0;

// Flat top-down presentation: one metre has the same screen scale north/south as
// east/west. Painter order gives overlap without introducing perspective.
inline constexpr double kVerticalSquash = 1.0;
// Pulled back far enough that the whole local map fits on screen several times
// over: past that there is nothing left to read at tile scale, and the camera
// shows the country instead (D84). This is only the floor the camera starts
// with - once there is a world map to look at, main() lowers Camera::minZoom to
// whatever puts the whole country on screen, because a world a hundred and
// eighty kilometres across is two hundred thousand tiles and 0.9 pixels to the
// tile shows a thousandth of it.
inline constexpr double kMinPixelsPerTile = 0.9;
inline constexpr double kMaxPixelsPerTile = 96.0;
inline constexpr double kDefaultCameraYaw = 0.7853981633974483;
inline constexpr double kDefaultCameraPitch = 0.4636476090008061;
inline constexpr double kMinCameraPitch = 0.15;
inline constexpr double kMaxCameraPitch = 1.35;
// Where the detail goes. Below the first, bodies and goods stop being drawn as
// sprites and become marks; below the second the local map is left behind and
// the world map is drawn, with each settlement a marker on it.
inline constexpr double kSpriteDetailPixelsPerTile = 9.0;
inline constexpr double kWorldViewPixelsPerTile = 2.6;

struct Camera {
    enum class Mode { Map, Orbit, Free };
    Mode mode = Mode::Map;
    double verticalFov = std::numbers::pi / 3.0;
    double nearPlane = 0.5, farPlane = 200000.0;
    double flightSpeed = 80.0;
    bool perspective() const { return mode != Mode::Map; }
    const char* modeName() const;
    void setMode(Mode next);
    double orbitDistance() const;
    std::array<double, 3> eyePosition() const;
    struct Ray { std::array<double, 3> origin, direction; };
    Ray screenRay(double sx, double sy) const;
    // Centre of the view, in metres. Doubles: this is presentation, not
    // simulation, so it does not have to be deterministic.
    double centreX = 0;
    double centreY = 0;
    // The terrain explorer opts into a 2:1 orthographic view. The settlement
    // renderer leaves this false until its art is prepared for that projection.
    bool isometric = false;
    double focusHeight = 0;
    double yaw = kDefaultCameraYaw;
    double pitch = kDefaultCameraPitch;
    double heightOffset = 0;
    double pixelsPerTile = kDefaultPixelsPerTile;
    // How far back the camera may be pulled, and the ground it may be pointed
    // at. Both are the world's business, not the local map's: the country runs
    // away from the map in every direction, and its cells have negative
    // coordinates in map space whenever the played site is not at the origin.
    double minZoom = kMinPixelsPerTile;
    double boundsMinX = 0, boundsMinY = 0;
    double boundsMaxX = 0, boundsMaxY = 0;

    int viewportWidth = 1280;
    int viewportHeight = 800;

    void pan(double dxMetres, double dyMetres) { centreX += dxMetres; centreY += dyMetres; }
    void orbit(double yawDelta, double pitchDelta);
    void zoomAt(double factor, int mouseX, int mouseY);
    void clampTo(int mapWidth, int mapHeight);
    // The rectangle the centre of the view is kept inside, in map metres.
    void setBounds(double minX, double minY, double maxX, double maxY);

    // The projection, as sixteen numbers: world metres to clip space, row by
    // row, ready to be handed to a shader.
    //
    // This is the one place the projection is written down. Everything else -
    // the screen positions below, the culling, every shader that draws anything
    // in the world - goes through these numbers, so there is no second copy of
    // the dimetric to drift out of step with the first. It was written out by
    // hand in the camera and again in the shaders, and the two agreeing was
    // nobody's job.
    //
    // `depthCentre` and `depthSpan` set the slab of world the depth buffer
    // covers: an orthographic view has no natural near and far, so the caller
    // says how much of the axis maps onto nought to one. They do not touch the
    // screen position, so anything that only wants x and y may pass 0 and 1.
    void viewProjection(float out[16], double depthCentre, double depthSpan) const;

    void worldToScreen(double wx, double wy, float& sx, float& sy) const;
    void worldToScreen3D(double wx, double wy, double wz, float& sx, float& sy) const;
    void worldOfScreenAtHeight(int sx, int sy, double wz, double& wx, double& wy) const;
    void tileToScreen(core::TilePos t, float& sx, float& sy) const;   // centre of the tile
    void worldOfScreen(int sx, int sy, double& wx, double& wy) const;
    core::TilePos screenToTile(int sx, int sy) const;

    // Half the horizontal and vertical span of a tile on screen.
    float tileRadiusX() const;
    float tileRadiusY() const;
    // Screen pixels per metre of object height. A tile is one metre across, so
    // this is what sizes a sprite drawn at its real height.
    float metreOfHeight() const;

    // Tile bounds of what is on screen, with a margin so that a tall sprite
    // whose tile is just off screen still gets drawn. A rejection box, not a
    // shape.
    core::TileRect visibleTiles(int mapWidth, int mapHeight) const;
    // World-XY bounds of the view intersected with a height slab. Perspective
    // includes near/far edges, including views crossing the horizon. Empty
    // intersections return the degenerate rectangle at the camera centre.
    [[nodiscard]] std::array<double,4> visibleGroundBounds(double lowHeight,
                                                           double highHeight) const;
};

} // namespace client
