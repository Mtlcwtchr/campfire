#pragma once
// Scatter primitives (doc/plan_procedural_environment_2026-10-03.md, part E).
//
// Two jobs, one set of rules about what a good arrangement is:
//
//   dress()  a feature's supporting set pieces - the talus under a cliff, the
//            stones along a channel, the moss and fungi round a fallen trunk -
//            by the primitives its recipe names;
//   cover()  the secondary tier of ground cover by zone (cover.hpp): flowers,
//            ferns, shrubs, branches and small stones in clumps and patches.
//
// Cluster beats scatter. Every primitive builds a small coherent arrangement
// round one anchor, stretched along the direction the place gives it (slope,
// flow, a cliff line), with sizes falling away from the source, and leaves
// the ground round a composition's anchor empty (negative space).
//
// Objects are plain records: a model id the game resolved, a position, a
// scale and a yaw. What they become - ECS entities, render instances - is the
// game's business.
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/environment/catalogue.hpp"
#include "engine/environment/feature_layer.hpp"
#include "engine/environment/masks.hpp"

namespace engine::environment {

struct PlacedObject {
    std::uint64_t id = 0;
    double x = 0, y = 0, z = 0;
    float scale = 1, yaw = 0, tint = 1, sink = 0;
    std::uint32_t model = 0;
    std::uint32_t recipe = 0xffffffffu;   // the recipe that dressed it, or none for cover
    std::uint16_t rule = 0;               // index of the scatter or cover rule
    bool alignToGround = false;
    bool operator==(const PlacedObject&) const = default;
};

// The ground objects stand on, metres. The visual height, not H_sim.
using GroundAt = std::function<double(double x, double y)>;

// A feature's supporting pieces, by its recipe's scatter rules.
void dress(const Catalogue& catalogue, const FeatureInstance& instance, const GroundAt& ground,
           std::vector<PlacedObject>& out);

// The levels of a rock hierarchy (spec §7): bedrock -> cliff -> boulder ->
// cluster -> cobble -> pebble -> scree. The recipe's model list is read big
// to small and spread over the levels it reaches.
struct RockLevel {
    const char* name;
    double scale;       // of the rule's scale range's top
    int countMin, countMax;
    double reach;       // of the rule's radius
};
inline constexpr RockLevel kRockLevels[] = {
        {"boulder", 1.0, 1, 2, 0.15}, {"cluster", 0.55, 2, 4, 0.4}, {"cobble", 0.28, 4, 9, 0.7},
        {"pebble", 0.13, 8, 16, 0.95}, {"scree", 0.07, 10, 22, 1.0}};

struct CoverContext {
    const Catalogue* catalogue = nullptr;
    const FieldSource* fields = nullptr;
    const ZoneField* zones = nullptr;
    const FeatureLayer* features = nullptr;
    ZoneMasks zoneMasks;
    HeightAt height;      // before features, for the masks
    GroundAt ground;      // what objects stand on
    std::uint64_t seed = 0;
    std::size_t budget = 200000;   // objects a call, at most
};

// The secondary cover over [x0, x1) x [y0, y1).
void cover(const CoverContext& context, double x0, double y0, double x1, double y1, std::vector<PlacedObject>& out);

// Whether a point is inside the negative space or the reveal corridor of
// any composition near it.
bool keptClear(const FeatureLayer& features, double x, double y);

// The mask channels at one point: the zones' share, then every feature's writes.
MaskValues masksAt(const FeatureLayer* features, const ZoneField* zones, const ZoneMasks& zoneMasks,
                   const HeightAt& height, double x, double y, EnvironmentZone* zoneOut = nullptr);

} // namespace engine::environment
