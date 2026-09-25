#pragma once
// The whole simulated world. Headless by construction: nothing in this header or
// anything it includes knows about windows, textures or input (GDD 11).
//
// Everything that can influence a decision is integer or Fixed, and every random
// draw comes from a seeded per-system stream, so two runs of the same seed and
// the same player commands produce byte-identical states.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "game/content/content_db.hpp"
#include "game/generation/world_map_gen.hpp"
#include "engine/core/fixed.hpp"
#include "engine/core/binary.hpp"
#include "engine/core/geometry.hpp"
#include "engine/core/hash.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/rng.hpp"
#include "engine/core/time.hpp"
#include "game/simulation/ecs.hpp"
#include "game/ecs/render/extraction.hpp"
#include "game/world/weather.hpp"

namespace sim {

using content::DefId;
using content::ItemCategory;
using content::ToolClass;
using content::WorkCategory;
using core::BuildingId;
using core::Fixed;
using core::HouseholdId;
using core::ItemStackId;
using core::JobId;
using core::PersonId;
using core::ResourceNodeId;
using core::SettlementId;
using core::TilePos;
using core::TileRect;
using core::WorldPos;
using core::WorldRect;
using core::AnimalId;
using core::ZoneId;

class World;

// ---------------------------------------------------------------------------
// Terrain
// ---------------------------------------------------------------------------
enum class Terrain : std::uint8_t { Grass, Forest, Dirt, Rock, Water, Sand, Marsh, Count };
std::string_view terrainName(Terrain t);
bool terrainPassable(Terrain t);

// Chance per tree per day of a branch falling nearby. A wood keeps itself
// stocked with loose wood; a felled wood stops.
inline constexpr std::int32_t kDeadfallOneIn = 40;

// How far a seed carries from the parent that dropped it.
inline constexpr std::int32_t kSpreadReach = 4;

// Grazing. Full grass is what an animal wants to find; it eats a little every
// day it stands there, and the ground puts it back over a season - slower in
// winter, and not at all on ground that never had grass on it.
inline constexpr std::uint8_t kGrassFull = 200;
inline constexpr std::uint8_t kGrassEatenPerDay = 12;
inline constexpr std::uint8_t kGrassGrowsPerDay = 2;
// Below this there is not enough underfoot to keep an animal in condition.
inline constexpr std::uint8_t kGrassBare = 40;
// How much grass ground of each kind carries when the map is made.
std::uint8_t grassFor(Terrain t);
Fixed terrainMoveCost(Terrain t);

// Footfall at which a tile counts as a made path, and the most a path can take
// off the cost of crossing it.
inline constexpr std::uint16_t kPathTraffic = 600;
inline constexpr std::uint16_t kPathVisible = 120;

struct Tile {
    Terrain terrain = Terrain::Grass;
    Fixed fertility = core::kZero;              // 0..1, drives farming yield
    ResourceNodeId node;                        // natural feature standing here
    BuildingId building;                        // completed or under construction
    Fixed pollution = core::kZero;              // sanitation field (GDD 7)

    // Which zones cover this tile, one bit per zone. Zones are asked about on
    // every candidate job for every idle pawn, so the question has to be answered
    // by reading the tile rather than by testing every zone in the settlement.
    std::uint64_t zoneMask = 0;

    // Land the community has actually walked over. GDD 2.1 has the world existing
    // without the player, but a community does not work ground it has never seen:
    // ordinary work is offered only on explored tiles, and it is the scouts who
    // widen that edge.
    bool explored = false;

    // --- tillage (GDD 14: the grain chain) ------------------------------
    // A tile inside a farm zone can be broken, sown, and reaped. The crop is
    // content, the growth is state.
    DefId crop;                                 // invalid = nothing sown
    Fixed cropGrowth = core::kZero;             // 0..1, ripe at 1
    bool tilled = false;
    // Watered by a channel. Recomputed once a day from the canals that stand,
    // so a silted-up or abandoned channel stops watering what it used to.
    bool irrigated = false;
    // How much this tile is walked on. A way people take every day wears into a
    // path: bare, level, and quicker to cross than what it was. Counted, not
    // felt: an integer, so it stays deterministic.
    std::uint16_t traffic = 0;
    // A crossing over the river: shallow, stony, and no ground to build on. It
    // is dry land only in the sense that it can be walked.
    bool ford = false;
    // Height above the river bed, in steps of about a third of a metre. The map
    // is drawn flat, so this is what tells a player the land is not: it is what
    // the hill shading in the renderer reads, and what makes the floodplain look
    // like a floodplain rather than a green rectangle.
    std::uint8_t elevation = 0;
    // How much there is to graze here, out of kGrassFull. Eaten down by the
    // animals standing on it and grown back by the season, so a flock has to
    // move and the pasture has to move with it.
    std::uint8_t grass = 0;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// The cost of crossing a tile, with the wear of use taken off it. Everything
// that asks how hard the ground is to cross asks this, not the terrain.
Fixed tileMoveCost(const Tile& t);

// The cache tiles a building of this definition would touch if its north-west
// corner were at `origin`, optionally with a ring of margin around them.
std::vector<TilePos> footprintOf(const content::ContentDb& db, DefId def, TilePos origin,
                                 std::int32_t margin = 0);

// A rectangular local map. The seamless chunked party map of GDD 11 is a
// streaming concern layered on top of this; the slice runs one map, and the tile
// store is already indexed so a chunk grid can replace the backing array without
// touching anything that reads through at()/passable().
class TileMap {
public:
    void resize(std::int32_t w, std::int32_t h);

