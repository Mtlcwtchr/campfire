#pragma once
// One field of islands the whole picture is built on: where the moss stands and
// where the water does between it, where the flowers are, where the shrubs make
// islands and the grass leaves paths. A noise for each of them gave a ground
// that was busy and all alike; one noise for all of them makes them agree.
//
// 0..1, features from ~20 m (small islands) to ~200 m (broad fields), the broad
// kind in some country and the small in the rest (patchRegion). THE SAME
// ARITHMETIC IS IN assets/shaders/noise.hlsli (patchFieldAt, patchRegionAt):
// change both together.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace engine::biomes {

namespace patch_detail {
inline double hash(double cx, double cy) {
    const auto ix = std::uint32_t(std::int32_t(std::floor(cx)) + 4096);
    const auto iy = std::uint32_t(std::int32_t(std::floor(cy)) + 4096);
    std::uint32_t h = ix * 0x27d4eb2du + iy * 0x9e3779b1u;
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
    return double(h & 0xffffu) / 65535.0;
}
inline double value(double x, double y) {
    const double wx = std::floor(x), wy = std::floor(y);
    double px = x - wx, py = y - wy;
    px = px * px * (3.0 - 2.0 * px);
    py = py * py * (3.0 - 2.0 * py);
    const double a = hash(wx, wy), b = hash(wx + 1, wy), c = hash(wx, wy + 1), d = hash(wx + 1, wy + 1);
    const double top = a + (b - a) * px, bottom = c + (d - c) * px;
    return top + (bottom - top) * py;
}
inline double smooth(double a, double b, double x) {
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
} // namespace patch_detail

// How broad the islands are here: 0 small and ragged, 1 broad fields.
inline double patchRegion(double x, double y) {
    using namespace patch_detail;
    return smooth(0.42, 0.62, value(x / 760.0 + 9.1, y / 760.0 - 3.3));
}

inline double patchField(double x, double y) {
    using namespace patch_detail;
    // Pushed about by a slow noise, so no island lies along the lattice.
    x += (value(x / 90.0 + 5.3, y / 90.0 - 1.1) - 0.5) * 56.0;
    y += (value(x / 90.0 - 7.7, y / 90.0 + 2.9) - 0.5) * 56.0;
    const double small = value(x / 46.0 + 3.1, y / 46.0 + 7.7) * 0.6 + value(x / 21.0 + 11.3, y / 21.0 - 4.2) * 0.4;
    const double large = value(x / 190.0 + 1.7, y / 190.0 + 5.3) * 0.65 + value(x / 80.0 - 6.4, y / 80.0 + 2.2) * 0.35;
    const double mix = patchRegion(x, y);
    const double p = small + (large - small) * mix;
    return std::clamp((p - 0.5) * 1.9 + 0.5, 0.0, 1.0);
}

} // namespace engine::biomes
