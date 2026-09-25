#include "game/client/camera.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace client {
namespace {
// Half a tile: the renderer's own arithmetic, kept out of the simulation's
// fixed-point world.
constexpr double kHalfTile = 0.5;
} // namespace

const char* Camera::modeName() const {
    return mode == Mode::Free ? "1st / free flight" : mode == Mode::Orbit ? "3rd / orbit" : "map / ortho";
}

double Camera::orbitDistance() const {
    return std::clamp(viewportHeight / (2.0 * std::tan(verticalFov * 0.5) *
        std::max(0.00001, pixelsPerTile * 0.5)), 2.0, farPlane * 0.8);
}

std::array<double, 3> Camera::eyePosition() const {
    if (mode == Mode::Free) return {centreX, centreY, focusHeight};
    const double distance = orbitDistance();
    return {centreX + std::cos(yaw) * std::cos(pitch) * distance,
            centreY + std::sin(yaw) * std::cos(pitch) * distance,
            focusHeight + std::sin(pitch) * distance};
}

void Camera::setMode(Mode next) {
    if (next == mode) return;
    if (next == Mode::Free) {
        const auto eye = eyePosition();
        centreX = eye[0]; centreY = eye[1]; focusHeight = eye[2];
    } else if (mode == Mode::Free && next == Mode::Orbit) {
        pitch = std::clamp(pitch, kMinCameraPitch, kMaxCameraPitch);
        const double distance = orbitDistance();
        centreX -= std::cos(yaw) * std::cos(pitch) * distance;
        centreY -= std::sin(yaw) * std::cos(pitch) * distance;
        focusHeight -= std::sin(pitch) * distance;
    }
    mode = next;
    if (mode != Mode::Free) pitch = std::clamp(pitch, kMinCameraPitch, kMaxCameraPitch);
}

Camera::Ray Camera::screenRay(double sx, double sy) const {
    if (!perspective()) {
        double x0, y0, x1, y1;
        worldOfScreenAtHeight(int(sx), int(sy), focusHeight, x0, y0);
        worldOfScreenAtHeight(int(sx), int(sy), focusHeight - 1.0, x1, y1);
        return {{x0, y0, focusHeight}, {x1 - x0, y1 - y0, -1.0}};
    }
    const double cy = std::cos(yaw), syaw = std::sin(yaw);
    const double cp = std::cos(pitch), sp = std::sin(pitch);
    const double tanFov = std::tan(verticalFov * 0.5);
    const double x = (2.0 * sx / std::max(1, viewportWidth) - 1.0) * tanFov *
                     std::max(1, viewportWidth) / std::max(1, viewportHeight);
    const double y = (1.0 - 2.0 * sy / std::max(1, viewportHeight)) * tanFov;
    // Forward depth (not normalized distance) is the ray parameter.
    return {eyePosition(), {-cy * cp + syaw * x - cy * sp * y,
                            -syaw * cp - cy * x - syaw * sp * y, -sp + cp * y}};
}

void Camera::orbit(double yawDelta, double pitchDelta) {
    yaw = std::remainder(yaw + yawDelta, 2.0 * std::numbers::pi);
    pitch = std::clamp(pitch + pitchDelta, mode == Mode::Free ? -1.55 : kMinCameraPitch,
                       mode == Mode::Free ? 1.55 : kMaxCameraPitch);
}

void Camera::zoomAt(double factor, int mouseX, int mouseY) {
    if (mode == Mode::Free) {
        flightSpeed = std::clamp(flightSpeed * factor, 1.0, 20000.0);
        return;
    }
    if (perspective()) {
        pixelsPerTile = std::clamp(pixelsPerTile * factor, std::min(minZoom, kMinPixelsPerTile), kMaxPixelsPerTile);
        return;
    }
    // Keep whatever is under the cursor under the cursor.
    double beforeX, beforeY;
    worldOfScreen(mouseX, mouseY, beforeX, beforeY);
    pixelsPerTile = std::clamp(pixelsPerTile * factor, std::min(minZoom, kMinPixelsPerTile),
                               kMaxPixelsPerTile);
    double afterX, afterY;
    worldOfScreen(mouseX, mouseY, afterX, afterY);
    centreX += beforeX - afterX;
    centreY += beforeY - afterY;
}

void Camera::setBounds(double minX, double minY, double maxX, double maxY) {
    boundsMinX = minX;
    boundsMinY = minY;
    boundsMaxX = maxX;
    boundsMaxY = maxY;
}