    std::int32_t width() const { return width_; }
    std::int32_t height() const { return height_; }
    bool inBounds(TilePos p) const { return p.x >= 0 && p.y >= 0 && p.x < width_ && p.y < height_; }

    const Tile& at(TilePos p) const { return tiles_[index(p)]; }
    Tile& at(TilePos p) { return tiles_[index(p)]; }

    bool passable(TilePos p) const;
    TileRect bounds() const { return {{0, 0}, {width_, height_}}; }

    // Blocked-ness is derived from terrain plus whatever stands on the tile, and
    // is queried on every A* expansion, so it is cached as a flat bitmap and
    // refreshed whenever a building or resource node appears or disappears.
    void setBlocked(TilePos p, bool blocked);
    bool blocked(TilePos p) const { return blocked_[index(p)] != 0; }

private:
    std::size_t index(TilePos p) const { return std::size_t(p.y) * width_ + p.x; }
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    std::vector<Tile> tiles_;
    std::vector<std::uint8_t> blocked_;
};

// ---------------------------------------------------------------------------
// Physical goods. GDD 7: there is no teleporting settlement stockpile - every
// batch exists at a place, in a pair of hands, or inside a building.
// ---------------------------------------------------------------------------
enum class StackWhere : std::uint8_t { Ground, Carried, InBuilding, Equipped };

struct ItemStack {
    ItemStackId id;
    // Bumped every time this slot is reused. Batch slots are recycled to keep ids
    // dense, which means a job that stored an id across ticks can wake up holding
    // a handle to an entirely different batch. The generation is what lets it
    // notice: a hauler once walked across the map and picked up a flint knife
    // somebody else was working with, because the berries it had been sent for
    // had been eaten and their slot handed to the knife.
    std::uint32_t generation = 0;
    DefId def;
    std::int32_t count = 0;

    StackWhere where = StackWhere::Ground;
    TilePos tile;
    PersonId holder;                 // Carried or Equipped
    BuildingId building;             // InBuilding

    Fixed freshness = core::kOne;    // 1 fresh, 0 spoiled; only for perishables
    // How well it was made. A level of skill does not decide whether somebody may
    // attempt a thing - it decides how good the thing turns out. Only meaningful
    // on items made one at a time (tools, weapons, garments); bulk goods carry 1.
    Fixed quality = core::kOne;
    std::int32_t durabilityLeft = 0; // tools; 0 for everything else
    SettlementId owner;

    bool alive = true;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// Buildings
// ---------------------------------------------------------------------------
enum class BuildState : std::uint8_t { Blueprint, Building, Complete };

struct Building {
    BuildingId id;
    // What this is being pulled down to make room for, if anything. A house is
    // replaced where it stands rather than beside itself: the ground is already
    // the right ground, and most of the material comes out of the old one.
    DefId replacedBy;
    // Whose it is. A house belongs to the family it was raised for: four houses
    // covering twenty-four sleeping places is not the same thing as a roof for
    // every family, and by default families do not share one.
    HouseholdId household;
    DefId def;
    TilePos origin;                             // north-west cache tile of the footprint
    BuildState state = BuildState::Blueprint;
    Fixed workDone = core::kZero;
    // Materials physically delivered to the site so far, parallel to def.materials.
    std::vector<std::int32_t> delivered;
    SettlementId settlement;
    // Last tick anything was delivered here or any work was put in. A site that
    // has stalled for long enough is abandoned: the community gives up on a plan
    // it cannot supply rather than blocking on it forever.
    std::int64_t lastProgressTick = 0;
    bool alive = true;

