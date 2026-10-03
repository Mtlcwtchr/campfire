#pragma once
// Ground cover and secondary scatter by zone
// (doc/plan_procedural_environment_2026-10-03.md, part E).
//
// Grass and moss are the ground's ecological state, not authored features:
// where the conditions allow, dense cover can be nearly everywhere, but which
// cover and how dense follows from the same fields that chose the ground's
// material. A rule says, for one zone and one tier, what grows, how dense,
// and how that density answers to fields, zone scalars and mask channels,
// broken into patches at two scales (3-10 m inside 20-100 m) so it is never a
// uniform carpet.
//
//   tier "ground"     dense cover drawn on the GPU (foliage_pages.hlsl): the
//                     rule becomes a row of the cover table the shader reads
//   tier "secondary"  flowers, ferns, shrubs, branches, small stones, placed
//                     on the CPU by scatter.hpp
//
// The engine evaluates rules; the game writes them
// (content/config/environment/cover.json).
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/environment/fields.hpp"
#include "engine/environment/recipe.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

enum class CoverTier : std::uint8_t { Ground, Secondary };
inline constexpr const char* kCoverTierNames[] = {"ground", "secondary"};

// One factor of a density: smoothstep of a source between lo and hi.
struct CoverModifier {
    enum class Source : std::uint8_t { Field, ZoneScalar, Mask } source = Source::Field;
    std::string name;
    std::uint8_t index = 0;    // resolved field id, zone scalar index or mask channel
    float lo = 0, hi = 1;
    bool invert = false;
    float floor = 0;           // the factor never goes below this
};

struct CoverRule {
    std::string zone;          // zone name, or "*" for every zone
    ZoneTypeId zoneId = kNoZone;
    bool anyZone = false;
    CoverTier tier = CoverTier::Secondary;
    std::vector<ModelChoice> models;   // secondary
    std::string foliage;               // ground: the foliage the game draws (its own name)
    double density = 1;        // ground: 0..1 cover; secondary: objects a 100 m2
    double patchSmall = 6, patchLarge = 50;   // metres
    double patchContrast = 0.6;               // 0 uniform, 1 hard islands
    double patchElongation = 1.6;             // along the slope's contour
    int clusterMin = 1, clusterMax = 1;       // secondary: objects a clump
    double clusterRadius = 1.5;
    double scaleMin = 0.8, scaleMax = 1.2;
    double height = 1;         // ground: blade height multiplier
    std::array<float, 3> tint{1, 1, 1};
    std::vector<CoverModifier> modifiers;
    std::string tag;
};

struct CoverRules {
    std::vector<CoverRule> rules;
};

struct CoverProblem {
    std::string file;
    std::string what;
};
CoverRules loadCover(const std::filesystem::path& file, std::vector<CoverProblem>* problems);

// What a modifier reads, at a point.
struct CoverInputs {
    const FieldSample* fields = nullptr;
    const ZoneScalars* zone = nullptr;
    const std::array<float, 8>* masks = nullptr;
};
float modifierFactor(const CoverModifier& m, const CoverInputs& in);
// Density of a rule at a point before patches: rule density x every modifier.
float ruleDensity(const CoverRule& rule, const CoverInputs& in);
// The two-scale patch field, 0..1, elongated along `contourAngle` (radians).
float coverPatch(std::uint64_t seed, const CoverRule& rule, double x, double y, double contourAngle);

// The GPU cover table: per zone type, kCoverRows float4 rows describing its
// ground tier (the first ground rule of that zone, or of "*").
//
//   row 0: density, height, patchSmall, patchLarge
//   row 1: patchContrast, patchElongation, tint index (reserved), modifiers used
//   row 2..5: one modifier each: source*64 + index, lo, hi, invert + 2*floor
inline constexpr std::size_t kCoverRows = 6;
std::vector<std::array<float, 4>> coverTable(const CoverRules& rules, std::size_t zoneTypes);

} // namespace engine::environment
