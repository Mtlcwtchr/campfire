#include "game/generation/world_map_gen.hpp"
#include "game/generation/hybrid_terrain.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <array>
#include <cmath>
#include <deque>
#include <limits>
#include <queue>
#include <set>
#include <vector>

#include "engine/core/rng.hpp"

namespace generation {
namespace {

using core::TilePos;

// Heights are carried through the fill and the river tracing at sixty-four times
// their stored resolution. Filling a hollow has to raise its floor above the
// ground around it by *something*, and that something is a whole metre-step on
// the stored scale: over a wide flood plain the increments piled up until the
// plain was a hill. At this resolution the step is a sixty-fourth of one, which
// is enough to give the water a direction and not enough to be terrain.
constexpr std::int32_t kHeightScale = 64;

// How much of a noise octave follows the size of the map, in percent. The
// continental octave follows it entirely, so a wider world is a wider continent
// rather than an archipelago of the old one; the fine octaves do not follow at
// all, so the extra width is spent on more coastline instead of a bigger
// version of the same coastline.
std::int32_t octaveScale(std::int32_t base, std::int32_t width, std::int32_t follows) {
    const std::int32_t grown = std::max(1, base * width / kReferenceWidth);
    return std::max(2, base + (grown - base) * follows / 100);
}

// The same value noise the local map uses, at the scale of a country rather than
// a field. Kept here rather than shared because the two are free to diverge: the
// local one answers "where is the stone", this one answers "where is the sea".
std::int32_t valueNoise(std::uint64_t seed, std::int32_t x, std::int32_t y) {
    std::uint64_t h = seed;
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(x)) * 0x9e3779b97f4a7c15ULL));
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(y)) * 0xc2b2ae3d27d4eb4fULL));
    return static_cast<std::int32_t>(h & 1023);
}

std::int32_t smoothNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t scale) {
    const std::int32_t cx = x / scale, cy = y / scale;
    const std::int32_t fx = x % scale, fy = y % scale;
    const std::int32_t v00 = valueNoise(seed, cx, cy);
    const std::int32_t v10 = valueNoise(seed, cx + 1, cy);
    const std::int32_t v01 = valueNoise(seed, cx, cy + 1);
    const std::int32_t v11 = valueNoise(seed, cx + 1, cy + 1);
    const std::int32_t top = v00 + (v10 - v00) * fx / scale;
    const std::int32_t bottom = v01 + (v11 - v01) * fx / scale;
    return top + (bottom - top) * fy / scale;
}

// Five octaves at continental scale: the big one decides where the land is, the
// small ones give it a coastline. Two octaves drew a rounded rectangle.
std::int32_t landNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    return (smoothNoise(seed, x, y, octaveScale(90, width, 100)) * 8 +
            smoothNoise(seed + 11, x, y, octaveScale(38, width, 60)) * 5 +
            smoothNoise(seed + 101, x, y, octaveScale(17, width, 30)) * 3 +
            smoothNoise(seed + 1009, x, y, octaveScale(7, width, 0)) * 2 +
            smoothNoise(seed + 10007, x, y, octaveScale(3, width, 0))) /
           19;
}

// The SHAPE of the land, as opposed to the texture on it.
//
// This exists because the shape was coming from somewhere it had no business
// coming from. The crust step between an oceanic plate and a continental one is
// four hundred and fifty units; the noise, after being scaled down everywhere
// the ground is not being pushed up, was contributing under a hundred. So the
// coastline was the plate diagram - Voronoi cells, warped and blurred, but
// still cells, and what you see is exactly that: long straightish runs, sharp
// corners, and squares. No amount of fraying an edge fixes a shape that is a
// polygon underneath.
//
// Plates decide FEATURES - where a range stands, where a rift opens, where a
// trench runs. They are very bad at deciding where a continent is, because a
// continent is not a cell of anything. This is what decides that: five octaves
// with the low ones carrying most of the amplitude, warped through itself so
// that even the largest lobe is not an ellipse.
std::int32_t continentNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    // The longest wave has to FIT, and it did not.
    //
    // Scaled off the reference width the first octave came out at about a
    // hundred kilometres, which on a two-hundred-kilometre map is two lobes
    // across the whole world - so the world was one big land mass and one big
    // island, every time, whatever the seed. A map wants four or five lobes in
    // it before it has anything to offer: continents to be separate, seas
    // between them, and the smaller octaves left over to break the edges into
    // islands.
    //
    // A fifth of the map, then, and capped so that a very large world gains
    // more continents rather than the same few drawn bigger.
    const std::int32_t first = std::clamp(width / 5, 8, 150);
    // Warped by a third of that: enough to pull a lobe into a peninsula and
    // tear a strait through it, not so much that it is only the continent
    // moving about.
    const std::int32_t swing = std::max(3, first / 3);
    const std::int32_t wx = x + (smoothNoise(seed ^ 0xC0A7F, x, y, first) - 512) * swing / 512;
    const std::int32_t wy = y + (smoothNoise(seed ^ 0xC0A80, x, y, first) - 512) * swing / 512;
    // Nothing below four cells, ever. This field is stored on the macro lattice
    // and read back through a spline over it, so a wave of two or three cells is
    // at that lattice's own frequency: it cannot be reconstructed, and what
    // comes out instead is a crease on every cell line. The linter measures it
    // directly, and measured it at seventy centimetres the moment these octaves
    // were allowed down there.
    const auto octave = [&](int divisor) { return std::max(4, first / divisor); };
    return (smoothNoise(seed ^ 0x1A2D, wx, wy, first) * 13 +
            smoothNoise(seed ^ 0x1A2E, wx, wy, octave(2)) * 8 +
            smoothNoise(seed ^ 0x1A2F, wx, wy, octave(4)) * 5 +
            smoothNoise(seed ^ 0x1A30, wx, wy, octave(8)) * 3 +
            smoothNoise(seed ^ 0x1A31, wx, wy, octave(16)) * 2) /
           31;
}

// Ridges, which is what makes mountains mountains rather than a dome: the noise
// folded at its middle, so its peaks are creases and not bumps.
std::int32_t ridgeNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t width) {
    const auto fold = [](std::int32_t v) { return 1023 - std::abs(2 * v - 1023); };
    return (fold(smoothNoise(seed, x, y, octaveScale(60, width, 100))) * 5 +
            fold(smoothNoise(seed + 7, x, y, octaveScale(26, width, 50))) * 3 +
            fold(smoothNoise(seed + 71, x, y, octaveScale(11, width, 0))) * 2) /
           10;
}

// Every settlement is named, and there are hundreds of them: two lists, because
// a Sumerian city and an Achaean one are not the same people and the map should
// say so. Repeats are numbered.
const std::array<const char*, 24> kSumerianNames{
        "Eridu",  "Uruk",   "Ur",      "Lagash", "Nippur",  "Kish",    "Umma",   "Larsa",
        "Sippar", "Shurup", "Adab",    "Isin",   "Girsu",   "Bad-Tib", "Akshak", "Zabala",
        "Marad",  "Kutha",  "Kisurra", "Awan",   "Dilbat",  "Nina",    "Kuara",  "Urum"};
// Four peoples, four books of names (D100). Sharing a list made three different
// cultures on one map all call their first settlement Mycenae, because the
// numbering runs per people: names have to be per people too.
const std::array<const char*, 24> kSteppeNames{
        "Arkaim",  "Sintasht", "Botai",   "Ulan",    "Kamenka", "Tarim",   "Yurga",  "Berel",
        "Kelermes", "Tuzla",   "Sarkel",  "Orlovka", "Chirik",  "Verkhne", "Ishim",  "Tobol",
        "Kurgan",  "Ustye",    "Alakul",  "Petrov",  "Rodniki", "Solonka", "Tamgaly", "Zhaman"};
const std::array<const char*, 24> kNorthNames{
        "Vantaa",  "Kierikki", "Sarsa",   "Pyheensilta", "Kukkarko", "Onega",  "Vygozero", "Suomus",
        "Rovaniemi", "Kemi",   "Pielinen", "Saimaa",     "Vuoksa",  "Ladoga", "Ilmen",    "Vodla",
        "Kargopol", "Beloozero", "Sukhona", "Vaga",      "Mezen",   "Pinega", "Emtsa",    "Vashka"};

const std::array<const char*, 24> kAchaeanNames{
        "Mycenae", "Tiryns", "Pylos",  "Argos",  "Thebes",  "Orchom", "Iolcos", "Sparta",
        "Gla",     "Midea",  "Asine",  "Lerna",  "Dimini",  "Krisa",  "Nichor", "Eutresis",
        "Aigion",  "Elateia", "Zygour", "Berbat", "Prosymna", "Aidon", "Kalydon", "Pharai"};


// Which of the cultures the content ships belongs in a given country (D100).
//
// A people is put where its way of living works, which is the whole reason the
// map carries climates at all: a culture that builds in reed and mud brick has
// nothing to build with on a wooded hillside, and one that lives off its flock
// has nothing to graze in a rain forest.
const char* ethnosForClimate(Climate c) {
    switch (c) {
        // Silt, reed and no stone: the valley cultures, and the country this
        // game's content was written for.
        case Climate::Delta:
        case Climate::RiverValley:
            return "sumerian";
        // Dry grass and thorn. Too little water for a field to be worth the
        // work, plenty for a flock that browses: herders, felt and tents.
        case Climate::Steppe:
        case Climate::Savanna:
        case Climate::Desert:
            return "yamna";
        // Cold and wooded, where grain is a gamble and timber is not: log
        // houses, hunting, and a winter kept in a smokehouse.
        case Climate::Taiga:
        case Climate::Tundra:
        case Climate::Ice:
            return "northfolk";
        // Warm, broken, wooded coasts and hills: the Achaeans.
        case Climate::Mediterranean:
        case Climate::TemperateForest:
        case Climate::TropicalForest:
        case Climate::Alpine:
        default:
            return "achaean";
    }
}

} // namespace

const char* livelihoodName(Livelihood l) {
    switch (l) {
        case Livelihood::Farmers:   return "farmers";
        case Livelihood::Herders:   return "herders";
        case Livelihood::Fishers:   return "fishers";
        case Livelihood::Foresters: return "foresters";
        case Livelihood::Miners:    return "miners";
    }
    return "farmers";
}

const char* climateName(Climate c) {
    switch (c) {
        case Climate::Ice:             return "ice";
        case Climate::Tundra:          return "tundra";
        case Climate::Taiga:           return "taiga";
        case Climate::TemperateForest: return "temperate forest";
        case Climate::Steppe:          return "steppe";
        case Climate::Mediterranean:   return "mediterranean";
        case Climate::Desert:          return "desert";
        case Climate::Savanna:         return "savanna";
        case Climate::TropicalForest:  return "tropical forest";
        case Climate::Alpine:          return "alpine";
        case Climate::RiverValley:     return "river valley";
        case Climate::Delta:           return "delta";
        case Climate::Count:           break;
    }
    return "temperate forest";
}

const char* rockTypeName(RockType type) {
    switch (type) {
        case RockType::HardRock:      return "hard rock";
        case RockType::SoftSediment:  return "soft sediment";
        case RockType::Limestone:     return "limestone";
        case RockType::Volcanic:      return "volcanic";
        case RockType::ClayRich:      return "clay rich";
        case RockType::Sandstone:     return "sandstone";
        case RockType::Alluvial:      return "alluvial";
        case RockType::Count:         break;
    }
    return "soft sediment";
}

std::int32_t riverFlowThresholdFrom(const WorldMapParams& params,
                                    const std::vector<std::int32_t>& flow,
                                    const std::vector<WorldCell>& cells) {
    if (params.riverFlow > 0) return params.riverFlow;
    // Not a number of drops but a share of the land: whatever flow puts one cell
    // in a hundred over it is what a river is here.
    //
    // It used to be a number, scaled off the map's area by a formula, and the
    // formula was tuned by measuring one size. Flow is the drainage area above a
    // cell, so it grows with the whole map: the same number that draws a
    // reasonable network on a map three hundred cells across draws nothing at
    // all on one sixty-four across - measured, literally nothing, not one river
    // cell on a world thirty-five kilometres over. And a world with no river in
    // it is not a world this game can be played in.
    //
    // Taken as a share, the question tunes itself at every size and cannot come
    // out empty. One in a hundred is the density the formula was aiming at where
    // it was tuned: eight to twelve river cells per thousand of land.
    std::vector<std::int32_t> land;
    land.reserve(cells.size() / 2);
    for (std::size_t i = 0; i < cells.size(); ++i)
        if (!cells[i].sea) land.push_back(flow[i]);
    if (land.empty()) return 1;
    const std::size_t over = std::max<std::size_t>(1, land.size() / 100);
    std::nth_element(land.begin(), land.end() - static_cast<std::ptrdiff_t>(over), land.end());
    return std::max(2, *(land.end() - static_cast<std::ptrdiff_t>(over)));
}

std::int32_t siteSpacingFor(const WorldMapParams& params);

std::int32_t siteCountFor(const WorldMapParams& params, std::int64_t landCells) {
    if (params.sites > 0) return params.sites;
    // As many as the land will hold at the spacing they have to keep, and not a
    // density picked in the abstract.
    //
    // It used to be one to every hundred thousand cells of land, which on the
    // worlds this game now offers is the floor of two, whatever their size - a
    // world of two neighbours whether it is thirty-five kilometres across or a
    // hundred. Tied to the spacing instead, every size comes out with a handful
    // of them, properly spread: a world always has neighbours, and a bigger
    // world has more of them rather than the same few further apart.
    const std::int64_t spacing = siteSpacingFor(params);
    return static_cast<std::int32_t>(
            std::clamp<std::int64_t>(landCells / (spacing * spacing), 2, 24));
}

std::int32_t siteSpacingFor(const WorldMapParams& params) {
    // How far apart two communities have to be, in cells, and therefore in real
    // distance: eighty cells is forty-three kilometres.
    //
    // Read as the spacing for a world of the reference width, and scaled down
    // with the map from there - never up past what was asked for.
    //
    // On a world thirty-five kilometres across, forty-three kilometres between
    // communities puts every one of them but the first outside it. Scaled rather
    // than merely capped, because the presets differ in this and the difference
    // has to survive: an archipelago's fifty and one land's hundred and ten come
    // out fifteen and thirty-three on a medium world instead of both hitting the
    // same ceiling.
    const std::int32_t across = std::min(params.width, params.height);
    return std::clamp(params.siteSpacing * across / kReferenceWidth, 3, params.siteSpacing);
}

