#pragma once
// Declarative content definitions (GDD 11: "content is described by declarative
// text data"). Everything here is loaded from content/ at startup and is const
// for the rest of the run. Simulation code refers to content only by DefId.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/ids.hpp"

namespace content {

using core::DefId;
using core::Fixed;

// ---------------------------------------------------------------------------
// Work categories
//
// These are the axis along which the player's zones, permissions and priorities
// filter (GDD 9). They are a closed enum rather than data because the zone UI and
// the planner both need to enumerate them exhaustively.
// ---------------------------------------------------------------------------
enum class WorkCategory : std::uint8_t {
    Hauling,
    Construction,
    Woodcutting,
    Mining,
    Foraging,
    Farming,
    Hunting,
    Fishing,
    Herding,
    Crafting,
    Cooking,
    WaterCarrying,
    Cleaning,
    Medical,
    Childcare,
    Ritual,
    Patrol,
    Scouting,
    Teaching,
    Count
};
inline constexpr std::size_t kWorkCategoryCount = static_cast<std::size_t>(WorkCategory::Count);

std::string_view workCategoryName(WorkCategory c);
bool parseWorkCategory(std::string_view s, WorkCategory& out);

// ---------------------------------------------------------------------------
// Items
// ---------------------------------------------------------------------------
enum class ItemCategory : std::uint8_t {
    Food,
    Liquid,
    Raw,
    Tool,
    Weapon,
    Clothing,
    BuildingMaterial,
    Component,
    Waste,
    Count
};
std::string_view itemCategoryName(ItemCategory c);
bool parseItemCategory(std::string_view s, ItemCategory& out);

// Broad dietary groups (GDD 7: "the exact list requires separate fixing" — this is
// the provisional set recorded in DECISIONS.md D6). Diet quality is derived from
// which groups a pawn has eaten recently, not from vitamins.
enum class FoodGroup : std::uint8_t { Grain, Vegetable, Fruit, Meat, Fish, Dairy, Fat, Count };
inline constexpr std::size_t kFoodGroupCount = static_cast<std::size_t>(FoodGroup::Count);
std::string_view foodGroupName(FoodGroup g);
bool parseFoodGroup(std::string_view s, FoodGroup& out);

// A tool class is what a job asks for ("something that can chop"), not a specific
// item, so a flint axe and a bronze axe both satisfy a woodcutting job.
enum class ToolClass : std::uint8_t { None, Axe, Pick, Knife, Hoe, Hammer, Spear, Bow, Needle, Quern, Vessel, Count };
std::string_view toolClassName(ToolClass c);
bool parseToolClass(std::string_view s, ToolClass& out);

struct ItemDef {
    DefId id;
    std::string name;              // stable content id, e.g. "wood_log"
    std::string label;             // display name
    ItemCategory category = ItemCategory::Raw;

    Fixed massPerUnit = core::kOne;    // kg
    std::int32_t stackLimit = 50;      // how many units live in one physical batch

    // Food
    Fixed nutrition = core::kZero;             // satiety restored per unit
    std::vector<FoodGroup> foodGroups;
    bool edibleRaw = false;                    // safe to eat without cooking
    bool rawUnsafe = false;                    // eating raw risks illness
    std::int32_t spoilDays = 0;                // 0 = does not spoil
    Fixed cookedSpoilMultiplier = core::kOne;  // preservation from processing

    // Tool / weapon
    ToolClass toolClass = ToolClass::None;
    Fixed toolEfficiency = core::kOne;         // multiplies work rate
    std::int32_t durability = 0;               // work units before it breaks

    // Clothing
    Fixed insulation = core::kZero;            // degrees of cold offset

    // Liquid containers
    Fixed containerCapacity = core::kZero;     // litres, > 0 makes it a vessel

