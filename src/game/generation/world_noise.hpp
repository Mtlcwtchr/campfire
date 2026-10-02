#pragma once
// The noise the world generator's passes are made of.
//
// It lived inside the generator, where nothing else could reach it, and that
// was right while a pass could only ever run over the whole world. Now the
// passes that decide a value at a place from their own dials alone - the shape
// of the continents, the hills - are also brushes (world_brush.hpp), painting
// a footprint of an editable layer (world_layers.hpp) with exactly what the
// generator would have put there. One function for both, or the brush and the
// pass drift apart and "paint it the way the generator would" stops meaning
// anything.
//
// Integer and deterministic, like the generator: the world is a function of its
// seeds and nothing else.
#include <cstdint>

namespace generation {

// How much of a noise octave follows the size of the map, in percent. The
// continental octave follows it entirely, so a wider world is a wider continent
// rather than an archipelago of the old one; the fine octaves do not follow at
// all, so the extra width is spent on more coastline instead of a bigger
// version of the same coastline.
std::int32_t octaveScale(std::int32_t base, std::int32_t width, std::int32_t follows);

// Hashed value, 0..1023, one per lattice point.
std::int32_t valueNoise(std::uint64_t seed, std::int32_t x, std::int32_t y);
// Bilinear value noise, 0..1023, lattice points `scale` cells apart.
std::int32_t smoothNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t scale);

// Five octaves at continental scale: the big one decides where the land is, the
// small ones give it a coastline. 0..1023. `width` is the world's, in cells:
// the octaves are tuned against kReferenceWidth and grow with it.
std::int32_t landNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width);

// The continental shape (PASS G1): warped, five octaves, the low ones carrying
// most of the amplitude. 0..1023. `lobeCells` is the longest wave, in cells.
std::int32_t continentShape(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t lobeCells);
// The longest wave the generator gives a world `width` cells across: a fifth of
// it, capped so a very large world gains continents rather than growing them.
std::int32_t continentLobeCells(std::int32_t width);
// continentShape at the generator's own lobe for a world this wide.
std::int32_t continentNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width);

// Ridged noise: folded at its middle so its peaks are creases, not bumps.
std::int32_t ridgeNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width);

} // namespace generation

