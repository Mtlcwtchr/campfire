#pragma once
// How the ground's textures are laid, for every material at once
// (content/config/terrain_look.json). The renderer reads the file again when it
// changes, so the ground panel's sliders reach the picture live; the shader
// side is terrain_material.hlsli (Scene::terrainLook).
#include <array>
#include <filesystem>
#include <string>

namespace game {

struct TerrainLook {
    // Times every material's own tile size (ground.json metres_per_turn).
    float tileScale = 1.0f;
    // Distance tiling: how many times the scan may double its size as a
    // texel shrinks on the screen. Nought keeps one size at every distance.
    float octaves = 3.0f;
    // The texels of the scan one pixel covers when the next octave starts:
    // smaller hands over sooner (less repetition, softer mid-range).
    float octaveStart = 8.0f;
    // The broad light/dark variation laid over the scan, as a multiple.
    float macroVariation = 1.0f;

    void clampAll();
    [[nodiscard]] std::array<float, 4> uniform() const {
        return {tileScale, octaves, octaveStart, macroVariation};
    }
    // Missing or unreadable: the defaults, and false.
    bool load(const std::filesystem::path& file);
    bool save(const std::filesystem::path& file, std::string* why = nullptr) const;
};

// content/config/terrain_look.json beside the terrain categories' directory.
std::filesystem::path terrainLookFile();

} // namespace game

