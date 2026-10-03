#pragma once
// Terrain categories and the biomes of the control layers
// (doc/plan_ground_types_2026-10-01.md).
//
// The `ground` control map says only WHAT the ground is: an id, a terrain
// category. Everything that makes one category look and grow unlike its
// neighbour is chosen inside it, from named libraries kept as content
// (content/config/terrain/*.json):
//
//   layers.json          the ground texture array, in its order: what a soil
//                        or a rock names when it names a texture
//   noises.json          kinds of noise: fbm, cellular, streaks, ridged
//   soils.json           a soil: up to three layers and the noise that mixes them
//   rocks.json           rock on slopes: the face layer, strata, scree at the foot
//   foliage.json         ground cover: which grass cards, how high, how dry
//   plants.json          trees and shrubs: the model, its size and colour
//   props.json           decoration: logs, stumps, branches, stones, bones
//   decals.json          decal sets: speckle, stain, streak (shader) or instance
//   categories.json      id -> choices from the libraries, with inheritance
//   forest_biomes.json   the forest layer's ids: trees, shrubs, undergrowth, floor
//   water_biomes.json    the water layer's ids: colour, turbidity, scum, glow
//   decor_biomes.json    the decor layer's ids: props and decals away from forest
//
// Id 0 of every set is "as it is today": the default category draws the
// ground exactly as the renderer did before categories, and a layer biome 0
// means "what the ground category says" (its `defaults`). A category that
// changes nothing it inherits from the default renders the same bytes.
//
// The material classes (grass, dirt, sand, rock, marsh, snow) stay physics -
// WHICH surface is here by slope, wetness and height. A category decides
// what soil, what rock and what grass stand behind each class.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace engine::biomes {

// The six material classes, in the order the terrain blends them.
inline constexpr std::size_t kClasses = 6;
inline constexpr const char* kClassNames[kClasses] = {"grass", "dirt", "sand", "rock", "marsh", "snow"};

// The four categorical control layers, in the order a sample keeps them, and
// their names in a world source (the water's is water_type: "water" is the
// package's lake flags).
enum class Layer : std::uint8_t { Ground, Forest, Water, Decor };
inline constexpr std::size_t kLayers = 4;
inline constexpr const char* kLayerNames[kLayers] = {"ground", "forest", "water_type", "decor"};

using Rgb = std::array<double, 3>;
// A named thing and how much of it: a share (sums to one), a weight, or a
// count a hectare, as the list says.
using Weighted = std::vector<std::pair<std::string, double>>;

// The engine's own sixteen ground layers, in the shader's order: the default
// category draws with these, and layers.json must begin with them.
inline constexpr const char* kBuiltInLayers[16] = {
        "leafy_grass", "dirt_floor", "red_sand", "rocks_ground_05", "brown_mud_02", "snow_02",
        "withered_grass", "forest_leaves_02", "coast_sand_04", "damp_beach_sand", "sand_01",
        "sandy_gravel_02", "rock_face_03", "mossy_rock", "cliff_side", "mud_cracked_dry_riverbed_002"};

struct TextureLayer {
    std::string name;      // "moon_dusted_04": assets/terrain/ph/<name>/ph_<name>
    double metres = 2.0;   // ground one turn of the texture covers
    std::string role;
    std::string path;      // optional packed stem relative to assets; empty: legacy ph/<name>
};

enum class NoiseKind : std::uint8_t { Fbm, Cellular, Streaks, Ridged, Patch };
inline constexpr std::size_t kNoiseKinds = 5;
inline constexpr const char* kNoiseKindNames[kNoiseKinds] = {"fbm", "cellular", "streaks", "ridged", "patch"};

struct Noise {
    std::string name;                // empty when written inline
    NoiseKind kind = NoiseKind::Fbm;
    double metres = 8.0;             // feature size
    double contrast = 0.5;           // 0 soft .. 1 hard edges
    double warp = 0.0;               // domain warp, in feature sizes
    double angle = 0.0;              // degrees: streaks run along it
};

struct Soil {
    std::string name;
    Weighted layers;                 // texture layers and their shares, one to three
    Noise noise;                     // what mixes them
    Rgb tint{1, 1, 1};
};

struct Rock {
    std::string name;
    std::string layer;               // the face's texture layer
    double strataMetres = 0.0;       // bands up the face; nought: none
    double strataTilt = 0.0;         // how far the bands lean, per metre across
    std::string scree;               // a soil at the foot, or nothing
    Rgb tint{1, 1, 1};
};

struct Foliage {
    std::string name;
    double density = 1.0;            // times the climate's grass
    double height = 1.0;             // times the card's own
    double dryness = 0.0;            // 0 as the climate says, 1 parched
    Rgb tint{1, 1, 1};
    double flowers = 1.0;            // times the meadow's flowers
};

struct Plant {
    std::string name;
    std::string model;               // a scene model: CommonTree_1, Pine_1, Bush_Common...
    double heightMin = 1.0, heightMax = 1.0;   // times the model's own
    Rgb tint{1, 1, 1};
};

struct Prop {
    std::string name;
    std::string model;
    double scaleMin = 1.0, scaleMax = 1.0;
};

