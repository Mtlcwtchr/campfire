#include "game/environment/natural_zones.hpp"

#include <algorithm>
#include <cmath>

namespace world::environment {

namespace field = env::field;

namespace {

float up(float x, float a, float b) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float down(float x, float a, float b) { return 1 - up(x, a, b); }
// Rises over a..b, falls over c..d.
float band(float x, float a, float b, float c, float d) { return up(x, a, b) * down(x, c, d); }

struct Named {
    Zone zone;
    const char* name;
    std::array<float, 3> colour;
};
constexpr Named kNames[] = {
        {Zone::Unclassified, "unclassified", {0.45f, 0.45f, 0.45f}},
        {Zone::OldGrowth, "old_growth", {0.05f, 0.30f, 0.10f}},
        {Zone::OpenWoodland, "open_woodland", {0.25f, 0.55f, 0.20f}},
        {Zone::ForestEdge, "forest_edge", {0.45f, 0.65f, 0.25f}},
        {Zone::Clearing, "clearing", {0.70f, 0.85f, 0.35f}},
        {Zone::Windthrow, "windthrow", {0.55f, 0.40f, 0.20f}},
        {Zone::WetHollow, "wet_hollow", {0.20f, 0.45f, 0.45f}},
        {Zone::StreamBank, "stream_bank", {0.20f, 0.60f, 0.65f}},
        {Zone::CliffFoot, "cliff_foot", {0.45f, 0.40f, 0.35f}},
        {Zone::RockySlope, "rocky_slope", {0.55f, 0.52f, 0.45f}},
        {Zone::Meadow, "meadow", {0.65f, 0.80f, 0.25f}},
        {Zone::Scrub, "scrub", {0.55f, 0.55f, 0.20f}},
        {Zone::Heath, "heath", {0.60f, 0.35f, 0.50f}},
        {Zone::Steppe, "steppe", {0.85f, 0.75f, 0.40f}},
        {Zone::DryWash, "dry_wash", {0.85f, 0.65f, 0.45f}},
        {Zone::Floodplain, "floodplain", {0.40f, 0.70f, 0.55f}},
        {Zone::SwampForest, "swamp_forest", {0.15f, 0.35f, 0.30f}},
        {Zone::Bog, "bog", {0.55f, 0.45f, 0.30f}},
        {Zone::Fen, "fen", {0.40f, 0.55f, 0.30f}},
        {Zone::Quagmire, "quagmire", {0.30f, 0.25f, 0.15f}},
        {Zone::Marsh, "marsh", {0.35f, 0.65f, 0.40f}},
        {Zone::RiverMarsh, "river_marsh", {0.25f, 0.55f, 0.55f}},
        {Zone::SaltMarsh, "salt_marsh", {0.55f, 0.60f, 0.55f}},
        {Zone::Shore, "shore", {0.75f, 0.75f, 0.65f}},
        {Zone::Dunes, "dunes", {0.95f, 0.85f, 0.55f}},
        {Zone::Scree, "scree", {0.65f, 0.62f, 0.58f}},
        {Zone::Outcrop, "outcrop", {0.50f, 0.48f, 0.50f}},
        {Zone::AlpineMeadow, "alpine_meadow", {0.60f, 0.75f, 0.60f}},
        {Zone::Tundra, "tundra", {0.70f, 0.70f, 0.75f}},
};
static_assert(std::size(kNames) == kZoneCount);

} // namespace

const GameFields& gameFields() {
    static const GameFields f{*env::fieldId("river_near", true), *env::fieldId("lake_near", true),
                              *env::fieldId("sea_near", true), *env::fieldId("woodland", true),
                              *env::fieldId("canopy_broad", true)};
    return f;
}

NaturalZones::NaturalZones() {
    for (const auto& n : kNames) types_.push_back({n.name, n.colour});
}

void NaturalZones::classify(const env::FieldSample& f, std::span<float> w, env::ZoneScalars& s) const {
    std::fill(w.begin(), w.end(), 0.0f);
    const auto& g = gameFields();
    const float canopy = f.get(field::Canopy, 0), wet = f.get(field::Wetness, 0), moist = f.get(field::Moisture, 0.5f);
    const float slope = f.get(field::Slope, 0), rock = f.get(field::Rockiness, 0), sand = f.get(field::Sand, 0);
    const float snow = f.get(field::Snow, 0), temp = f.get(field::Temperature, 10), elev = f.get(field::Elevation, 100);
    const float fertile = f.get(field::Fertility, 0.5f), drain = f.get(field::Drainage, 0.6f);
    const float tpiS = f.get(field::TpiSmall, 0), tpiL = f.get(field::TpiLarge, 0);
    const float conv = f.get(field::Convergence, 0), exposure = f.get(field::Exposure, 0.5f);
    const float soil = f.get(field::SoilDepth, 0.5f);
    const float river = f.get(g.riverNear, 0), lake = f.get(g.lakeNear, 0), sea = f.get(g.seaNear, 0);
    const float woodland = f.get(g.woodland, canopy);
    const float around = f.get(g.canopyBroad, canopy);
    const float waterNear = std::max({river, lake, sea});
    const auto set = [&](Zone z, float v) { w[std::size_t(z)] = std::max(0.0f, v); };

    s.density = canopy;
    s.wetness = wet;
    s.exposure = exposure;
    s.age = soil;
    s.disturbance = up(exposure, 0.75f, 0.92f) * up(tpiL, 5, 20);
    s.rockiness = rock;
    s.canopy = canopy;
    s.soilDepth = soil;

    // Open water, ice and snow fields: no zone the ground's dressing cares for.
    if (f.get(field::Water, 0) > 0.5f) { set(Zone::Unclassified, 1); return; }
    set(Zone::Unclassified, 0.02f + 0.5f * up(snow, 0.5f, 0.8f));

    const float dry = down(wet, 0.5f, 0.68f);
    const float level = down(slope, 0.04f, 0.12f);
    const float open = down(canopy, 0.12f, 0.3f);

    // Forest.
    set(Zone::OldGrowth, up(canopy, 0.6f, 0.85f) * dry * down(slope, 0.5f, 0.8f) * up(soil, 0.35f, 0.65f));
    set(Zone::OpenWoodland, band(canopy, 0.22f, 0.35f, 0.55f, 0.7f) * dry);
    set(Zone::ForestEdge, band(canopy, 0.08f, 0.18f, 0.3f, 0.45f) * dry * 0.9f);
    // An opening the wood stands round, not any open ground in wooded country.
    set(Zone::Clearing, down(canopy, 0.08f, 0.2f) * up(around, 0.28f, 0.45f) * up(woodland, 0.45f, 0.7f) * dry);
    set(Zone::Windthrow, up(canopy, 0.35f, 0.6f) * s.disturbance * 1.2f);
    set(Zone::WetHollow, up(canopy, 0.25f, 0.45f) * band(wet, 0.45f, 0.6f, 0.75f, 0.85f) * up(conv, 0.35f, 0.6f));
    set(Zone::StreamBank, up(river, 0.04f, 0.2f) * down(river, 0.5f, 0.75f) * up(canopy, 0.15f, 0.35f));
    set(Zone::CliffFoot, down(slope, 0.35f, 0.6f) * up(-tpiL, 3, 10) * up(rock, 0.12f, 0.3f));
    set(Zone::RockySlope, up(slope, 0.45f, 0.7f) * up(rock, 0.2f, 0.45f) * up(canopy, 0.1f, 0.25f));

    // Open country.
    set(Zone::Meadow, open * dry * up(moist, 0.3f, 0.45f) * down(slope, 0.4f, 0.65f) * up(fertile, 0.25f, 0.45f) *
                      down(around, 0.3f, 0.5f));
    set(Zone::Scrub, band(canopy, 0.03f, 0.1f, 0.22f, 0.35f) * down(moist, 0.45f, 0.6f) * dry);
    set(Zone::Heath, open * down(fertile, 0.3f, 0.45f) * up(drain, 0.5f, 0.7f) * band(moist, 0.3f, 0.4f, 0.7f, 0.85f));
    set(Zone::Steppe, open * down(moist, 0.28f, 0.4f) * up(temp, 4, 10) * dry);
    set(Zone::DryWash, down(moist, 0.22f, 0.32f) * up(conv, 0.5f, 0.7f) * level);

    // Wet ground. The wettest decide: the bog types share standing water and
    // differ by what feeds them - rain on poor ground (bog), groundwater on
    // rich (fen), a river's floods (river marsh), the sea's tides (salt marsh).
    const float sodden = up(wet, 0.6f, 0.75f);
    set(Zone::Floodplain, up(river, 0.05f, 0.2f) * level * down(canopy, 0.3f, 0.5f) * down(wet, 0.62f, 0.8f));
    set(Zone::SwampForest, sodden * up(canopy, 0.35f, 0.55f));
    set(Zone::Bog, sodden * down(canopy, 0.25f, 0.4f) * down(fertile, 0.3f, 0.45f) * down(temp, 12, 18) *
                   down(waterNear, 0.1f, 0.3f));
    set(Zone::Fen, sodden * down(canopy, 0.25f, 0.4f) * up(fertile, 0.35f, 0.5f) * down(river, 0.1f, 0.3f) *
                   down(sea, 0.05f, 0.15f));
    set(Zone::Quagmire, up(wet, 0.82f, 0.94f) * level * up(conv, 0.45f, 0.65f) * down(canopy, 0.2f, 0.35f) * 1.3f);
    set(Zone::Marsh, up(wet, 0.62f, 0.78f) * up(temp, 7, 13) * down(canopy, 0.15f, 0.3f) * up(lake + river, 0.04f, 0.18f) *
                     down(sea, 0.05f, 0.15f));
    set(Zone::RiverMarsh, up(river, 0.15f, 0.35f) * up(wet, 0.5f, 0.68f) * level * down(canopy, 0.3f, 0.5f) * 1.2f);
    set(Zone::SaltMarsh, up(sea, 0.1f, 0.3f) * up(wet, 0.45f, 0.65f) * level);
    set(Zone::Shore, up(waterNear, 0.15f, 0.4f) * dry * down(sand, 0.4f, 0.6f) * down(canopy, 0.3f, 0.5f) * 0.8f);
    set(Zone::Dunes, up(sand, 0.4f, 0.65f) * down(wet, 0.5f, 0.65f));

    // High and cold.
    set(Zone::Scree, up(slope, 0.6f, 0.9f) * up(rock, 0.35f, 0.6f) * down(canopy, 0.12f, 0.25f));
    set(Zone::Outcrop, up(rock, 0.55f, 0.8f) * up(tpiS, 0.5f, 3.0f));
    set(Zone::AlpineMeadow, down(temp, 3, 8) * open * down(rock, 0.4f, 0.6f) * up(elev, 1100, 1700) * dry);
    set(Zone::Tundra, down(temp, -3, 2) * open * down(snow, 0.5f, 0.8f));
}

void naturalZoneMasks(double, double, const env::EnvironmentZone& zone, env::MaskValues& out) {
    const auto of = [&](Zone z) { return zone.weights.of(env::ZoneTypeId(z)); };
    enum { Wetness, Suppress, Boost, Rock, Talus, Litter, Moss, Scar };
    out[Wetness] = std::max(out[Wetness], std::clamp(zone.scalars.wetness - 0.45f, 0.0f, 1.0f) * 1.8f);
    out[Moss] = std::max(out[Moss], of(Zone::Bog) + of(Zone::SwampForest) * 0.8f + of(Zone::WetHollow) * 0.6f +
                                            of(Zone::OldGrowth) * 0.4f + of(Zone::Fen) * 0.5f);
    out[Litter] = std::max(out[Litter], of(Zone::OldGrowth) * 0.9f + of(Zone::OpenWoodland) * 0.6f +
                                                of(Zone::SwampForest) * 0.5f + of(Zone::Windthrow) * 0.8f);
    out[Talus] = std::max(out[Talus], of(Zone::Scree) + of(Zone::CliffFoot) * 0.7f);
    out[Rock] = std::max(out[Rock], of(Zone::Outcrop) * 0.8f + of(Zone::Scree) * 0.4f);
    out[Scar] = std::max(out[Scar], of(Zone::Windthrow) * 0.7f + of(Zone::DryWash) * 0.5f);
    for (auto& v : out) v = std::clamp(v, 0.0f, 1.0f);
}

} // namespace world::environment
