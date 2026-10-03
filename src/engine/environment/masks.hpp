#pragma once
// Influence masks over a page (doc/plan_procedural_environment_2026-10-03.md, part G).
//
// Eight channels a page, named by the game (masks.json) - wetness, suppress,
// boost, rock, talus, litter, moss, scar, or whatever its materials and
// foliage want to read - and the zones, as the two strongest types and how
// the second blends in. The material shader, the ground cover and the CPU
// scatter all read the same numbers: the material and the foliage are both
// consequences of one state, never one of the other.
//
// Written in two passes: what the zones say (a callback of the game's, since
// only the game knows that a wet hollow is wet), then every feature's mask
// writes over it, each by its shape and mode.
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/environment/feature_layer.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

using MaskValues = std::array<float, kMaskChannels>;

// What the zones put into the channels before any feature, at a point.
using ZoneMasks = std::function<void(double x, double y, const EnvironmentZone& zone, MaskValues& out)>;

struct PageMasks {
    double x0 = 0, y0 = 0, step = 4;
    int side = 0;                        // samples a side, padding included
    std::vector<std::uint8_t> channels;  // side * side * kMaskChannels
    std::vector<std::uint8_t> zones;     // side * side * 4: first type, second type, second's share 0..255, density
    [[nodiscard]] float channel(int c, int r, int k) const { return channels[(std::size_t(r) * side + c) * kMaskChannels + k] / 255.0f; }
};

// The masks over a square of `side` samples `step` metres apart from (x0, y0).
// `height` is the ground before features (the shape a mask is drawn against).
PageMasks rasteriseMasks(const FeatureLayer* features, const ZoneField* zones, const ZoneMasks& zoneMasks,
                         const HeightAt& height, double x0, double y0, double step, int side);

// One instance's writes at a point, folded into `values`. What rasteriseMasks
// does a sample at a time; public for the CPU scatter, which asks single points.
void applyMaskWrites(const FeatureRecipe& recipe, const FeatureInstance& instance, const OpGeometry& geometry,
                     core::WorldPos p, MaskValues& values);
float maskShapeWeight(const MaskWrite& write, const FeatureInstance& instance, const OpGeometry& geometry,
                      core::WorldPos p);

} // namespace engine::environment