    // Medicine (D98). How much of an ailment one application answers: a
    // poultice closes a wound, a draught brings a fever down. Two fields rather
    // than one, because knowing which plant answers which hurt is the whole of
    // early medicine - a poultice on a fever is a waste of a poultice.
    Fixed healsWound = core::kZero;
    Fixed healsSickness = core::kZero;
};

// ---------------------------------------------------------------------------
// Knowledge (GDD 6: "knowing a method is separate from practical skill")
// ---------------------------------------------------------------------------
struct KnowledgeDef {
    DefId id;
    std::string name;
    std::string label;
    // Discoveries are not researched directly; they surface out of related work
    // (GDD 6). These two fields say which work can surface this knowledge and how
    // unlikely that is per completed job.
    WorkCategory discoveredFrom = WorkCategory::Count;   // Count = not discoverable
    std::int64_t discoveryChanceDenominator = 0;         // 1-in-N per job completion
    std::vector<std::string> requiresKnowledge;          // prerequisite method names
};

// A level of skill demanded before something can be attempted at all.
//
// This is deliberately a separate condition, and deliberately unused by every
// definition currently in content/. Ordinary work has no level requirement: a
// person who knows the method can knap a blade, mould a brick or raise a hut on
// their first day, and the level they have only decides how fast they do it and
// how good the result is. A requirement belongs on the exceptional thing - the
// ziggurat, the embroidered robe - and on nothing else.
struct SkillRequirement {
    WorkCategory category = WorkCategory::Count;   // Count = no requirement
    std::int32_t level = 0;

    bool demanded() const { return category != WorkCategory::Count && level > 0; }
};

// ---------------------------------------------------------------------------
// Recipes: an explicit transformation. GDD 2.2 - nothing material appears
// implicitly, so every conversion is one of these with an executor, a place,
// inputs, time and (where required) a tool.
// ---------------------------------------------------------------------------
struct IngredientSpec {
    DefId item;
    std::int32_t count = 1;
};

struct RecipeDef {
    DefId id;
    std::string name;
    std::string label;
    WorkCategory category = WorkCategory::Crafting;

    std::vector<IngredientSpec> inputs;
    std::vector<IngredientSpec> outputs;

    Fixed workAmount = core::kOne;
    // What a building it replaces gives back, as a share of that building's
    // materials. A reed hut pulled down is mats and bundles again; brick comes
    // out of a wall mostly whole.
    Fixed salvageShare = Fixed::ratio(3, 5);
    // What this is a better version of. A family that has outgrown its hut
    // builds the house on the same ground and uses what the hut gives back.
    std::string replaces;
    DefId replacesDef;             // work units; a pawn contributes ~1/tick at skill 0
    ToolClass requiredTool = ToolClass::None;
    SkillRequirement requiredSkill;            // empty for everything ordinary
    // A building name, or a function several buildings can serve. An Aegean kiln
    // of stone and a Mesopotamian one of mud brick are different buildings doing
    // the same job; a recipe should ask for the job.
    std::string requiredWorkplace;
    std::string requiredKnowledge;             // knowledge def name, empty = universally known
    Fixed waterLitres = core::kZero;           // water consumed by the process itself

    std::vector<DefId> workplaceDefs;          // every building that serves, resolved after load
    DefId knowledgeDef;                        // resolved after load
};

// ---------------------------------------------------------------------------
// Natural resources: what is standing in the world before anyone builds anything.
// ---------------------------------------------------------------------------
enum class ResourceKind : std::uint8_t { Tree, Rock, Bush, WildPlant, WaterSource, Game, Count };
std::string_view resourceKindName(ResourceKind k);
bool parseResourceKind(std::string_view s, ResourceKind& out);

struct HarvestSpec {
    std::vector<IngredientSpec> yields;
    Fixed workAmount = core::kOne;
    // What a building it replaces gives back, as a share of that building's
    // materials. A reed hut pulled down is mats and bundles again; brick comes
    // out of a wall mostly whole.
    Fixed salvageShare = Fixed::ratio(3, 5);
    // What this is a better version of. A family that has outgrown its hut
    // builds the house on the same ground and uses what the hut gives back.
    std::string replaces;
    DefId replacesDef;
    ToolClass requiredTool = ToolClass::None;      // None = bare hands are enough
    ToolClass preferredTool = ToolClass::None;     // faster, but not required
    Fixed bareHandPenalty = core::kOne;            // multiplier when working without the preferred tool
    WorkCategory category = WorkCategory::Foraging;
    std::string requiredKnowledge;
    DefId knowledgeDef;
};

struct ResourceNodeDef {
    DefId id;
    std::string name;
    std::string label;
    ResourceKind kind = ResourceKind::Bush;