    // Gameplay asks these in world metres first; the tile footprint is only the
    // occupancy cache and renderer convenience.
    std::vector<TilePos> footprintTiles(const content::ContentDb& db) const;
    bool covers(const content::ContentDb& db, TilePos p) const;
    WorldRect footprintBounds(const content::ContentDb& db) const;
    std::vector<WorldPos> footprintOutline(const content::ContentDb& db) const;
    WorldPos centreWorld(const content::ContentDb& db) const;
    bool covers(const content::ContentDb& db, WorldPos p) const;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// Natural resources
// ---------------------------------------------------------------------------
struct ResourceNode {
    ResourceNodeId id;
    DefId def;
    TilePos tile;
    Fixed workDone = core::kZero;
    bool depleted = false;
    std::int64_t regrowAtTick = 0;
    bool alive = true;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// People
// ---------------------------------------------------------------------------
enum class LifeStage : std::uint8_t { Child, Adult, Elder };
enum class Sex : std::uint8_t { Female, Male };

struct Skill {
    std::int32_t level = 0;
    Fixed progress = core::kZero;   // toward the next level
};

// Why a pawn is doing what it is doing. The headless report groups idleness and
// hunger by these, which is what GDD 14 asks the vertical slice to prove.
enum class JobKind : std::uint8_t {
    None,
    Harvest,        // take from a resource node
    Craft,          // run a recipe
    Construct,      // add work to a building site
    HaulToStore,    // move a loose batch into storage
    HaulToSite,     // deliver a construction material to a site
    FetchTool,      // pick up a tool the next job needs
    Fell,           // take the tree itself, for its timber; it does not come back
    Clear,          // fell, break or pull out what stands on ground that is wanted
    Till,           // break ground in a field
    Sow,            // put seed in broken ground
    ReapCrop,       // take a ripe crop
    Shear,          // take wool from an animal
    Milk,
    Slaughter,
    Hunt,           // take a wild animal; it is gone
    Tame,           // take a wild young one into the settlement
    Demolish,       // pull down what is no longer wanted, and keep what comes out of it
    Pen,            // drive an animal into the byre for the night
    Wear,           // put a garment on
    Scout,          // walk beyond the known land and see what is there
    Tend,           // treat somebody's wound or fever with what was made for it
    Bury,           // carry the dead out and lay them in the ground
    Eat,
    Drink,
    Sleep,
    Wander,
    Count
};
std::string_view jobKindName(JobKind k);

enum class JobPhase : std::uint8_t { Travelling, Working, Delivering, Done };

struct Job {
    JobKind kind = JobKind::None;
    WorkCategory category = WorkCategory::Hauling;
    JobPhase phase = JobPhase::Travelling;

    ResourceNodeId node;
    BuildingId building;
    ItemStackId stack;
    std::uint32_t stackGeneration = 0;   // which batch `stack` meant when this job was made
    AnimalId animal;
    DefId recipe;
    // Who this job is being done for: the patient of a Tend, the dead of a
    // Bury. A person rather than a tile, because both of them move - one is
    // carried, and the other may have got up and walked off to drink (D98).
    PersonId person;
    DefId crop;
    DefId wantedItem;
    std::int32_t wantedCount = 0;

    TilePos target;                  // where the pawn must stand
    TilePos deliverTo;               // second leg, for hauling jobs
    BuildingId deliverBuilding;

    Fixed workDone = core::kZero;
    Fixed workRequired = core::kOne;

    std::vector<TilePos> path;
    std::size_t pathIndex = 0;

    bool valid() const { return kind != JobKind::None; }

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// Binds a job to a specific batch. Always use this rather than assigning
// `job.stack` directly, so the generation can never be left behind.
void setJobStack(const World& w, Job& job, ItemStackId id);

// Why a pawn has no job this tick. Aggregated in the report so a stalled economy
// names its own cause instead of showing up as "everyone idle".
enum class IdleReason : std::uint8_t {
    Working,
    // Just put a job down and has not looked for the next one yet. An honest
    // answer rather than a guess: the pawn has not been asked anything.
    BetweenJobs,
    NoWorkAvailable,
    NoToolForAnyJob,
    NoMaterialsForAnyJob,
    NoReachableTarget,
    NotSkilledEnough,   // only ever for the exceptional; nothing ordinary demands a level
    Unfit,              // too young, too sick, asleep
    Count
};
std::string_view idleReasonName(IdleReason r);

struct Person {
    PersonId id;
    std::string name;
    Sex sex = Sex::Female;
    LifeStage stage = LifeStage::Adult;
    std::int64_t birthTick = 0;
    std::int32_t ageYears = 0;
    // How much health each thing has taken over this life, so a death can be
    // reported by what caused it rather than by what was true at the last tick.
    Fixed harmFromHunger;
    Fixed harmFromThirst;
    Fixed harmFromCold;
    Fixed harmFromIllness;

    SettlementId settlement;
    HouseholdId household;
    PersonId father, mother, spouse;

    WorldPos pos;
    TilePos tile;

    // Bodily states (GDD 7). Each is its own axis; there is no single wellbeing bar.
    Fixed satiety = core::kOne;
    Fixed hydration = core::kOne;
    Fixed rest = core::kOne;
    Fixed health = core::kOne;
    Fixed bodyTempOffset = core::kZero;   // degrees away from comfortable
    bool asleep = false;
    bool alive = true;

    // What is wrong with them, if anything (D98). Its own axis rather than a
    // bite out of health, because the two behave differently: health is what
    // hunger and cold wear away and what sleep and food build back, and an
    // ailment is a thing that happened - a gash, a fever - which runs its course
    // and can be treated. A person carries one at a time: a second wound while
    // the first is open is the same wound getting worse, which is what severity
    // is for.
    enum class Ailment : std::uint8_t {
        None,
        Wound,     // cut, gore, fall: bleeds, then knits, and can go bad
        Fever,     // chill and exhaustion, or a wound gone bad
        Flux,      // dirty water and dirty ground
    };
    Ailment ailment = Ailment::None;
    // Somebody is on their way to treat them, so the rest of the settlement
    // does not all set off to the same sickbed.
    PersonId tendedBy;
    Fixed ailmentSeverity = core::kZero;   // 0..1; over about a half it stops them working
    Fixed ailmentTended = core::kZero;     // how much of it a healer has already answered
    std::int64_t ailmentSinceTick = 0;
    // Dead and not yet in the ground (D99). A body is left where it fell until
    // somebody carries it out and buries it, which is a job somebody has to
    // choose to do - and until they do, it is a reason the ground around it
    // makes people ill.
    bool buried = false;