void Camera::clampTo(int mapWidth, int mapHeight) {
    if (mode == Mode::Free) return;
    if (mode == Mode::Orbit) {
        centreX = std::clamp(centreX, boundsMinX, std::max(boundsMinX, boundsMaxX));
        centreY = std::clamp(centreY, boundsMinY, std::max(boundsMinY, boundsMaxY));
        return;
    }
    // Two different pens, because there are two different things to look at.
    // Inside tile scale the camera belongs to the local map and must not wander
    // off it into ground that is not drawn. Pulled back past it, the picture is
    // the country (D84), and the country runs away from the map in every
    // direction - held to the map there, the camera could show a continent but
    // only point at one hundred and eighty metre square of it.
    if (pixelsPerTile >= kWorldViewPixelsPerTile || boundsMaxX <= boundsMinX) {
        centreX = std::clamp(centreX, 0.0, static_cast<double>(mapWidth));
        centreY = std::clamp(centreY, 0.0, static_cast<double>(mapHeight));
        return;
    }

    const double visibleX = viewportWidth / pixelsPerTile;
    const double visibleY = viewportHeight / (pixelsPerTile * kVerticalSquash);
    const auto pen = [](double centre, double low, double high, double visible) {
        // Once the whole of it fits on screen there is nothing to pan to, and
        // letting the centre sit at an edge just pushes the world into a corner
        // and fills the rest of the window with background.
        if (visible >= high - low) return (low + high) / 2.0;
        return std::clamp(centre, low + visible / 2.0, high - visible / 2.0);
    };
    centreX = pen(centreX, boundsMinX, boundsMaxX, visibleX);
    centreY = pen(centreY, boundsMinY, boundsMaxY, visibleY);
}

void Camera::viewProjection(float out[16], double depthCentre, double depthSpan) const {
    const double wide = std::max(1, viewportWidth);
    const double high = std::max(1, viewportHeight);
    double row[16] = {};
    if (perspective()) {
        const auto eye = eyePosition();
        const double cy = std::cos(yaw), sy = std::sin(yaw);
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        const double focal = 1.0 / std::tan(verticalFov * 0.5);
        const double right[3]{sy, -cy, 0}, up[3]{-cy * sp, -sy * sp, cp};
        const double forward[3]{-cy * cp, -sy * cp, -sp};
        const double depth = farPlane / (farPlane - nearPlane);
        for (int i = 0; i < 3; ++i) {
            row[i] = right[i] * focal * high / wide;
            row[4 + i] = up[i] * focal;
            row[8 + i] = forward[i] * depth;
            row[12 + i] = forward[i];
            row[3] -= row[i] * eye[i]; row[7] -= row[4 + i] * eye[i];
            row[11] -= row[8 + i] * eye[i]; row[15] -= forward[i] * eye[i];
        }
        row[11] -= nearPlane * depth; // SDL clip depth is [0, w].
    } else if (isometric) {
        // Classic 2:1 RTS dimetric: a metre along either ground axis moves half
        // a tile sideways and a quarter tile down, and a metre of height rises
        // by half a tile - which is what lets the heightfield change the
        // silhouette rather than only the shading.
        const double across = pixelsPerTile / wide;          // x and y, sideways
        const double down = pixelsPerTile * 0.5 / high;      // x and y, downwards
        const double lift = pixelsPerTile / high;            // height, upwards
        row[0] = across;
        row[1] = -across;
        row[3] = -across * centreX + across * centreY;
        row[4] = -down;
        row[5] = -down;
        row[6] = lift;
        row[7] = down * centreX + down * centreY - lift * focusHeight;
        // Depth along the axis the camera actually looks down.
        //
        // Moving along a direction that does not move a point on the screen is
        // moving straight towards or away from the eye, and for this projection
        // that direction is (1, 1, 2): one east and one north push the point a
        // quarter tile down twice, and two metres up lift it a whole tile - they
        // cancel. So what decides what is in front of what is x + y + 2z, and it
        // has to get smaller towards the eye, which is above and to the
        // north-west.
        //
        // It was x + y + z with the sign the other way round. Height therefore
        // made a point farther away rather than nearer: a hilltop lost the depth
        // test to the valley behind it and every ridge came out chewed, and
        // water - which is by definition above the bed it lies on - lost to its
        // own riverbed everywhere except the last few centimetres at the bank.
        // That is why a river was drawn as two blue threads with a brown floor
        // between them.
        row[8] = -depthSpan;
        row[9] = -depthSpan;
        row[10] = -2.0 * depthSpan;
        row[11] = depthSpan * (centreX + centreY + 2.0 * focusHeight) + 0.5 +
                  depthCentre * depthSpan;
        // Rotate the original dimetric basis about its focus. The default
        // angles reproduce the legacy image, including its depth convention.
        const double angle = std::atan(std::tan(pitch) * std::sqrt(2.0));
        const double base = std::atan(std::tan(kDefaultCameraPitch) * std::sqrt(2.0));
        const double ca = std::cos(angle - base), sa = std::sin(angle - base);
        const double turn = yaw - kDefaultCameraYaw, ct = std::cos(turn), st = std::sin(turn);
        for (int r = 0; r < 3; ++r) {
            const int i = r * 4;
            const double horizontal = (row[i] + row[i + 1]) / std::sqrt(2.0);
            const double right = (row[i] - row[i + 1]) / std::sqrt(2.0);
            const double h = horizontal * ca - row[i + 2] * sa;
            const double z = horizontal * sa + row[i + 2] * ca;
            const double x = (h + right) / std::sqrt(2.0), y = (h - right) / std::sqrt(2.0);
            row[i] = x * ct - y * st; row[i + 1] = x * st + y * ct; row[i + 2] = z;
            row[i + 3] = -row[i] * centreX - row[i + 1] * centreY - z * focusHeight;
            if (r == 2) row[i + 3] += 0.5 + depthCentre * depthSpan;
        }
    } else {
        // Flat and top-down: a metre is a metre either way and height does not
        // move anything, because the art for this view is drawn from above.
        row[0] = pixelsPerTile * 2.0 / wide;
        row[3] = -centreX * pixelsPerTile * 2.0 / wide;
        row[5] = -pixelsPerTile * kVerticalSquash * 2.0 / high;
        row[7] = centreY * pixelsPerTile * kVerticalSquash * 2.0 / high;
        // Painter's order, as depth: south is nearer, and so is up. Height moves
        // nothing on the screen here, but a wall on a bank still has to be in
        // front of the ground behind it.
        row[9] = -depthSpan;
        row[10] = -2.0 * depthSpan;
        row[11] = depthSpan * (centreY + 2.0 * focusHeight) + 0.5 + depthCentre * depthSpan;
    }
    if (!perspective()) row[15] = 1.0;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(row[i]);
}

