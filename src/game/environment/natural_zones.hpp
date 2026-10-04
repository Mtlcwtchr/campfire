#pragma once
// This game's natural zones (doc/plan_procedural_environment_2026-10-03.md, part J):
// the local scene types of a believable world, and how each follows from the
// fields. Fantasy regions are not here; they are not decided yet.
//
// Every zone is a product of soft factors over the fields - canopy, standing
// wetness, slope, rock, warmth, nearness to rivers and the sea - so a zone
// fades into its neighbour where its causes do, and nothing is decided by a
// noise threshold.
#include <array>
#include <span>
#include <vector>

#include "engine/environment/fields.hpp"
#include "engine/environment/masks.hpp"
#include "engine/environment/zones.hpp"

namespace world::environment {

namespace env = engine::environment;

// The game's own fields, beyond the engine's (registered by name).
struct GameFields {
    env::FieldId riverNear, lakeNear, seaNear, woodland;
    env::FieldId canopyBroad;   // the canopy on a ring 120 m round: is this an opening in a wood?
};
const GameFields& gameFields();

// The zone types, in classifier order. 0 is "unclassified".
enum class Zone : std::uint8_t {
    Unclassified,
    // forest
    OldGrowth, OpenWoodland, ForestEdge, Clearing, Windthrow, WetHollow, StreamBank, CliffFoot, RockySlope,
    // open country
    Meadow, Scrub, Heath, Steppe, DryWash,
    // wet ground and water's edge
    Floodplain, SwampForest, Bog, Fen, Quagmire, Marsh, RiverMarsh, SaltMarsh, Shore, Dunes,
    // high and cold
    Scree, Outcrop, AlpineMeadow, Tundra,
    Count
};
inline constexpr std::size_t kZoneCount = std::size_t(Zone::Count);

class NaturalZones final : public env::ZoneClassifier {
public:
    NaturalZones();
    [[nodiscard]] std::span<const env::ZoneType> types() const override { return types_; }
    void classify(const env::FieldSample& fields, std::span<float> weights, env::ZoneScalars& scalars) const override;

private:
    std::vector<env::ZoneType> types_;
};

// What the zones put into the mask channels before any feature: wet ground
// is wet, a bog is mossy, a forest floor has litter, scree is talus. The
// channel order is content/config/environment/masks.json's, checked by
// content_validator against kZoneMaskChannels.
inline constexpr std::array<const char*, 8> kZoneMaskChannels{
        "wetness", "suppress", "boost", "rock", "talus", "litter", "moss", "scar"};
void naturalZoneMasks(double x, double y, const env::EnvironmentZone& zone, env::MaskValues& out);

} // namespace world::environment
