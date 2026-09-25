#pragma once
#include "game/generation/world_domain.hpp"
#include <string>

namespace generation {
enum class ScaleClass { WorldRelative, SemiRelative, Absolute };
struct ScaleRule {
    ScaleClass kind = ScaleClass::Absolute;
    double value = 1; // fraction for WorldRelative; metres at 100 km for SemiRelative
    double exponent = 0.35;
    double minimumMetres = 1;
    double maximumMetres = 200000;
    std::array<double,2> metres(const WorldDomain&) const;
};

// New-mode metadata only. No implicit conversion into a dense WorldMapData.
struct WorldDescriptor {
    static constexpr std::uint32_t kFormatVersion = 1, kGeneratorVersion = 1, kPolicyVersion = 1;
    WorldDomain domain{207360,207360};
    std::uint64_t seed = 1;
    std::uint32_t coarseStepMetres = 256;
    std::array<std::uint32_t,2> overviewSamples{811,811};
    std::uint64_t coarseSampleCount() const; // estimate, does NOT allocate
    std::uint64_t fingerprint() const;
    std::string serialize() const;
    static WorldDescriptor parse(std::string_view);
    void validate() const;
    bool operator==(const WorldDescriptor&) const = default;
};

struct WorldScalePolicy {
    inline static constexpr std::array<std::uint32_t,4> kCoarseSteps{256,512,1024,2048};
    std::uint32_t coarseStepMetres = 0; // auto, resolved once and persisted
    std::uint32_t overviewSideLimit = 1024;
    std::uint64_t coarseSampleBudget = 1024*1024;
    WorldDescriptor describe(const WorldDomain&, std::uint64_t seed) const;
};
} // namespace generation

