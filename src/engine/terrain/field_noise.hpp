#pragma once
// The noise the procedural terrain is built out of, in one place.
//
// Its own header rather than each field's own copy, because the SHAPE of the
// world depends on it: two fields that warped themselves with two different
// value noises would disagree about where a margin is, and a change to one
// would move the ground under the other.
//
// Nothing here is a general-purpose noise library. It is the two functions this
// terrain needs, and the second one is the interesting one.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/core/rng.hpp"

namespace engine::terrain::noise {

inline std::uint64_t cellKey(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    return core::splitmix64(seed ^ (std::uint64_t(x) * 0x9e3779b97f4a7c15ull) ^
                            (std::uint64_t(y) * 0xc2b2ae3d27d4eb4full));
}

// A number in [0,1) from a key, and the key advanced - so one site yields as
// many independent numbers as it needs without another hash for each.
inline double take(std::uint64_t& key) {
    key = core::splitmix64(key);
    return double(key >> 11) * (1.0 / 9007199254740992.0);
}

inline double ease(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }

// Value noise, -1 to 1, on a lattice of the given spacing.
inline double value(std::uint64_t seed, double x, double y, double scale) {
    if (!(scale > 0)) return 0;
    const double fx = x / scale, fy = y / scale;
    const std::int64_t ix = std::int64_t(std::floor(fx)), iy = std::int64_t(std::floor(fy));
    const double tx = fx - double(ix), ty = fy - double(iy);
    const auto corner = [&](std::int64_t cx, std::int64_t cy) {
        auto key = cellKey(seed, cx, cy);
        return take(key) * 2.0 - 1.0;
    };
    const double ex = ease(tx), ey = ease(ty);
    const double a = corner(ix, iy), b = corner(ix + 1, iy);
    const double c = corner(ix, iy + 1), d = corner(ix + 1, iy + 1);
    const double top = a + (b - a) * ex, bottom = c + (d - c) * ex;
    return top + (bottom - top) * ey;
}

// The ridged multifractal, 0 to 1, and the reason mountains look like mountains
// rather than like blobs.
//
// Ordinary summed noise is smooth everywhere: its extremes are rounded, so a
// range built from it is a row of domes however many octaves are stacked on it.
// Taking one minus the ABSOLUTE value folds the field at every zero crossing,
// and a fold is a crease - a sharp line, which is what a crest is. Squaring it
// sharpens the crease and flattens what is between, so the valleys are broad
// and the ridges are thin, which is the shape erosion actually leaves.
//
// The multifractal part is the weighting: each octave is scaled by how high the
// one above it came out, so detail collects on the ridges and the valleys stay
// smooth. Without it every octave is everywhere and the result is uniform
// roughness - noise, not landscape.
inline double ridged(std::uint64_t seed, double x, double y, double scale, int octaves = 5,
                     double lacunarity = 2.07, double gain = 0.52) {
    if (!(scale > 0)) return 0;
    double sum = 0, norm = 0, amplitude = 1, frequency = 1, weight = 1;
    for (int o = 0; o < std::max(1, octaves); ++o) {
        double signal = 1.0 - std::abs(value(seed + std::uint64_t(o) * 0x9E3779B1u, x * frequency,
                                             y * frequency, scale));
        signal *= signal;
        signal *= weight;
        weight = std::clamp(signal * gain * 2.0, 0.0, 1.0);
        sum += signal * amplitude;
        norm += amplitude;
        amplitude *= gain;
        frequency *= lacunarity;
    }
    return norm > 0 ? std::clamp(sum / norm, 0.0, 1.0) : 0.0;
}

} // namespace engine::terrain::noise

