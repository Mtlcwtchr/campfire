#include "game/render/graphics_settings.hpp"
#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

namespace game {
void GraphicsSettings::clampAll() {
    quality = std::clamp(quality, 0, 3);
    antialiasing = std::clamp(antialiasing, 0, 1);
    terrainTextures = std::clamp(terrainTextures, 0, 1);
    shadowSoftness = std::clamp(shadowSoftness, 0.5f, 3.0f);
    terrainBlend = std::clamp(terrainBlend, 0.5f, 12.0f);
    objectError = std::clamp(objectError, 1.0f, 16.0f);
    farTreesStart = std::clamp(farTreesStart, 400.0f, 6000.0f);
    resolution = std::clamp(resolution, 0, int(kResolutions.size()) - 1);
    vegetationMeshPixels = std::clamp(vegetationMeshPixels, 20.0f, 8000.0f);
    vegetationMeshKiloTriangles = std::clamp(vegetationMeshKiloTriangles, 5.0f, 8000.0f);
    drawDistanceKm = std::clamp(drawDistanceKm, 2.0f, 60.0f);
    fogStart = std::clamp(fogStart, 0.0f, 0.9f);
    sunElevation = std::clamp(sunElevation, 2.0f, 89.0f);
    sunAzimuth = std::fmod(std::fmod(sunAzimuth, 360.0f) + 360.0f, 360.0f);
    sunIntensity = std::clamp(sunIntensity, 0.0f, 3.0f);
    ambient = std::clamp(ambient, 0.0f, 3.0f);
    exposure = std::clamp(exposure, 0.25f, 3.0f);
    skyRotation = std::fmod(std::fmod(skyRotation, 360.0f) + 360.0f, 360.0f);
    cloudQuality = std::clamp(cloudQuality, 0, 2);
    cloudCoverage = std::clamp(cloudCoverage, 0.0f, 1.0f);
    cloudDensity = std::clamp(cloudDensity, 0.0f, 4.0f);
    cloudAltitude = std::clamp(cloudAltitude, 300.0f, 6000.0f);
}

GraphicsSettings graphicsPreset(int quality, GraphicsSettings keep) {
    keep.quality = std::clamp(quality, 0, 3);
    static constexpr float draw[] = {10, 20, 30, 50};
    static constexpr int clouds[] = {0, 1, 1, 2};
    static constexpr float soft[] = {1.0f, 1.0f, 1.25f, 1.5f};
    keep.drawDistanceKm = draw[keep.quality];
    keep.cloudQuality = clouds[keep.quality];
    keep.shadowSoftness = soft[keep.quality];
    keep.farForest = keep.quality >= 1;
    keep.antialiasing = keep.quality >= 1 ? 1 : 0;
    keep.clampAll();
    return keep;
}

#define GRAPHICS_FIELDS(X) \
    X(quality) X(antialiasing) X(resolution) X(shadows) X(shadowSoftness) X(forestProxies) X(farForest) \
    X(massClusters) X(objectError) X(vegetationMeshPixels) X(vegetationMeshKiloTriangles) X(farTrees) X(farTreesStart) \
    X(terrainBlend) X(terrainTextures) X(drawDistanceKm) X(fog) X(fogStart) X(sunElevation) X(sunAzimuth) \
    X(sunIntensity) X(ambient) X(exposure) X(skybox) X(skyRotation) X(clouds) X(cloudQuality) \
    X(cloudCoverage) X(cloudDensity) X(cloudAltitude)

GraphicsSettings loadGraphicsSettings(const std::filesystem::path& file) {
    GraphicsSettings settings;
    std::ifstream input(file);
    if (!input) return settings;
    const auto json = nlohmann::json::parse(input, nullptr, false);
    if (!json.is_object()) return settings;
#define READ(name) if (json.contains(#name)) { try { json.at(#name).get_to(settings.name); } catch (...) {} }
    GRAPHICS_FIELDS(READ)
#undef READ
    settings.clampAll();
    return settings;
}

bool saveGraphicsSettings(const std::filesystem::path& file, const GraphicsSettings& settings) {
    nlohmann::json json;
#define WRITE(name) json[#name] = settings.name;
    GRAPHICS_FIELDS(WRITE)
#undef WRITE
    const auto temporary = file.string() + ".tmp";
    {
        std::ofstream output(temporary);
        if (!output) return false;
        output << json.dump(2) << '\n';
        if (!output) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, file, error);
    return !error;
}
} // namespace game