void Camera::worldToScreen(double wx, double wy, float& sx, float& sy) const {
    worldToScreen3D(wx, wy, focusHeight, sx, sy);
}

void Camera::worldToScreen3D(double wx, double wy, double wz, float& sx, float& sy) const {
    // Through the same matrix the card is given, and back out to pixels. The
    // depth slab is irrelevant to where a point lands on the screen, so this
    // asks for the simplest one there is.
    float m[16];
    viewProjection(m, 0, 1);
    const double w = m[12] * wx + m[13] * wy + m[14] * wz + m[15];
    const double ndcX = (m[0] * wx + m[1] * wy + m[2] * wz + m[3]) / w;
    const double ndcY = (m[4] * wx + m[5] * wy + m[6] * wz + m[7]) / w;
    sx = static_cast<float>((ndcX * 0.5 + 0.5) * viewportWidth);
    sy = static_cast<float>((0.5 - ndcY * 0.5) * viewportHeight);
}

void Camera::tileToScreen(core::TilePos t, float& sx, float& sy) const {
    worldToScreen(static_cast<double>(t.x), static_cast<double>(t.y), sx, sy);
}

void Camera::worldOfScreen(int sx, int sy, double& wx, double& wy) const {
    worldOfScreenAtHeight(sx, sy, focusHeight, wx, wy);
}

void Camera::worldOfScreenAtHeight(int sx, int sy, double wz, double& wx, double& wy) const {
    if (perspective()) {
        const auto ray = screenRay(sx, sy);
        const double t = std::abs(ray.direction[2]) > 1e-9 ? (wz - ray.origin[2]) / ray.direction[2] : -1;
        wx = t >= 0 ? ray.origin[0] + ray.direction[0] * t : centreX;
        wy = t >= 0 ? ray.origin[1] + ray.direction[1] * t : centreY;
        return;
    }
    if (isometric) {
        float m[16];
        viewProjection(m, 0, 1);
        const double x = 2.0 * sx / std::max(1, viewportWidth) - 1.0 - m[2] * wz - m[3];
        const double y = 1.0 - 2.0 * sy / std::max(1, viewportHeight) - m[6] * wz - m[7];
        const double determinant = double(m[0]) * m[5] - double(m[1]) * m[4];
        wx = (x * m[5] - y * m[1]) / determinant;
        wy = (y * m[0] - x * m[4]) / determinant;
        return;
    }
    wx = centreX + (sx - viewportWidth / 2.0) / pixelsPerTile;
    wy = centreY + (sy - viewportHeight / 2.0) / (pixelsPerTile * kVerticalSquash);
}

