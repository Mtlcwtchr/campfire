#pragma once
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/cell_field.hpp"
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
struct WorldLayout;
struct ImportedGround;
}
namespace engine::biomes {
class CategoryField;
class DetailEdits;
}
namespace generation {

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
    // The categorical control layers an import brought (engine/biomes): the
    // ground's terrain category and the forest, water and decor biomes at
    // 256 m, land only. Null: none painted - the climate names what it can
    // by the registry's derive rules, the rest is the default category.
    std::shared_ptr<const engine::biomes::CategoryField> categories;
    // The hand edits to the details they place (source/details): kept apart
    // from the maps, so repainting an id re-derives the rest.
    std::shared_ptr<const engine::biomes::DetailEdits> details;
    TerrainStage terrainStage = TerrainStage::Final;
    CellField<std::int32_t> primaryHeightField;
    // PASS G1 — continental mask.
    CellField<std::int32_t> continentalField;
    CellField<std::int32_t> distanceToCoast;
    CellField<std::uint8_t> initialLandMask;
    // PASS G2 — pseudo-tectonics.
    CellField<std::int32_t> upliftField;
    CellField<std::int32_t> riftField;
    CellField<std::int32_t> faultField;
    CellField<std::uint8_t> geologyRegion;
    // PASS G3 — base macro height.
    CellField<std::int32_t> macroHeightField;
    // PASS G4 — geology.
    CellField<RockType> rockTypeField;
    CellField<std::int32_t> erosionResistanceField;
    CellField<std::uint8_t> soilParentMaterialField;
    CellField<std::int32_t> permeabilityField;
    // PASS G5 — thermal erosion.
    CellField<std::int32_t> thermallyRelaxedHeightField;
    // PASS G6 — hydraulic / fluvial erosion.
    CellField<std::int32_t> hydrologicallyCorrectedHeightField;
    CellField<std::int32_t> basinIdField;
    CellField<ClimateVector> spillPointField;
    CellField<std::int8_t> flowDirectionField;
    CellField<std::int32_t> flowAccumulationField;
    CellField<std::int32_t> riverDischargeField;
    CellField<std::uint8_t> riverSourceField;
    CellField<std::int32_t> lakeRegionField;
    CellField<std::int32_t> lakeLevelField;
    // Difference between the priority-flood lake surface and the original
    // basin floor, in elevation steps. Zero means this cell is not lake bed.
    CellField<std::int32_t> lakeDepthField;

    // Which recipe the country here was built to. A world used to be one
    // recipe everywhere, which is why every world came out the same world at a
    // different seed: the same share of sea, the same ruggedness, the same
    // grain of coastline from edge to edge. These are regions of it - a flood
    // plain between two rivers, a young continent with a cordillera down one
    // side, a scatter of islands - and the map says which is which so the
    // passes below can agree with the shape above them.
    CellField<std::int32_t> provinceField;