    // Innate physical capacities, modified by traits and age stage.
    Fixed strength = core::kOne;
    Fixed endurance = core::kOne;
    Fixed dexterity = core::kOne;

    std::array<Skill, content::kWorkCategoryCount> skills{};
    // When this person last took up a different trade. A trade is a life, not a
    // rota: without this people swapped at every review and settlements lost
    // their footing entirely.
    std::int64_t tradeTakenUpTick = 0;
    std::vector<DefId> traits;
    std::vector<DefId> knownMethods;              // GDD 6: knowing a method
    WorkCategory profession = WorkCategory::Hauling;

    ItemStackId carrying;                          // one hauled batch at a time
    ItemStackId equippedTool;
    // Garments actually on the body. Cloth sitting in a store keeps nobody warm,
    // and for a long while nothing here ever put any on: the community wove
    // tunics all autumn and then froze wearing none of them.
    std::vector<ItemStackId> worn;

    // Recent diet, for the variety term of GDD 7. Counts of each food group eaten
    // in the trailing window; decays daily.
    std::array<std::int32_t, content::kFoodGroupCount> recentFoodGroups{};

    Job job;
    IdleReason idleReason = IdleReason::Working;
    // A pawn that found nothing to do will find nothing again next tick. Re-asking
    // every tick costs a full round of path searches for no new answer.
    std::int64_t nextPlanTick = 0;
    std::int64_t deathTick = -1;
    std::string deathCause;

    bool knows(DefId method) const;
    bool canWork() const;   // awake, alive, not a small child

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// Livestock (GDD 7). An animal is a body in the world with an age, a condition
// and a place, not a number in a herd counter: it has to be walked to, and it
// eats the pasture it stands on.
// ---------------------------------------------------------------------------
struct Animal {
    AnimalId id;
    DefId def;
    SettlementId owner;

    WorldPos pos;
    TilePos tile;
    TilePos grazeTarget;

    std::int32_t ageDays = 0;
    Sex sex = Sex::Female;
    Fixed condition = core::kOne;        // falls when there is nothing to graze
    std::int64_t nextShearTick = 0;
    std::int64_t nextMilkTick = 0;
    std::int64_t nextBreedTick = 0;
    // Penned for the night. A penned animal does not drift, does not graze, and
    // is not worth a wolf's trouble.
    bool penned = false;
    bool alive = true;

    bool adult(const content::ContentDb& db) const;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// Player-facing controls (GDD 9). Zones and priorities are *technical* direction
// to the planner - they are not laws, cannot be broken, and need no institution.
// ---------------------------------------------------------------------------
enum class ZoneMode : std::uint8_t { Allowed, Forbidden, Preferred, HighPriority };

// The zone types listed in GDD 9. Kind says what the area is for; mode says how
// strongly the planner should treat it.
// The areas a settlement is made of. These are the drivers: the community lays
// them out from what the ground offers and where its families work, and builds
// inside them - a house in a residential quarter, a workshop in the craftsmen's,
// the granary on the common ground, the wall on the line. The player will draw
// them instead; what gets built in one is still the community's own decision.
enum class ZoneKind : std::uint8_t {
    General, Settlement, Storage, Extraction, Farm, Fishing, Hunting, Pasture,
    Patrol, Fortification, Timber, Residential, Craft, Civic, Ritual, Count
};
std::string_view zoneKindName(ZoneKind k);

// The most zones that can exist at once. One bit each in Tile::zoneMask, which is
// what makes the per-tile question cheap; sixty-four is far more than a
// settlement's seven default areas plus whatever the player draws.
inline constexpr std::size_t kMaxZones = 64;

struct Zone {
    ZoneId id;                                  // also the bit index in zoneMask
    std::string label;
    ZoneKind kind = ZoneKind::General;
    ZoneMode mode = ZoneMode::Allowed;
    // Empty = applies to every category.
    std::vector<WorkCategory> categories;
    SettlementId settlement;
    bool alive = true;
    // Areas the community laid out for itself are replaced when it reconsiders;
    // areas the player drew are never touched (GDD 9).
    bool playerDrawn = false;

    // The true area: world-space polygons the player or simulation painted. The
    // tile cache below is rebuilt from these and exists only so fast per-tile
    // questions stay O(1).
    std::vector<std::vector<WorldPos>> areas;
    std::vector<std::vector<WorldPos>> cutouts;

    // Cache tiles touched by the zone, in deterministic scan order.
    std::vector<TilePos> tiles;
    TileRect bounds{{0, 0}, {0, 0}};
    WorldRect worldBounds{{core::kOne * 1000000, core::kOne * 1000000},
                          {core::kOne * -1000000, core::kOne * -1000000}};
    WorldPos centroid{};

