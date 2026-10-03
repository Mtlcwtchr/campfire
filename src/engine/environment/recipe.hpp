#pragma once
// Feature recipes (doc/plan_procedural_environment_2026-10-03.md, part C).
//
// A recipe is not a prefabricated piece of level. It generates a recognisable
// kind of place - a dry gully, a cliff with its talus, a fallen giant, a rock
// outcrop - wherever the ground asks for one: it may move the ground, add
// meshes, write material and ecology masks, push the foliage back or bring it
// in, and set down the props that make the place read.
//
//   FeatureRecipe { placement, terrain, meshes, materials, scatter, composition }
//
// The engine owns this schema, its parser and validator, and every operation
// a recipe can name. The game owns the recipes: JSON under
// content/config/environment/recipes, one file per recipe or a list.
//
// Names the engine cannot resolve by itself - zones, models, mask channels -
// stay names here and are resolved against a Catalogue (catalogue.hpp).
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/environment/fields.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

enum class FeatureScale : std::uint8_t { Micro, Local, Macro };
inline constexpr const char* kScaleNames[] = {"micro", "local", "macro"};
// Metres between planning cells of each scale.
inline constexpr double kPlanningCell[] = {128.0, 512.0, 8192.0};

// How long the feature has stood, which decides how far the water and the
// growth around it have adapted (spec §10).
enum class FeatureAge : std::uint8_t { Geological, Ancient, Recent, Cataclysmic };
inline constexpr const char* kAgeNames[] = {"geological", "ancient", "recent", "cataclysmic"};

// Where candidates come from.
enum class PlacementSource : std::uint8_t {
    Point,       // jittered points over the planning cell
    Channel,     // along the historical drainage (drainage.hpp)
    Ridge,       // along crests (large TPI, convex)
    CliffFoot,   // at the foot of steep ground
    Edge,        // on the border between two zones
};
inline constexpr const char* kSourceNames[] = {"point", "channel", "ridge", "cliff_foot", "edge"};

// What a feature lines itself up with.
enum class Align : std::uint8_t { None, Slope, Contour, Flow, Wind, Random };
inline constexpr const char* kAlignNames[] = {"none", "slope", "contour", "flow", "wind", "random"};

enum class ChannelClass : std::uint8_t {
    PermanentRiver, SeasonalStream, EphemeralChannel, AbandonedChannel, Paleochannel
};
inline constexpr std::size_t kChannelClasses = 5;
inline constexpr const char* kChannelClassNames[] = {
        "permanent_river", "seasonal_stream", "ephemeral_channel", "abandoned_channel", "paleochannel"};

struct FieldRange {
    FieldId field = 0;
    std::string name;
    float min = -1e30f, max = 1e30f;
};

struct Placement {
    std::vector<std::string> zones;          // any of these, by name; empty: anywhere
    std::vector<ZoneTypeId> zoneIds;         // resolved
    float minZoneWeight = 0.35f;
    std::vector<std::string> excludeZones;
    std::vector<ZoneTypeId> excludeZoneIds;
    std::vector<FieldRange> fields;          // every one must hold
    PlacementSource source = PlacementSource::Point;
    std::vector<ChannelClass> channelClasses;// for Channel; empty: any
    double densityPerKm2 = 1.0;              // candidates a square kilometre before the rules
    double minSpacing = 64.0;                // to another instance of this recipe, metres
    double clearance = 0.0;                  // to an instance of any recipe, metres
    double chance = 1.0;                     // of keeping a candidate that passed
    double lengthMin = 80.0, lengthMax = 400.0;   // spline sources
    double scaleMin = 1.0, scaleMax = 1.0;
    Align align = Align::None;
    bool avoidWater = true;
};

enum class TerrainOpKind : std::uint8_t { CarveProfile, Step, Raise, Depress, Terrace, SmoothTo };
inline constexpr const char* kTerrainOpNames[] = {"carve_profile", "step", "raise", "depress", "terrace", "smooth_to"};

// Every number in metres unless said, and scaled by the instance's scale.
struct TerrainOp {
    TerrainOpKind kind = TerrainOpKind::Raise;
    // carve_profile: three nested scales of one cross-section (spec §4).
    double outerWidth = 40, outerDepth = 6;     // the valley
    double innerWidth = 8, innerDepth = 2;      // the erosion channel
    double waterWidth = 0;                      // today's water, 0: dry
    double asymmetry = 0.35;                    // 0 symmetric, 1 one bank twice as steep
    double taper = 30;                          // fade in/out along the ends
    // step
    double height = 12, width = 10;             // rise, horizontal run of the face
    double breaks = 0.3;                        // 0..1 how broken the line is along its length
    double amphitheatre = 0;                    // metres the face bites back in bays
    // raise / depress / terrace / smooth_to
    double radiusA = 20, radiusB = 14;          // ellipse half-axes
    double exponent = 2;                        // profile: 1 cone, 2 dome, high flat-topped
    double edgeNoise = 0.2;                     // 0..1 of the radius
    bool harden = false;                        // raise: write the rock mask
    double terraceStep = 3, terraceSharpness = 0.7;
    double blend = 8;                           // edge falloff
};