std::vector<WorldPreset> loadWorldPresets(const std::filesystem::path& file) {
    std::vector<WorldPreset> presets;
    std::ifstream in(file);
    if (in) {
        nlohmann::json j;
        try {
            in >> j;
            for (const auto& e : j) {
                WorldPreset preset;
                preset.name = e.value("name", std::string("world"));
                preset.label = e.value("label", preset.name);
                preset.note = e.value("note", std::string());
                preset.params.seaPercent = e.value("sea_percent", preset.params.seaPercent);
                preset.params.plates = e.value("plates", preset.params.plates);
                preset.params.erosionPasses = e.value("erosion_passes", preset.params.erosionPasses);
                preset.params.rainfallPercent =
                        e.value("rainfall_percent", preset.params.rainfallPercent);
                preset.params.siteSpacing = e.value("site_spacing", preset.params.siteSpacing);
                preset.params.sites = e.value("sites", preset.params.sites);
                // The size the world kind wants to be. A name from kWorldSizes,
                // or a number of cells for anything the interface does not
                // offer. It is a starting point rather than a fixture: the
                // player moves the size dial afterwards like any other.
                std::int32_t cells = e.value("cells", 0);
                if (e.contains("size")) {
                    const std::string wanted = e.value("size", std::string());
                    for (const WorldSize& size : kWorldSizes)
                        if (wanted == size.name) cells = size.cells;
                }
                if (cells > 0) preset.params.width = preset.params.height = cells;
                presets.push_back(std::move(preset));
            }
        } catch (const std::exception&) {
            presets.clear();
        }
    }
    if (presets.empty()) {
        WorldPreset preset;
        preset.name = "default";
        preset.label = "The generator's own";
        preset.note = "No presets could be read, so this is what the code itself defaults to.";
        presets.push_back(std::move(preset));
    }
    return presets;
}

bool saveWorldMapData(const WorldMapData& world, const std::filesystem::path& file) {
    using nlohmann::json;
    json j;
    j["version"] = world.terrainFoundation ? 3 : world.hybridTerrain ? 2 : 1;
    j["width"] = world.width;
    j["height"] = world.height;
    j["seed"] = world.seed;
    if (world.hybridTerrain)
        j["hybrid_terrain"] = json::parse(saveHybridTerrain(*world.hybridTerrain));
    if (world.terrainFoundation)
        j["terrain_foundation"] = json::parse(saveTerrainFoundation(*world.terrainFoundation));
    j["constants"] = {
            {"world_seed", world.constants.worldSeed},
            {"world_width", world.constants.worldWidth},
            {"world_height", world.constants.worldHeight},
            {"sea_level", world.constants.seaLevel},
            {"temperature_scale", world.constants.globalTemperatureScale},
            {"moisture_scale", world.constants.globalMoistureScale},
            {"rotation_sign", world.constants.planetRotationSign},
            {"season", {{"amplitude", world.constants.season.amplitude},
                        {"wet_bias", world.constants.season.wetSeasonBias},
                        {"cold_bias", world.constants.season.coldSeasonBias}}},
    };
    j["played_cell"] = {world.playedCell.x, world.playedCell.y};
    j["played_block"] = {world.playedBlock.x, world.playedBlock.y};

    j["cells"] = json::array();
    for (const WorldCell& c : world.cells)
        j["cells"].push_back({
                {"elevation", c.elevation},
                {"biome", static_cast<int>(c.biome)},
                {"sea", c.sea},
                {"river", c.river},
                {"river_out", c.riverOut},
                {"river_size", c.riverSize},
                {"drain_out", c.drainOut},
                {"drain_size", c.drainSize},
                {"moisture", c.moisture},
                {"fertility", c.fertility},
                {"temperature", c.temperature},
                {"climate", static_cast<int>(c.climate)},
        });

    j["sites"] = json::array();
    for (const WorldSite& s : world.sites)
        j["sites"].push_back({
                {"cell", {s.cell.x, s.cell.y}},
                {"name", s.name},
                {"ethnos", s.ethnos},
                {"played", s.played},
                {"population", s.population},
                {"livelihood", static_cast<int>(s.livelihood)},
                {"climate", static_cast<int>(s.climate)},
        });

    const auto ints = [&](const char* key, const std::vector<std::int32_t>& v) { j[key] = v; };
    const auto i8 = [&](const char* key, const std::vector<std::int8_t>& v) {
        j[key] = json::array();
        for (std::int8_t x : v) j[key].push_back(static_cast<int>(x));
    };
    const auto u8 = [&](const char* key, const std::vector<std::uint8_t>& v) { j[key] = v; };
    const auto vec2 = [&](const char* key, const std::vector<ClimateVector>& v) {
        j[key] = json::array();
        for (const ClimateVector& p : v) j[key].push_back({p.x, p.y});
    };
    const auto rock = [&](const char* key, const std::vector<RockType>& v) {
        j[key] = json::array();
        for (RockType t : v) j[key].push_back(static_cast<int>(t));
    };
    const auto soil = [&](const char* key, const std::vector<SoilType>& v) {
        j[key] = json::array();
        for (SoilType t : v) j[key].push_back(static_cast<int>(t));
    };
    const auto m6 = [&](const char* key, const std::vector<std::array<std::uint8_t, 6>>& v) {
        j[key] = json::array();
        for (const auto& a : v) j[key].push_back({a[0], a[1], a[2], a[3], a[4], a[5]});
    };

    ints("primary_height", world.primaryHeightField);
    ints("continental", world.continentalField);
    ints("distance_to_coast", world.distanceToCoast);
    u8("initial_land_mask", world.initialLandMask);
    ints("uplift", world.upliftField);
    ints("rift", world.riftField);
    ints("fault", world.faultField);
    u8("geology_region", world.geologyRegion);
    ints("macro_height", world.macroHeightField);
    rock("rock_type", world.rockTypeField);
    ints("erosion_resistance", world.erosionResistanceField);
    u8("soil_parent_material", world.soilParentMaterialField);
    ints("permeability", world.permeabilityField);
    ints("thermal_height", world.thermallyRelaxedHeightField);
    ints("hydro_height", world.hydrologicallyCorrectedHeightField);
    ints("basin_id", world.basinIdField);
    vec2("spill_point", world.spillPointField);
    i8("flow_direction", world.flowDirectionField);
    ints("flow_accumulation", world.flowAccumulationField);
    ints("river_discharge", world.riverDischargeField);
    u8("river_source", world.riverSourceField);
    ints("lake_region", world.lakeRegionField);
    ints("lake_level", world.lakeLevelField);
    ints("lake_depth", world.lakeDepthField);
    ints("province", world.provinceField);
    u8("waterfall", world.waterfallField);
    ints("sediment", world.sedimentPotentialField);
    ints("erosion", world.erosionField);
    ints("floodplain", world.floodplainPotentialField);
    ints("delta", world.deltaPotentialField);
    ints("eroded_height", world.erodedHeightField);
    ints("base_temperature", world.baseTemperatureField);
    vec2("prevailing_wind", world.prevailingWindField);
    ints("wind_strength", world.windStrengthField);
    ints("wind_variability", world.windVariabilityField);
    ints("precipitation", world.precipitationField);
    ints("air_moisture", world.airMoistureField);
    ints("rain_shadow", world.rainShadowField);
    vec2("ocean_current", world.oceanCurrentField);
    ints("sst_bias", world.seaSurfaceTemperatureBiasField);
    ints("coastal_bias", world.coastalClimateBiasField);
    ints("temperature", world.temperatureField);
    ints("annual_rainfall", world.annualRainfallField);
    ints("humidity", world.humidityField);
    ints("seasonality", world.seasonalityField);
    ints("winter_rain", world.winterRainField);
    ints("summer_rain", world.summerRainField);
    ints("dry_season_strength", world.drySeasonStrengthField);
    soil("soil_type", world.soilTypeField);
    ints("soil_fertility", world.soilFertilityField);
    ints("soil_drainage", world.soilDrainageField);
    ints("soil_organic", world.soilOrganicPotentialField);
    u8("primary_biome", world.primaryBiomeSuitabilityField);
    u8("secondary_biome", world.secondaryBiomeSuitabilityField);
    ints("biome_transition", world.biomeTransitionField);
    m6("material_suitability", world.materialSuitabilityField);

    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2);
    return static_cast<bool>(out);
}

bool loadWorldMapData(const std::filesystem::path& file, WorldMapData& out) {
    using nlohmann::json;
    std::ifstream in(file);
    if (!in) return false;
    json j;
    try { in >> j; }
    catch (...) { return false; }

    out = {};
    out.width = j.value("width", 0);
    out.height = j.value("height", 0);
    out.seed = j.value("seed", 0ull);
    const json constants = j.value("constants", json::object());
    out.constants.worldSeed = constants.value("world_seed", out.seed);
    out.constants.worldWidth = constants.value("world_width", out.width);
    out.constants.worldHeight = constants.value("world_height", out.height);
    out.constants.seaLevel = constants.value("sea_level", 0);
    out.constants.globalTemperatureScale = constants.value("temperature_scale", 100);
    out.constants.globalMoistureScale = constants.value("moisture_scale", 100);
    out.constants.planetRotationSign = constants.value("rotation_sign", 1);
    const json season = constants.value("season", json::object());
    out.constants.season.amplitude = season.value("amplitude", 20);
    out.constants.season.wetSeasonBias = season.value("wet_bias", 0);
    out.constants.season.coldSeasonBias = season.value("cold_bias", 0);

    const auto readPos = [](const json& a, core::TilePos fallback = {0, 0}) {
        if (!a.is_array() || a.size() < 2) return fallback;
        return core::TilePos{a[0].get<std::int32_t>(), a[1].get<std::int32_t>()};
    };
    out.playedCell = readPos(j.value("played_cell", json::array()));
    out.playedBlock = readPos(j.value("played_block", json::array()));

    for (const json& c : j.value("cells", json::array())) {
        WorldCell cell;
        cell.elevation = c.value("elevation", 0);
        cell.biome = static_cast<Biome>(c.value("biome", 0));
        cell.sea = c.value("sea", false);
        cell.river = c.value("river", false);
        cell.riverOut = static_cast<std::int8_t>(c.value("river_out", -1));
        cell.riverSize = static_cast<std::uint8_t>(c.value("river_size", 0));
        cell.drainOut = static_cast<std::int8_t>(c.value("drain_out", -1));
        cell.drainSize = static_cast<std::uint8_t>(c.value("drain_size", 0));
        cell.moisture = static_cast<std::uint8_t>(c.value("moisture", 0));
        cell.fertility = static_cast<std::uint8_t>(c.value("fertility", 0));
        cell.temperature = static_cast<std::uint8_t>(c.value("temperature", 0));
        cell.climate = static_cast<Climate>(c.value("climate", 0));
        out.cells.push_back(cell);
    }
    for (const json& s : j.value("sites", json::array())) {
        WorldSite site;
        site.cell = readPos(s.value("cell", json::array()));
        site.name = s.value("name", std::string{});
        site.ethnos = s.value("ethnos", std::string{});
        site.played = s.value("played", false);
        site.population = s.value("population", kFoundingCommunity);
        site.livelihood = static_cast<Livelihood>(s.value("livelihood", 0));
        site.climate = static_cast<Climate>(s.value("climate", 0));
        out.sites.push_back(std::move(site));
    }

    const auto ints = [&](const char* key, std::vector<std::int32_t>& v) {
        v = j.value(key, std::vector<std::int32_t>{});
    };
    const auto u8 = [&](const char* key, std::vector<std::uint8_t>& v) {
        v = j.value(key, std::vector<std::uint8_t>{});
    };
    const auto i8 = [&](const char* key, std::vector<std::int8_t>& v) {
        v.clear();
        for (const auto& e : j.value(key, json::array())) v.push_back(static_cast<std::int8_t>(e.get<int>()));
    };
    const auto vec2 = [&](const char* key, std::vector<ClimateVector>& v) {
        v.clear();
        for (const auto& e : j.value(key, json::array()))
            if (e.is_array() && e.size() >= 2)
                v.push_back({static_cast<std::int16_t>(e[0].get<int>()),
                             static_cast<std::int16_t>(e[1].get<int>())});
    };
    const auto rock = [&](const char* key, std::vector<RockType>& v) {
        v.clear();
        for (const auto& e : j.value(key, json::array())) v.push_back(static_cast<RockType>(e.get<int>()));
    };
    const auto soil = [&](const char* key, std::vector<SoilType>& v) {
        v.clear();
        for (const auto& e : j.value(key, json::array())) v.push_back(static_cast<SoilType>(e.get<int>()));
    };
    const auto m6 = [&](const char* key, std::vector<std::array<std::uint8_t, 6>>& v) {
        v.clear();
        for (const auto& e : j.value(key, json::array())) {
            if (!e.is_array() || e.size() < 6) continue;
            v.push_back({static_cast<std::uint8_t>(e[0].get<int>()),
                         static_cast<std::uint8_t>(e[1].get<int>()),
                         static_cast<std::uint8_t>(e[2].get<int>()),
                         static_cast<std::uint8_t>(e[3].get<int>()),
                         static_cast<std::uint8_t>(e[4].get<int>()),
                         static_cast<std::uint8_t>(e[5].get<int>())});
        }
    };

    ints("primary_height", out.primaryHeightField);
    ints("continental", out.continentalField);
    ints("distance_to_coast", out.distanceToCoast);
    u8("initial_land_mask", out.initialLandMask);
    ints("uplift", out.upliftField);
    ints("rift", out.riftField);
    ints("fault", out.faultField);
    u8("geology_region", out.geologyRegion);
    ints("macro_height", out.macroHeightField);
    rock("rock_type", out.rockTypeField);
    ints("erosion_resistance", out.erosionResistanceField);
    u8("soil_parent_material", out.soilParentMaterialField);
    ints("permeability", out.permeabilityField);
    ints("thermal_height", out.thermallyRelaxedHeightField);
    ints("hydro_height", out.hydrologicallyCorrectedHeightField);
    ints("basin_id", out.basinIdField);
    vec2("spill_point", out.spillPointField);
    i8("flow_direction", out.flowDirectionField);
    ints("flow_accumulation", out.flowAccumulationField);
    ints("river_discharge", out.riverDischargeField);
    u8("river_source", out.riverSourceField);
    ints("lake_region", out.lakeRegionField);
    ints("lake_level", out.lakeLevelField);
    ints("lake_depth", out.lakeDepthField);
    ints("province", out.provinceField);
    u8("waterfall", out.waterfallField);
    ints("sediment", out.sedimentPotentialField);
    ints("erosion", out.erosionField);
    ints("floodplain", out.floodplainPotentialField);
    ints("delta", out.deltaPotentialField);
    ints("eroded_height", out.erodedHeightField);
    ints("base_temperature", out.baseTemperatureField);
    vec2("prevailing_wind", out.prevailingWindField);
    ints("wind_strength", out.windStrengthField);
    ints("wind_variability", out.windVariabilityField);
    ints("precipitation", out.precipitationField);
    ints("air_moisture", out.airMoistureField);
    ints("rain_shadow", out.rainShadowField);
    vec2("ocean_current", out.oceanCurrentField);
    ints("sst_bias", out.seaSurfaceTemperatureBiasField);
    ints("coastal_bias", out.coastalClimateBiasField);
    ints("temperature", out.temperatureField);
    ints("annual_rainfall", out.annualRainfallField);
    ints("humidity", out.humidityField);
    ints("seasonality", out.seasonalityField);
    ints("winter_rain", out.winterRainField);
    ints("summer_rain", out.summerRainField);
    ints("dry_season_strength", out.drySeasonStrengthField);
    soil("soil_type", out.soilTypeField);
    ints("soil_fertility", out.soilFertilityField);
    ints("soil_drainage", out.soilDrainageField);
    ints("soil_organic", out.soilOrganicPotentialField);
    u8("primary_biome", out.primaryBiomeSuitabilityField);
    u8("secondary_biome", out.secondaryBiomeSuitabilityField);
    ints("biome_transition", out.biomeTransitionField);
    m6("material_suitability", out.materialSuitabilityField);

    if (j.contains("hybrid_terrain")) {
        try { out.hybridTerrain = loadHybridTerrain(j.at("hybrid_terrain").dump(), out.width, out.height); }
        catch (...) { out = {}; return false; }
    }
    try {
        const int version=j.value("version",1);
        if (version<1 || version>3 || (version==3 && !j.contains("terrain_foundation"))) return false;
        if (j.contains("terrain_foundation"))
            out.terrainFoundation=loadTerrainFoundation(j.at("terrain_foundation").dump(),out.width,out.height);
    } catch (...) { out={}; return false; }
    return out.width > 0 && out.height > 0 && !out.cells.empty();
}