    bool appliesTo(WorkCategory c) const;
    TilePos centre() const;
    WorldPos centreWorld() const;
    bool contains(WorldPos p) const;
    bool covers(TilePos p) const;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// A family, and the trade it lives by. Trades stay in families because children
// learn them at home (GDD 6), and a family that lives by one trade wants to live
// near where that trade is done - which is what turns a settlement from a heap
// of huts around the fire into quarters.
struct Household {
    HouseholdId id;
    SettlementId settlement;
    content::WorkCategory trade = content::WorkCategory::Foraging;
    // Where this family's dwelling and its own stores belong. Sited once the
    // community knows where its work areas are.
    TilePos seat;
    bool seated = false;
    bool alive = true;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

struct Settlement {
    SettlementId id;
    std::string name;
    DefId ethnos;
    TilePos hearth;

    std::vector<PersonId> members;
    std::vector<ZoneId> zones;
    // Methods that have become a tradition of the whole settlement (GDD 6).
    std::vector<DefId> traditions;
    // Player's category priorities: multiplier weights on the planner's score.
    std::array<Fixed, content::kWorkCategoryCount> priorities{};

    // The population the community last laid its areas out for.
    std::int32_t zonedForPopulation = 0;

    // Where the wall lets people through. The line is laid out closed, so
    // without this the community walls itself in and starves with its fields
    // outside; the gate goes here and nothing that blocks movement ever does.
    TilePos gateway{0, 0};
    bool hasGateway = false;

    // How many days running the community has wanted a building it could not
    // get: indexed by building definition. A plan that keeps failing gives up
    // its place to the next one - "we want this, it is not working, so we do
    // nothing" is the worst answer available. Cleared when any plan does get
    // laid out, and halved every season so nothing is written off for ever.
    std::vector<std::int32_t> planAttempts;

    // Days of food in store, as of this morning. Kept here because the eating
    // decision needs it and is taken far from the ledger that computes it.
    Fixed foodDays = core::kZero;
    // The same, counting what still needs threshing, grinding or cooking.
    Fixed larderDays = core::kZero;

    // How strictly people keep to their own trade, from nothing to fully. A fed
    // community lets its shepherds herd and its bakers bake; a hungry one puts
    // every pair of hands on the threshing floor. Set once a day from the days
    // of food in store.
    Fixed tradeDiscipline = core::kOne;

    // Ground broken and not yet sown. Breaking more of it while this much lies
    // idle is work thrown away: a settlement of twenty broke five hundred and
    // seventy plots and had two hundred and forty sown.
    std::int32_t brokenUnsown = 0;

    // Seed the community will not spend on anything else, by item. A farming
    // people that grinds its seed corn breaks a hundred and fifty plots in the
    // spring and sows a dozen, because by sowing time the grain is flour.
    // Indexed by item id; refilled once a day by tickFarming from the ground
    // that has actually been broken.
    std::vector<std::int32_t> seedReserve;

    bool alive = true;

