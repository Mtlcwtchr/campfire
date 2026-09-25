#pragma once
#include "game/generation/terrain_foundation.hpp"
// The world above the local map.
//
// GDD 4.2 asks for a macro pipeline - tectonics, climate, biomes, peoples - and
// this is the first stage of it that exists: a coarse map of the whole country,
// one cell to a local map. The community that is actually simulated lives in one
// of those cells; the rest is generated land with its rivers, its heights and
// the sites where other peoples have settled, so that the map a player looks at
// when they pull the camera back is a world rather than a border.
//
// It is data, not simulation. Nothing here ticks.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <array>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/generation/local_map_gen.hpp"

namespace generation {

class HybridTerrain;

struct SeasonParams {
    std::int32_t amplitude = 20;
    std::int32_t wetSeasonBias = 0;
    std::int32_t coldSeasonBias = 0;
};

struct WorldConstants {
    std::uint64_t worldSeed = 0;
    std::int32_t worldWidth = 0;
    std::int32_t worldHeight = 0;
    std::int32_t seaLevel = 0;
    std::int32_t globalTemperatureScale = 100;
    std::int32_t globalMoistureScale = 100;
    std::int32_t planetRotationSign = 1;
    SeasonParams season;
};

struct ClimateVector {
    std::int16_t x = 0;
    std::int16_t y = 0;
};

enum class RockType : std::uint8_t {
    HardRock,
    SoftSediment,
    Limestone,
    Volcanic,
    ClayRich,
    Sandstone,
    Alluvial,
    Count,
};

const char* rockTypeName(RockType type);

enum class SoilType : std::uint8_t {
    Bedrock,
    Mineral,
    Alluvial,
    Clay,
    Sandy,
    Peat,
    Count,
};

enum class TerrainArchetype : std::uint8_t {
    Grass,
    Dirt,
    Sand,
    Rock,
    Marsh,
    Snow,
    Count,
};

// What kind of country a cell is, in the terms a person would use looking at it.
// The local generator only knows two kinds of ground (Biome), which is what it
// needs to scatter reeds or oaks; this is the other question - what part of the
// world is this - and it is the one that decides where a people belongs.
//
// Read off temperature, rain and height together, which is Koeppen's idea in
// miniature: everything here is a line on that chart. What it buys the game is
// that a map can be looked at and the places named - that is a delta, that is a
// steppe, that is the dry side of a range - and a people can be put where its
// way of living actually works.
enum class Climate : std::uint8_t {
    Ice,            // frozen the year round
    Tundra,         // cold, treeless, mossy
    Taiga,          // cold and wet: conifer forest
    TemperateForest,// mild and wet: oak, game, timber
    Steppe,         // mild and middling: grass to the horizon, herders' country
    Mediterranean,  // warm, wet winters, dry summers, broken coast
    Desert,         // hot and dry
    Savanna,        // hot with a wet season
    TropicalForest, // hot and wet
    Alpine,         // high enough that height decides everything
    RiverValley,    // a river's flood plain, whatever else the climate is
    Delta,          // where a big river meets the sea: silt, reeds, no stone
    Count,
};

const char* climateName(Climate c);

struct WorldCell {
    // Metres above the sea, in steps of kMetresPerElevationStep. Zero is the
    // sea itself.
    std::uint8_t elevation = 0;
    Biome biome = Biome::Temperate;
    bool sea = false;
    // A river runs through this cell, and which way it leaves: the direction
    // index into core::kNeighbourOffsets, or -1 at the mouth.
    bool river = false;
    std::int8_t riverOut = -1;
    // How much water comes down it, as a rough power of two of the accumulated
    // flow: 0 is no river at all, 15 is the trunk of a continent's drainage.
    // The play-scale generator needs it to know how wide to cut the channel and
    // how broad a valley to hang either side of it - a brook and the Euphrates
    // are the same flag otherwise.
    std::uint8_t riverSize = 0;
    // And the rest of the drainage: which way the ground here sheds its water
    // even when there is not enough of it to call a river, and how much.
    //
    // Every valley on Earth was cut by water, and most of them have no river in
    // them now. Carving only the rivers gave ranges with no way through: the
    // headwaters of a mountain are gullies, they fall below any threshold worth
    // drawing a blue line at, and they are exactly what makes a col between two
    // peaks (D116).
    std::int8_t drainOut = -1;
    std::uint8_t drainSize = 0;
    // How much rain the cell gets and how good its ground is, 0..255. What makes
    // one valley worth settling and the next one empty.
    std::uint8_t moisture = 0;
    std::uint8_t fertility = 0;
    // How warm, 0..255: cold at the top of the map, hot at the bottom, and
    // colder the higher the ground. Sea cells carry it too - a frozen sea is a
    // different thing to sail than a warm one.
    std::uint8_t temperature = 0;
    Climate climate = Climate::TemperateForest;
};

// How many people a community is founded with. Every settlement on the map
// starts as the same thing the player starts as: one band of this many, arrived
// this spring with what it could carry. Nobody begins with a city, and nobody
// begins with an advantage.
inline constexpr std::int32_t kFoundingCommunity = 24;

// What the land at a site mainly offers. Not what its people "are" - everyone
// starts equal and takes up whatever their ground will bear - but what that
// ground gives: it is read off the cell, and it is what a founding community
// there will end up living on.
enum class Livelihood : std::uint8_t {
    Farmers,     // river valley, good soil: grain
    Herders,     // dry upland: sheep and goats
    Fishers,     // a coast worth landing on
    Foresters,   // high wet ground: timber and game
    Miners,      // mountains: stone, copper, tin
};

const char* livelihoodName(Livelihood l);

// A place people live. One to a cell at most: a settlement fills its cell, which
// is what a local map is.
struct WorldSite {
    core::TilePos cell;
    std::string name;
    std::string ethnos;
    // The one the player's community lives in.
    bool played = false;
    // Everybody starts with the same band on the same day (kFoundingCommunity).
    // What they become is what they make of the ground they are on, which is the
    // whole point of putting them on different ground.
    std::int32_t population = kFoundingCommunity;
    Livelihood livelihood = Livelihood::Farmers;
    // The country they settled, which is why they settled it: a people is put
    // where its way of living works, so this is the reason the site is here at
    // all and the reason its ethnos is the one it is.
    Climate climate = Climate::RiverValley;
};

struct WorldMapData {
    std::int32_t width = 0;
    std::int32_t height = 0;
    // The seed it came out of. Carried so a renderer can tell one world from
    // another without comparing a million cells: the map is a pure function of
    // this and the size.
    std::uint64_t seed = 0;
    WorldConstants constants;
    std::vector<WorldCell> cells;
    std::vector<WorldSite> sites;
    // Immutable baked regional forms. Shared by generation/render workers and
    // persisted with the world, rather than reselected when a chunk is loaded.
    std::shared_ptr<const HybridTerrain> hybridTerrain;
    std::shared_ptr<const TerrainFoundation> terrainFoundation;
    TerrainStage terrainStage = TerrainStage::Final;
    std::vector<std::int32_t> primaryHeightField;
    // PASS G1 — continental mask.
    std::vector<std::int32_t> continentalField;
    std::vector<std::int32_t> distanceToCoast;
    std::vector<std::uint8_t> initialLandMask;
    // PASS G2 — pseudo-tectonics.
    std::vector<std::int32_t> upliftField;
    std::vector<std::int32_t> riftField;
    std::vector<std::int32_t> faultField;
    std::vector<std::uint8_t> geologyRegion;
    // PASS G3 — base macro height.
    std::vector<std::int32_t> macroHeightField;
    // PASS G4 — geology.
    std::vector<RockType> rockTypeField;
    std::vector<std::int32_t> erosionResistanceField;
    std::vector<std::uint8_t> soilParentMaterialField;
    std::vector<std::int32_t> permeabilityField;
    // PASS G5 — thermal erosion.
    std::vector<std::int32_t> thermallyRelaxedHeightField;
    // PASS G6 — hydraulic / fluvial erosion.
    std::vector<std::int32_t> hydrologicallyCorrectedHeightField;
    std::vector<std::int32_t> basinIdField;
    std::vector<ClimateVector> spillPointField;
    std::vector<std::int8_t> flowDirectionField;
    std::vector<std::int32_t> flowAccumulationField;
    std::vector<std::int32_t> riverDischargeField;
    std::vector<std::uint8_t> riverSourceField;
    std::vector<std::int32_t> lakeRegionField;
    std::vector<std::int32_t> lakeLevelField;
    // Difference between the priority-flood lake surface and the original
    // basin floor, in elevation steps. Zero means this cell is not lake bed.
    std::vector<std::int32_t> lakeDepthField;

