#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "engine/core/fixed.hpp"

namespace generation {

struct WorldMapData;
struct WorldMapParams;

enum class LandformFamily : std::uint8_t {
    Alpine, Glacial, Volcanic, Canyon, Folded, Arid, Old, Towers, Count
};
const char* landformFamilyName(LandformFamily family);

// Content decoding is the only floating-point boundary. Sampling is Fixed,
// endian-independent and independent of the camera, chunk and worker order.
struct TerrainReference {
    std::string id;
    LandformFamily family = LandformFamily::Old;
    std::uint32_t width = 0, height = 0;
    core::Fixed spacing, offset, scale, borderHeight, relief;
    std::uint64_t fingerprint = 0;
    std::vector<std::uint16_t> heights;
    [[nodiscard]] core::Fixed sample(core::Fixed u, core::Fixed v) const;
    // Bake-only Gaussian low-pass in source metres; retain a small residual so
    // the real ridges stay recognisable without inheriting every small summit.
    [[nodiscard]] core::Fixed sampleMountain(core::Fixed u, core::Fixed v) const;
};

// Throws with the offending path on malformed/truncated content.
TerrainReference loadTerrainReference(const std::filesystem::path& metadata);
std::filesystem::path defaultTerrainReferenceRoot();

struct LandformPatch {
    std::string referenceId; // empty means analytic fallback, not a missing DEM
    std::uint64_t sourceFingerprint = 0;
    LandformFamily family = LandformFamily::Old;
    std::int32_t originX = 0, originY = 0, radius = 0;
    std::uint32_t side = 0;
    std::uint8_t freshness = 0;
    bool island = false;
    // Signed decimetres relative to the original macro surface. These are
    // saved with the world: removing/changing a source DEM cannot alter a save.
    std::vector<std::int16_t> delta;
};

class HybridTerrain {
public:
    static constexpr std::int32_t kSampleMetres = 32;
    static constexpr std::int32_t kMaxPatchRadiusMetres = 10240;
    static constexpr std::int32_t kMaxPatchSide = 2*kMaxPatchRadiusMetres/kSampleMetres+1;
    static constexpr std::int32_t kVersion = 3; // noisy deposits; v1/v2 retain circular material masks
    static constexpr std::int32_t kMaxResidualMetres = 450;
    static constexpr std::int32_t kSeaBedMetres = -8;
    struct Sample {
        core::Fixed delta, influence, volcanic, barren;
    };
    std::int32_t width = 0, height = 0;
    std::int32_t version = kVersion;
    std::vector<LandformPatch> patches;
    // Exactly the quantised change fed to climate and drainage, in metres.
    std::vector<std::int32_t> macroDelta;
    // Whole-world downhill erosion, unsigned decimetres to subtract. A bounded
    // resolution grid keeps broad tectonic slopes connected across patch edges.
    std::int32_t incisionStep = 0;
    std::vector<std::uint16_t> incision;
    std::vector<std::string> diagnostics;

    [[nodiscard]] Sample sample(core::Fixed x, core::Fixed y) const;
    [[nodiscard]] core::Fixed incisionAt(core::Fixed x, core::Fixed y) const;
    [[nodiscard]] core::Fixed residual(core::Fixed x, core::Fixed y, core::Fixed delta) const;
    [[nodiscard]] std::uint64_t fingerprint() const { return fingerprint_; }
    // Once before publication as shared_ptr<const>. Builds a macro-cell index
    // and hashes baked values, not paths, addresses or native object padding.
    void prepare();

private:
    std::vector<std::vector<std::uint32_t>> spatial_;
    std::uint64_t fingerprint_ = 0;
};

// Called after elevation/sea classification, BEFORE climate and priority flood.
// Explicit disabled mode preserves the legacy generator; absent assets use
// family-specific analytic forms and report the fallback in diagnostics.
void generateHybridTerrain(WorldMapData& world, const WorldMapParams& params);

// JSON payload embedded in the existing world save, with strict version/size
// validation and a checksum. Legacy saves have no hybrid payload.
std::string saveHybridTerrain(const HybridTerrain& terrain);
std::shared_ptr<const HybridTerrain> loadHybridTerrain(const std::string& payload,
                                                      std::int32_t width, std::int32_t height);

} // namespace generation