enum class DecalKind : std::uint8_t { Speckle, Stain, Streak, Instance, Trail, Texture };
inline constexpr std::size_t kDecalKinds = 6;
inline constexpr const char* kDecalKindNames[kDecalKinds] = {"speckle", "stain", "streak", "instance", "trail", "texture"};

struct Decal {
    std::string name;
    DecalKind kind = DecalKind::Speckle;
    double cellMetres = 2.0;         // one candidate a cell
    double density = 0.5;            // share of cells that hold one
    std::array<double, 2> sizeMetres{0.2, 0.5};
    double clusterMetres = 0.0;      // clustered into patches this big; nought: even
    double clusterShare = 1.0;       // share of the ground the patches cover
    Rgb colour{0.5, 0.5, 0.5};
    double opacity = 1.0;
    double emissive = 0.0, metal = 0.0, rough = 0.8;
    std::array<double, 2> slope{0.0, 1.0};   // steepness it lies on
    double underFoliage = 1.0;       // how much of it shows where grass grows
    std::array<double, 2> fadeMetres{0.0, 4000.0};   // seen from .. to
    double edgeNoise = 0.3;          // stains: how torn the edge is
    double nearWaterMetres = 0.0;    // only this far above the water; nought: anywhere
    bool alongWind = false;          // streaks: along the wind, else along `angle`
    double angle = 0.0;
    std::string model;               // instances only
    std::string texture;             // texture decals: named terrain layer, opacity in properties.a
};

struct WaterBiome {
    std::string name;
    std::uint32_t id = 0;
    std::optional<Rgb> colour;       // deep colour; unset: the water's own
    double turbidity = -1.0;         // 0 clear .. 1 opaque; negative: the water's own
    double scum = 0.0;               // film on the surface
    double foam = 1.0;               // times the water's own foam
    double emissive = 0.0;           // glow of the colour
};

struct Curve {
    std::string by;                  // a control channel: "forest_bias"; empty: none
    std::vector<std::pair<double, double>> points;
    [[nodiscard]] double at(double x) const;
};

struct ForestBiome {
    std::string name;
    std::uint32_t id = 0;
    Weighted trees;                  // plants, shares
    Weighted shrubs;                 // plants, shares
    double shrubDensity = 0.3;
    std::string undergrowth;         // foliage under the canopy; "none"; empty: as the ground
    std::string floorSoil;           // the forest floor; empty: the engine's
    std::optional<Noise> floorNoise;
    Weighted props;                  // a hectare
    double fertility = 1.0;          // how much forest the ground gives at forest_bias 0.5
    Curve density;
    std::optional<Noise> clearings;
    double clearingShare = 0.0;
    double edgeShrubs = 1.0, edgeMetres = 0.0;
};

struct DecorBiome {
    std::string name;
    std::uint32_t id = 0;
    Weighted props;                  // a hectare
    Weighted decals;                 // weights: times the decal's own density
};

// Values a control channel takes where the map does not say.
struct Controls {
    std::optional<double> moisture, forest, mountain, erosion;
};

struct Category {
    std::string name;
    std::uint32_t id = 0;
    std::string inherit;             // as written; resolved categories carry it for show
    // Per material class: a soil, or empty for the engine's own ground.
    std::array<std::string, kClasses> soils{};
    std::string slopeRock;           // rock on slopes; empty: the engine's cliffs
    std::string scree;               // soil at the foot of slopes; empty: none
    double steepFrom = 0.45;         // steepness the face starts at
    std::string groundFoliage;       // foliage, "none", or empty: the engine's grass
    Weighted decals;                 // decal sets and their density weights
    std::string forest, water, decor;   // layer biomes where the layer says 0
    Rgb tint{1, 1, 1};
    double saturation = 1.0;
    Controls controls;
    // The climate the ground stands under until a world works its own out
    // (an import's primary stage): a zone (steppe, taiga, temperate_forest,
    // tropical_forest, mediterranean, savanna, tundra, alpine, desert, ice),
    // how warm (0 polar .. 1 equatorial) and how fertile. Empty zone: none said.
    std::string climateZone;
    std::optional<double> warmth, fertility;
    // Does it draw the ground any differently from the default?
    [[nodiscard]] bool changesGround() const;
};

// A rule that names a category for ground the map does not: generated worlds,
// and imported ground outside the painted ids.
struct DeriveRule {
    std::string category;
    // Each condition is left out by its default, which everything passes.
    double desertAbove = -1.0;       // desert cover above this
    double moistureAbove = -1.0;     // moisture rank above this
    double moistureBelow = 2.0;      // moisture rank below this
    double temperatureBelow = 2.0;   // the climate's 0..1 temperature
    double temperatureAbove = -1.0;
    [[nodiscard]] bool unconditional() const {
        return desertAbove < 0 && moistureAbove < 0 && moistureBelow > 1 && temperatureBelow > 1 && temperatureAbove < 0;
    }
    [[nodiscard]] bool passes(double desert, double moisture, double temperature) const {
        return desert > desertAbove && moisture > moistureAbove && moisture < moistureBelow &&
               temperature < temperatureBelow && temperature > temperatureAbove;
    }
};