    // Its own binary form. What the field order is, is this struct's business;
    // what order the entities go in, and what version the whole is, belongs to
    // savegame.cpp - which is the only thing allowed to call these.
    void write(core::BinaryWriter& out) const;
    void read(core::BinaryReader& in);
};

// ---------------------------------------------------------------------------
// Headless run report (GDD 14)
// ---------------------------------------------------------------------------
struct RunReport {
    std::int64_t ticks = 0;
    std::int32_t population = 0;
    std::int32_t births = 0;
    std::int32_t deaths = 0;
    // Medicine and the dead (D98, D99). Counted because the only way to tell
    // whether a healer is worth the hands they take is to compare how many fell
    // ill with how many got better.
    std::int32_t fellIll = 0;
    std::int32_t wounded = 0;
    std::int32_t recovered = 0;
    std::int32_t treatments = 0;
    std::int32_t burials = 0;
    std::int32_t unburiedPeak = 0;
    std::unordered_map<std::string, std::int32_t> deathCauses;
    // What was true of a person when they died. Cause alone answers "exposure"
    // and leaves the question standing: exposure at what temperature, wearing
    // what, with how much food in store. Kept for the first forty deaths, which
    // is more than enough to see the shape of them.
    struct DeathNote {
        std::string cause;
        std::int32_t day = 0;
        std::int32_t age = 0;
        Fixed satiety;
        Fixed hydration;
        Fixed feltTempC;
        Fixed outdoorTempC;
        std::int32_t garments = 0;
        bool underARoof = false;
        Fixed foodDaysInStore;
        Fixed larderDaysInStore;
        std::string doing;          // the job they were on when they died
        std::int32_t foodWithin = -1;   // tiles to the nearest meal they could have had
    };
    std::vector<DeathNote> deathNotes;
    std::array<std::int64_t, static_cast<std::size_t>(IdleReason::Count)> idleTicks{};
    std::array<std::int64_t, static_cast<std::size_t>(JobKind::Count)> jobTicks{};
    std::array<std::int32_t, static_cast<std::size_t>(JobKind::Count)> jobsCompleted{};
    std::int64_t hungryPersonTicks = 0;      // person-ticks spent below the eat threshold
    std::int64_t thirstyPersonTicks = 0;
    std::int64_t starvingPersonTicks = 0;    // below a quarter satiety
    // Times a job was rejected purely because no suitable tool existed anywhere.
    std::int64_t toolShortageRejections = 0;
    std::int64_t materialShortageRejections = 0;
    std::unordered_map<std::string, std::int32_t> itemsProduced;
    std::unordered_map<std::string, std::int32_t> buildingsCompleted;
    std::int32_t livestock = 0;
    std::int32_t animalsBorn = 0;
    std::int32_t animalsLost = 0;
    std::int32_t animalsTakenByWolves = 0;
    std::int32_t animalsHunted = 0;
    std::int32_t treesFelled = 0;
    std::int32_t animalsTamed = 0;
    std::int32_t buildingsDemolished = 0;
    std::int32_t animalsPenned = 0;
    std::int32_t animalsStrayed = 0;
    std::int32_t cropsSown = 0;
    std::int32_t cropsReaped = 0;
    std::int32_t cropsLost = 0;
    std::int32_t nodesCleared = 0;
    std::int32_t tilesExplored = 0;
};

// ---------------------------------------------------------------------------
// World
// ---------------------------------------------------------------------------
struct WorldConfig {
    std::uint64_t seed = 1;
    // A local map is one cell of the world map (D84), and this is how big a
    // cell is: room for a settlement, its fields, its pasture and the country
    // it walks out into.
    std::int32_t mapWidth = 180;
    std::int32_t mapHeight = 180;
    // How many cells the world map is across. Zero takes the generator's own
    // default, which is the size the game is meant to be played at; a smaller
    // one is for tests, which build a world per fixture and do not look at it.
    std::int32_t worldCells = 0;
    // Which cell of the world map to draw at play scale. Invalid (the default)
    // means "the one the played community lives in". Set to another cell, the
    // world that comes out is the country around somebody else's settlement -
    // the ground they live on, drawn the same way. Nobody is put on it: their
    // people are numbers on the world map (D91), not simulated.
    core::TilePos localCell{-1, -1};
    // A world map already built, to be used instead of generating one. The
    // whole map costs about a second and is a pure function of the seed, so
    // looking at a neighbour should not pay for it twice.
    const generation::WorldMapData* sharedWorldMap = nullptr;
    std::string ethnos = "sumerian";
    std::int32_t startingPopulation = 24;
};

class World {
public:
    World(const content::ContentDb& db, const WorldConfig& cfg);

    // Built empty, to be filled from a save rather than generated. The content
    // and the country have to be the ones it was saved with; nothing here checks
    // that, and the save's own header is what carries the seed to check against.
    struct FromSave {};
    World(const content::ContentDb& db, const WorldConfig& cfg, FromSave);

    // The whole of this world in its own binary form. Every entity writes
    // itself; this decides the order they go in, and savegame.cpp decides the
    // version and what surrounds it.
    void write(core::BinaryWriter& out) const;
    bool read(core::BinaryReader& in);

    void tick();
    void runTicks(std::int64_t n) { for (std::int64_t i = 0; i < n; ++i) tick(); }

    // How closely this community is being simulated. A map holds a dozen or more
    // of them and only one is ever on screen; the rest do not need every pawn's
    // path worked out to go on being communities (D103).
    //
    // Detailed is the whole loop: needs, planning, jobs, every body walking.
    // Abstract keeps the same people, the same stores and the same buildings -
    // nothing is thrown away and nothing has to be rebuilt when the player looks
    // - but the day is settled by accounting rather than by acting: the work the
    // hands could do against the work there is, food grown against food eaten.
    // Switching between them is free in both directions, which is the point.
    enum class Detail : std::uint8_t { Detailed, Abstract };
    void setDetail(Detail d) { detail_ = d; }
    Detail detail() const { return detail_; }

    std::int64_t tickCount() const { return tick_; }
    core::DateTime now() const { return core::decompose(tick_, db_.time()); }
    const content::ContentDb& db() const { return db_; }
    const WorldConfig& config() const { return cfg_; }
    // The country this local map is one cell of. Generated once, never ticked.
    // Shared: a client that simulates every community on the map holds a dozen
    // worlds, and a copy of the country in each of them is half a gigabyte of
    // identical cells.
    const generation::WorldMapData& worldMap() const { return *worldMapRef_; }
    // Which cell of it this world is, and the corner of the block the local map
    // refines. Held here rather than in the map because the map is shared and
    // every community is standing on a different part of it.
    core::TilePos localCell() const { return localCell_; }
    // How many people are ill right now, counted once a tick by needs. The
    // planner asks before it goes looking for a patient: scanning everybody for
    // everybody is a square and it showed up as half the tick rate (D98).
    std::int32_t ailing() const { return ailing_; }
    // And how many are dead and still above ground (D99), for the same reason.
    std::int32_t unburied() const { return unburied_; }
    void setUnburied(std::int32_t n) { unburied_ = n; }
    void setAiling(std::int32_t n) { ailing_ = n; }
    core::TilePos localBlock() const { return localBlock_; }
    // How many of each wild thing the country grew before anybody touched it.
    // What "leave a quarter of the palms standing" is measured against.
    std::int32_t nodesAtFirst(DefId def) const {
        return def.valid() && def.value < nodesAtFirst_.size() ? nodesAtFirst_[def.value] : 0;
    }

