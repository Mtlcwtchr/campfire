#pragma once
#include <array>

namespace world::foliage {
// Kenney Shaded IDs, not array layers. Flat equivalents are separate artwork.
// Only this allow-list enters procedural decoration. No directory-wide loading.
inline constexpr std::array<int, 6> kWildGrassSprites{52, 53, 54, 55, 56, 57};

enum class AssetRole { WildGrass, ReservedGameplayPlants, ReservedPlantParts, Unassigned };

constexpr AssetRole assetRole(int shadedId) {
    for (int allowed : kWildGrassSprites)
        if (allowed == shadedId) return AssetRole::WildGrass;
    // Provisional reserve, not botanical identification or implemented crops.
    if (shadedId >= 58 && shadedId <= 77) return AssetRole::ReservedGameplayPlants;
    if (shadedId >= 78 && shadedId <= 89) return AssetRole::ReservedPlantParts;
    return AssetRole::Unassigned;
}
} // namespace world::foliage

