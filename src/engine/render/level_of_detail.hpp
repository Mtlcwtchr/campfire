#pragma once
// How much geometry a thing is worth, as one rule in one place.
//
// Its own header, depending on nothing but the standard library, because three
// different layers need it and none of them should have to drag in the others
// to ask: the ECS render systems that group a frame, the world's own scatter,
// and the GPU cluster culler whose shader states the same rule in HLSL. Two
// implementations that drifted would show as objects changing detail when the
// frame merely changed how it was submitted.
#include <cmath>
#include <cstddef>
#include <span>

namespace engine::render {

// How far a representation's shape may move on screen before the next finer one
// is worth its triangles.
//
// Three pixels, and the number is calibrated rather than derived: the measured
// errors are 95th-percentile distances from the imported SURFACE, and on
// alpha-cut foliage most of that distance is a leaf traded for a slightly
// larger leaf beside it rather than a silhouette that moved. Holding the chain
// to a fraction of a pixel of surface error would keep six thousand triangles
// on a tree sixty pixels high. See doc/reports/gen_rework_step_5.
inline constexpr double kLevelPixelError = 3.0;

// The coarsest level of a chain whose projected error still fits the allowance.
//
// `errors` are the levels' measured shape errors in model metres, finest first
// and never decreasing; `extent` is the model's largest dimension in the same
// metres; `pixels` is how large this instance is on screen. The instance scale
// cancels, so a sapling and a giant of one species that reach the same height
// on screen get the same triangles - the choice is a property of the model and
// the screen, never of distance on its own.
inline std::size_t levelFor(std::span<const float> errors, double pixels, double extent,
                            double allowance = kLevelPixelError) {
    if (errors.empty() || !std::isfinite(pixels) || !std::isfinite(extent) ||
        !std::isfinite(allowance) || !(extent > 0) || !(allowance > 0) || !(pixels > 0))
        return 0;
    const double perMetre = pixels / extent;
    std::size_t chosen = 0;
    for (std::size_t i = 0; i < errors.size(); ++i) {
        if (!std::isfinite(errors[i]) || errors[i] < 0) break;
        // A chain that is not ordered is not a chain: stop where the ordering
        // does rather than trusting the rest of it.
        if (i && errors[i] < errors[i - 1]) break;
        if (errors[i] * perMetre <= allowance) chosen = i;
    }
    return chosen;
}

} // namespace engine::render
