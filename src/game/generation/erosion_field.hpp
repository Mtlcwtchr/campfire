#pragma once
// Erosion, split into the two different things it is.
//
// One is DRAINAGE: where the water goes, which basin a drop is in, where a
// channel runs. That is physics, it crosses the whole world, and it belongs to
// the hydrology graph - it decides where a river is, and a river in the wrong
// place is wrong however pretty the ground around it is.
//
// The other is what this header does: the erosion that makes a mountain look
// like a mountain. Talus below a cliff, gullies down a face, the faceting that
// makes a young range read as triangular rather than as a loaf. None of it
// decides anything; all of it is what the eye reads as "rock" instead of "a
// height field".
//
// Keeping them apart matters because they answer to different things. Drainage
// must be the same at every level of detail or a river moves when the camera
// does. Cosmetic erosion must NOT be - a gully two hundred metres across is
// geometry at a four-metre step and a picture at a sixty-four-metre one, and
// pretending otherwise is either a mesh nobody can afford or a mountain made of
// plasticine.
//
// So every answer here comes in two parts: what the step being drawn can carry
// as SHAPE, and what it cannot and therefore owes its shading. That is the rule
// from the plan (P4), and it runs both ways - a caller that ignores the second
// part has silently lost the detail, which is exactly how ground ends up smooth.
#include <cstdint>
#include <functional>

namespace generation {

struct ErosionSettings {
    // The angle loose rock stands at. Everything steeper than this is cut back
    // to it, which is what makes a face a facet instead of a bulge.
    //
    // The old thermal pass used an absolute height difference - forty metres
    // between neighbours - which is an angle only if you already know the
    // spacing. At sixty-four metres it happened to mean thirty-two degrees; at
    // four metres it would mean eighty-four, so nothing would ever erode. That
    // is why the thermal stage in this world does nothing at all.
    double talusDegrees = 34;
    // How far the cut-back looks, in RINGS of the caller's own step.
    //
    // What it does, exactly: a break up to tan(talus) * rings * step tall is
    // laid back to the angle. What it cannot do, and this is worth stating
    // because two attempts at it here failed for two different reasons:
    //
    // It cannot lay back a face longer than its reach. A six-hundred-metre
    // cliff needs a nine-hundred-metre apron, and no operator that looks a few
    // steps around can build one - nor can the real thing, which is why talus
    // aprons are finite and faces above them are not.
    //
    // And it cannot use a fixed world lattice to reach further. Anchors on a
    // lattice the query slides across lose one and gain another as it moves,
    // and if the lost one was the lowest the envelope jumps: measured at
    // seventeen times the angle, from a forty-eight metre step in the surface
    // at the place where the window's edge fell. Anchors that move WITH the
    // query cannot jump, which is why they do.
    //
    // So each level of detail lays back what it can see: twelve-metre crags at
    // a four-metre step, hundred-and-thirty-metre cliffs at sixty-four. That is
    // not a compromise - it is the same rule the rest of this terrain follows,
    // that a step carries the shape it can carry.
    int talusRings = 3;

    // The deepest gully, and how far apart they run.
    //
    // ONE scale, not a stack of octaves, and the depth is bounded by the angle:
    // a gully of depth d across a width L has walls of gradient 2d/L, so a
    // stack of octaves cuts walls far steeper than the talus the surface was
    // just limited to - measured at more than one and a half times it. Gullies
    // at several scales come from asking at several scales, which is what a
    // terrain built level by level does anyway.
    double gullyMetres = 38;
    double gullyScaleMetres = 240;
    // How much steeper than the angle of repose a gully wall may be, as a
    // fraction of it. The depth is derived from this and the width, never taken
    // from `gullyMetres` alone - so a gully is as deep as its width can carry
    // and the surface has a bound rather than a hope.
    double gullySlopeShare = 0.5;
    // Below this much local relief nothing is eroded: a plain does not have
    // gullies in it, and a talus slope under a hillock is not a talus slope.
    double reliefStartMetres = 90;
    double reliefFullMetres = 400;
    // And over what distance that relief is measured.
    //
    // A fixed distance, not a multiple of the step. Measuring it over the step
    // makes "is there any country here" an answer that shrinks as the sampling
    // gets finer - so at four metres nothing is ever eroded at all, which is
    // precisely the wrong way round. Relief is a property of the ground.
    double reliefSpanMetres = 400;
};

struct ErosionSample {
    // Subtract this from the height. Never positive: erosion removes.
    double geometryMetres = 0;
    // What the step could not carry, as an amplitude in metres, and the
    // horizontal scale it lives at. A shader draws these; a caller that drops
    // them has lost the detail in silence.
    double shadingMetres = 0;
    double shadingScaleMetres = 0;
    // What the surface here is like, for anything that wants to know without
    // asking again: the slope as a gradient, and how much of the talus cut
    // happened.
    double slope = 0;
    double talusCut = 0;
};

// The surface being eroded. Erosion has to see the shape it erodes, and that
// shape is whatever the caller has built so far - so it is passed in rather
// than known.
using HeightAt = std::function<double(double x, double y)>;

class ErosionField {
public:
    explicit ErosionField(std::uint64_t seed, ErosionSettings settings = {});

    // `stepMetres` is the spacing the caller is sampling at, and it is what
    // decides the split: a feature smaller than a few steps cannot be shape.
    [[nodiscard]] ErosionSample at(const HeightAt& height, double x, double y,
                                   double stepMetres) const;

    [[nodiscard]] const ErosionSettings& settings() const { return settings_; }

private:
    std::uint64_t seed_ = 0;
    ErosionSettings settings_;
    double talusGradient_ = 0;
};

} // namespace generation