    TileMap& map() { return map_; }
    const TileMap& map() const { return map_; }

    std::vector<Person>& people() { return people_; }
    const std::vector<Person>& people() const { return people_; }
    std::vector<ItemStack>& stacks() { return stacks_; }
    const std::vector<ItemStack>& stacks() const { return stacks_; }
    std::vector<Building>& buildings() { return buildings_; }
    const std::vector<Building>& buildings() const { return buildings_; }
    std::vector<ResourceNode>& nodes() { return nodes_; }
    const std::vector<ResourceNode>& nodes() const { return nodes_; }
    std::vector<Household>& households() { return households_; }
    const std::vector<Household>& households() const { return households_; }
    Household& household(HouseholdId id) { return households_[id.value]; }
    const Household& household(HouseholdId id) const { return households_[id.value]; }
    HouseholdId createHousehold(SettlementId s, content::WorkCategory trade);

    std::vector<Zone>& zones() { return zones_; }
    const std::vector<Zone>& zones() const { return zones_; }
    std::vector<Animal>& animals() { return animals_; }
    const std::vector<Animal>& animals() const { return animals_; }
    std::vector<Settlement>& settlements() { return settlements_; }
    const std::vector<Settlement>& settlements() const { return settlements_; }

    // ECS access is read/write for systems during the migration. The legacy
    // vectors remain available until each domain is moved to a component view.
    ecs::EcsWorld& ecs() { return ecs_; }
    const ecs::EcsWorld& ecs() const { return ecs_; }
    void setAiFocus(ecs::CellId focus) { aiFocus_ = focus; }
    void clearAiFocus() { aiFocus_.reset(); }
    std::size_t ecsEntityCount() const { return ecs_.view<ecs::Identity>().size(); }
    entt::entity ecsEntity(ecs::Kind kind, std::uint32_t legacyIndex) const;
    void syncEcsStack(ItemStackId id);
    void syncEcsBuilding(BuildingId id);
    void syncEcsResourceNode(ResourceNodeId id);
    void syncEcsAnimal(AnimalId id);
    void syncEcsPerson(PersonId id);
    void syncEcsNeeds(PersonId person);
    // Publish one person's assignment immediately; used while jobs are assigned
    // or released within a single planner/execution pass.
    void syncEcsJob(PersonId person);
    ecs::render::Frame extractPresentationFrame() const;

    Person& person(PersonId id) { return people_[id.value]; }
    const Person& person(PersonId id) const { return people_[id.value]; }
    ItemStack& stack(ItemStackId id) { return stacks_[id.value]; }
    const ItemStack& stack(ItemStackId id) const { return stacks_[id.value]; }
    Building& building(BuildingId id) { return buildings_[id.value]; }
    const Building& building(BuildingId id) const { return buildings_[id.value]; }
    ResourceNode& node(ResourceNodeId id) { return nodes_[id.value]; }
    const ResourceNode& node(ResourceNodeId id) const { return nodes_[id.value]; }
    Animal& animal(AnimalId id) { return animals_[id.value]; }
    const Animal& animal(AnimalId id) const { return animals_[id.value]; }
    Settlement& settlement(SettlementId id) { return settlements_[id.value]; }
    const Settlement& settlement(SettlementId id) const { return settlements_[id.value]; }

    Fixed outdoorTemperature() const { return outdoorTemp_; }

    // Reveals a patch of ground and returns how many tiles were new.
    std::int32_t explore(TilePos centre, std::int32_t radius);
    std::int32_t exploredCount() const { return exploredCount_; }

    // --- entity creation ------------------------------------------------
    ItemStackId spawnStack(DefId item, std::int32_t count, TilePos at, SettlementId owner);
    // True when the handle still refers to the batch it was taken from.
    bool stackStillIs(ItemStackId id, std::uint32_t generation) const;
    bool resourceNodeAlive(ResourceNodeId id) const;
    bool resourceNodeDepleted(ResourceNodeId id) const;
    bool buildingAlive(BuildingId id) const;
    bool buildingComplete(BuildingId id) const;
    std::uint8_t buildingPhase(BuildingId id) const;
    bool personAlive(PersonId id) const;
    bool animalAlive(AnimalId id) const;
    void setPersonAlive(PersonId id, bool value);
    void setAnimalAlive(AnimalId id, bool value);
    void setBuildingAlive(BuildingId id, bool value);
    void setResourceNodeAlive(ResourceNodeId id, bool value);
    void setStackAlive(ItemStackId id, bool value);
    ResourceNodeId spawnNode(DefId def, TilePos at);
    AnimalId spawnAnimal(DefId def, TilePos at, SettlementId owner, Sex sex, std::int32_t ageDays);
    BuildingId placeBlueprint(DefId def, TilePos origin, SettlementId s,
                              HouseholdId forHousehold = {});
    // Creates an empty zone and returns its id, or an invalid id if all sixty-four
    // slots are taken.
    ZoneId createZone(const std::string& label, ZoneKind kind, ZoneMode mode, SettlementId s,
                      bool playerDrawn);
    void addAreaToZone(ZoneId id, const std::vector<WorldPos>& polygon);
    void addDiscToZone(ZoneId id, WorldPos centre, Fixed radius);
    void removeAreaFromZone(ZoneId id, const std::vector<WorldPos>& polygon);
    void removeDiscFromZone(ZoneId id, WorldPos centre, Fixed radius);
    void addTilesToZone(ZoneId id, const std::vector<TilePos>& tiles);
    void removeTilesFromZone(ZoneId id, const std::vector<TilePos>& tiles);
    void destroyZone(ZoneId id);
    // Every alive zone covering this tile, cheapest question in the planner.
    std::uint64_t zoneMaskAt(TilePos p) const {
        return map_.inBounds(p) ? map_.at(p).zoneMask : 0;
    }
    std::uint64_t zoneMaskAt(WorldPos p) const;