    // Which recipe the country here was built to. A world used to be one
    // recipe everywhere, which is why every world came out the same world at a
    // different seed: the same share of sea, the same ruggedness, the same
    // grain of coastline from edge to edge. These are regions of it - a flood
    // plain between two rivers, a young continent with a cordillera down one
    // side, a scatter of islands - and the map says which is which so the
    // passes below can agree with the shape above them.
    std::vector<std::int32_t> provinceField;

    // How many basins came out holding water and how many came out dry. Worth
    // counting rather than guessing: the number of lakes is the loudest thing
    // about a landscape from above, and the pass that decides it is the same
    // pass that fills hollows, which must fill every one of them.
    std::int32_t lakesKept = 0;
    std::int32_t basinsDried = 0;
    std::vector<std::uint8_t> waterfallField;
    std::vector<std::int32_t> sedimentPotentialField;
    std::vector<std::int32_t> erosionField;
    std::vector<std::int32_t> floodplainPotentialField;
    std::vector<std::int32_t> deltaPotentialField;
    std::vector<std::int32_t> erodedHeightField;
    // PASS C0-C5 — climate generation.
    std::vector<std::int32_t> baseTemperatureField;
    std::vector<ClimateVector> prevailingWindField;
    std::vector<std::int32_t> windStrengthField;
    std::vector<std::int32_t> windVariabilityField;
    std::vector<std::int32_t> precipitationField;
    std::vector<std::int32_t> airMoistureField;
    std::vector<std::int32_t> rainShadowField;
    std::vector<ClimateVector> oceanCurrentField;
    std::vector<std::int32_t> seaSurfaceTemperatureBiasField;
    std::vector<std::int32_t> coastalClimateBiasField;
    std::vector<std::int32_t> temperatureField;
    std::vector<std::int32_t> annualRainfallField;
    std::vector<std::int32_t> humidityField;
    std::vector<std::int32_t> seasonalityField;
    std::vector<std::int32_t> winterRainField;
    std::vector<std::int32_t> summerRainField;
    std::vector<std::int32_t> drySeasonStrengthField;
    // PASS B0 — soil.
    std::vector<SoilType> soilTypeField;
    std::vector<std::int32_t> soilFertilityField;
    std::vector<std::int32_t> soilDrainageField;
    std::vector<std::int32_t> soilOrganicPotentialField;
    // PASS B1 — biome suitability.
    std::vector<std::uint8_t> primaryBiomeSuitabilityField;
    std::vector<std::uint8_t> secondaryBiomeSuitabilityField;
    std::vector<std::int32_t> biomeTransitionField;
    // PASS B2 — terrain material suitability.
    std::vector<std::array<std::uint8_t, 6>> materialSuitabilityField;
    // Which cell the played site sits in, and the corner of the block of cells
    // the local map refines.
    core::TilePos playedCell{0, 0};
    core::TilePos playedBlock{0, 0};

