#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include "engine/core/fixed.hpp"

namespace world {

// Substrate, not surface cover or biome. Snow/grass may cover any of these.
enum class Soil : std::uint8_t { Sand, Clay, Silt, Loam, Rocky, Peat, Alluvium, Saline, Count };
inline constexpr std::size_t kSoilCount = static_cast<std::size_t>(Soil::Count);
const char* soilName(Soil soil);

// Relative model indices in [0,1], NOT physical units or building permissions.
// Fertility is intrinsic potential, not the mutable nutrient/depletion state.
struct SoilProperties {
    core::Fixed fertility;
    core::Fixed permeability;
    core::Fixed diggingResistance;
    core::Fixed bearingStrength;
    core::Fixed erodibility;
    core::Fixed mudPotential;
};

struct SoilWeights {
    std::array<core::Fixed, kSoilCount> weight{};
    core::Fixed of(Soil soil) const { return weight[static_cast<std::size_t>(soil)]; }
    void add(Soil soil, core::Fixed value) { weight[static_cast<std::size_t>(soil)] += value; }
    // Nonnegative, sum exactly one in fixed point; empty input falls back to loam.
    void normalise();
    Soil strongest() const;
    SoilProperties properties() const;
};

struct SoilSample {
    SoilWeights weights;
    SoilProperties properties;
};

} // namespace world