    void destroyStack(ItemStackId id);

    // --- reservations ---------------------------------------------------
    // A pawn claims its target so ten idle pawns do not all walk to the same tree.
    bool reserve(std::uint64_t key, PersonId by);
    bool reserveAll(const std::vector<std::uint64_t>& keys, PersonId by);
    void release(std::uint64_t key);
    void releaseAllBy(PersonId by);
    bool isReserved(std::uint64_t key, PersonId ignoring = PersonId{}) const;
    void applyReservationIntents(const ecs::CommandBuffer& intents);
    void applyPlacementIntents(const ecs::CommandBuffer& intents);

    static std::uint64_t nodeKey(ResourceNodeId id) { return (1ull << 60) | id.value; }
    static std::uint64_t stackKey(ItemStackId id) { return (2ull << 60) | id.value; }
    static std::uint64_t buildingKey(BuildingId id) { return (3ull << 60) | id.value; }
    static std::uint64_t animalKey(AnimalId id) { return (5ull << 60) | id.value; }
    static std::uint64_t tileKey(TilePos p) {
        return (4ull << 60) | (std::uint64_t(std::uint32_t(p.x)) << 20) | std::uint32_t(p.y);
    }

    core::Rng& rng(std::uint64_t stream);

    const RunReport& report() const { return report_; }
    RunReport& report() { return report_; }
    world::weather::Sample weather() const;

    // A running fingerprint of every value that can affect a later decision. Two
    // runs of one seed must agree on this at every tick; the runner prints it and
    // the determinism test compares two worlds tick by tick.
    std::uint64_t checksum() const;

    // Recompute the blocked bitmap for a tile after something appeared or left.
    void refreshBlocked(TilePos p);

    // Rebuild the ECS projection from the still-authoritative legacy records.
    // Serialization and deterministic gameplay remain unchanged while systems
    // migrate one domain at a time.
    void syncEcs();

private:
    void ensureWeatherClimate() const;
    void updateWeather();
    // A day of this community's life without simulating anybody: what the hands
    // could have done, what was eaten, who was born and who died.
    void tickAbstract();
    void updateSpoilage();
    void updateResourceRegrowth();
    void updateDeadfall();
    void updateWildSpread();
    void updateGrass();
    void updatePaths();
    void forgetOldPlanFailures();

    const content::ContentDb& db_;
    WorldConfig cfg_;
    std::int64_t tick_ = 0;
    Detail detail_ = Detail::Detailed;

    TileMap map_;
    // Owned only when this world generated the country itself; when one was
    // handed in (WorldConfig::sharedWorldMap) this stays empty and the pointer
    // below is what everything reads.
    generation::WorldMapData ownedWorldMap_;
    const generation::WorldMapData* worldMapRef_ = &ownedWorldMap_;
    core::TilePos localCell_{0, 0};
    core::TilePos localBlock_{0, 0};
    std::int32_t ailing_ = 0;
    std::int32_t unburied_ = 0;
    std::vector<std::int32_t> nodesAtFirst_;
    std::vector<Person> people_;
    std::vector<ItemStack> stacks_;
    std::vector<Building> buildings_;
    std::vector<ResourceNode> nodes_;
    std::vector<Household> households_;
    std::vector<Zone> zones_;
    std::vector<Animal> animals_;
    std::vector<Settlement> settlements_;

    std::vector<ItemStackId> freeStacks_;   // recycled slots keep ids dense
    std::unordered_map<std::uint64_t, PersonId> reservations_;
    std::unordered_map<std::uint64_t, core::Rng> rngs_;

    Fixed outdoorTemp_ = core::kZero;
    mutable std::optional<std::array<Fixed, 7>> weatherClimate_;
    std::int32_t exploredCount_ = 0;
    RunReport report_;
    ecs::EcsWorld ecs_;
    std::unordered_map<std::uint64_t, entt::entity> ecsEntities_;
    std::optional<ecs::CellId> aiFocus_;

    friend class WorldBuilder;
};

} // namespace sim
