#pragma once
#include <algorithm>
#include <array>

namespace world::terrain {

struct ViewBounds {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    double low = 0, high = 0;
};

inline double focusDistanceSquared(const ViewBounds& box, double x, double y) {
    const double dx = x - std::clamp(x, box.minX, box.maxX);
    const double dy = y - std::clamp(y, box.minY, box.maxY);
    return dx * dx + dy * dy;
}

// A fixed two-cell skirt cannot cover independent LOD/morph height changes.
// Reach below the conservative surface bound, including the absent ocean page.
inline double skirtFloor(double surfaceLow) { return std::min(surfaceLow, -60.0) - 1.0; }

// Homogeneous clip-space rejection using the SAME matrix as the vertex shader.
// Height matters: offscreen XY can project into the viewport on a mountainside.
inline bool intersectsView(const ViewBounds& box, const std::array<float, 16>& matrix,
                           double marginX = 0, double marginY = 0) {
    const bool perspective = matrix[12] != 0 || matrix[13] != 0 || matrix[14] != 0;
    const double lo[3]{box.minX, box.minY, box.low}, hi[3]{box.maxX, box.maxY, box.high};
    for (int plane = 0; plane < (perspective ? 6 : 4); ++plane) {
        const int row = (plane / 2) * 4;
        const double sign = plane % 2 ? -1.0 : 1.0;
        const double w = plane == 4 ? 0.0 : 1.0 + (plane < 2 ? marginX : plane < 4 ? marginY : 0.0);
        double high = w * matrix[15] + sign * matrix[row + 3];
        for (int i = 0; i < 3; ++i) {
            const double coefficient = w * matrix[12 + i] + sign * matrix[row + i];
            high += std::max(coefficient * lo[i], coefficient * hi[i]);
        }
        if (high < 0) return false;
    }
    return true;
}

} // namespace world::terrain