    HarvestSpec harvest;
    // Taking the thing itself rather than what it bears. A date palm gives dates
    // to whoever picks them and goes on standing; felled, it gives its timber
    // once and is gone. Two actions, two tools, two trades - and the difference
    // between a community that lives off its palms and one that eats them.
    HarvestSpec fell;
    bool consumedOnHarvest = true;             // a tree is; a berry bush regrows
    std::int32_t regrowDays = 0;               // 0 = never
    // What it costs to get the thing off the ground when the ground is wanted
    // for something else: felling a tree, breaking up an outcrop, pulling out a
    // thicket. Zero means "as much work as harvesting it".
    Fixed clearWork = core::kZero;
    // Chance per standing one of these, per day, of a new one taking root
    // nearby. This is how a wood comes back: seed falls near the parent and
    // grows somewhere else. Nothing grows back out of its own stump.
    std::int32_t spreadOneIn = 0;
    // What share of the original stand is left alone, as a percentage. The same
    // rule the hunters keep for a breeding herd: a wood that seeds itself needs
    // trees to seed from, and once nothing grew back out of its own stump a
    // community that would fell the last palm did fell the last palm - four
    // hundred and eighty of them, and then starved in the winter with nothing
    // left in the country to gather.
    //
    // A share and not a count, because the count has to mean the same thing on a
    // ninety-six tile map as on a two hundred tile one: a floor of a hundred and
    // twenty palms on a map that only ever grew ninety forbade gathering
    // outright, and that community starved inside a month.
    std::int32_t keepStandingPercent = 0;
    // Seasons in which the node can be harvested at all. Empty = all year.
    std::vector<std::string> seasons;
    bool blocksMovement = false;
};

// ---------------------------------------------------------------------------
// Buildings
// ---------------------------------------------------------------------------
enum class BuildingKind : std::uint8_t { Housing, Storage, Workshop, Hearth, Fortification, Other, Count };
std::string_view buildingKindName(BuildingKind k);
bool parseBuildingKind(std::string_view s, BuildingKind& out);

using Fertility = Fixed;

struct BuildingDef {
    DefId id;
    std::string name;
    std::string label;
    BuildingKind kind = BuildingKind::Other;

    // Buildings cover a rectangle of tiles: width and depth in tiles, with the
    // origin at its north-west corner. A radius made every building an odd
    // square - one tile or nine, nothing between - so a house and a granary
    // were both three by three whatever they were meant to be.
    std::int32_t footprintWidth = 1;
    std::int32_t footprintDepth = 1;
    bool blocksMovement = true;
    // Needs ground that will hold it. A mud-brick house does not stand on a bog,
    // and a storage pit dug in one fills with water.
    bool firmGround = false;
    bool sheltered = false;                    // counts as indoors for warmth/weather

    std::vector<IngredientSpec> materials;
    Fixed workAmount = core::kOne;
    // What a building it replaces gives back, as a share of that building's
    // materials. A reed hut pulled down is mats and bundles again; brick comes
    // out of a wall mostly whole.
    Fixed salvageShare = Fixed::ratio(3, 5);
    // What this is a better version of. A family that has outgrown its hut
    // builds the house on the same ground and uses what the hut gives back.
    std::string replaces;
    DefId replacesDef;
    ToolClass requiredTool = ToolClass::None;
    SkillRequirement requiredSkill;            // empty for everything ordinary
    std::string requiredKnowledge;
    DefId knowledgeDef;

