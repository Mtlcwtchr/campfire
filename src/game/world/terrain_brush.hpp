#pragma once
// The tools a person shapes ground with.
//
// Each one is a function of the ground it is given and the edit layer it writes
// to: no camera, no input, no frame. That is what lets them be tested - a brush
// is "this stroke on this hill leaves that hill", which is a thing to check, and
// none of it needs a window open.
//
// They all write DIFFERENCES into an EditLayer rather than heights into a
// world, for the reason that file gives: the generator stays a function of its
// seed and the edit stays undoable by deletion.
//
// The two erosion brushes are the same operators the generator uses, run over a
// patch instead of over a world. That matters more than it sounds: a person
// smoothing a ridge by hand and the generator smoothing it at build time should
// leave the same kind of ridge, or the editor is a second landscape pasted over
// the first.
#include <cstdint>
#include <functional>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/edit_layer.hpp"

namespace world {

enum class BrushKind : std::uint8_t {
    Raise,     // lift the ground under the brush
    Lower,     // and the same, downwards
    Smooth,    // towards the average of the neighbourhood
    Flatten,   // towards the height under the middle of the brush
    Noise,     // a generative stamp: ridged noise, so it makes ground not fuzz
    Thermal,   // scree: nothing steeper than its angle of repose survives
    Hydraulic, // water's work: cut the steep, fill what lies below it
    Carve,     // a channel, for putting a river where one is wanted
    Count,
};

struct Brush {
    BrushKind kind = BrushKind::Raise;
    double radiusMetres = 40;
    // Metres a second of a held stroke, before the falloff. A brush is applied
    // over TIME rather than per click, so a stroke is controllable.
    double strength = 6;
    // Nought is a hard edge, one is a brush that is all edge. Squared cosine
    // between, which is the shape that leaves no visible rim.
    double softness = 0.65;
    // For Noise, the wavelength of its largest octave; for Carve, the width of
    // the channel; ignored by the rest.
    double scaleMetres = 60;
    std::uint64_t seed = 1;
};

// The ground as it stands, INCLUDING whatever the layer already holds: a brush
// reads what it can see, or two strokes over the same place fight each other.
using GroundAt = std::function<core::Fixed(core::Fixed x, core::Fixed y)>;

// One application of a brush, at a point, for `seconds` of stroke. Returns how
// many samples it wrote, which is what a test counts and a tool reports.
std::size_t applyBrush(EditLayer& into, const GroundAt& ground, const Brush& brush,
                       core::WorldPos at, double seconds);

} // namespace world