core::TilePos Camera::screenToTile(int sx, int sy) const {
    double wx, wy;
    worldOfScreen(sx, sy, wx, wy);
    return core::toTile({core::Fixed::fromDoubleForContent(wx), core::Fixed::fromDoubleForContent(wy)});
}

float Camera::metreOfHeight() const { return static_cast<float>(pixelsPerTile); }

float Camera::tileRadiusX() const { return static_cast<float>(pixelsPerTile * kHalfTile); }
float Camera::tileRadiusY() const {
    return static_cast<float>(pixelsPerTile * kHalfTile * (isometric ? kHalfTile : kVerticalSquash));
}

core::TileRect Camera::visibleTiles(int mapWidth, int mapHeight) const {
    double leftX, topY, rightX, bottomY;
    worldOfScreen(0, 0, leftX, topY);
    worldOfScreen(viewportWidth, viewportHeight, rightX, bottomY);

    // A margin each way, larger below: a tile whose centre is just off the
    // bottom of the screen can still have a tall sprite standing up into it,
    // and a small margin keeps sprite edges from popping at the border.
    const int rowMin = static_cast<int>(std::floor(topY)) - 2;
    const int rowMax = static_cast<int>(std::ceil(bottomY)) + 6;
    const int colMin = static_cast<int>(std::floor(leftX)) - 2;
    const int colMax = static_cast<int>(std::ceil(rightX)) + 2;

    return {{std::max(0, colMin), std::max(0, rowMin)},
            {std::min(mapWidth, colMax), std::min(mapHeight, rowMax)}};
}

std::array<double,4> Camera::visibleGroundBounds(double lowHeight,double highHeight) const {
    if (!std::isfinite(lowHeight) || !std::isfinite(highHeight))
        return {centreX,centreY,centreX,centreY};
    if (lowHeight>highHeight) std::swap(lowHeight,highHeight);
    std::array<double,4> bounds{std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()};
    const auto add=[&](double x,double y) {
        if (!std::isfinite(x) || !std::isfinite(y)) return;
        bounds[0]=std::min(bounds[0],x);bounds[1]=std::min(bounds[1],y);
        bounds[2]=std::max(bounds[2],x);bounds[3]=std::max(bounds[3],y);
    };
    const std::array<std::array<int,2>,4> corners{{{0,0},{viewportWidth,0},
                                                  {viewportWidth,viewportHeight},{0,viewportHeight}}};
    if (perspective()) {
        std::array<std::array<double,3>,8> vertices{};
        for (std::size_t i=0;i<4;++i) {
            const auto ray=screenRay(corners[i][0],corners[i][1]);
            for (int j=0;j<3;++j) {
                vertices[i][j]=ray.origin[j]+ray.direction[j]*nearPlane;
                vertices[i+4][j]=ray.origin[j]+ray.direction[j]*farPlane;
            }
        }
        const auto edge=[&](const auto& a,const auto& b) {
            for (const auto& p:{a,b}) if (p[2]>=lowHeight && p[2]<=highHeight) add(p[0],p[1]);
            if (a[2]==b[2]) return;
            for (double z:{lowHeight,highHeight}) {
                const double t=(z-a[2])/(b[2]-a[2]);
                if (t>=0 && t<=1) add(std::lerp(a[0],b[0],t),std::lerp(a[1],b[1],t));
            }
        };
        for (std::size_t i=0;i<4;++i) {
            edge(vertices[i],vertices[(i+1)%4]);
            edge(vertices[i+4],vertices[(i+1)%4+4]);
            edge(vertices[i],vertices[i+4]);
        }
    } else for (const double height:{lowHeight,highHeight}) for (const auto& corner:corners) {
        double x=0,y=0;
        worldOfScreenAtHeight(corner[0],corner[1],height,x,y);
        add(x,y);
    }
    if (!std::isfinite(bounds[0]+bounds[1]+bounds[2]+bounds[3]))
        return {centreX,centreY,centreX,centreY};
    return bounds;
}

} // namespace client