    // How many basins came out holding water and how many came out dry. Worth
    // counting rather than guessing: the number of lakes is the loudest thing
    // about a landscape from above, and the pass that decides it is the same
    // pass that fills hollows, which must fill every one of them.
    std::int32_t lakesKept = 0;
    std::int32_t basinsDried = 0;
    // What was painted on the water layer at a cell (WaterPaint): the ground
    // it shaped and the water it forced or kept out. Nought where nothing was
    // painted - which is everywhere in a world nobody painted water on.
    CellField<std::uint8_t> waterPaintField;
    // River abundance (AuthoringDials::rivers): the drainage graph uses it to
    // select main catchments and the length/flow needed by attached branches.
    float riverShare = 1.0f;
    // Minimum flow (log2) of an independent river's catchment; zero uses the
    // graph's basin selection rule. Attached tributaries may be smaller.
    // Composed continents set this in world_compose.cpp.
    std::uint8_t graphRiverFlow = 0;
    CellField<std::uint8_t> waterfallField;
    CellField<std::int32_t> sedimentPotentialField;
    CellField<std::int32_t> erosionField;
    CellField<std::int32_t> floodplainPotentialField;
    CellField<std::int32_t> deltaPotentialField;
    CellField<std::int32_t> erodedHeightField;
    // PASS C0-C5 — climate generation.
    CellField<std::int32_t> baseTemperatureField;
    CellField<ClimateVector> prevailingWindField;
    CellField<std::int32_t> windStrengthField;
    CellField<std::int32_t> windVariabilityField;
    CellField<std::int32_t> precipitationField;
    CellField<std::int32_t> airMoistureField;
    CellField<std::int32_t> rainShadowField;
    CellField<ClimateVector> oceanCurrentField;
    CellField<std::int32_t> seaSurfaceTemperatureBiasField;
    CellField<std::int32_t> coastalClimateBiasField;
    CellField<std::int32_t> temperatureField;
    CellField<std::int32_t> annualRainfallField;
    CellField<std::int32_t> humidityField;
    CellField<std::int32_t> seasonalityField;
    CellField<std::int32_t> winterRainField;
    CellField<std::int32_t> summerRainField;
    CellField<std::int32_t> drySeasonStrengthField;
    // PASS B0 — soil.
    CellField<SoilType> soilTypeField;
    CellField<std::int32_t> soilFertilityField;
    CellField<std::int32_t> soilDrainageField;
    CellField<std::int32_t> soilOrganicPotentialField;
    // PASS B1 — biome suitability.
    CellField<std::uint8_t> primaryBiomeSuitabilityField;
    CellField<std::uint8_t> secondaryBiomeSuitabilityField;
    CellField<std::int32_t> biomeTransitionField;
    // PASS B2 — terrain material suitability.
    CellField<std::array<std::uint8_t, 6>> materialSuitabilityField;
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
//
// Five hundred and twelve now, not five hundred and forty: a power of two, so a
// cell is exactly one 512 m terrain page, a region (world_layout.hpp) is exactly
// 256 cells and 256 pages, and every level of the page quadtree lands on a cell
// line. The slopes it makes are five per cent steeper than at 540, which the
// elevation step below leaves as it is.
inline constexpr std::int32_t kMetresPerCell = 512;

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
// A cell is five hundred and twelve metres, so these run from thirty-two
// kilometres across to two thousand and ninety-seven - each one twice the side,
// and four times the area, of the one before it, and every one of them a power
// of two in cells. From "small" up they are whole numbers of regions
// (world_layout.hpp): one, two, four, eight and sixteen a side.
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
        {"tiny", "Tiny", 64},          //    33 km
        {"smaller", "Smaller", 128},   //    66 km
        {"small", "Small", 256},       //   131 km, one region
        {"average", "Average", 512},   //   262 km, 2 x 2 regions
        {"medium", "Medium", 1024},    //   524 km, 4 x 4
        {"large", "Large", 2048},      //  1049 km, 8 x 8
        {"giant", "Giant", 4096},      //  2097 km, 16 x 16
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

// Where a world lies on its planet: the latitude of its top edge and how many
// kilometres of map a degree of latitude takes. A game world two thousand
// kilometres across on a planet of any size is a strip of latitudes, and its
// climate belts are whatever strip it is set to - squeezed (few kilometres to
// the degree) for a world that should cross several of them, true to scale
// (a hundred and eleven) for one that should not.
//
// Not fixed means the old rule, kept for worlds made under it: the world's
// own height is the span of the belts, so a taller world moves them. Fixed,
// a place's latitude is where it is, and a world that grows keeps its
// climate where it was.
struct Latitude {
    bool fixed = false;
    double northDegrees = 50.0;   // at the world's top edge; south is negative
    double kmPerDegree = 6.0;
    // The old rule, frozen as it was for a world of `legacyRows` rows: its
    // strip of temperature (from, span, in the generator's 0..230) and the
    // share of its height the winds were laid out by. Kept word for word, so
    // a world frozen when it first grows makes its old regions bit for bit as
    // before; nought once the degrees above are what is meant.
    std::int32_t legacyFrom = 0, legacySpan = 0, legacyRows = 0;
    // Rows (cells) the world has grown by at its top since this was set: the
    // latitude belongs to the ground, so a row added in the north is further
    // north, and what was there keeps the latitude it had.
    std::int32_t rowOffset = 0;
    bool operator==(const Latitude&) const = default;
    // Degrees at a distance from the world's top edge.
    double at(double metresFromTop) const { return northDegrees - metresFromTop / 1000.0 / kmPerDegree; }
};
// The legacy rule written as a fixed latitude, for a world of this size and
// seed: the strip its climate belts spanned, so a world frozen to it keeps
// its temperatures.
Latitude legacyLatitude(std::int32_t widthCells, std::int32_t heightCells, std::uint64_t seed);

// How far one run of the generator goes: the authoring pipeline's stages
// (world_layout.hpp, RegionStage) as the generator sees them. Nothing past the
// stage is worked out at all.
enum class GenerationStage : std::uint8_t {
    Full,      // everything
    Primary,   // relief, coast and slopes; the climate is a neutral stand-in, no water, no peoples
    Relief,    // everything but the water: the drainage shapes the ground, and is then taken away
};

// How the ground of a world made by hand is worked out from its paint (the
// Coast & relief stage of the editor): the whole world's, since land is
// painted without regard to regions.
// What the water layer says of a cell. Painted, it is an instruction the
// drainage pass keeps: a lake there (the ground is dug to hold one and it
// never dries), a river course (a trough the water finds, and a river in it
// however little collects), or dry ground (no lake and no stream, whatever
// the rain). Unpainted, the generator decides as it always did.
enum class WaterPaint : std::uint8_t { None, Dry, Course, Lake };
inline WaterPaint waterPaintOf(float value, float cover) {
    if (cover < 0.5f) return WaterPaint::None;
    const float kind = value;   // the layer's own value, not premultiplied
    return kind < 250.0f ? WaterPaint::Dry : kind < 750.0f ? WaterPaint::Course : WaterPaint::Lake;
}

struct AuthoringDials {
    float coast = 1.0f;          // how torn the coast is: 0 the brush's own edge, 2 very broken
    float coastKm = 20.0f;       // the size of its bays and headlands; inlets are a fifth of it
    float relief = 1.0f;         // the primary relief noise, as a share of the generator's own
    float minLandMetres = 4.0f;  // painted land never comes out lower than this
    // The drainage (the Water stage): how many of the valleys carry water, and
    // how readily a basin holds a lake, each as a share of the generator's own.
    float rivers = 1.0f;
    float lakes = 1.0f;
    bool operator==(const AuthoringDials&) const = default;
};

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
    // A world built region by region (world_layout.hpp). When set, the size
    // above is the layout's, and each region's seed, sea share, erosion and
    // rainfall - and whether it is generated at all - come from the layout,
    // blended across a band at every region border. The plates, the climate
    // and the rivers stay the world's. The dials above are then the world-wide
    // averages the generator's world-level constants are taken from.
    std::shared_ptr<const WorldLayout> layout;
    // Where on the planet (see Latitude), and where in a bigger world this
    // one is, in cells: a region generated on its own is a world of its own
    // size placed at its region, and its latitude is its place's, not its
    // own top edge's.
    Latitude latitude;
    std::int32_t originX = 0, originY = 0;
    // How far to go, and whether this is a region made by hand under the
    // staged pipeline: then its land is where it was painted, its coast made
    // ragged and natural by noise, and its mountains are the painted ranges -
    // the plates raise nothing of their own over it.
    GenerationStage stage = GenerationStage::Full;
    bool authored = false;
    AuthoringDials authoring;
    // An imported region (world_import.hpp): its ground is this skeleton, in
    // metres from the run's own north-west corner, rather than paint and noise.
    std::shared_ptr<const ImportedGround> imported;
    // The foundation's step when not the budget's (0): a run whose lattice is
    // laid into a coarser composite is built at the composite's step, not at
    // sixty-four metres it would only be sampled down from.
    std::int32_t foundationStep = 0;
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

// How long each pass of generateWorldMap took, in milliseconds, in order -
// for probes and the loading screen's log. Set on the thread that generates;
// null (the default) records nothing and costs nothing.
struct GenerationTimings {
    std::vector<std::pair<std::string, double>> passes;
};
void setGenerationTimings(GenerationTimings* sink);

bool saveWorldMapData(const WorldMapData& world, const std::filesystem::path& file);
bool loadWorldMapData(const std::filesystem::path& file, WorldMapData& out);

// The named worlds a player picks from, read from content/config/world_presets.json.
// A missing or broken file gives back one preset - the generator's own defaults -
// rather than nothing: a game that cannot be started is worse than a game with
// one choice in it.
std::vector<WorldPreset> loadWorldPresets(const std::filesystem::path& file);

} // namespace generation