struct Problem {
    std::string file;
    std::string what;
};

class Registry {
public:
    // Reads every file of `dir`. Problems the files have as JSON, or as
    // shapes, are in `problems`; a registry is returned whenever the files
    // could be read at all, so the validator can report everything at once.
    static std::shared_ptr<Registry> load(const std::filesystem::path& dir, std::vector<Problem>* problems = nullptr);
    // Only id 0 of everything: the renderer as it was before categories.
    static std::shared_ptr<Registry> builtIn();

    // References resolve, ids are unique and in range, shares sum to one, no
    // inheritance cycles, layers exist in the texture catalogue (and on disk,
    // when `assets` is given: assets/terrain/ph/<layer>).
    [[nodiscard]] std::vector<Problem> validate(const std::filesystem::path& assets = {}) const;

    [[nodiscard]] const std::vector<TextureLayer>& textureLayers() const { return layers_; }
    [[nodiscard]] std::optional<int> textureLayer(const std::string& name) const;

    // Resolved: inheritance applied. Ordered by id.
    [[nodiscard]] const std::vector<Category>& categories() const { return categories_; }
    [[nodiscard]] const Category* category(std::uint32_t id) const;
    [[nodiscard]] const Category* category(const std::string& name) const;
    [[nodiscard]] const std::vector<ForestBiome>& forestBiomes() const { return forests_; }
    [[nodiscard]] const ForestBiome* forestBiome(std::uint32_t id) const;
    [[nodiscard]] const std::vector<WaterBiome>& waterBiomes() const { return waters_; }
    [[nodiscard]] const WaterBiome* waterBiome(std::uint32_t id) const;
    [[nodiscard]] const std::vector<DecorBiome>& decorBiomes() const { return decors_; }
    [[nodiscard]] const DecorBiome* decorBiome(std::uint32_t id) const;
    [[nodiscard]] const std::vector<DeriveRule>& deriveRules() const { return rules_; }

    [[nodiscard]] const std::vector<Noise>& noises() const { return noises_; }
    [[nodiscard]] const std::vector<Soil>& soils() const { return soils_; }
    [[nodiscard]] const std::vector<Rock>& rocks() const { return rocks_; }
    [[nodiscard]] const std::vector<Foliage>& foliage() const { return foliage_; }
    [[nodiscard]] const std::vector<Plant>& plants() const { return plants_; }
    [[nodiscard]] const std::vector<Prop>& props() const { return props_; }
    [[nodiscard]] const std::vector<Decal>& decals() const { return decals_; }
    [[nodiscard]] std::optional<std::size_t> soilIndex(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> rockIndex(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> foliageIndex(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> plantIndex(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> propIndex(const std::string& name) const;
    [[nodiscard]] std::optional<std::size_t> decalIndex(const std::string& name) const;
    [[nodiscard]] std::optional<Noise> noiseNamed(const std::string& name) const;

    // A layer's legend by name, what an import renumbers a package onto
    // (engine/world_source ImportTarget::legends).
    [[nodiscard]] std::map<std::string, std::uint32_t> legend(Layer layer) const;
    // The category's biome for a layer, its id there: the sample's own when
    // it is not 0, else the category's default (0 when it has none).
    [[nodiscard]] std::uint32_t resolve(Layer layer, std::uint32_t categoryId, std::uint32_t layerId) const;

    // The value a control channel takes on a category, when it says one.
    [[nodiscard]] std::optional<double> control(std::uint32_t categoryId, const std::string& channel) const;

    // When the files were last written, the newest of them.
    [[nodiscard]] std::filesystem::file_time_type newest() const { return newest_; }
    [[nodiscard]] const std::filesystem::path& directory() const { return dir_; }

private:
    friend struct RegistryReader;
    std::filesystem::path dir_;
    std::filesystem::file_time_type newest_{};
    std::vector<TextureLayer> layers_;
    std::vector<Noise> noises_;
    std::vector<Soil> soils_;
    std::vector<Rock> rocks_;
    std::vector<Foliage> foliage_;
    std::vector<Plant> plants_;
    std::vector<Prop> props_;
    std::vector<Decal> decals_;
    std::vector<Category> categories_;
    std::vector<ForestBiome> forests_;
    std::vector<WaterBiome> waters_;
    std::vector<DecorBiome> decors_;
    std::vector<DeriveRule> rules_;
    // What the files said before it was resolved: inheritance, for the validator.
    std::map<std::string, std::string> inherits_;   // category -> parent
    std::vector<Problem> resolveProblems_;
};

// The registry the running process draws with: loaded at start, swapped on a
// live reload. Null: nothing loaded, everything id 0.
std::shared_ptr<const Registry> active();
void setActive(std::shared_ptr<const Registry> registry);
// Moves on every setActive: whatever was placed from the last registry (the
// scene scatter) is stale when this is not the number it was placed under.
std::uint64_t activeGeneration();

// content/config/terrain beside the working directory, found walking up.
std::filesystem::path defaultDirectory();

} // namespace engine::biomes
