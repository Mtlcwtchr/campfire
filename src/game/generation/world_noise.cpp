#include "game/generation/world_noise.hpp"

#include <algorithm>
#include <cstdlib>

#include "engine/core/rng.hpp"
#include "game/generation/world_map_gen.hpp"

namespace generation {

std::int32_t octaveScale(std::int32_t base, std::int32_t width, std::int32_t follows) {
    const std::int32_t grown = std::max(1, base * width / kReferenceWidth);
    return std::max(2, base + (grown - base) * follows / 100);
}

// The same value noise the local map uses, at the scale of a country rather than
// a field. Kept apart from it because the two are free to diverge: the local one
// answers "where is the stone", this one answers "where is the sea".
std::int32_t valueNoise(std::uint64_t seed, std::int32_t x, std::int32_t y) {
    std::uint64_t h = seed;
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(x)) * 0x9e3779b97f4a7c15ULL));
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(y)) * 0xc2b2ae3d27d4eb4fULL));
    return static_cast<std::int32_t>(h & 1023);
}

std::int32_t smoothNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t scale) {
    const std::int32_t cx = x / scale, cy = y / scale;
    const std::int32_t fx = x % scale, fy = y % scale;
    const std::int32_t v00 = valueNoise(seed, cx, cy);
    const std::int32_t v10 = valueNoise(seed, cx + 1, cy);
    const std::int32_t v01 = valueNoise(seed, cx, cy + 1);
    const std::int32_t v11 = valueNoise(seed, cx + 1, cy + 1);
    const std::int32_t top = v00 + (v10 - v00) * fx / scale;
    const std::int32_t bottom = v01 + (v11 - v01) * fx / scale;
    return top + (bottom - top) * fy / scale;
}

// Two octaves drew a rounded rectangle.
std::int32_t landNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    return (smoothNoise(seed, x, y, octaveScale(90, width, 100)) * 8 +
            smoothNoise(seed + 11, x, y, octaveScale(38, width, 60)) * 5 +
            smoothNoise(seed + 101, x, y, octaveScale(17, width, 30)) * 3 +
            smoothNoise(seed + 1009, x, y, octaveScale(7, width, 0)) * 2 +
            smoothNoise(seed + 10007, x, y, octaveScale(3, width, 0))) /
           19;
}

// The SHAPE of the land, as opposed to the texture on it.
//
// This exists because the shape was coming from somewhere it had no business
// coming from. The crust step between an oceanic plate and a continental one is
// four hundred and fifty units; the noise, after being scaled down everywhere
// the ground is not being pushed up, was contributing under a hundred. So the
// coastline was the plate diagram - Voronoi cells, warped and blurred, but
// still cells, and what you see is exactly that: long straightish runs, sharp
// corners, and squares. No amount of fraying an edge fixes a shape that is a
// polygon underneath.
//
// Plates decide FEATURES - where a range stands, where a rift opens, where a
// trench runs. They are very bad at deciding where a continent is, because a
// continent is not a cell of anything. This is what decides that: five octaves
// with the low ones carrying most of the amplitude, warped through itself so
// that even the largest lobe is not an ellipse.
std::int32_t continentShape(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t first) {
    first = std::max(first, 4);
    // Warped by a third of the longest wave: enough to pull a lobe into a
    // peninsula and tear a strait through it, not so much that it is only the
    // continent moving about.
    const std::int32_t swing = std::max(3, first / 3);
    const std::int32_t wx = x + (smoothNoise(seed ^ 0xC0A7F, x, y, first) - 512) * swing / 512;
    const std::int32_t wy = y + (smoothNoise(seed ^ 0xC0A80, x, y, first) - 512) * swing / 512;
    // Nothing below four cells, ever. This field is stored on the macro lattice
    // and read back through a spline over it, so a wave of two or three cells is
    // at that lattice's own frequency: it cannot be reconstructed, and what
    // comes out instead is a crease on every cell line. The linter measures it
    // directly, and measured it at seventy centimetres the moment these octaves
    // were allowed down there.
    const auto octave = [&](int divisor) { return std::max(4, first / divisor); };
    return (smoothNoise(seed ^ 0x1A2D, wx, wy, first) * 13 +
            smoothNoise(seed ^ 0x1A2E, wx, wy, octave(2)) * 8 +
            smoothNoise(seed ^ 0x1A2F, wx, wy, octave(4)) * 5 +
            smoothNoise(seed ^ 0x1A30, wx, wy, octave(8)) * 3 +
            smoothNoise(seed ^ 0x1A31, wx, wy, octave(16)) * 2) /
           31;
}

std::int32_t continentLobeCells(std::int32_t width) {
    // The longest wave has to FIT, and it did not.
    //
    // Scaled off the reference width the first octave came out at about a
    // hundred kilometres, which on a two-hundred-kilometre map is two lobes
    // across the whole world - so the world was one big land mass and one big
    // island, every time, whatever the seed. A map wants four or five lobes in
    // it before it has anything to offer: continents to be separate, seas
    // between them, and the smaller octaves left over to break the edges into
    // islands.
    //
    // A fifth of the map, then, and capped so that a very large world gains
    // more continents rather than the same few drawn bigger.
    return std::clamp(width / 5, 8, 150);
}

std::int32_t continentNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    return continentShape(seed, x, y, continentLobeCells(width));
}

std::int32_t ridgeNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    const auto fold = [](std::int32_t v) { return 1023 - std::abs(2 * v - 1023); };
    return (fold(smoothNoise(seed, x, y, octaveScale(60, width, 100))) * 5 +
            fold(smoothNoise(seed + 7, x, y, octaveScale(26, width, 50))) * 3 +
            fold(smoothNoise(seed + 71, x, y, octaveScale(11, width, 0))) * 2) /
           10;
}

} // namespace generation