    bool inBounds(core::TilePos p) const {
        return p.x >= 0 && p.y >= 0 && p.x < width && p.y < height;
    }
    const WorldCell& at(core::TilePos p) const {
        return cells[static_cast<std::size_t>(p.y) * width + p.x];
    }
    WorldCell& at(core::TilePos p) {
        return cells[static_cast<std::size_t>(p.y) * width + p.x];
    }
};

// How many world cells a local map covers along each side.
//
// One. A cell is a place - the ground a community lives on and walks over - and
// a local map is that place at play scale. Ten cells to a map made the cell
// eighteen metres across, and eighteen metres is not a place: it made the whole
// world 320 x 18 = five and a half kilometres of country, which is a parish
// drawn very carefully rather than a world. The heights the local map needs are
// interpolated between cells either way (worldHeightAt), so the finer cell was
// buying detail the local generator already produces from its own noise.
inline constexpr std::int32_t kCellsPerLocalMap = 1;

// The ground one world cell covers, in metres.
//
// Five hundred and forty, which makes the world eleven hundred kilometres
// across: a continent rather than a county. It was a hundred and eighty, chosen
// so that one cell was one local map, and that turned out to be the reason
// three quarters of the world was too steep to walk on. A cell holds heights of
// continental scale - a range is a few cells - and at a hundred and eighty
// metres a neighbouring cell two hundred metres higher is a slope of one in
// one: a wall. Widened, the same range is a range, with sides a body can climb
// and valleys between.
//
// A local map is no longer a whole cell, then; it is a piece of one, and which
// piece is the local generator's business (D116).
inline constexpr std::int32_t kMetresPerCell = 540;

// What one step of WorldCell::elevation is worth, in metres.
//
// Nine. It was ten against a cell of a hundred and eighty metres, which made
// three quarters of the land too steep to walk on; six against a cell of five
// hundred and forty was gentle enough to lose the mountains. The two numbers
// set how steep the country is and have to be chosen together - this pair puts
// the highest ground at two and a quarter kilometres and leaves ranges that are
// ranges.
//
// It also has to be the only place the conversion lives. The ground was being
// built at ten metres a step while the rivers were given their water level at
// six, so every river in the world ran forty per cent below the valley it had
// cut - which looks like a generator that is bad at rivers rather than like two
// numbers that disagree.
inline constexpr std::int32_t kMetresPerElevationStep = 9;

// The size the generator's noise scales were tuned against. A bigger world is
// not the same world enlarged: the continental octaves grow with the map so the
// land keeps its shape, the fine ones stay put so a wider world holds more
// coastline, more ranges and more rivers rather than the same ones drawn fatter.
inline constexpr std::int32_t kReferenceWidth = 320;

// The sizes a world is offered in.
//
// A cell is five hundred and forty metres, so these are thirty-five, fifty-two,
// sixty-nine and a hundred and four kilometres across - twelve hundred to eleven
// thousand square kilometres. The largest of them is still a hundred and thirty
// times the map of a driving game, and the smallest is a country a person can
// walk across in a day: at this scale a neighbour is somewhere you go, not
// somewhere you hear about.
//
// It used to be two thousand cells - eleven hundred kilometres and one and a
// quarter million square kilometres, which is Egypt. Nothing was wrong with the
// world it generated; the wrong thing was asking anybody to live in it.
struct WorldSize {
    const char* name;
    const char* label;
    std::int32_t cells;
};
// A cell is five hundred and forty metres, so these run from thirty-two
// kilometres across to two thousand and forty-eight - each one twice the side,
// and four times the area, of the one before it.
//
// They no longer cost the square of the side to hold. The foundation used to be
// a sixty-four metre grid whatever the world was, which put a wall in front of
// anything past four hundred kilometres and made the presets an argument about
// memory rather than about what a world should be. Its step is chosen against a
// fixed cell budget now, so the biggest worlds are held at a coarser authority
// instead of not being held at all - see foundationStepFor. What a large world
// costs is TIME, which is the honest cost: the passes are linear in the cells
// and the cells are capped, but the macro map above them is not.
//
// Tiny is the one to test with. It is a third of a degree of latitude - small
// enough to raise in a second and large enough to have a coast, a range and a
// river system in it.
inline constexpr WorldSize kWorldSizes[] = {
        {"tiny", "Tiny", 60},          //    32 km
        {"smaller", "Smaller", 120},   //    65 km
        {"small", "Small", 238},       //   129 km
        {"average", "Average", 474},   //   256 km
        {"medium", "Medium", 948},     //   512 km
        {"large", "Large", 1896},      //  1024 km
        {"giant", "Giant", 3794},      //  2049 km
};
inline constexpr std::size_t kWorldSizeCount = sizeof(kWorldSizes) / sizeof(kWorldSizes[0]);
// What the PLAYER's new world starts at: the middle preset, two hundred
// kilometres across.
inline constexpr std::int32_t kDefaultPlayerWorldCells = kWorldSizes[2].cells;

// And what a WorldMapParams is when nobody says - which is a different number
// on purpose, and was not once, at a cost worth recording. The presets went up
// four-fold; this went up with them; and every test and tool that builds a
// params and does not set a size started raising a two-hundred-kilometre world
// with a ten-million-cell H64 foundation under it. A suite that ran in a minute
// took over half an hour. Nothing that does not ask for a world wants one that
// size.
inline constexpr std::int32_t kDefaultWorldCells = 96;

// The recipes a region can be built to. Not biomes - those are climate, and
// they are decided far below this. These are decided BEFORE the ground, and
// they are what the ground is made of.
enum class Province : std::int32_t {
    Crescent = 0,    // two rivers and a wide flat alluvial plain between them
    NewWorld = 1,    // one big land mass with a cordillera along it
    Archipelago = 2, // more sea than land, and the land in small pieces
    Highlands = 3,   // broken upland, which is what the generator made throughout
};
inline constexpr std::int32_t kProvinceCount = 4;

struct WorldMapParams {
    std::uint64_t seed = 1;
    // How many cells across, which is the size of the world - see kWorldSizes.
    // Not a free number in the interface, but the generator takes any of them:
    // the tools and the tests use sizes of their own.
    std::int32_t width = kDefaultWorldCells;
    std::int32_t height = kDefaultWorldCells;
    // How much of the map is under water, as a percentage of cells. Ocean crust
    // covers most of a world - it is seventy-one percent on Earth - and at
    // forty-two the low ground between two continents stayed dry and the whole
    // map fused into one land mass with lakes in it.
    std::int32_t seaPercent = 71;
    // How many peoples have settled it, the player's community included. Zero
    // means "work it out from the area". Few and far apart: every one of them is
    // a founding community of twenty-four that is meant to be simulated, so this
    // is a number of neighbours, not a population density. A thousand villages
    // was a map of a country nobody could live in.
    std::int32_t sites = 0;
    // Nearest two settlements may be, in cells. A settlement is a cell, and a
    // cell is a local map, so this is real distance: eighty cells is fourteen
    // kilometres, half a day's walk, which is as close as two independent
    // communities of this age can be without being one.
    std::int32_t siteSpacing = 80;
    // How much collected rain makes a river. Zero means "work it out from the
    // size of the map": catchments grow with area, so a threshold tuned on a
    // small world draws every gully on a large one.
    std::int32_t riverFlow = 0;
    // How many rigid pieces the crust is broken into. Zero works it out from the
    // area, which is what the generator did before this was a dial: few plates
    // make big continents with long ranges, many make a broken world of islands
    // and shatter zones.
    std::int32_t plates = 0;
    // How long the weather has had at the rock, in passes of thermal erosion.
    // Each pass takes ground steeper than loose material can stand at and sheds
    // it downhill, so more passes mean older country: rounded shoulders, aprons
    // of scree, valleys with floors. None at all leaves the raw tectonic edges.
    std::int32_t erosionPasses = 3;
    // How wet the world is, as a percentage of the default. Below a hundred the
    // rain shadows widen and the deserts spread; above it the forests close in.
    std::int32_t rainfallPercent = 100;
    bool hybridTerrain = true;
    // Empty resolves the packaged/development library. An explicit missing
    // directory deliberately uses analytic fallback, useful for headless runs.
    std::filesystem::path terrainReferenceRoot;
    // Save/replay input: reuse the baked forms before rebuilding climate and
    // drainage. Never access external DEMs while loading an existing world.
    std::shared_ptr<const HybridTerrain> hybridSnapshot;
    bool stagedTerrain = true;
    std::shared_ptr<const TerrainFoundation> foundationSnapshot;
    // Which country the played cell is: the local generator has presets for it.
    Biome playedBiome = Biome::RiverValley;
};

// A named set of those parameters, loaded from content. The generator does not
// know about presets; this is what the player picks from before touching the
// dials themselves.
struct WorldPreset {
    std::string name;
    std::string label;
    std::string note;
    WorldMapParams params;
};

// How far apart generateWorldMap keeps two settlements, in cells: what the
// parameter asks for, or a quarter of the map when the map is too small to
// grant it.
std::int32_t siteSpacingFor(const WorldMapParams& params);
// How many settlements generateWorldMap places when params.sites is zero: as
// many as the land holds at that spacing. Each is a community of
// kFoundingCommunity that has to be simulated, so this is deliberately a small
// number.
std::int32_t siteCountFor(const WorldMapParams& params, std::int64_t landCells);

WorldMapData generateWorldMap(const WorldMapParams& params);

bool saveWorldMapData(const WorldMapData& world, const std::filesystem::path& file);
bool loadWorldMapData(const std::filesystem::path& file, WorldMapData& out);

// The named worlds a player picks from, read from content/config/world_presets.json.
// A missing or broken file gives back one preset - the generator's own defaults -
// rather than nothing: a game that cannot be started is worse than a game with
// one choice in it.
std::vector<WorldPreset> loadWorldPresets(const std::filesystem::path& file);

} // namespace generation