enum class MaskShape : std::uint8_t { Footprint, Bed, Bank, Edge, Fan, Ring, Corridor };
inline constexpr const char* kMaskShapeNames[] = {"footprint", "bed", "bank", "edge", "fan", "ring", "corridor"};
enum class MaskMode : std::uint8_t { Max, Add, Min, Replace };
inline constexpr const char* kMaskModeNames[] = {"max", "add", "min", "replace"};

struct MaskWrite {
    std::string channel;          // a mask channel by name (catalogue)
    std::uint8_t channelId = 0;   // resolved
    MaskShape shape = MaskShape::Footprint;
    MaskMode mode = MaskMode::Max;
    float value = 1.0f;
    double radius = 0;            // extra reach beyond the shape, metres
    double falloff = 6;           // metres to fade to nothing
    double length = 30;           // fan / corridor reach
    double spread = 0.6;          // fan half-angle, radians
};

// The supporting set pieces (spec §3 "supporting decor", §7 rocks).
enum class ScatterPrimitive : std::uint8_t { Cluster, Fan, AlongChannel, Ring, Edge, Line, RockHierarchy };
inline constexpr const char* kScatterPrimitiveNames[] = {
        "cluster", "fan", "along_channel", "ring", "edge", "line", "rock_hierarchy"};

struct ModelChoice {
    std::string name;
    double weight = 1;
    std::uint32_t id = 0;         // resolved by the catalogue
};

struct ScatterRule {
    ScatterPrimitive primitive = ScatterPrimitive::Cluster;
    std::vector<ModelChoice> models;
    int countMin = 3, countMax = 8;
    double radius = 12;           // cluster/ring radius, line/fan length
    double spread = 0.6;          // fan half-angle; line width
    double scaleMin = 0.8, scaleMax = 1.2;
    double sizeFalloff = 0.5;     // fan/channel: how much smaller far from the source
    double sink = 0.1;            // share of height sunk into the ground
    bool alignToGround = false;
    double minSpacing = 1.5;
    std::string tag;              // what the game files the objects under
};

enum class MeshAttach : std::uint8_t { Anchor, Spline, Edge };
inline constexpr const char* kMeshAttachNames[] = {"anchor", "spline", "edge"};
enum class ProceduralForm : std::uint8_t { None, Shelf, Overhang, Undercut, RootPlate, Spire, Arch };
inline constexpr const char* kProceduralFormNames[] = {"none", "shelf", "overhang", "undercut", "root_plate", "spire", "arch"};

struct MeshRule {
    ModelChoice model;                // a catalogue model, or
    ProceduralForm form = ProceduralForm::None;   // a form made here (feature_mesh.hpp)
    MeshAttach attach = MeshAttach::Anchor;
    int count = 1;
    double spacing = 20;              // along a spline
    double offset = 0;                // to the side of the spline / anchor
    double sink = 0.15;
    double scaleMin = 1, scaleMax = 1;
    bool alignToNormal = false;
    // Form size: length along, depth into, height of.
    double length = 12, depth = 3, height = 4;
    double roughness = 0.3;
};

struct Composition {
    double negativeSpace = 0;         // metres around the anchor other recipes' scatter keeps out of
    double elongation = 1;            // >1: clusters stretched along the alignment
    double revealWidth = 0, revealLength = 0;   // a corridor kept clear of tall things
    int secondaries = 0;              // extra instances of the same recipe round the anchor
    double secondaryRadius = 40;
    double secondaryScale = 0.6;
};

struct FeatureRecipe {
    std::string name;
    std::string file;
    FeatureScale scale = FeatureScale::Local;
    FeatureAge age = FeatureAge::Ancient;
    Placement placement;
    std::vector<TerrainOp> terrain;
    std::vector<MeshRule> meshes;
    std::vector<MaskWrite> masks;
    std::vector<ScatterRule> scatter;
    Composition composition;
    // The furthest any part of an instance reaches from its anchor or spline,
    // metres at scale 1: the halo neighbours are asked through.
    [[nodiscard]] double reach() const;
};

struct RecipeProblem {
    std::string file;
    std::string what;
};

// Reads every *.json under `dir` (recursively). A file is one recipe object,
// a list of them, or {"recipes": [...]}. Problems with a file's JSON or shape
// are reported and the file is skipped; a recipe that parses is returned even
// if it does not validate, so a tool can show what is wrong with it.
std::vector<FeatureRecipe> loadRecipes(const std::filesystem::path& dir, std::vector<RecipeProblem>* problems);

// Recipe <-> JSON, for tools and round-trip tests.
std::optional<FeatureRecipe> parseRecipe(const std::string& json, const std::string& file,
                                         std::vector<RecipeProblem>* problems);
std::string recipeToJson(const FeatureRecipe& recipe);

} // namespace engine::environment
