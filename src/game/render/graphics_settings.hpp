#pragma once
// Every graphics option the explorer exposes, in one value.
//
// Owned by the client, edited by the settings panel, handed to WorldRenderer
// each frame. Nothing here is a gameplay value: changing any of it never
// changes the world, only how it is drawn.
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace game {
// The choices of the resolution dropdown, in physical pixels. 0 x 0 is the
// desktop's own, fullscreen.
struct Resolution { const char* name; int width, height; };
inline constexpr std::array<Resolution, 8> kResolutions{{
    {"Window (as is)", -1, -1},
    {"1280 x 720", 1280, 720}, {"1600 x 900", 1600, 900}, {"1920 x 1080", 1920, 1080},
    {"2560 x 1440", 2560, 1440}, {"3200 x 1800", 3200, 1800}, {"3840 x 2160", 3840, 2160},
    {"Fullscreen (desktop)", 0, 0}}};
struct GraphicsSettings {
    // Quality
    int quality = 2;                 // 0 low, 1 medium, 2 high, 3 ultra: LOD error allowance
    int antialiasing = 2;            // 0 off, 1 MSAA 4x (rebuilds the render state), 2 FXAA (post)
    // Window resolution: 0 leaves the window as it is, the rest are the
    // entries of kResolutions (pixels, not points), the last is fullscreen.
    int resolution = 0;
    bool shadows = true;
    float shadowSoftness = 1.0f;     // procedural shadow filter spread, texels
    bool forestProxies = true;       // 32 m runtime proxies
    bool farForest = true;           // 128/512/2048 m hierarchy
    bool massClusters = false;       // 128 m merged region meshes replacing their members
    float objectError = 2.0f;        // multiplier on the object (tree/rock) LOD pixel error
    // Trees/bushes are meshes only when at least this tall on screen AND among
    // the largest that fit the triangle budget; the rest are impostors.
    float vegetationMeshPixels = 220.0f;
    float vegetationMeshKiloTriangles = 600.0f;
    bool farTrees = true;            // GPU-scattered tree impostors out to the horizon
    float farTreesStart = 1500.0f;   // where placed objects hand over to them, metres
    // How far grass and ground flora are drawn, metres: the far tier's
    // candidates are spread over this disc only, and fade out at its rim.
    float foliageDistance = 2000.0f;
    float terrainBlend = 3.0f;       // metres of material transition band
    int terrainTextures = 1;         // 0 half (1024), 1 full (2048 UE originals); rebuilds the renderer
    // Distance and fog
    float drawDistanceKm = 30.0f;
    bool fog = true;
    float fogStart = 0.3f;           // fraction of the draw distance
    // Lighting
    float sunElevation = 39.0f;      // degrees above the horizon
    float sunAzimuth = 225.0f;       // degrees, compass (0 = +Y north, 90 = +X east)
    float sunIntensity = 1.0f;
    float ambient = 1.0f;
    float exposure = 1.0f;
    // The picture's grade (GradePass): warm light, a print's curve, a golden
    // bloom, vignette and grain. Strength blends it with the ungraded frame.
    bool grade = true;
    float gradeStrength = 1.0f;
    // Sky and clouds
    bool skybox = true;
    float skyRotation = 0.0f;        // degrees
    bool clouds = true;
    int cloudQuality = 1;            // 0 low (12 steps), 1 medium (24), 2 high (48)
    float cloudCoverage = 0.45f;
    float cloudDensity = 1.0f;
    float cloudAltitude = 1800.0f;   // metres

    // How much coarser than "high" the LOD may be.
    [[nodiscard]] double lodScale() const {
        static constexpr double scales[] = {2.5, 1.6, 1.0, 0.7};
        return scales[quality < 0 ? 0 : quality > 3 ? 3 : quality];
    }
    [[nodiscard]] int cloudSteps() const {
        static constexpr int steps[] = {12, 24, 48};
        return clouds ? steps[cloudQuality < 0 ? 0 : cloudQuality > 2 ? 2 : cloudQuality] : 0;
    }
    // Towards the light, z up. Azimuth is a compass bearing.
    [[nodiscard]] std::array<float, 3> sunDirection() const {
        const double e = sunElevation * 3.14159265358979 / 180.0, a = sunAzimuth * 3.14159265358979 / 180.0;
        return {float(std::sin(a) * std::cos(e)), float(std::cos(a) * std::cos(e)), float(std::sin(e))};
    }
    void clampAll();
    bool operator==(const GraphicsSettings&) const = default;
};
// Presets the quality dropdown applies to everything, not only the LOD.
GraphicsSettings graphicsPreset(int quality, GraphicsSettings keep = {});
// graphics.json beside the content directory; missing or broken files give defaults.
GraphicsSettings loadGraphicsSettings(const std::filesystem::path& file);
bool saveGraphicsSettings(const std::filesystem::path& file, const GraphicsSettings& settings);
} // namespace game