    // Housing
    std::int32_t sleepingSlots = 0;
    // How many families can live under it. One, for everything a Bronze Age
    // village builds; the field exists because some cultures will build the
    // tenement where several do.
    std::int32_t familySlots = 1;
    Fixed warmthBonus = core::kZero;
    Fixed comfortBonus = core::kZero;            // fractional sleep recovery bonus

    // Storage
    // How many head it shelters. A flock penned at night is not scattered by
    // morning and not taken by wolves.
    std::int32_t animalSlots = 0;

    std::int32_t storageSlots = 0;             // number of item batches held
    Fixed spoilRateMultiplier = core::kOne;    // a cool dry store slows spoilage
    std::vector<ItemCategory> acceptsCategories;   // empty = accepts everything

    // Waterworks. Tiles within this many of the channel hold their moisture,
    // which is the whole of what irrigation is (GDD 7): the fertility of the
    // ground beside a canal stops being what the river left there.
    std::int32_t irrigationRadius = 0;
    Fertility irrigationBonus = core::kZero;

    // How many of this a settlement will build, and how many people can work
    // one at a time. A mill is one building that several people work, not four
    // hand-querns dotted about.
    std::int32_t maxPerSettlement = 0;         // 0 = as many as are wanted
    std::int32_t workerSlots = 1;

    // Hearth / workshop
    bool providesHeat = false;
    bool providesFire = false;                 // enables cooking and boiling
    // What job this building does, so a recipe can ask for "a kiln" rather than
    // for one particular culture's kiln.
    std::string function;
    // The kind of area this belongs beside, by name ("pasture", "farm",
    // "extraction", "fishing"). A dairy stands by the flock, not in the middle
    // of the village; empty means the village itself.
    std::string nearArea;
};

// ---------------------------------------------------------------------------
// Crops (GDD 14: the grain chain, sown rather than gathered)
// ---------------------------------------------------------------------------
struct CropDef {
    DefId id;
    std::string name;
    std::string label;

    std::string seed;                          // item consumed when sowing
    std::string harvest;                       // item the ripe crop gives
    std::int32_t harvestCount = 1;
    std::int32_t seedCount = 1;
    std::int32_t growDays = 20;
    std::vector<std::string> sowSeasons;       // empty = any

    Fixed tillWork = Fixed::fromInt(40);
    Fixed sowWork = Fixed::fromInt(25);
    Fixed harvestWork = Fixed::fromInt(35);
    ToolClass tillTool = ToolClass::None;       // faster with it, possible without
    Fixed bareHandPenalty = core::kOne;
    std::string requiredKnowledge;

    DefId seedItem;                             // resolved after load
    DefId harvestItem;
    DefId knowledgeDef;
};

// ---------------------------------------------------------------------------
// Livestock (GDD 7: domestic animals and their products)
// ---------------------------------------------------------------------------
struct AnimalDef {
    DefId id;
    std::string name;
    std::string label;

    std::int32_t adultAgeDays = 240;
    std::int32_t maxAgeDays = 3000;
    // Days between one animal being ready to give again.
    std::int32_t shearIntervalDays = 0;         // 0 = never sheared
    std::int32_t milkIntervalDays = 0;
    std::int32_t breedIntervalDays = 0;
    // How many head the community keeps to breed from. A flock is capital before
    // it is meat: only the surplus above this is eaten, and eating into it is
    // what turned a starting flock of eight into two over a decade.
    std::int32_t breedingStock = 6;
    // A ewe often bears two. Without this a flock cannot outgrow its own losses.
    std::int32_t twinOneIn = 0;