WorldMapData generateWorldMap(const WorldMapParams& params) {
    WorldMapData world;
    world.width = params.width;
    world.height = params.height;
    world.seed = params.seed;
    world.cells.assign(static_cast<std::size_t>(params.width) * params.height, WorldCell{});
    core::Rng rng(params.seed, 0x9E3779B97F4A7C15ULL);
    const std::size_t count = world.cells.size();
    world.constants.worldSeed = params.seed;
    world.constants.worldWidth = params.width;
    world.constants.worldHeight = params.height;
    world.constants.globalTemperatureScale = 100;
    world.constants.globalMoistureScale = std::clamp(params.rainfallPercent, 20, 250);
    world.constants.planetRotationSign = (core::splitmix64(params.seed ^ 0xA11CE55FULL) & 1ull) ? 1 : -1;
    world.constants.season.amplitude = 16 + static_cast<std::int32_t>(core::splitmix64(params.seed ^ 0x5EA50FFull) % 21);
    world.constants.season.wetSeasonBias = static_cast<std::int32_t>(core::splitmix64(params.seed ^ 0xBEEFull) % 33) - 16;
    world.constants.season.coldSeasonBias = static_cast<std::int32_t>(core::splitmix64(params.seed ^ 0xC01Dull) % 25) - 12;
    world.continentalField.assign(count, 0);
    world.distanceToCoast.assign(count, 0);
    world.initialLandMask.assign(count, 0);
    world.upliftField.assign(count, 0);
    world.riftField.assign(count, 0);
    world.faultField.assign(count, 0);
    world.geologyRegion.assign(count, 0);
    world.primaryHeightField.assign(count, 0);
    world.macroHeightField.assign(count, 0);
    world.rockTypeField.assign(count, RockType::SoftSediment);
    world.erosionResistanceField.assign(count, 0);
    world.soilParentMaterialField.assign(count, 0);
    world.permeabilityField.assign(count, 0);
    world.thermallyRelaxedHeightField.assign(count, 0);
    world.hydrologicallyCorrectedHeightField.assign(count, 0);
    world.basinIdField.assign(count, -1);
    world.spillPointField.assign(count, ClimateVector{});
    world.flowDirectionField.assign(count, -1);
    world.flowAccumulationField.assign(count, 0);
    world.riverDischargeField.assign(count, 0);
    world.riverSourceField.assign(count, 0);
    world.lakeRegionField.assign(count, -1);
    world.lakeLevelField.assign(count, 0);
    world.lakeDepthField.assign(count, 0);
    world.provinceField.assign(count, static_cast<std::int32_t>(Province::Highlands));
    world.waterfallField.assign(count, 0);
    world.sedimentPotentialField.assign(count, 0);
    world.erosionField.assign(count, 0);
    world.floodplainPotentialField.assign(count, 0);
    world.deltaPotentialField.assign(count, 0);
    world.erodedHeightField.assign(count, 0);
    world.baseTemperatureField.assign(count, 0);
    world.prevailingWindField.assign(count, ClimateVector{});
    world.windStrengthField.assign(count, 0);
    world.windVariabilityField.assign(count, 0);
    world.precipitationField.assign(count, 0);
    world.airMoistureField.assign(count, 0);
    world.rainShadowField.assign(count, 0);
    world.oceanCurrentField.assign(count, ClimateVector{});
    world.seaSurfaceTemperatureBiasField.assign(count, 0);
    world.coastalClimateBiasField.assign(count, 0);
    world.temperatureField.assign(count, 0);
    world.annualRainfallField.assign(count, 0);
    world.humidityField.assign(count, 0);
    world.seasonalityField.assign(count, 0);
    world.winterRainField.assign(count, 0);
    world.summerRainField.assign(count, 0);
    world.drySeasonStrengthField.assign(count, 0);
    world.soilTypeField.assign(count, SoilType::Mineral);
    world.soilFertilityField.assign(count, 0);
    world.soilDrainageField.assign(count, 0);
    world.soilOrganicPotentialField.assign(count, 0);
    world.primaryBiomeSuitabilityField.assign(count, 0);
    world.secondaryBiomeSuitabilityField.assign(count, 0);
    world.biomeTransitionField.assign(count, 0);
    world.materialSuitabilityField.assign(count, std::array<std::uint8_t, 6>{0, 0, 0, 0, 0, 0});
    const auto indexOf = [&](std::int32_t x, std::int32_t y) {
        return static_cast<std::size_t>(y) * params.width + x;
    };

    // --- PASS G1: continental mask ----------------------------------------
    for (std::int32_t y = 0; y < params.height; ++y)
        for (std::int32_t x = 0; x < params.width; ++x)
            world.continentalField[indexOf(x, y)] = continentNoise(params.seed, x, y, params.width);
    {
        std::vector<std::int32_t> sorted = world.continentalField;
        std::sort(sorted.begin(), sorted.end());
        const std::size_t cut = std::min(sorted.size() - 1,
                                         sorted.size() * static_cast<std::size_t>(params.seaPercent) / 100);
        const std::int32_t shore = sorted[cut];
        std::deque<std::size_t> queue;
        std::vector<char> seen(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                world.initialLandMask[i] = world.continentalField[i] > shore ? 1 : 0;
            }
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const bool land = world.initialLandMask[i] != 0;
                bool coast = false;
                for (int dir = 0; dir < core::kNeighbourCount && !coast; ++dir) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    coast = !world.inBounds(n) || (world.initialLandMask[indexOf(n.x, n.y)] != 0) != land;
                }
                if (!coast) continue;
                seen[i] = 1;
                world.distanceToCoast[i] = 0;
                queue.push_back(i);
            }
        while (!queue.empty()) {
            const std::size_t i = queue.front();
            queue.pop_front();
            const TilePos p{static_cast<std::int32_t>(i) % params.width,
                            static_cast<std::int32_t>(i) / params.width};
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const std::size_t j = indexOf(n.x, n.y);
                if (seen[j]) continue;
                seen[j] = 1;
                world.distanceToCoast[j] = world.distanceToCoast[i] + 1;
                queue.push_back(j);
            }
        }
    }

    // --- the plates -------------------------------------------------------
    // Where the land is at all is decided by tectonics rather than by noise
    // (GDD 4.2, stage 1). Noise alone answers "is this cell high" and has no
    // answer to "why is there a mountain range here and not there": what it
    // draws is a blur with ridges scattered over it, and the give-away is that
    // ranges have no direction, coasts have no shelf, and every island is round.
    //
    // Plates are the cheapest model that produces the real thing: a handful of
    // rigid pieces, each drifting, each either continental (light, stands high)
    // or oceanic (dense, lies low). Everything interesting happens where two of
    // them meet, and what happens depends on whether they are closing or
    // opening, and on which kinds they are.
    // Plates every ninety kilometres or so, which is a SPACING and not a count.
    //
    // It used to be the cell count over four hundred and twenty squared, and a
    // cell is five hundred and forty metres - so a four-hundred-kilometre world
    // came to three and was clamped up to six, exactly as a hundred-kilometre
    // one was. However large the world got it had the same six plates, which is
    // why it always had one mountain belt: two or three margins, most of them
    // between ocean floors, crossing the whole map.
    const std::int64_t acrossMetres = std::int64_t(params.width) * kMetresPerCell;
    const std::int64_t downMetres = std::int64_t(params.height) * kMetresPerCell;
    const std::int32_t plateCount =
            params.plates > 0
                    ? std::clamp(params.plates, 2, 256)
                    : std::clamp(static_cast<std::int32_t>(acrossMetres * downMetres /
                                                           (90000LL * 90000LL)),
                                 6, 96);
    struct Plate {
        TilePos centre;
        std::int32_t vx = 0, vy = 0;    // drift, in hundredths of a cell a step
        bool oceanic = false;
    };
    std::vector<Plate> plates;
    plates.reserve(static_cast<std::size_t>(plateCount));
    {
        // Seeds on a jittered grid rather than at random: purely random points
        // clump, and a clump of plate seeds is a shatter zone the size of a
        // country with no plate in it.
        const std::int32_t columns = std::max(1, static_cast<std::int32_t>(std::sqrt(
                static_cast<double>(plateCount))));
        const std::int32_t rows = std::max(1, (plateCount + columns - 1) / columns);
        const std::int32_t stepX = std::max(1, params.width / columns);
        const std::int32_t stepY = std::max(1, params.height / rows);
        for (std::int32_t gy = 0; gy < rows; ++gy)
            for (std::int32_t gx = 0; gx < columns; ++gx) {
                if (static_cast<std::int32_t>(plates.size()) >= plateCount) break;
                const std::uint64_t h = core::splitmix64(
                        params.seed ^ 0x51ED270B'D7373BFDull ^
                        (std::uint64_t(std::uint32_t(gx)) << 32) ^ std::uint32_t(gy));
                Plate p;
                p.centre = {std::clamp(gx * stepX + static_cast<std::int32_t>(h % stepX), 0,
                                       params.width - 1),
                            std::clamp(gy * stepY + static_cast<std::int32_t>((h >> 20) % stepY), 0,
                                       params.height - 1)};
                p.vx = static_cast<std::int32_t>((h >> 40) % 201) - 100;
                p.vy = static_cast<std::int32_t>((h >> 48) % 201) - 100;
                // Two continents' worth of light crust and the rest ocean, which
                // is roughly the real proportion and leaves room for a sea that
                // is not a lake between two land masses.
                p.oceanic = ((h >> 56) % 100) >= 42;
                plates.push_back(p);
            }
    }

    // Which plate every cell belongs to. Nearest seed - a Voronoi diagram - but
    // asked at a lied-about position: the cell is looked up as if it were some
    // way off, and how far it is lied to varies smoothly over the map. That
    // turns every straight Voronoi edge into a torn one, which is what a plate
    // margin looks like. Straight edges were the giveaway that showed through
    // everything downstream - a coastline that runs dead straight for eighty
    // kilometres and then turns a corner is not a coastline, it is a diagram.
    std::vector<std::int16_t> plateOf(count, 0);
    {
        // Two scales of warp: a broad one that swings whole stretches of margin
        // about, and a fine one that frays the edge itself.
        const std::int32_t broad = octaveScale(70, params.width, 100);
        const std::int32_t fine = octaveScale(19, params.width, 40);
        const std::int32_t swing = std::max(6, params.width / 22);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::int32_t wx =
                        x + (smoothNoise(params.seed + 4001, x, y, broad) - 512) * swing / 512 +
                        (smoothNoise(params.seed + 4007, x, y, fine) - 512) * swing / 1536;
                const std::int32_t wy =
                        y + (smoothNoise(params.seed + 4013, x, y, broad) - 512) * swing / 512 +
                        (smoothNoise(params.seed + 4019, x, y, fine) - 512) * swing / 1536;
                std::int64_t best = std::numeric_limits<std::int64_t>::max();
                std::int16_t owner = 0;
                for (std::size_t i = 0; i < plates.size(); ++i) {
                    const std::int64_t dx = wx - plates[i].centre.x;
                    const std::int64_t dy = wy - plates[i].centre.y;
                    const std::int64_t d = dx * dx + dy * dy;
                    if (d < best) { best = d; owner = static_cast<std::int16_t>(i); }
                }
                plateOf[indexOf(x, y)] = owner;
            }
    }

    // What each boundary does to the ground, spread inland from it. A margin is
    // not a line: the Andes are a hundred and fifty kilometres wide, so the
    // stress at the boundary is carried inland and fades.
    std::vector<std::int32_t> tectonic(count, 0);
    {
        struct Front { std::size_t index; std::int32_t value; };
        std::vector<Front> queue;
        std::vector<std::int32_t> best(count, 0);
        std::vector<std::int32_t> uplift(count, 0);
        std::vector<std::int32_t> rift(count, 0);
        std::vector<std::int32_t> fault(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const Plate& here = plates[static_cast<std::size_t>(plateOf[i])];
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (!world.inBounds(n)) continue;
                    const std::int16_t other = plateOf[indexOf(n.x, n.y)];
                    if (other == plateOf[i]) continue;
                    const Plate& there = plates[static_cast<std::size_t>(other)];

                    // Closing or opening, measured along the line between the
                    // two plates' centres: the component of their relative
                    // drift that points from one into the other.
                    std::int32_t nx = there.centre.x - here.centre.x;
                    std::int32_t ny = there.centre.y - here.centre.y;
                    const std::int32_t norm = std::max(1, std::abs(nx) + std::abs(ny));
                    nx = nx * 100 / norm;
                    ny = ny * 100 / norm;
                    const std::int32_t closing =
                            ((here.vx - there.vx) * nx + (here.vy - there.vy) * ny) / 100;

                    std::int32_t value = 0;
                    const std::int32_t shear =
                            std::abs((here.vx - there.vx) * ny - (here.vy - there.vy) * nx) / 100;
                    if (closing > 0) {
                        // Converging. Two continents crumple into the highest
                        // ground there is; a continent over an ocean floor
                        // raises a cordillera along its edge and a trench in
                        // front of it; two ocean floors give an island arc.
                        if (!here.oceanic && !there.oceanic) value = closing * 9;
                        else if (!here.oceanic) value = closing * 7;
                        else if (!there.oceanic) value = -closing * 5;   // the trench
                        else value = closing * 3;
                        uplift[i] = std::max(uplift[i], std::max(0, value));
                    } else {
                        // Opening. A rift on land is a valley, a rift at sea is
                        // a ridge on the floor - never enough to break surface.
                        value = here.oceanic ? -closing * 1 : closing * 4;
                        rift[i] = std::max(rift[i], std::max(0, -value));
                    }
                    if (shear > 0 && std::abs(closing) <= shear)
                        fault[i] = std::max(fault[i], shear * (here.oceanic == there.oceanic ? 2 : 1));
                    if (std::abs(value) > std::abs(best[i])) best[i] = value;
                }
                if (best[i] != 0) queue.push_back({i, best[i]});
            }

        // Carried inland, losing a fixed amount a cell, and each cell keeping
        // the strongest thing that reached it. Width comes out of the ratio:
        // a boundary worth 600 at a decay of 9 is a margin some sixty cells -
        // ten kilometres - across.
        const std::int32_t decay = std::max(1, 900 / std::max(1, octaveScale(90, params.width, 100)));
        std::size_t head = 0;
        while (head < queue.size()) {
            const Front here = queue[head++];
            if (std::abs(best[here.index]) > std::abs(here.value)) continue;
            const TilePos p{static_cast<std::int32_t>(here.index) % params.width,
                            static_cast<std::int32_t>(here.index) / params.width};
            // Eight neighbours, and a diagonal costs what a diagonal is.
            //
            // Four neighbours with one cost per step is a taxicab distance, and
            // the contours of a taxicab distance are DIAMONDS - so every margin
            // in the world carried a lozenge around it, aligned to the array,
            // and everything shaped by the margin inherited the alignment. That
            // is most of the circuit-board pattern on the land: not the rivers,
            // the field they run over.
            for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
                const TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const bool diagonal = n.x != p.x && n.y != p.y;
                const std::int32_t cost = diagonal ? decay * 1414 / 1000 : decay;
                const std::int32_t reached = here.value > 0 ? here.value - cost
                                                            : here.value + cost;
                if (reached == 0 || (here.value > 0) != (reached > 0)) continue;
                const std::size_t j = indexOf(n.x, n.y);
                if (std::abs(reached) <= std::abs(best[j])) continue;
                best[j] = reached;
                queue.push_back({j, reached});
            }
        }
        tectonic = std::move(best);
        world.upliftField = std::move(uplift);
        world.riftField = std::move(rift);
        world.faultField = std::move(fault);
        for (std::size_t i = 0; i < count; ++i)
            world.geologyRegion[i] = static_cast<std::uint8_t>(plateOf[i] & 0xff);
    }

    // --- PASS G3: base macro height --------------------------------------
    // Plate first, boundary second, noise last: the crust decides whether this
    // is land at all, the margins decide where the mountains are, and the noise
    // only roughens what they produced. Noise on its own drew a blur.
    std::vector<std::int32_t>& raw = world.macroHeightField;
    {
        // The crust is a step - light rock stands high, dense rock lies low -
        // and a step drawn straight off the plate map is a cliff along the
        // whole margin, in the shape of the margin. Real crust thins towards
        // its edge: continental shelf, then slope, then floor. Blurring the
        // step gives exactly that, and it is the single change that stopped the
        // world looking like tiles laid on a floor.
        //
        // And it is a BIAS, not the decision. It used to be a step of four
        // hundred and fifty against a noise contribution of under a hundred,
        // which meant the coastline was the plate map wearing a blur. An ocean
        // basin does sit lower than a continent, so the step stays - at a
        // little over half of what it was, and against a continental field that
        // now carries the amplitude it should always have had.
        std::vector<std::int32_t> crust(count, 0);
        for (std::size_t i = 0; i < count; ++i)
            crust[i] = plates[static_cast<std::size_t>(plateOf[i])].oceanic ? 250 : 520;

        // Separable box blur, twice: two passes of a box are near enough a
        // gaussian, and a gaussian shelf is what the sea floor does.
        const std::int32_t reach = std::max(3, params.width / 40);
        std::vector<std::int32_t> pass(count, 0);
        const auto blur = [&](std::vector<std::int32_t>& field) {
            for (std::int32_t y = 0; y < params.height; ++y) {
                std::int64_t sum = 0;
                for (std::int32_t x = -reach; x <= reach; ++x)
                    sum += field[indexOf(std::clamp(x, 0, params.width - 1), y)];
                for (std::int32_t x = 0; x < params.width; ++x) {
                    pass[indexOf(x, y)] = static_cast<std::int32_t>(sum / (2 * reach + 1));
                    const std::int32_t out = std::clamp(x - reach, 0, params.width - 1);
                    const std::int32_t in = std::clamp(x + reach + 1, 0, params.width - 1);
                    sum += field[indexOf(in, y)] - field[indexOf(out, y)];
                }
            }
            for (std::int32_t x = 0; x < params.width; ++x) {
                std::int64_t sum = 0;
                for (std::int32_t y = -reach; y <= reach; ++y)
                    sum += pass[indexOf(x, std::clamp(y, 0, params.height - 1))];
                for (std::int32_t y = 0; y < params.height; ++y) {
                    field[indexOf(x, y)] = static_cast<std::int32_t>(sum / (2 * reach + 1));
                    const std::int32_t out = std::clamp(y - reach, 0, params.height - 1);
                    const std::int32_t in = std::clamp(y + reach + 1, 0, params.height - 1);
                    sum += pass[indexOf(x, in)] - pass[indexOf(x, out)];
                }
            }
        };
        blur(crust);
        blur(crust);
        // The stress field too, lightly: it was spread by a flood fill, and a
        // flood fill on a square grid leaves diamond-shaped contours. One pass
        // is enough to lose the diamonds and keep the range.
        blur(tectonic);

        // Peaks and passes ALONG a range, which the flood fill has no way to
        // produce. Its value falls linearly from the boundary, so the section
        // across a margin is already a wedge - but it is the same wedge for the
        // whole length of the margin, and a wedge of constant height is a wall.
        // That is the "one long ridge" a world of this generator comes out as.
        //
        // Ridged rather than summed, for the same reason as everywhere else
        // here: one minus the absolute value folds at every zero crossing, and
        // a fold is a crest. Summed noise would put domes along the wall
        // instead of peaks.
        //
        // Two scales - massifs and the summits within them - and the range is
        // never cut below four tenths, because a pass is lower ground, not a
        // gap: a range that vanished between its peaks would not be a range.
        const std::int32_t massifScale = std::max(4, octaveScale(26, params.width, 100));
        const std::int32_t summitScale = std::max(2, octaveScale(9, params.width, 100));
        // The lookup is warped before the fold is read, so the pattern of peaks
        // does not line up with the array it is stored in. Distance to anything
        // measured on an unwarped grid comes out aligned to the grid, and the
        // eye finds that alignment immediately however good the shape is.
        const std::int32_t crestWarp = std::max(2, params.width / 14);
        const auto crested = [&](std::int32_t x0, std::int32_t y0) {
            const std::int32_t x = x0 + (smoothNoise(params.seed ^ 0xC12A, x0, y0,
                                                     std::max(3, params.width / 5)) - 512) *
                                                crestWarp / 512;
            const std::int32_t y = y0 + (smoothNoise(params.seed ^ 0xC12B, x0, y0,
                                                     std::max(3, params.width / 5)) - 512) *
                                                crestWarp / 512;
            const auto fold = [&](std::uint64_t key, std::int32_t scale) {
                const std::int32_t n = smoothNoise(params.seed ^ key, x, y, scale) - 512;
                const std::int32_t q = 512 - std::abs(n);
                return q * q / 512;                       // 0..512, creased at the fold
            };
            const std::int32_t along = (fold(0x51D6E, massifScale) * 2 + fold(0x9EA5, summitScale)) / 3;
            return 410 + along;                           // 410..922 of 1024
        };

        // Regions, and what each of them is for.
        //
        // One recipe over a whole map is why two worlds from two seeds were the
        // same world: the same share of sea, the same ruggedness and the same
        // grain of coastline from one edge to the other, with only the noise
        // moved around. A world should have PARTS - a flood plain between two
        // rivers, a young continent with a cordillera down its side, a scatter
        // of islands - and a player should be able to cross from one into
        // another.
        //
        // Sites on a jittered lattice, one every two hundred kilometres or so,
        // each given a recipe by its own hash. What a cell gets is a weighted
        // blend of the sites near it rather than the nearest one, for the same
        // reason the plate crust is a blend: whatever is decided by "which site
        // is closest" has that site's cell boundary drawn across it, and the eye
        // finds a Voronoi edge instantly. Blended, there is no edge anywhere -
        // a coast simply gets rockier as you sail along it.
        // Two hundred kilometres, or a third of the map if the map is smaller
        // than that - a world with one region in it is the world this is meant
        // to replace, so there are always at least three across.
        const std::int32_t provinceSpacing =
                std::clamp(static_cast<std::int32_t>(200000 / kMetresPerCell), 6,
                           std::max(6, params.width / 3));
        struct Recipe { std::int32_t sea, rugged, grain; };   // per 1024
        // Crescent is flat and low; the New World is high and rugged; the
        // archipelago is mostly water and broken into small pieces; the
        // highlands are the recipe this generator had all along.
        //
        // The sea column is worth reading against what it is added to: oceanic
        // crust here is a hundred and seventy and continental six hundred and
        // twenty, so four hundred and fifty is the whole difference between a
        // sea floor and a continent. An archipelago has to push its ground most
        // of the way down that gap and no further - push it the whole way and
        // there is no archipelago, just sea.
        static constexpr Recipe kRecipes[kProvinceCount] = {
                {+180, 360, 420},    // Crescent
                {-170, 1640, 1000},  // NewWorld
                {+330, 900, 1850},   // Archipelago
                {0, 1150, 1000},     // Highlands
        };
        const auto floorGrid = [&](std::int32_t v) {
            return v >= 0 ? v / provinceSpacing : -((-v + provinceSpacing - 1) / provinceSpacing);
        };
        const auto siteOf = [&](std::int32_t gx, std::int32_t gy) {
            const std::uint64_t h = core::splitmix64(params.seed ^ 0x9E3779B97F4A7C15ull ^
                                                     (std::uint64_t(std::uint32_t(gx)) << 32) ^
                                                     std::uint64_t(std::uint32_t(gy)));
            struct Site { std::int32_t x, y, kind; };
            return Site{gx * provinceSpacing + std::int32_t(h % std::uint64_t(provinceSpacing)),
                        gy * provinceSpacing + std::int32_t((h >> 20) % std::uint64_t(provinceSpacing)),
                        std::int32_t((h >> 40) % std::uint64_t(kProvinceCount))};
        };
        for (std::int32_t y = 0; y < params.height; ++y) {
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);

                // Warped before the distances are taken, or the lattice the
                // sites sit on shows through as surely as the sites would.
                const std::int32_t wx =
                        x + (smoothNoise(params.seed ^ 0x9A0C, x, y, std::max(3, provinceSpacing)) - 512) *
                                    provinceSpacing / 1400;
                const std::int32_t wy =
                        y + (smoothNoise(params.seed ^ 0x9A0D, x, y, std::max(3, provinceSpacing)) - 512) *
                                    provinceSpacing / 1400;
                std::int64_t weighted[3] = {0, 0, 0};
                std::int64_t total = 0;
                std::int64_t bestDistance = -1;
                std::int32_t bestKind = static_cast<std::int32_t>(Province::Highlands);
                const std::int32_t gx = floorGrid(wx);
                const std::int32_t gy = floorGrid(wy);
                struct Near { std::int64_t distance; std::int32_t kind; };
                Near near[9];
                std::int32_t nearCount = 0;
                for (std::int32_t j = -1; j <= 1; ++j)
                    for (std::int32_t k = -1; k <= 1; ++k) {
                        const auto site = siteOf(gx + k, gy + j);
                        const std::int64_t dx = wx - site.x, dy = wy - site.y;
                        const std::int64_t d = std::int64_t(std::sqrt(double(dx * dx + dy * dy)));
                        near[nearCount++] = {d, site.kind};
                        if (bestDistance < 0 || d < bestDistance) {
                            bestDistance = d;
                            bestKind = site.kind;
                        }
                    }
                // The blend is over how much further each site is than the
                // nearest, across a shelf as wide as the spacing: at the
                // boundary two recipes are equal, which is what makes it not a
                // boundary.
                for (std::int32_t n = 0; n < nearCount; ++n) {
                    const std::int64_t reach =
                            (near[n].distance - bestDistance) * 2048 / provinceSpacing;
                    if (reach >= 1024) continue;
                    const std::int64_t t = 1024 - reach;
                    const std::int64_t w = t * t / 1024;           // eased, so there is no crease
                    const Recipe& r = kRecipes[near[n].kind];
                    weighted[0] += w * r.sea;
                    weighted[1] += w * r.rugged;
                    weighted[2] += w * r.grain;
                    total += w;
                }
                const std::int32_t sea = total > 0 ? std::int32_t(weighted[0] / total) : 0;
                const std::int32_t rugged = total > 0 ? std::int32_t(weighted[1] / total) : 1024;
                const std::int32_t grain = total > 0 ? std::int32_t(weighted[2] / total) : 1024;
                world.provinceField[i] = bestKind;

                // The edge of the map is not the edge of the world, but it has
                // to be water, or the country runs off the side and the
                // coastline the player sees is a straight line drawn by the
                // array bounds. Only the outermost stretch is pulled down - a
                // margin, not the old pull towards the middle that made every
                // world one round island.
                const std::int32_t toEdge = std::min({x, y, params.width - 1 - x, params.height - 1 - y});
                const std::int32_t margin = std::max(8, params.width / 24);
                // And the margin WANDERS, or the cure is the disease. A band
                // whose width is the array index is still the array index: the
                // ramp crosses sea level at the same distance all the way along
                // the side, so the coast it draws is a line ruled parallel to
                // the map edge - which is exactly the cut border that shows up
                // wherever the land happens to reach the margin. Noise on the
                // width of the band is what turns that line back into a coast,
                // with bays where the band reaches in and headlands where it
                // does not.
                //
                // Quadratic rather than linear, too. A linear ramp has a corner
                // in it where it starts, and a corner running the length of the
                // map is its own straight line; this one leaves the inland side
                // alone and puts its weight at the edge, where a range has to go
                // under the water rather than be sliced off above it.
                const std::int32_t wander =
                        (smoothNoise(params.seed ^ 0xC0A57ull, x, y, std::max(3, margin * 2)) * 3 +
                         smoothNoise(params.seed ^ 0x5EA11ull, x, y, std::max(2, margin / 2))) /
                                4 -
                        512;
                const std::int32_t reach = std::max(3, margin + wander * margin * 7 / 5120);
                const std::int32_t into = reach - toEdge;
                const std::int32_t drown = into <= 0 ? 0 : 1000 * into * into / (reach * reach);

                // An archipelago is not a continent with a higher sea level -
                // it is land broken into small pieces, which is a finer grain
                // of noise and not a lower one. So the grain knob mixes in an
                // octave of its own rather than only scaling what is there.
                // What decides where the land is. Full amplitude, never scaled
                // down by how tectonic the ground is: a coast is a coast whether
                // or not anything is pushing it up.
                const std::int32_t shore =
                        (world.continentalField[i] - 512) * 3 / 2;
                const std::int32_t coarse = landNoise(params.seed, x, y, params.width) - 512;
                const std::int32_t fine =
                        smoothNoise(params.seed ^ 0x15AD, x, y,
                                    std::max(2, octaveScale(9, params.width, 0))) - 512;
                const std::int32_t detail = grain <= 1024
                        ? coarse * grain / 1024
                        : coarse + (fine - coarse) * (grain - 1024) / 1024;
                // Fine noise belongs where there is country to vary, not on a
                // plain. Laid flat over everything it is tens of metres of
                // ripple on ground that should be smooth - and every ripple is
                // a hollow for the water pass to stand in, and a direction for
                // the erosion to follow. A quarter of it everywhere so a plain
                // is not a table, all of it where the crust is being pushed up.
                const std::int32_t country =
                        std::clamp(256 + std::abs(tectonic[i]) / 2, 256, 1024);
                const std::int32_t varied = detail * country / 1024;
                world.primaryHeightField[i] = 400 + shore / 2 + detail * 3 / 4 - drown - sea / 2;
                // Only the initial field is noisy. Ranges belong to plate
                // compression; branching relief is produced by erosion below.
                // Uplift is shaped along its length; a trench or a rift is not,
                // because neither has a crest to put summits on.
                const std::int32_t shaped =
                        (tectonic[i] > 0 ? tectonic[i] * crested(x, y) / 922 : tectonic[i]) *
                        rugged / 1024;
                raw[i] = crust[i] + shore + shaped + varied * 3 / 4 + world.faultField[i] / 3 -
                         world.riftField[i] / 2 - drown - sea;
            }
        }
    }

    // --- PASS G4: geology -------------------------------------------------
    for (std::size_t i = 0; i < count; ++i) {
        const bool uplifted = world.upliftField[i] > 180;
        const bool faulted = world.faultField[i] > 60;
        const bool rifted = world.riftField[i] > 80;
        const bool coastal = world.distanceToCoast[i] <= std::max(2, params.width / 40);
        RockType rock = RockType::SoftSediment;
        if (rifted && uplifted) rock = RockType::Volcanic;
        else if (uplifted && world.continentalField[i] > 560) rock = RockType::HardRock;
        else if (faulted && world.continentalField[i] > 520) rock = RockType::Limestone;
        else if (coastal) rock = RockType::Sandstone;
        else if (world.continentalField[i] > 540) rock = RockType::ClayRich;
        world.rockTypeField[i] = rock;
        switch (rock) {
            case RockType::HardRock:
                world.erosionResistanceField[i] = 220;
                world.permeabilityField[i] = 80;
                world.soilParentMaterialField[i] = 0;
                break;
            case RockType::SoftSediment:
                world.erosionResistanceField[i] = 90;
                world.permeabilityField[i] = 150;
                world.soilParentMaterialField[i] = 1;
                break;
            case RockType::Limestone:
                world.erosionResistanceField[i] = 170;
                world.permeabilityField[i] = 210;
                world.soilParentMaterialField[i] = 2;
                break;
            case RockType::Volcanic:
                world.erosionResistanceField[i] = 200;
                world.permeabilityField[i] = 95;
                world.soilParentMaterialField[i] = 3;
                break;
            case RockType::ClayRich:
                world.erosionResistanceField[i] = 110;
                world.permeabilityField[i] = 70;
                world.soilParentMaterialField[i] = 4;
                break;
            case RockType::Sandstone:
                world.erosionResistanceField[i] = 140;
                world.permeabilityField[i] = 165;
                world.soilParentMaterialField[i] = 5;
                break;
            case RockType::Alluvial:
                world.erosionResistanceField[i] = 75;
                world.permeabilityField[i] = 185;
                world.soilParentMaterialField[i] = 6;
                break;
            case RockType::Count:
                break;
        }
    }

    // --- PASS G5: thermal erosion ----------------------------------------
    world.thermallyRelaxedHeightField = world.macroHeightField;
    std::vector<std::int32_t>& thermal = world.thermallyRelaxedHeightField;
    // Weather on the rock. Two passes of thermal erosion: ground steeper than
    // the angle loose material can stand at sheds it to the low side. It is the
    // cheapest erosion there is and it does the one thing the eye checks for -
    // slopes get a foot, ridges get a shoulder, and nothing is a wall any more.
    // Hydraulic erosion would carve valleys as well, and costs an order more.
    {
        const std::int32_t talus = 26;
        std::vector<std::int32_t> moved(count, 0);
        const std::int32_t passes = std::clamp(params.erosionPasses, 0, 24);
        for (int pass = 0; pass < passes; ++pass) {
            std::fill(moved.begin(), moved.end(), 0);
            for (std::int32_t y = 0; y < params.height; ++y)
                for (std::int32_t x = 0; x < params.width; ++x) {
                    const std::size_t i = indexOf(x, y);
                    std::int32_t lowest = thermal[i];
                    std::size_t sink = i;
                    for (int dir : core::kCardinalDirections) {
                        const TilePos n = core::neighbour({x, y}, dir);
                        if (!world.inBounds(n)) continue;
                        const std::size_t j = indexOf(n.x, n.y);
                        if (thermal[j] < lowest) { lowest = thermal[j]; sink = j; }
                    }
                    const std::int32_t resistance = std::clamp(world.erosionResistanceField[i], 60, 240);
                    const std::int32_t localTalus = talus + resistance / 18;
                    const std::int32_t drop = thermal[i] - lowest;
                    if (sink == i || drop <= localTalus) continue;
                    const std::int32_t slide = (drop - localTalus) / 2;
                    moved[i] -= slide;
                    moved[sink] += slide;
                }
            for (std::size_t i = 0; i < count; ++i) thermal[i] += moved[i];
        }
    }

    // Sea level is chosen so that the asked-for share of the map is water,
    // whatever the noise happened to produce: a world that is nine tenths ocean
    // on one seed and none on the next is not a world, it is a coin.
    std::vector<std::int32_t> sorted = thermal;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t cut = std::min(sorted.size() - 1,
                                     sorted.size() * static_cast<std::size_t>(params.seaPercent) / 100);
    const std::int32_t seaLevel = sorted[cut];
    const std::int32_t highest = sorted.back();
    world.constants.seaLevel = seaLevel;

    // The coastline gets its own noise, and only near the water. A coast is the
    // most fractal line on a planet - fjords, spits, headlands, an archipelago
    // off every cape - and it is what the eye recognises a world by; a coast
    // drawn by the same octaves as the interior comes out as smooth bays, which
    // is what a lake looks like, not what a sea does.
    //
    // It has to be applied here rather than with the heights, because "near the
    // water" is not knowable until the sea level has been chosen: done earlier
    // it roughened a contour somewhere up the hillside instead.
    {
        const std::int32_t band = std::max(40, (highest - seaLevel) / 6);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const std::int32_t fromShore = std::abs(thermal[i] - seaLevel);
                if (fromShore >= band) continue;
                const std::int32_t bite = band - fromShore;          // strongest at the water
                const std::int32_t fret =
                        (smoothNoise(params.seed + 2213, x, y, octaveScale(13, params.width, 0)) - 512) * 3 / 4 +
                        (smoothNoise(params.seed + 2221, x, y, octaveScale(6, params.width, 0)) - 512) / 2 +
                        (smoothNoise(params.seed + 2237, x, y, octaveScale(3, params.width, 0)) - 512) / 3;
                thermal[i] += fret * bite / band;
            }
    }

    for (std::int32_t y = 0; y < params.height; ++y) {
        for (std::int32_t x = 0; x < params.width; ++x) {
            WorldCell& c = world.at({x, y});
            const std::int32_t v = thermal[indexOf(x, y)];
            c.sea = v <= seaLevel;
            // Scaled against the highest land there actually is, not against the
            // noise's ceiling: measured against the ceiling nothing on a flat
            // seed ever rose above a third, and rivers - which start on high
            // ground - never started at all.
            c.elevation = static_cast<std::uint8_t>(
                    c.sea ? 0 : std::clamp((v - seaLevel) * 255 / std::max(1, highest - seaLevel), 1, 255));
        }
    }

    // Real landforms and complete volcanic edifices enter BEFORE climate and
    // drainage. Their macro component is carried by the cells; HeightField
    // restores only the missing sub-cell component, not a second mountain.
    generateHybridTerrain(world, params);
    // H64 is the geometry authority. Macro thermal/coast fields above establish
    // sea-level calibration and volcano placement, not a second runtime surface.
    if (params.stagedTerrain && params.hybridTerrain) {
        world.terrainFoundation = params.foundationSnapshot ? params.foundationSnapshot :
            buildTerrainFoundation(world, params, seaLevel, highest);
        world.terrainFoundation->applyTo(world);
    }

    // --- PASS C0: base temperature ---------------------------------------
    const std::int32_t climateSpan = std::clamp(230 * params.width / 2048, 70, 230);
    const std::int32_t climateCentre =
            150 + static_cast<std::int32_t>(core::splitmix64(params.seed ^ 0xC11Aull) % 61) - 30;
    const std::int32_t climateFrom =
            std::clamp(climateCentre - climateSpan / 2, 0, std::max(0, 230 - climateSpan));
    const std::int32_t climateWarp = octaveScale(60, params.width, 100);
    for (std::int32_t y = 0; y < params.height; ++y)
        for (std::int32_t x = 0; x < params.width; ++x) {
            const std::size_t i = indexOf(x, y);
            const WorldCell& c = world.cells[i];
            const std::int32_t wobble = (smoothNoise(params.seed + 9311, x, y, climateWarp) - 512) / 12;
            const std::int32_t band = std::clamp(
                    climateFrom + (y + wobble) * climateSpan / std::max(1, params.height - 1), 0, 230);
            const std::int32_t latitudeTemperature = 25 + band;
            const std::int32_t altitudeChill = c.sea ? 0 : c.elevation * 3 / 10;
            const std::int32_t maritime = std::clamp(18 - world.distanceToCoast[i] * 2, 0, 18);
            const std::int32_t coastalModeration = latitudeTemperature < 120 ? maritime / 3 : -maritime / 3;
            world.baseTemperatureField[i] =
                    std::clamp(latitudeTemperature - altitudeChill + coastalModeration, 0, 255);
        }

    // --- PASS C1/C2/C3/C5: climate transport, currents, rainfall ----------
    // Rain is carried, not scattered. A noise field for moisture put deserts and
    // marshes down at random and had no answer to the question the map most
    // obviously raises: why is this side of the range green and that side sand?
    //
    // So the wind is given a direction and walked across the map. Over water it
    // picks moisture up; over land it drops some as it goes, and drops a great
    // deal more when the ground climbs under it, because air that is pushed up
    // cools and cannot hold what it carried. Behind a range it has little left,
    // which is a rain shadow - and rain shadows are where the real deserts of
    // the world are.
    {
        const std::uint64_t h = core::splitmix64(params.seed ^ 0xB5AD4ECE'DA1CE2A9ull);
        // Off the sea, mostly along a parallel, the way trade winds and
        // westerlies both run: a wind blowing along the map's diagonal produces
        // a country striped corner to corner, which no continent is.
        const bool eastward = (h & 1) != 0;
        const std::int32_t drift = static_cast<std::int32_t>((h >> 8) % 3) - 1;   // -1, 0 or 1 rows

        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const double latitude = params.height > 1 ? static_cast<double>(y) / (params.height - 1) : 0.5;
                const bool tropical = latitude > 0.33 && latitude < 0.66;
                const bool polar = latitude < 0.12 || latitude > 0.88;
                ClimateVector wind;
                wind.x = static_cast<std::int16_t>((tropical ? -90 : 110) * world.constants.planetRotationSign);
                wind.y = static_cast<std::int16_t>((drift + (polar ? (latitude < 0.5 ? 2 : -2) : 0)) * 12);
                if (eastward) wind.x = static_cast<std::int16_t>(-wind.x);
                world.prevailingWindField[i] = wind;
                world.windStrengthField[i] = tropical ? 180 : (polar ? 110 : 150);
                world.windVariabilityField[i] = std::clamp(20 + std::abs(drift) * 16 +
                                                                  std::max(0, 8 - world.distanceToCoast[i]),
                                                          0, 255);
            }

        const std::int32_t carry = 255;               // how much the air holds when it comes off the sea
        std::vector<std::int32_t> wet(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y) {
            std::int32_t air = carry / 2;             // the edge of the map is not a shore
            std::int32_t lastHeight = 0;
            for (std::int32_t step = 0; step < params.width; ++step) {
                const std::int32_t x = eastward ? step : params.width - 1 - step;
                // The wind wanders a little as it crosses, so the picture is not
                // combed into rows.
                const std::int32_t row = std::clamp(y + drift * step / std::max(1, params.width / 3),
                                                    0, params.height - 1);
                const std::size_t i = indexOf(x, row);
                const WorldCell& c = world.cells[i];
                const std::int32_t height = c.sea ? 0 : c.elevation;

                if (c.sea) {
                    // Evaporation: the longer the fetch, the wetter the air.
                    air = std::min(carry, air + 12);
                    wet[i] = std::max(wet[i], air);
                    world.airMoistureField[i] = std::max(world.airMoistureField[i], air);
                    world.precipitationField[i] = std::max(world.precipitationField[i], air / 4);
                    lastHeight = 0;
                    continue;
                }
                // Orographic rain: what the air gives up is mostly a function of
                // how hard the ground is pushing it upwards.
                const std::int32_t climb = std::max(0, height - lastHeight);
                std::int32_t fall = air / 40 + climb * air / 220;
                fall = std::min(air, fall);
                air -= fall;
                // Some of what falls runs on: the far side of a range is dry, but
                // not sterile, and a wide plain does not go to nothing.
                wet[i] = std::max(wet[i], fall * 8 + air / 6);
                world.airMoistureField[i] = std::max(world.airMoistureField[i], air);
                world.precipitationField[i] = std::max(world.precipitationField[i], fall * 8);
                world.rainShadowField[i] = std::max(world.rainShadowField[i], std::max(0, climb * 2 - air / 20));
                lastHeight = height;
            }
        }

        // Smoothed, and mixed with a little noise so the fronts are not ruled
        // lines. The noise is a fifth of it: enough to break the edge, not
        // enough to put a marsh in the middle of a rain shadow.
        std::vector<std::int32_t> blended(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                std::int32_t sum = 0, n = 0;
                for (std::int32_t dy = -2; dy <= 2; ++dy)
                    for (std::int32_t dx = -2; dx <= 2; ++dx) {
                        const TilePos q{x + dx, y + dy};
                        if (!world.inBounds(q)) continue;
                        sum += wet[indexOf(q.x, q.y)];
                        ++n;
                    }
                const std::int32_t speckle =
                        smoothNoise(params.seed + 7717, x, y, octaveScale(9, params.width, 100)) / 4;
                blended[i] = (sum / std::max(1, n)) * 4 / 5 + speckle / 5;
                world.precipitationField[i] = std::max(world.precipitationField[i], blended[i]);
            }

        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                if (!world.cells[i].sea) continue;
                const double latitude = params.height > 1 ? static_cast<double>(y) / (params.height - 1) : 0.5;
                ClimateVector current;
                current.x = static_cast<std::int16_t>((latitude > 0.33 && latitude < 0.66 ? 70 : -55) *
                                                      world.constants.planetRotationSign);
                current.y = static_cast<std::int16_t>((0.5 - latitude) * 90.0);
                if (eastward) current.x = static_cast<std::int16_t>(-current.x);
                world.oceanCurrentField[i] = current;
                world.seaSurfaceTemperatureBiasField[i] =
                        std::clamp(static_cast<std::int32_t>((0.5 - std::abs(latitude - 0.5)) * 80.0) + current.y / 3,
                                   -24, 24);
            }
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                if (world.cells[i].sea) continue;
                const std::int32_t coastBias = std::clamp(10 - world.distanceToCoast[i], 0, 10);
                std::int32_t currentBias = 0;
                for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (!world.inBounds(n) || !world.at(n).sea) continue;
                    currentBias = std::max(currentBias, world.seaSurfaceTemperatureBiasField[indexOf(n.x, n.y)]);
                }
                world.coastalClimateBiasField[i] = currentBias * coastBias / 10;
            }

        // Spread over the whole scale by rank rather than by value. How much
        // rain a wind carries depends on how far it blew over water and how
        // high the ground got - both of which vary hugely from seed to seed, so
        // taken at face value one world came out a swamp and the next a desert,
        // and the rivers and the biomes went with it. What the map wants from
        // this field is *where* it is wetter, not the absolute number, so the
        // driest land on any world is 0 and the wettest is 255.
        {
            std::array<std::int32_t, 1024> histogram{};
            std::int32_t landCells = 0, wettest = 1;
            for (std::size_t i = 0; i < count; ++i)
                if (!world.cells[i].sea) wettest = std::max(wettest, blended[i]);
            for (std::size_t i = 0; i < count; ++i) {
                if (world.cells[i].sea) continue;
                ++histogram[static_cast<std::size_t>(
                        std::clamp(blended[i] * 1023 / wettest, 0, 1023))];
                ++landCells;
            }
            std::array<std::int32_t, 1024> below{};
            std::int32_t running = 0;
            for (std::size_t b = 0; b < histogram.size(); ++b) {
                below[b] = running;
                running += histogram[b];
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (world.cells[i].sea) {
                    world.cells[i].moisture = 255;
                    world.annualRainfallField[i] = 255;
                    world.humidityField[i] = 255;
                    world.seasonalityField[i] = 0;
                    world.winterRainField[i] = 128;
                    world.summerRainField[i] = 127;
                    world.drySeasonStrengthField[i] = 0;
                    continue;
                }
                const std::size_t bucket = static_cast<std::size_t>(
                        std::clamp(blended[i] * 1023 / wettest, 0, 1023));
                // Rank-normalised, then scaled by how wet this world is meant to
                // be: the rank keeps one seed from coming out all swamp and the
                // next all desert, and the dial then says which way the whole
                // world leans.
                const std::int32_t ranked = below[bucket] * 255 / std::max(1, landCells);
                const std::int32_t annual =
                        std::clamp(ranked * std::clamp(params.rainfallPercent, 20, 250) / 100, 0, 255);
                world.annualRainfallField[i] = annual;
                world.humidityField[i] = std::clamp((annual * 3 + world.airMoistureField[i]) / 4, 0, 255);
                world.seasonalityField[i] = std::clamp(
                        world.windVariabilityField[i] * 3 + std::max(0, world.distanceToCoast[i] - 3) * 4,
                        0, 255);
                const std::int32_t wetSeason = std::clamp(128 + world.constants.season.wetSeasonBias, 48, 208);
                world.winterRainField[i] = annual * wetSeason / 255;
                world.summerRainField[i] = std::max(0, annual - world.winterRainField[i]);
                world.drySeasonStrengthField[i] =
                        std::clamp(world.seasonalityField[i] - world.humidityField[i] / 2, 0, 255);
                world.cells[i].moisture = static_cast<std::uint8_t>(annual);
            }
        }
    }

    // --- PASS G6: hydraulic / fluvial erosion ----------------------------
    // From the high ground to the sea, one step at a time downhill. A river that
    // cannot get lower stops: that is a lake in everything but name, and the
    // cells it passed through are still watered.
    //
    // Hollows filled first. Noise leaves the land full of little basins with no
    // outlet, and water that runs into one stops there: with them left in, the
    // whole continent's rainfall collected in a few hundred puddles and there
    // were no rivers at all.
    //
    // Filled by priority flood: the sea and the map's edge are the outlets, and
    // the land is flooded inwards from them lowest-first, each cell raised to
    // just above whatever it was reached from. That is a lake filling to its
    // outlet and then flowing on, in one pass. It replaced sixty relaxation
    // passes over the whole map - which was affordable at a hundred thousand
    // cells and is not at a million, and which stopped before it converged on
    // the wide basins a big map has, leaving exactly the hollows it was there to
    // remove.
    std::vector<std::int32_t>& filled = world.hydrologicallyCorrectedHeightField;
    {
        struct Front {
            std::int32_t level;
            std::size_t index;
            // Ties broken by index, or the order two equal cells come off the
            // queue depends on the heap's internals and the world stops being a
            // function of its seed (rule 2).
            bool operator>(const Front& o) const {
                return level != o.level ? level > o.level : index > o.index;
            }
        };
        // TWO surfaces come out of this flood, and conflating them is what put
        // rows of puddles across every plateau.
        //
        // The drainage surface needs a gradient everywhere, so each cell is set
        // a hair above the one it was reached from - `+ 1`, in the sixty-fourths
        // of a step this works in. That hair ACCUMULATES along the walk: across
        // sixty-four cells of flat ground it is a whole step of elevation, and a
        // step of elevation is nine metres. Measure standing water against that
        // surface and a featureless plateau comes out banded - a stripe of
        // nine-metre "lake" every sixty-four cells, drawn by the order the queue
        // happened to walk in and by nothing else at all.
        //
        // So the water surface is flooded WITHOUT the hair: a cell's water level
        // is the level of the cell it was reached from, or its own ground,
        // whichever is higher. On flat ground that is the ground, exactly, and
        // there is no water. The hair stays where it belongs, in the surface the
        // flow routing reads, and the ground the player sees is the honest one.
        std::vector<std::int32_t> spill(count, 0);
        std::priority_queue<Front, std::vector<Front>, std::greater<Front>> queue;
        std::vector<char> seen(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const bool edge = x == 0 || y == 0 || x == params.width - 1 || y == params.height - 1;
                const WorldCell& c = world.cells[i];
                if (!c.sea && !edge) continue;
                filled[i] = spill[i] = c.sea ? 0 : static_cast<std::int32_t>(c.elevation) * kHeightScale;
                seen[i] = 1;
                queue.push({filled[i], i});
            }
        while (!queue.empty()) {
            const Front here = queue.top();
            queue.pop();
            const TilePos p{static_cast<std::int32_t>(here.index) % params.width,
                            static_cast<std::int32_t>(here.index) / params.width};
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const std::size_t j = indexOf(n.x, n.y);
                if (seen[j]) continue;
                seen[j] = 1;
                const std::int32_t own = world.cells[j].sea
                                                 ? 0
                                                 : static_cast<std::int32_t>(world.cells[j].elevation) * kHeightScale;
                spill[j] = std::max(own, spill[here.index]);
                filled[j] = std::max(own, here.level + 1);
                queue.push({filled[j], j});
            }
        }
        // Keep the basin floor as a separate field before publishing the filled
        // drainage surface. Connected flooded cells share one head; render and
        // gameplay can therefore recover the original bed without independently
        // rediscovering a lake from the smoothed terrain.
        //
        // The ground keeps the DRAINAGE surface, hair and all. That hair is what
        // makes the surface strictly descending, and the river profiles are read
        // off it: take it away and a filled basin is dead flat, a course across
        // one has no gradient to follow, and the rivers come out as dashed lines
        // - measured, a hundred and eighty metres of gap where there had been
        // sixty. It is a tenth of a metre per cell. It is not what anybody sees.
        //
        // What the hair must not do is decide where the WATER is, and that is
        // the separate surface above.
        std::vector<std::int32_t> original(count, 0);
        std::vector<std::int32_t> water(count, 0);
        for (std::size_t i = 0; i < count; ++i) {
            WorldCell& c = world.cells[i];
            if (c.sea) continue;
            original[i] = c.elevation;
            water[i] = std::max(0, spill[i] - original[i] * kHeightScale);
            c.elevation = static_cast<std::uint8_t>(std::clamp(filled[i] / kHeightScale, 1, 255));
            world.lakeDepthField[i] = water[i] > 0 ? 1 : 0;
        }
        // And now the part that is NOT the flood's business: which of those
        // basins holds water.
        //
        // The flood fills every hollow, because a drainage surface with a hole
        // in it is not a drainage surface. What it must not do is call every
        // hollow a lake, and that is exactly what this did: each flooded cell
        // was given `max(1, head - original)` - and one step of elevation is
        // NINE METRES, so a dimple a metre deep, below the resolution the map
        // even stores, came out as nine metres of standing water. Noise leaves
        // the land full of such dimples; the comment above says so. The result
        // was the whole country under a rash of ponds.
        //
        // A basin holds water when it is deep enough that what collects in it
        // does not leave: depth measured at the fill's own precision, not at the
        // nine-metre step, and against a bar that rises as the climate dries. A
        // basin that fails dries out - and it dries out to the FILLED floor, not
        // back to its pitted original, so what is left is a flat valley floor
        // that blends into the ground below it instead of a hole in it.
        std::vector<std::uint8_t> lakeSeen(count, 0);
        std::int32_t lakesKept = 0, lakesDried = 0;
        for (std::size_t start = 0; start < count; ++start) {
            if (lakeSeen[start] || world.lakeDepthField[start] <= 0 || world.cells[start].sea) continue;
            std::vector<std::size_t> members;
            std::vector<std::size_t> pending{start};
            lakeSeen[start] = 1;
            std::int32_t head = spill[start] / kHeightScale;
            std::int32_t region = static_cast<std::int32_t>(start);
            while (!pending.empty()) {
                const std::size_t i = pending.back();
                pending.pop_back();
                members.push_back(i);
                head = std::max(head, spill[i] / kHeightScale);
                region = std::min(region, static_cast<std::int32_t>(i));
                const TilePos p{static_cast<std::int32_t>(i % params.width),
                                static_cast<std::int32_t>(i / params.width)};
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour(p, dir);
                    if (!world.inBounds(n)) continue;
                    const std::size_t j = indexOf(n.x, n.y);
                    if (lakeSeen[j] || world.lakeDepthField[j] <= 0 || world.cells[j].sea) continue;
                    lakeSeen[j] = 1;
                    pending.push_back(j);
                }
            }

            // How deep it really is, at the sixty-fourth of a step the fill
            // works in, and how wet the country around it is.
            std::int32_t deepestRaw = 0;
            std::int64_t rain = 0;
            for (const std::size_t i : members) {
                deepestRaw = std::max(deepestRaw, water[i]);
                rain += world.cells[i].moisture;
            }
            const std::int32_t deepest = deepestRaw * kMetresPerElevationStep / kHeightScale;
            const std::int32_t wet = static_cast<std::int32_t>(rain / std::int64_t(members.size()));

            // Twenty metres where it rains, eighty where it does not, and a
            // wide basin gathers from further so it needs less of them. Below
            // that the valley is dry: a floor, not a lake.
            std::int32_t needed = 20 + (255 - std::clamp(wet, 0, 255)) * 60 / 255;
            needed = needed * 100 / (100 + static_cast<std::int32_t>(std::min<std::size_t>(members.size(), 80)));
            if (deepest < needed) {
                for (const std::size_t i : members) world.lakeDepthField[i] = 0;
                ++lakesDried;
                continue;
            }
            ++lakesKept;
            for (const std::size_t i : members) {
                world.lakeRegionField[i] = region;
                world.lakeLevelField[i] = head;
                world.lakeDepthField[i] = std::max(0, head - original[i]);
                world.cells[i].elevation = static_cast<std::uint8_t>(head);
            }
        }
        world.lakesKept = lakesKept;
        world.basinsDried = lakesDried;
    }

    // Rivers, by where the water would actually go: every cell drains to its
    // lowest neighbour, and the rain that falls on the land is added up down
    // those paths. Where enough of it has collected, there is a river. Tracing
    // a few dozen courses by hand instead gave straight dashes that stopped in
    // the first hollow they met - hundreds of them, and not one river system.
    {
        std::int32_t riverFlow = 0;
        std::vector<std::int32_t> order(count);
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<std::int32_t>(i);
        // Sorted on the filled heights, at their own resolution: on the stored
        // eight-bit ones a flood plain is one flat step thousands of cells wide,
        // every cell of it ties with its neighbours, and the water has nowhere
        // to go.
        std::sort(order.begin(), order.end(), [&](std::int32_t a, std::int32_t b) {
            const std::int32_t fa = filled[static_cast<std::size_t>(a)];
            const std::int32_t fb = filled[static_cast<std::size_t>(b)];
            if (fa != fb) return fa > fb;
            return a < b;
        });

        std::vector<std::int32_t> flow(count, 0);
        std::vector<std::int32_t> downhill(count, -1);
        for (std::int32_t index : order) {
            const TilePos p{index % params.width, index / params.width};
            const WorldCell& here = world.at(p);
            if (here.sea) continue;
            // The rain: wetter ground sheds more, which is what puts the big
            // rivers where the weather is rather than only where the hills are.
            flow[static_cast<std::size_t>(index)] += 1 + here.moisture / 64;

            // Downhill in any of the eight directions, not only the four.
            // Four made the whole drainage of the world Manhattan - every river
            // ran due north or due east and turned at right angles - which was
            // invisible while a river was a flag on a cell and became a grid of
            // straight troughs the moment the channels were actually cut into
            // the ground (D116). A diagonal step is longer, so it has to fall
            // further to be worth taking; without that correction the flow
            // prefers diagonals and the same problem comes back at forty-five
            // degrees.
            std::int32_t lowest = filled[static_cast<std::size_t>(index)];
            std::int32_t best = -1;
            std::int32_t bestFall = 0;
            for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
                const TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const std::int32_t e = world.at(n).sea ? -1 : filled[indexOf(n.x, n.y)];
                if (e >= filled[static_cast<std::size_t>(index)]) continue;
                // Fall per unit of distance: the diagonal is 1.41 times as far,
                // taken here as 10/14 of the drop.
                const std::int32_t fall = filled[static_cast<std::size_t>(index)] - e;
                const std::int32_t steepness = core::isDiagonal(dir) ? fall * 10 / 14 : fall;
                if (steepness > bestFall) {
                    bestFall = steepness;
                    best = dir;
                    lowest = e;
                }
            }
            if (best < 0) continue;                       // a hollow: a lake in all but name
            const TilePos n = core::neighbour(p, best);
            downhill[static_cast<std::size_t>(index)] = best;
            flow[indexOf(n.x, n.y)] += flow[static_cast<std::size_t>(index)];
        }

        for (std::size_t i = 0; i < count; ++i) world.flowDirectionField[i] = static_cast<std::int8_t>(downhill[i]);
        world.riverDischargeField = flow;

        // H0/H1/H6: terminal basins and lake candidates from the downhill graph.
        std::vector<std::int32_t> terminal(count, -2);
        for (std::size_t i = 0; i < count; ++i) {
            if (world.cells[i].sea) {
                terminal[i] = static_cast<std::int32_t>(i);
                world.basinIdField[i] = static_cast<std::int32_t>(i);
                continue;
            }
            std::vector<std::size_t> path;
            std::size_t cur = i;
            while (true) {
                if (terminal[cur] >= -1) break;
                path.push_back(cur);
                const std::int32_t dir = downhill[cur];
                if (dir < 0) {
                    terminal[cur] = static_cast<std::int32_t>(cur);
                    break;
                }
                const TilePos p{static_cast<std::int32_t>(cur) % params.width,
                                static_cast<std::int32_t>(cur) / params.width};
                const TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) {
                    terminal[cur] = -1;
                    break;
                }
                cur = indexOf(n.x, n.y);
            }
            const std::int32_t resolved = terminal[cur];
            for (std::size_t k : path) terminal[k] = resolved;
        }
        for (std::size_t i = 0; i < count; ++i) {
            world.basinIdField[i] = terminal[i];
            if (world.cells[i].sea) {
                world.spillPointField[i] = {static_cast<std::int16_t>(i % params.width),
                                            static_cast<std::int16_t>(i / params.width)};
                continue;
            }
            const std::int32_t t = terminal[i];
            if (t >= 0) {
                world.spillPointField[i] = {static_cast<std::int16_t>(t % params.width),
                                            static_cast<std::int16_t>(t / params.width)};
            }
        }

        // What counts as a river here, now that the water has been added up.
        riverFlow = riverFlowThresholdFrom(params, flow, world.cells);
        world.flowAccumulationField = flow;
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                WorldCell& c = world.cells[i];
                if (c.sea) continue;
                // The drainage, whether or not it is a river: everything that
                // sheds water downhill cut itself a valley on the way.
                if (downhill[i] >= 0) {
                    c.drainOut = static_cast<std::int8_t>(downhill[i]);
                    std::uint8_t drain = 0;
                    for (std::int32_t f = flow[i]; f > 1 && drain < 15; f /= 2) ++drain;
                    c.drainSize = drain;
                }
                if (flow[i] < riverFlow) continue;
                c.river = true;
                c.riverOut = static_cast<std::int8_t>(downhill[i]);
                // Kept as a power of two: flow spans four orders of magnitude
                // between a brook and a trunk river, and what the play-scale
                // generator wants out of it - a width, a depth, a valley - all
                // go by doublings rather than by the number itself.
                std::uint8_t size = 0;
                for (std::int32_t f = flow[i]; f > riverFlow && size < 15; f /= 2) ++size;
                c.riverSize = static_cast<std::uint8_t>(size + 1);

                // H4: source types (simplified) - headwater when no upstream river drains in.
                bool hasUpstream = false;
                for (int dir = 0; dir < core::kNeighbourCount && !hasUpstream; ++dir) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (!world.inBounds(n)) continue;
                    const std::size_t j = indexOf(n.x, n.y);
                    hasUpstream = downhill[j] >= 0 && core::neighbour(n, downhill[j]) == TilePos{x, y} &&
                                  flow[j] >= riverFlow;
                }
                world.riverSourceField[i] = hasUpstream ? 0u : 1u;
            }

        // Water cuts down into what it runs over. The heights so far are rock
        // as the plates left it; a landscape the eye believes has valleys in it,
        // and a valley is the ground a river has taken away. Cut in proportion
        // to how much water passes - a trickle scratches, a trunk river carves -
        // which is the same accumulation that decided where the rivers are, so
        // it costs nothing but the pass.
        //
        // Only the picture and the local map's slopes change: the courses were
        // decided above and are not re-derived, so no river is left running up
        // a hill it just dug.
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                WorldCell& c = world.cells[i];
                if (c.sea || flow[i] <= 0) continue;
                std::int32_t cut = 0;
                for (std::int32_t f = flow[i]; f > 1; f /= 2) cut += 2;   // ~2 per doubling
                const std::int32_t resistance = std::clamp(world.erosionResistanceField[i], 60, 240);
                const std::int32_t permeability = std::clamp(world.permeabilityField[i], 40, 240);
                cut = cut * (300 - resistance) / 220;
                cut = cut * (280 - permeability / 2) / 220;
                cut = std::clamp(cut, 0, 34);
                world.erosionField[i] = cut;
                std::int32_t steepness = 0;
                if (c.riverOut >= 0) {
                    const TilePos downstream = core::neighbour({x, y}, c.riverOut);
                    if (world.inBounds(downstream))
                        steepness = std::max(0, filled[i] - filled[indexOf(downstream.x, downstream.y)]);
                }
                world.sedimentPotentialField[i] = std::max(0, flow[i] * std::max(1, steepness) /
                                                                  std::max(1, resistance));
                world.floodplainPotentialField[i] = std::clamp(
                        flow[i] * std::max(0, 24 - steepness) / 12, 0, 255);
                world.waterfallField[i] =
                        static_cast<std::uint8_t>(c.river && flow[i] > riverFlow && steepness >= 18 ? 1 : 0);
                c.elevation = static_cast<std::uint8_t>(
                        std::clamp(static_cast<std::int32_t>(c.elevation) - cut, 1, 255));
            }
    }
    for (std::size_t i = 0; i < count; ++i) {
        world.erodedHeightField[i] = world.cells[i].sea ? 0 : static_cast<std::int32_t>(world.cells[i].elevation);
        if (world.cells[i].river && !world.cells[i].sea && world.sedimentPotentialField[i] < 8) {
            world.rockTypeField[i] = RockType::Alluvial;
            world.erosionResistanceField[i] = 75;
            world.permeabilityField[i] = 185;
            world.soilParentMaterialField[i] = 6;
        }
        if (!world.cells[i].sea && world.cells[i].river) {
            const TilePos p{static_cast<std::int32_t>(i) % params.width,
                            static_cast<std::int32_t>(i) / params.width};
            bool atMouth = false;
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(p, dir);
                if (world.inBounds(n) && world.at(n).sea) { atMouth = true; break; }
            }
            world.deltaPotentialField[i] = atMouth ? std::clamp(world.riverDischargeField[i] / 2, 0, 255) : 0;
        }
    }

    // One pass of smoothing over the finished heights. Everything above works in
    // whole steps - a plate is a step, a flood fill advances in steps, an
    // incision is a step - and steps are what the hill shading draws as facets.
    // The land keeps its shape; it stops being made of plates.
    {
        std::vector<std::uint8_t> smoothed(count, 0);
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                if (world.cells[i].sea) { smoothed[i] = 0; continue; }
                std::int32_t sum = 0, n = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy)
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const TilePos q{x + dx, y + dy};
                        if (!world.inBounds(q)) continue;
                        const WorldCell& o = world.at(q);
                        // The sea counts as the bottom, so a coast slopes into
                        // it instead of ending in a wall.
                        sum += o.sea ? 0 : o.elevation;
                        ++n;
                    }
                smoothed[i] = static_cast<std::uint8_t>(std::clamp(sum / std::max(1, n), 1, 255));
            }
        for (std::size_t i = 0; i < count; ++i)
            if (!world.cells[i].sea) world.cells[i].elevation = smoothed[i];
    }

    // Smoothing does not know about water. It averages a river cell with the
    // banks either side of it, and over a long enough reach that can leave a
    // stretch of the course standing higher than the stretch below it - a river
    // running uphill on the map, and a local map (D89) whose valley slopes the
    // wrong way. So the courses are walked from their heads down and each cell
    // is pressed down to no higher than the one above it. Highest first, so one
    // pass carries the whole length.
    {
        std::vector<std::int32_t> course;
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                if (world.cells[i].river && !world.cells[i].sea)
                    course.push_back(static_cast<std::int32_t>(i));
            }
        std::sort(course.begin(), course.end(), [&](std::int32_t a, std::int32_t b) {
            const auto ea = world.cells[static_cast<std::size_t>(a)].elevation;
            const auto eb = world.cells[static_cast<std::size_t>(b)].elevation;
            if (ea != eb) return ea > eb;
            return a < b;      // ties in index order, or the world stops being a function of its seed
        });
        // Repeated until nothing moves. One pass is not enough: pressing a cell
        // down can put it below a stretch that was already dealt with further
        // down the same river, and that stretch then has to come down too. It
        // settles in three or four passes; the cap is only there so a cycle
        // that should not exist cannot hang the generator.
        for (int pass = 0; pass < 12; ++pass) {
            bool moved = false;
            for (std::int32_t index : course) {
                const WorldCell& here = world.cells[static_cast<std::size_t>(index)];
                if (here.riverOut < 0) continue;
                const TilePos p{index % params.width, index / params.width};
                const TilePos n = core::neighbour(p, here.riverOut);
                if (!world.inBounds(n)) continue;
                WorldCell& next = world.at(n);
                if (next.sea) continue;
                if (next.elevation <= here.elevation) continue;
                next.elevation = here.elevation;
                moved = true;
            }
            if (!moved) break;
        }
    }

    // --- PASS C4: final temperature ---------------------------------------
    {
        for (std::size_t i = 0; i < count; ++i) {
            const WorldCell& c = world.cells[i];
            std::int32_t warmth = world.baseTemperatureField[i];
            warmth += world.coastalClimateBiasField[i] / 3;
            if (c.sea) warmth += world.seaSurfaceTemperatureBiasField[i] / 3;
            if (c.sea) warmth = warmth * 4 / 5 + 25;
            warmth = std::clamp(warmth, 0, 255);
            world.temperatureField[i] = warmth;
            world.cells[i].temperature = static_cast<std::uint8_t>(warmth);
        }
    }

    // Deltas: where a river of any size meets the sea it drops what it carried,
    // and what it drops is the best farmland there is and no stone at all. This
    // is the one landform this game's content was written for, so it is worth
    // finding properly rather than hoping a river happens to end somewhere flat.
    std::vector<char> delta(count, 0);
    {
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x) {
                const std::size_t i = indexOf(x, y);
                const WorldCell& c = world.cells[i];
                if (c.sea || !c.river) continue;
                bool mouth = false;
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (world.inBounds(n) && world.at(n).sea) mouth = true;
                }
                if (!mouth) continue;
                // The silt spreads over the low ground around the mouth, not
                // over the whole coast: a delta is flat, and where the land
                // climbs the river is in a gorge instead.
                for (std::int32_t dy = -6; dy <= 6; ++dy)
                    for (std::int32_t dx = -6; dx <= 6; ++dx) {
                        const TilePos q{x + dx, y + dy};
                        if (!world.inBounds(q)) continue;
                        const WorldCell& o = world.at(q);
                        if (o.sea || o.elevation > 12) continue;
                        if (core::tileDistance({x, y}, q) > 6) continue;
                        delta[indexOf(q.x, q.y)] = 1;
                    }
            }
    }

    for (std::int32_t y = 0; y < params.height; ++y) {
        for (std::int32_t x = 0; x < params.width; ++x) {
            const std::size_t i = indexOf(x, y);
            WorldCell& c = world.cells[i];
            if (c.sea) continue;
            const std::int32_t warm = c.temperature;
            const std::int32_t wet = c.moisture;

            // Koeppen in miniature, in the order the classification actually
            // resolves: height and water override climate, then cold overrides
            // rain, then rain decides the rest.
            if (delta[i]) c.climate = Climate::Delta;
            else if (c.elevation > 165) c.climate = Climate::Alpine;
            else if (c.river && wet < 150) c.climate = Climate::RiverValley;
            else if (warm < 30) c.climate = Climate::Ice;
            else if (warm < 60) c.climate = Climate::Tundra;
            else if (warm < 105) c.climate = wet > 90 ? Climate::Taiga : Climate::Steppe;
            else if (warm < 170) {
                if (wet < 70) c.climate = Climate::Steppe;
                else if (wet < 150) c.climate = Climate::TemperateForest;
                else c.climate = Climate::TemperateForest;
            } else if (warm < 205) {
                // Warm and seasonal. Dry summers by the sea is the country the
                // olive, the vine and the small city-state belong to.
                bool coastal = false;
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (world.inBounds(n) && world.at(n).sea) coastal = true;
                }
                if (wet < 60) c.climate = Climate::Desert;
                else if (wet < 150) c.climate = coastal ? Climate::Mediterranean : Climate::Steppe;
                else c.climate = Climate::Mediterranean;
            } else {
                if (wet < 60) c.climate = Climate::Desert;
                else if (wet < 150) c.climate = Climate::Savanna;
                else c.climate = Climate::TropicalForest;
            }

            // What the local generator has to build: it knows two kinds of
            // ground, and every climate above is one or the other of them.
            switch (c.climate) {
                case Climate::Delta:
                case Climate::RiverValley:
                case Climate::Desert:
                case Climate::Savanna:
                case Climate::Steppe:
                    c.biome = Biome::RiverValley;    // dry country, reed and clay
                    break;
                default:
                    c.biome = Biome::Temperate;      // wooded country, timber and stone
                    break;
            }

            std::int32_t fertility = c.moisture / 2 + (c.river ? 90 : 0) -
                                     std::max(0, c.elevation - 120) / 2;
            // Silt is the best ground there is, and ice and rock the worst,
            // whatever the rain says.
            if (c.climate == Climate::Delta) fertility += 90;
            if (c.climate == Climate::Ice || c.climate == Climate::Alpine) fertility /= 3;
            if (c.climate == Climate::Tundra) fertility /= 2;
            if (c.climate == Climate::Desert) fertility -= 40;
            const auto volcanic = world.hybridTerrain ? world.hybridTerrain->sample(
                core::Fixed::fromInt(x * kMetresPerCell), core::Fixed::fromInt(y * kMetresPerCell)).barren : core::kZero;
            const auto barren = static_cast<std::int32_t>((volcanic * 255).roundToInt());
            fertility = fertility * (255 - barren) / 255;
            c.fertility = static_cast<std::uint8_t>(std::clamp(fertility, 0, 255));

            // --- PASS B0: soil -------------------------------------------
            const std::int32_t moisture = world.humidityField[i];
            const std::int32_t drainage = std::clamp(world.permeabilityField[i] - world.floodplainPotentialField[i] / 3,
                                                     0, 255);
            const std::int32_t organic = std::clamp((moisture + world.floodplainPotentialField[i] +
                                                     world.deltaPotentialField[i]) /
                                                            3,
                                                    0, 255);
            SoilType soil = SoilType::Mineral;
            if (world.rockTypeField[i] == RockType::HardRock && c.elevation > 170) soil = SoilType::Bedrock;
            else if (c.climate == Climate::Delta || world.deltaPotentialField[i] > 80)
                soil = SoilType::Alluvial;
            else if (world.rockTypeField[i] == RockType::ClayRich || drainage < 90)
                soil = SoilType::Clay;
            else if (world.rockTypeField[i] == RockType::Sandstone || drainage > 170)
                soil = SoilType::Sandy;
            else if (organic > 170 && moisture > 150)
                soil = SoilType::Peat;
            world.soilTypeField[i] = soil;
            world.soilDrainageField[i] = drainage;
            world.soilOrganicPotentialField[i] = organic * (255 - barren) / 255;
            world.soilFertilityField[i] = std::clamp(
                    static_cast<std::int32_t>(c.fertility) + organic / 4 - std::max(0, c.elevation - 160) / 3,
                    0, 255) * (255 - barren) / 255;
            if (barren > 160) world.soilTypeField[i] = SoilType::Bedrock;

            // --- PASS B1: biome suitability -------------------------------
            world.primaryBiomeSuitabilityField[i] = static_cast<std::uint8_t>(c.climate);
            Climate secondary = c.climate;
            if (c.climate == Climate::Delta) secondary = Climate::RiverValley;
            else if (c.climate == Climate::RiverValley) secondary = moisture < 120 ? Climate::Steppe : Climate::TemperateForest;
            else if (c.climate == Climate::Desert) secondary = Climate::Steppe;
            else if (c.climate == Climate::Steppe) secondary = moisture < 90 ? Climate::Desert : Climate::TemperateForest;
            else if (c.climate == Climate::TemperateForest) secondary = moisture < 110 ? Climate::Steppe : Climate::Taiga;
            else if (c.climate == Climate::Savanna) secondary = moisture < 90 ? Climate::Desert : Climate::TropicalForest;
            else if (c.climate == Climate::TropicalForest) secondary = Climate::Savanna;
            else if (c.climate == Climate::Taiga) secondary = moisture < 100 ? Climate::Tundra : Climate::TemperateForest;
            else if (c.climate == Climate::Tundra) secondary = Climate::Taiga;
            world.secondaryBiomeSuitabilityField[i] = static_cast<std::uint8_t>(secondary);
            world.biomeTransitionField[i] = std::clamp(
                    std::min(std::abs(warm - 105), std::min(std::abs(warm - 170), std::abs(wet - 150))) * 4,
                    0, 255);

            // --- PASS B2: terrain material suitability --------------------
            std::array<std::int32_t, 6> material{
                    std::clamp(moisture + world.soilFertilityField[i] / 2 - c.elevation / 2, 0, 255),
                    std::clamp(world.soilFertilityField[i] + 40 - moisture / 3, 0, 255),
                    std::clamp(drainage + std::max(0, 140 - moisture), 0, 255),
                    std::clamp(static_cast<std::int32_t>(c.elevation) + world.erosionResistanceField[i] / 2,
                               0, 255),
                    std::clamp(world.floodplainPotentialField[i] + moisture / 2 - drainage / 3, 0, 255),
                    std::clamp(std::max(0, 180 - warm) + static_cast<std::int32_t>(c.elevation) / 2, 0, 255),
            };
            if (c.climate == Climate::Desert) material[2] = std::min(255, material[2] + 80);
            if (c.climate == Climate::Delta || c.climate == Climate::RiverValley)
                material[4] = std::min(255, material[4] + 70);
            if (c.climate == Climate::Ice || c.climate == Climate::Alpine) material[5] = std::min(255, material[5] + 80);
            // Volcanism is a geological overlay, not a replacement for climate:
            // old flanks can grow forests, fresh lava/ash cannot grow a meadow.
            material[0] = material[0] * (255 - barren) / 255;
            material[1] = std::min(255, material[1] + barren / 3);
            material[3] = std::min(255, material[3] + barren);
            std::int32_t total = 0;
            for (std::int32_t wv : material) total += std::max(1, wv);
            std::array<std::uint8_t, 6> packed{};
            for (std::size_t m = 0; m < packed.size(); ++m)
                packed[m] = static_cast<std::uint8_t>(std::clamp(material[m] * 255 / std::max(1, total), 0, 255));
            world.materialSuitabilityField[i] = packed;
        }
    }

    // --- where people live -------------------------------------------------
    // Best ground first, and never two settlements within sight of one another:
    // a site is a whole local map, and two of them in adjoining cells would be
    // one town drawn twice.
    // Worth of a site: the ground, the water, and a coast worth landing on -
    // plus a jitter drawn from the cell itself, because ranking by fertility
    // alone laid the settlements out in a regular lattice. Deterministic: the
    // same seed puts the same peoples in the same places.
    const auto worthOf = [&](TilePos p) {
        const WorldCell& c = world.at(p);
        std::int32_t score = c.fertility;
        if (c.river) score += 120;
        for (int dir : core::kCardinalDirections) {
            const TilePos n = core::neighbour(p, dir);
            if (world.inBounds(n) && world.at(n).sea) { score += 25; break; }
            if (world.inBounds(n) && world.at(n).river) score += 20;
        }
        score -= std::max(0, static_cast<std::int32_t>(c.elevation) - 140) / 2;
        return score + static_cast<std::int32_t>(valueNoise(params.seed + 5, p.x, p.y) % 90);
    };
    // Worked out once and kept, not recomputed inside the comparator: a million
    // candidates is twenty million comparisons, and each one was costing two
    // neighbourhood walks and a hash.
    std::vector<std::int32_t> candidates;
    std::vector<std::int32_t> worth(count, 0);
    for (std::int32_t y = 0; y < params.height; ++y)
        for (std::int32_t x = 0; x < params.width; ++x) {
            const std::size_t i = indexOf(x, y);
            const WorldCell& c = world.cells[i];
            if (c.sea || c.fertility <= 30) continue;
            // Nor where nothing can be grown or grazed. An ice cap has water and
            // no food; a bare alpine ridge has stone and no soil. Communities
            // put on either died to the last person inside two months, which is
            // the right answer to "can people live here" and the wrong place to
            // start a game. This is the bronze age: it settled valleys, plains
            // and coasts, and left the ice and the high rock alone.
            if (c.climate == Climate::Ice || c.climate == Climate::Alpine) continue;
            // Nobody settles where there is no water. It sounds obvious and it
            // was not enforced: once the local map stopped drawing a river
            // through every cell (D97), the communities that had been put on dry
            // cells were on maps with no water on them at all, and seven of
            // fifteen died of thirst inside a month. A site needs a river of its
            // own or a shore.
            bool watered = c.river;
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour({x, y}, dir);
                if (world.inBounds(n) && world.at(n).sea) watered = true;
            }
            if (!watered) continue;
            worth[i] = worthOf({x, y});
            candidates.push_back(static_cast<std::int32_t>(i));
        }
    std::sort(candidates.begin(), candidates.end(), [&](std::int32_t a, std::int32_t b) {
        const std::int32_t wa = worth[static_cast<std::size_t>(a)];
        const std::int32_t wb = worth[static_cast<std::size_t>(b)];
        if (wa != wb) return wa > wb;
        return a < b;    // index order is row-major order: north-west first
    });

    // Spacing enforced by a stamp on the map rather than by asking every site
    // already placed how far away it is. Four hundred sites against a million
    // candidates is four hundred million distance checks; a stamp is the area of
    // one circle per site, and it is paid once.
    const std::int32_t spacing = siteSpacingFor(params);

    // Taken a climate at a time rather than best-first. Ranked purely on how
    // good the ground is, every community lands in a river valley, because a
    // river valley is always the best ground - and a world of fifteen identical
    // valleys is not a world with peoples in it. Going round the climates deals
    // the sites out: the delta, the olive coast, the steppe, the forest, and
    // each of them the best cell of its own kind (D96).
    std::array<std::vector<std::int32_t>, static_cast<std::size_t>(Climate::Count)> byClimate;
    for (std::int32_t index : candidates)
        byClimate[static_cast<std::size_t>(world.cells[static_cast<std::size_t>(index)].climate)]
                .push_back(index);
    std::int64_t landCells = 0;
    for (const auto& c : world.cells)
        if (!c.sea) ++landCells;
    const std::int32_t wantedSites = siteCountFor(params, landCells);
    std::vector<char> taken(count, 0);
    // The order sites are offered in: one from each climate that has any, then
    // round again. Climates with no land of their own on this world are simply
    // skipped, so a map with no tundra on it settles the rest instead of
    // leaving a gap.
    std::vector<std::int32_t> order;
    {
        std::array<std::size_t, static_cast<std::size_t>(Climate::Count)> next{};
        bool anyLeft = true;
        while (anyLeft && static_cast<std::int32_t>(order.size()) < wantedSites * 40) {
            anyLeft = false;
            for (std::size_t climate = 0; climate < byClimate.size(); ++climate) {
                auto& list = byClimate[climate];
                if (next[climate] >= list.size()) continue;
                order.push_back(list[next[climate]++]);
                anyLeft = true;
            }
        }
    }
    for (std::int32_t index : order) {
        if (static_cast<std::int32_t>(world.sites.size()) >= wantedSites) break;
        if (taken[static_cast<std::size_t>(index)]) continue;
        const TilePos p{index % params.width, index / params.width};
        WorldSite site;
        site.cell = p;
        site.climate = world.at(p).climate;
        // Which people this is, decided by the country: the reed-and-mud-brick
        // culture belongs to the deltas, the flood plains and the dry country
        // where there is no timber; the timber-and-stone one to the forests,
        // the olive coasts and the hills. This is the whole point of having
        // climates - a people is put where its way of building works (GDD 5).
        site.ethnos = ethnosForClimate(site.climate);
        const auto& names = site.ethnos == "sumerian"   ? kSumerianNames
                            : site.ethnos == "yamna"     ? kSteppeNames
                            : site.ethnos == "northfolk" ? kNorthNames
                                                         : kAchaeanNames;
        // Counted per people, so the second Uruk is Uruk 2 and not the two
        // hundredth settlement on the map.
        std::size_t placed = 0;
        for (const auto& s : world.sites)
            if (s.ethnos == site.ethnos) ++placed;
        site.name = names[placed % names.size()];
        if (placed >= names.size()) site.name += " " + std::to_string(placed / names.size() + 1);

        // What the ground here mainly offers. Read off the cell, because that is
        // what a community founded here will end up living on - but it is a fact
        // about the land, not a head start: everybody arrives with the same
        // twenty-four people on the same morning.
        const WorldCell& ground = world.at(p);
        bool coastal = false;
        bool watered = ground.river;
        for (int dir : core::kCardinalDirections) {
            const TilePos n = core::neighbour(p, dir);
            if (!world.inBounds(n)) continue;
            if (world.at(n).sea) coastal = true;
            if (world.at(n).river) watered = true;
        }
        if (ground.elevation > 170) site.livelihood = Livelihood::Miners;
        else if (coastal && ground.fertility < 120) site.livelihood = Livelihood::Fishers;
        else if (watered && ground.fertility >= 110) site.livelihood = Livelihood::Farmers;
        else if (ground.moisture > 150 && ground.elevation > 90) site.livelihood = Livelihood::Foresters;
        else if (ground.fertility >= 110) site.livelihood = Livelihood::Farmers;
        else site.livelihood = Livelihood::Herders;

        site.population = kFoundingCommunity;
        world.sites.push_back(std::move(site));

        for (std::int32_t dy = -spacing; dy <= spacing; ++dy)
            for (std::int32_t dx = -spacing; dx <= spacing; ++dx) {
                const TilePos q{p.x + dx, p.y + dy};
                if (!world.inBounds(q)) continue;
                if (core::tileDistance(p, q) >= spacing) continue;
                taken[indexOf(q.x, q.y)] = 1;
            }
    }

    // The played community goes in the best delta or flood plain there is,
    // because that is the country this game's content is written for: its
    // buildings are reed and mud brick, and a community of it put down on dry
    // upland has no reeds and no clay to build with. The delta first - silt,
    // reeds, a river and the sea, which is where every one of the cultures this
    // game is about actually began - and the best one of them, not the first:
    // since the sites are dealt out a climate at a time (D96), first in the list
    // means nothing about how good the ground is.
    std::size_t played = world.sites.size();
    {
        std::int32_t bestScore = std::numeric_limits<std::int32_t>::min();
        for (std::size_t i = 0; i < world.sites.size(); ++i) {
            const TilePos cell = world.sites[i].cell;
            const WorldCell& c = world.at(cell);
            if (c.biome != params.playedBiome) continue;
            std::int32_t score = worth[indexOf(cell.x, cell.y)];
            if (world.sites[i].climate == Climate::Delta) score += 400;
            else if (world.sites[i].climate == Climate::RiverValley) score += 200;
            if (c.river) score += 100;
            // Low ground: a flood plain is flat by definition, and the content
            // assumes a valley floor rather than a gorge a kilometre up.
            score -= static_cast<std::int32_t>(c.elevation) * 2;
            if (score > bestScore) { bestScore = score; played = i; }
        }
        // And it has to be on water. The country asked for is the local biome,
        // which a coastal cell can carry without a river being anywhere near it:
        // on the seventy-one-percent-sea world that is how the community ended
        // up a fishing village on a savanna shore twice out of two seeds, with
        // the right biome and no river in it. Without one there is no flood
        // plain, no irrigation and no reeds, so if the best of the sites has no
        // river, none of them will do and one is founded below.
        if (played < world.sites.size() && !world.at(world.sites[played].cell).river)
            played = world.sites.size();
    }

    // None of the sites drew that country? Then one is founded on the best cell
    // of it there is. With a thousand settlements on the map the odds of every
    // one of them missing were nil; with fifteen it happens, and when it does
    // the player is handed a valley culture on a hillside and starves (D95).
    if (played == world.sites.size()) {
        std::int32_t bestWorth = std::numeric_limits<std::int32_t>::min();
        TilePos bestCell{-1, -1};
        // Three tries, each looser than the last. The country asked for; then
        // any cell with a river in it at all; then the best ground there is.
        //
        // The second was added when the sea went up to seven tenths for the
        // world built to look like this one: on those maps there is often no
        // cell of the exact country the content wants, the search came back
        // empty, and the community was handed to whatever site happened to be
        // first in the list - twice out of two seeds a fishing village on a
        // savanna coast, with no river, no flood plain and therefore no
        // irrigation, no reeds and nothing to build with. A river people put on
        // any river is a game; put on a beach it is not.
        for (int looseness = 0; looseness < 3 && !world.inBounds(bestCell); ++looseness)
            for (std::int32_t y = 0; y < params.height; ++y)
                for (std::int32_t x = 0; x < params.width; ++x) {
                    const WorldCell& c = world.at({x, y});
                    if (c.sea) continue;
                    if (looseness < 2 && !c.river) continue;
                    if (looseness < 1 && c.biome != params.playedBiome) continue;
                    const std::int32_t score = worth[indexOf(x, y)] != 0 ? worth[indexOf(x, y)]
                                                                         : worthOf({x, y});
                    if (score > bestWorth) { bestWorth = score; bestCell = {x, y}; }
                }
        if (world.inBounds(bestCell)) {
            WorldSite site;
            site.cell = bestCell;
            site.climate = world.at(bestCell).climate;
            site.ethnos = ethnosForClimate(site.climate);
            const auto& names = site.ethnos == "sumerian"   ? kSumerianNames
                            : site.ethnos == "yamna"     ? kSteppeNames
                            : site.ethnos == "northfolk" ? kNorthNames
                                                         : kAchaeanNames;
            std::size_t placed = 0;
            for (const auto& s : world.sites)
                if (s.ethnos == site.ethnos) ++placed;
            site.name = names[placed % names.size()];
            if (placed >= names.size()) site.name += " " + std::to_string(placed / names.size() + 1);
            site.livelihood = Livelihood::Farmers;
            site.population = kFoundingCommunity;
            played = world.sites.size();
            world.sites.push_back(std::move(site));
        }
    }
    if (played >= world.sites.size() && !world.sites.empty()) played = 0;
    if (played < world.sites.size()) {
        world.sites[played].played = true;



        world.playedCell = world.sites[played].cell;
        // The block of cells the local map refines, kept inside the world.
        const std::int32_t half = kCellsPerLocalMap / 2;
        world.playedBlock = {std::clamp(world.playedCell.x - half, 0,
                                        std::max(0, params.width - kCellsPerLocalMap)),
                             std::clamp(world.playedCell.y - half, 0,
                                        std::max(0, params.height - kCellsPerLocalMap))};
    }
    return world;
}

} // namespace generation
