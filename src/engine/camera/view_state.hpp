#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>

namespace engine::camera {
using Vec3 = std::array<double, 3>;
inline double dot(Vec3 a, Vec3 b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
inline Vec3 subtract(Vec3 a, Vec3 b) { return {a[0]-b[0], a[1]-b[1], a[2]-b[2]}; }
inline Vec3 normalized(Vec3 v) {
    const double n = std::sqrt(dot(v, v));
    return n > 1e-12 && std::isfinite(n) ? Vec3{v[0]/n, v[1]/n, v[2]/n} : Vec3{0, 0, -1};
}
struct ViewQualityProfile {
    double geometryErrorPx = 1.5;
    double impostorErrorPx = 2.0;
    double parallaxErrorPx = 1.5;
    double hysteresis = 0.15;
    double lookaheadSeconds = 0.25;
    double residencySeconds = 0.75;
    static ViewQualityProfile person() { return {0.75, 1.0, 0.75, 0.15, 0.15, 0.75}; }
    static ViewQualityProfile orbital() { return {}; }
};

// Immutable input to all representation policies. Forward is the orientation's
// optical axis, not a game control mode. Distances and errors are world metres.
struct ViewState {
    Vec3 position{};
    Vec3 forward{0, 0, -1};
    Vec3 velocity{};
    double fovY = std::numbers::pi / 3.0;
    int viewportWidth = 1280, viewportHeight = 800;
    bool orthographic = false;
    double orthographicScale = 1.0;
    double nearPlane = 0.5;
    ViewQualityProfile quality;
    [[nodiscard]] bool valid() const {
        for (const auto& v : {position, forward, velocity})
            for (double x : v) if (!std::isfinite(x)) return false;
        return viewportWidth > 0 && viewportHeight > 0 && dot(forward, forward) > 1e-12 &&
            std::isfinite(nearPlane) && nearPlane > 0 &&
            (orthographic ? std::isfinite(orthographicScale) && orthographicScale > 0 :
             std::isfinite(fovY) && fovY > 0 && fovY < std::numbers::pi);
    }
    [[nodiscard]] double projectionScale() const {
        return valid() ? (orthographic ? orthographicScale : viewportHeight / (2.0 * std::tan(fovY * 0.5))) : 0;
    }
    [[nodiscard]] double pixelsPerMetre(Vec3 centre, double radius = 0) const {
        if (!valid()) return 0;
        if (orthographic) return orthographicScale;
        const double depth = dot(subtract(centre, position), normalized(forward));
        if (depth + radius <= 0) return 0;
        return projectionScale() / std::max(nearPlane, depth - std::max(0.0, radius));
    }
    [[nodiscard]] Vec3 predictedPosition() const {
        const double t = std::clamp(quality.lookaheadSeconds, 0.0, 2.0);
        return {position[0]+velocity[0]*t, position[1]+velocity[1]*t, position[2]+velocity[2]*t};
    }
    [[nodiscard]] Vec3 directionFrom(Vec3 centre) const {
        const auto f = normalized(forward);
        return orthographic ? Vec3{-f[0], -f[1], -f[2]} : normalized(subtract(position, centre));
    }
};
} // namespace engine::camera