    // Wild animals belong to nobody. They wander where they like, breed on
    // their own account, and are hunted rather than slaughtered: a hunted animal
    // is gone, which is why hunters have to leave enough of them to breed.
    bool wild = false;
    // A wolf takes what it can catch, wild or penned.
    bool predator = false;
    // What a tamed one becomes, if it can be tamed at all.
    std::string tamesInto;
    // Keeps a flock together and the wolves off it.
    bool guardsFlock = false;
    // How many head of it the country carries before it stops breeding up.
    std::int32_t wildCarryingCapacity = 0;
    // How much ground it covers, in metres. A bear is not a hen: this is what
    // the drawing is sized by, and it is a fact about the animal rather than
    // about the picture, so it lives here with the rest of them.
    Fixed sizeMetres = Fixed::ratio(9, 10);

    std::vector<IngredientSpec> huntYields;
    Fixed huntWork = Fixed::fromInt(90);
    ToolClass huntTool = ToolClass::None;

    std::vector<IngredientSpec> shearYields;
    std::vector<IngredientSpec> milkYields;
    std::vector<IngredientSpec> slaughterYields;

    Fixed shearWork = Fixed::fromInt(40);
    Fixed milkWork = Fixed::fromInt(15);
    Fixed slaughterWork = Fixed::fromInt(60);

    std::string requiredKnowledge;
    DefId knowledgeDef;
};

// ---------------------------------------------------------------------------
// Ethnos (GDD 5-6): the culture a starting community carries in with it.
// ---------------------------------------------------------------------------
struct EthnosDef {
    DefId id;
    std::string name;
    std::string label;
    std::string language;

    // Methods every member of this ethnos knows from the first tick.
    std::vector<std::string> commonKnowledge;
    // Pool from which each starting family draws its own private traditions
    // (GDD 6: losing the only bearer of a rare family tradition has real cost).
    std::vector<std::string> familyKnowledgePool;
    std::int32_t familyKnowledgeDraws = 2;

    std::vector<std::string> preferredFoods;   // item names the culture values
    std::string architectureSet;
    // Which land this culture belongs in (GDD 5). For now it selects a terrain
    // preset for the local map; the macro-scale matching of GDD 4.2 is not built.
    std::string biome = "temperate";

    // A community of 2000 BC is not a band that has to invent flintknapping. Its
    // adults arrive competent: everyone has a working grounding in the trades the
    // culture practises, and each is better at one of them than the rest.
    std::int32_t baseSkill = 2;
    std::int32_t specialistSkill = 6;
    std::vector<std::string> trades;           // work categories, dealt out in turn

    // What walks in with them. GDD 6 says the founders carry no goods; livestock
    // is the deliberate exception, because a herding culture without a flock is
    // not that culture (see DECISIONS.md D22).
    std::vector<std::pair<std::string, std::int32_t>> startingLivestock;
    // What the founders carry in. GDD 6 has them arriving with nothing, and
    // that held while a community was ten; at twenty-four the first two winters
    // killed half the seeds outright. People walking to new ground carry their
    // tools and a few days of food, and that is all this is.
    std::vector<std::pair<std::string, std::int32_t>> startingGoods;
    std::vector<std::string> crops;            // what this culture sows
};

// ---------------------------------------------------------------------------
// Personal traits (GDD 6)
// ---------------------------------------------------------------------------
struct TraitDef {
    DefId id;
    std::string name;
    std::string label;
    bool heritable = true;
    std::int64_t innateChanceDenominator = 0;      // 1-in-N at birth, 0 = never innate
    std::int64_t heritableChanceNumerator = 1;     // per parent carrying it
    std::int64_t heritableChanceDenominator = 2;
    std::vector<std::string> excludes;

    Fixed strengthMod = core::kZero;               // additive body modifiers
    Fixed enduranceMod = core::kZero;
    Fixed dexterityMod = core::kZero;
    Fixed learnRateMod = core::kZero;              // multiplier offset on skill gain
    Fixed discoveryMod = core::kZero;              // multiplier offset on discovery chance
    // Per-category preference weight offsets used by the autonomous planner.
    std::vector<std::pair<WorkCategory, Fixed>> workAffinity;
};

} // namespace content
