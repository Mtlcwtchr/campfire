#include "game/simulation/savegame.hpp"

#include <algorithm>
#include <fstream>

#include "game/generation/world_map_gen.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "game/simulation/population.hpp"

namespace sim {
namespace {

using core::BinaryReader;
using core::BinaryWriter;

// The small shapes everything else is made of. Ids are a value and a validity;
// they are written as the value they are, with the invalid one round-tripping.
template <typename Id>
void writeId(BinaryWriter& out, Id id) {
    out.u32(id.value);
}
template <typename Id>
Id readId(BinaryReader& in) {
    Id id;
    id.value = in.u32();
    return id;
}
void writePos(BinaryWriter& out, core::TilePos p) {
    out.i32(p.x);
    out.i32(p.y);
}
core::TilePos readPos(BinaryReader& in) {
    core::TilePos p;
    p.x = in.i32();
    p.y = in.i32();
    return p;
}
void writeWorldPos(BinaryWriter& out, core::WorldPos p) {
    out.fixed(p.x);
    out.fixed(p.y);
}
core::WorldPos readWorldPos(BinaryReader& in) {
    core::WorldPos p;
    p.x = in.fixed();
    p.y = in.fixed();
    return p;
}
void writeWorldRect(BinaryWriter& out, core::WorldRect r) {
    writeWorldPos(out, r.min);
    writeWorldPos(out, r.max);
}
core::WorldRect readWorldRect(BinaryReader& in) {
    core::WorldRect r;
    r.min = readWorldPos(in);
    r.max = readWorldPos(in);
    return r;
}
template <typename T, typename WriteOne>
void writeVector(BinaryWriter& out, const std::vector<T>& v, WriteOne one) {
    out.u32(static_cast<std::uint32_t>(v.size()));
    for (const auto& item : v) one(out, item);
}
template <typename T, typename ReadOne>
void readVector(BinaryReader& in, std::vector<T>& v, ReadOne one) {
    const std::uint32_t n = in.u32();
    if (!in.ok()) return;
    v.clear();
    v.reserve(n);
    for (std::uint32_t i = 0; i < n && in.ok(); ++i) v.push_back(one(in));
}

} // namespace

// ---------------------------------------------------------------------------
// Each entity's own form. The fields are its business; the order the entities
// go in, and the version of the whole, belong to saveGame below.
// ---------------------------------------------------------------------------

void Tile::write(BinaryWriter& out) const {
    out.u8(static_cast<std::uint8_t>(terrain));
    out.fixed(fertility);
    writeId(out, node);
    writeId(out, building);
    out.fixed(pollution);
    out.u64(zoneMask);
    out.boolean(explored);
    writeId(out, crop);
    out.fixed(cropGrowth);
    out.boolean(tilled);
    out.boolean(irrigated);
    out.u16(traffic);
    out.boolean(ford);
    out.u8(elevation);
    out.u8(grass);
}
void Tile::read(BinaryReader& in) {
    terrain = static_cast<Terrain>(in.u8());
    fertility = in.fixed();
    node = readId<ResourceNodeId>(in);
    building = readId<BuildingId>(in);
    pollution = in.fixed();
    zoneMask = in.u64();
    explored = in.boolean();
    crop = readId<core::DefId>(in);
    cropGrowth = in.fixed();
    tilled = in.boolean();
    irrigated = in.boolean();
    traffic = in.u16();
    ford = in.boolean();
    elevation = in.u8();
    grass = in.u8();
}

void ItemStack::write(BinaryWriter& out) const {
    writeId(out, id);
    out.u32(generation);
    writeId(out, def);
    out.i32(count);
    out.u8(static_cast<std::uint8_t>(where));
    writePos(out, tile);
    writeId(out, holder);
    writeId(out, building);
    out.fixed(freshness);
    out.fixed(quality);
    out.i32(durabilityLeft);
    writeId(out, owner);
    out.boolean(alive);
}
void ItemStack::read(BinaryReader& in) {
    id = readId<ItemStackId>(in);
    generation = in.u32();
    def = readId<core::DefId>(in);
    count = in.i32();
    where = static_cast<StackWhere>(in.u8());
    tile = readPos(in);
    holder = readId<PersonId>(in);
    building = readId<BuildingId>(in);
    freshness = in.fixed();
    quality = in.fixed();
    durabilityLeft = in.i32();
    owner = readId<SettlementId>(in);
    alive = in.boolean();
}

void Building::write(BinaryWriter& out) const {
    writeId(out, id);
    writeId(out, replacedBy);
    writeId(out, household);
    writeId(out, def);
    writePos(out, origin);
    out.u8(static_cast<std::uint8_t>(state));
    out.fixed(workDone);
    writeVector(out, delivered, [](BinaryWriter& w, std::int32_t v) { w.i32(v); });
    writeId(out, settlement);
    out.i64(lastProgressTick);
    out.boolean(alive);
}
void Building::read(BinaryReader& in) {
    id = readId<BuildingId>(in);
    replacedBy = readId<core::DefId>(in);
    household = readId<HouseholdId>(in);
    def = readId<core::DefId>(in);
    origin = readPos(in);
    state = static_cast<BuildState>(in.u8());
    workDone = in.fixed();
    readVector(in, delivered, [](BinaryReader& r) { return r.i32(); });
    settlement = readId<SettlementId>(in);
    lastProgressTick = in.i64();
    alive = in.boolean();
}

void ResourceNode::write(BinaryWriter& out) const {
    writeId(out, id);
    writeId(out, def);
    writePos(out, tile);
    out.fixed(workDone);
    out.boolean(depleted);
    out.i64(regrowAtTick);
    out.boolean(alive);
}
void ResourceNode::read(BinaryReader& in) {
    id = readId<ResourceNodeId>(in);
    def = readId<core::DefId>(in);
    tile = readPos(in);
    workDone = in.fixed();
    depleted = in.boolean();
    regrowAtTick = in.i64();
    alive = in.boolean();
}

void Job::write(BinaryWriter& out) const {
    out.u8(static_cast<std::uint8_t>(kind));
    out.u8(static_cast<std::uint8_t>(category));
    out.u8(static_cast<std::uint8_t>(phase));
    writeId(out, node);
    writeId(out, building);
    writeId(out, stack);
    out.u32(stackGeneration);
    writeId(out, animal);
    writeId(out, recipe);
    writeId(out, person);
    writeId(out, crop);
    writeId(out, wantedItem);
    out.i32(wantedCount);
    writePos(out, target);
    writePos(out, deliverTo);
    writeId(out, deliverBuilding);
    out.fixed(workDone);
    out.fixed(workRequired);
    writeVector(out, path, [](BinaryWriter& w, core::TilePos p) { writePos(w, p); });
    out.u32(static_cast<std::uint32_t>(pathIndex));
}
void Job::read(BinaryReader& in) {
    kind = static_cast<JobKind>(in.u8());
    category = static_cast<content::WorkCategory>(in.u8());
    phase = static_cast<JobPhase>(in.u8());
    node = readId<ResourceNodeId>(in);
    building = readId<BuildingId>(in);
    stack = readId<ItemStackId>(in);
    stackGeneration = in.u32();
    animal = readId<AnimalId>(in);
    recipe = readId<core::DefId>(in);
    person = readId<PersonId>(in);
    crop = readId<core::DefId>(in);
    wantedItem = readId<core::DefId>(in);
    wantedCount = in.i32();
    target = readPos(in);
    deliverTo = readPos(in);
    deliverBuilding = readId<BuildingId>(in);
    workDone = in.fixed();
    workRequired = in.fixed();
    readVector(in, path, [](BinaryReader& r) { return readPos(r); });
    pathIndex = in.u32();
}

void Person::write(BinaryWriter& out) const {
    writeId(out, id);
    out.str(name);
    out.u8(static_cast<std::uint8_t>(sex));
    out.u8(static_cast<std::uint8_t>(stage));
    out.i64(birthTick);
    out.i32(ageYears);
    out.fixed(harmFromHunger);
    out.fixed(harmFromThirst);
    out.fixed(harmFromCold);
    out.fixed(harmFromIllness);
    writeId(out, settlement);
    writeId(out, household);
    writeId(out, father);
    writeId(out, mother);
    writeId(out, spouse);
    writeWorldPos(out, pos);
    writePos(out, tile);
    out.fixed(satiety);
    out.fixed(hydration);
    out.fixed(rest);
    out.fixed(health);
    out.fixed(bodyTempOffset);
    out.boolean(asleep);
    out.boolean(alive);
    out.u8(static_cast<std::uint8_t>(ailment));
    writeId(out, tendedBy);
    out.fixed(ailmentSeverity);
    out.fixed(ailmentTended);
    out.i64(ailmentSinceTick);
    out.boolean(buried);
    out.fixed(strength);
    out.fixed(endurance);
    out.fixed(dexterity);
    for (const auto& skill : skills) {
        out.i32(skill.level);
        out.fixed(skill.progress);
    }
    out.i64(tradeTakenUpTick);
    writeVector(out, traits, [](BinaryWriter& w, core::DefId d) { writeId(w, d); });
    writeVector(out, knownMethods, [](BinaryWriter& w, core::DefId d) { writeId(w, d); });
    out.u8(static_cast<std::uint8_t>(profession));
    writeId(out, carrying);
    writeId(out, equippedTool);
    writeVector(out, worn, [](BinaryWriter& w, ItemStackId s) { writeId(w, s); });
    for (std::int32_t n : recentFoodGroups) out.i32(n);
    job.write(out);
    out.u8(static_cast<std::uint8_t>(idleReason));
    out.i64(nextPlanTick);
    out.i64(deathTick);
    out.str(deathCause);
}
void Person::read(BinaryReader& in) {
    id = readId<PersonId>(in);
    name = in.str();
    sex = static_cast<Sex>(in.u8());
    stage = static_cast<LifeStage>(in.u8());
    birthTick = in.i64();
    ageYears = in.i32();
    harmFromHunger = in.fixed();
    harmFromThirst = in.fixed();
    harmFromCold = in.fixed();
    harmFromIllness = in.fixed();
    settlement = readId<SettlementId>(in);
    household = readId<HouseholdId>(in);
    father = readId<PersonId>(in);
    mother = readId<PersonId>(in);
    spouse = readId<PersonId>(in);
    pos = readWorldPos(in);
    tile = readPos(in);
    satiety = in.fixed();
    hydration = in.fixed();
    rest = in.fixed();
    health = in.fixed();
    bodyTempOffset = in.fixed();
    asleep = in.boolean();
    alive = in.boolean();
    ailment = static_cast<Ailment>(in.u8());
    tendedBy = readId<PersonId>(in);
    ailmentSeverity = in.fixed();
    ailmentTended = in.fixed();
    ailmentSinceTick = in.i64();
    buried = in.boolean();
    strength = in.fixed();
    endurance = in.fixed();
    dexterity = in.fixed();
    for (auto& skill : skills) {
        skill.level = in.i32();
        skill.progress = in.fixed();
    }
    tradeTakenUpTick = in.i64();
    readVector(in, traits, [](BinaryReader& r) { return readId<core::DefId>(r); });
    readVector(in, knownMethods, [](BinaryReader& r) { return readId<core::DefId>(r); });
    profession = static_cast<content::WorkCategory>(in.u8());
    carrying = readId<ItemStackId>(in);
    equippedTool = readId<ItemStackId>(in);
    readVector(in, worn, [](BinaryReader& r) { return readId<ItemStackId>(r); });
    for (auto& n : recentFoodGroups) n = in.i32();
    job.read(in);
    idleReason = static_cast<IdleReason>(in.u8());
    nextPlanTick = in.i64();
    deathTick = in.i64();
    deathCause = in.str();
}

void Animal::write(BinaryWriter& out) const {
    writeId(out, id);
    writeId(out, def);
    writeId(out, owner);
    writeWorldPos(out, pos);
    writePos(out, tile);
    writePos(out, grazeTarget);
    out.i32(ageDays);
    out.u8(static_cast<std::uint8_t>(sex));
    out.fixed(condition);
    out.i64(nextShearTick);
    out.i64(nextMilkTick);
    out.i64(nextBreedTick);
    out.boolean(penned);
    out.boolean(alive);
}
void Animal::read(BinaryReader& in) {
    id = readId<AnimalId>(in);
    def = readId<core::DefId>(in);
    owner = readId<SettlementId>(in);
    pos = readWorldPos(in);
    tile = readPos(in);
    grazeTarget = readPos(in);
    ageDays = in.i32();
    sex = static_cast<Sex>(in.u8());
    condition = in.fixed();
    nextShearTick = in.i64();
    nextMilkTick = in.i64();
    nextBreedTick = in.i64();
    penned = in.boolean();
    alive = in.boolean();
}

void Zone::write(BinaryWriter& out) const {
    writeId(out, id);
    out.str(label);
    out.u8(static_cast<std::uint8_t>(kind));
    out.u8(static_cast<std::uint8_t>(mode));
    writeVector(out, categories,
                [](BinaryWriter& w, content::WorkCategory c) { w.u8(static_cast<std::uint8_t>(c)); });
    writeId(out, settlement);
    out.boolean(alive);
    out.boolean(playerDrawn);
    writeVector(out, areas, [](BinaryWriter& w, const std::vector<core::WorldPos>& polygon) {
        writeVector(w, polygon, [](BinaryWriter& inner, core::WorldPos p) { writeWorldPos(inner, p); });
    });
    writeVector(out, cutouts, [](BinaryWriter& w, const std::vector<core::WorldPos>& polygon) {
        writeVector(w, polygon, [](BinaryWriter& inner, core::WorldPos p) { writeWorldPos(inner, p); });
    });
    writeVector(out, tiles, [](BinaryWriter& w, core::TilePos p) { writePos(w, p); });
    writePos(out, bounds.min);
    writePos(out, bounds.max);
    writeWorldRect(out, worldBounds);
    writeWorldPos(out, centroid);
}
void Zone::read(BinaryReader& in) {
    id = readId<ZoneId>(in);
    label = in.str();
    kind = static_cast<ZoneKind>(in.u8());
    mode = static_cast<ZoneMode>(in.u8());
    readVector(in, categories,
               [](BinaryReader& r) { return static_cast<content::WorkCategory>(r.u8()); });
    settlement = readId<SettlementId>(in);
    alive = in.boolean();
    playerDrawn = in.boolean();
    readVector(in, areas, [](BinaryReader& r) {
        std::vector<core::WorldPos> polygon;
        readVector(r, polygon, [](BinaryReader& inner) { return readWorldPos(inner); });
        return polygon;
    });
    readVector(in, cutouts, [](BinaryReader& r) {
        std::vector<core::WorldPos> polygon;
        readVector(r, polygon, [](BinaryReader& inner) { return readWorldPos(inner); });
        return polygon;
    });
    readVector(in, tiles, [](BinaryReader& r) { return readPos(r); });
    bounds.min = readPos(in);
    bounds.max = readPos(in);
    worldBounds = readWorldRect(in);
    centroid = readWorldPos(in);
}

void Household::write(BinaryWriter& out) const {
    writeId(out, id);
    writeId(out, settlement);
    out.u8(static_cast<std::uint8_t>(trade));
    writePos(out, seat);
    out.boolean(seated);
    out.boolean(alive);
}
void Household::read(BinaryReader& in) {
    id = readId<HouseholdId>(in);
    settlement = readId<SettlementId>(in);
    trade = static_cast<content::WorkCategory>(in.u8());
    seat = readPos(in);
    seated = in.boolean();
    alive = in.boolean();
}

void Settlement::write(BinaryWriter& out) const {
    writeId(out, id);
    out.str(name);
    writeId(out, ethnos);
    writePos(out, hearth);
    writeVector(out, members, [](BinaryWriter& w, PersonId p) { writeId(w, p); });
    writeVector(out, zones, [](BinaryWriter& w, ZoneId z) { writeId(w, z); });
    writeVector(out, traditions, [](BinaryWriter& w, core::DefId d) { writeId(w, d); });
    for (Fixed p : priorities) out.fixed(p);
    out.i32(zonedForPopulation);
    writePos(out, gateway);
    out.boolean(hasGateway);
    writeVector(out, planAttempts, [](BinaryWriter& w, std::int32_t v) { w.i32(v); });
    out.fixed(foodDays);
    out.fixed(larderDays);
    out.fixed(tradeDiscipline);
    out.i32(brokenUnsown);
    writeVector(out, seedReserve, [](BinaryWriter& w, std::int32_t v) { w.i32(v); });
    out.boolean(alive);
}
void Settlement::read(BinaryReader& in) {
    id = readId<SettlementId>(in);
    name = in.str();
    ethnos = readId<core::DefId>(in);
    hearth = readPos(in);
    readVector(in, members, [](BinaryReader& r) { return readId<PersonId>(r); });
    readVector(in, zones, [](BinaryReader& r) { return readId<ZoneId>(r); });
    readVector(in, traditions, [](BinaryReader& r) { return readId<core::DefId>(r); });
    for (auto& p : priorities) p = in.fixed();
    zonedForPopulation = in.i32();
    gateway = readPos(in);
    hasGateway = in.boolean();
    readVector(in, planAttempts, [](BinaryReader& r) { return r.i32(); });
    foodDays = in.fixed();
    larderDays = in.fixed();
    tradeDiscipline = in.fixed();
    brokenUnsown = in.i32();
    readVector(in, seedReserve, [](BinaryReader& r) { return r.i32(); });
    alive = in.boolean();
}

} // namespace sim

namespace sim {
namespace {

using core::BinaryReader;
using core::BinaryWriter;

// What each part of the file is, and which version of it this build writes.
// Sections carry their own length, so a reader that does not understand one can
// step over it instead of losing the file.
constexpr std::uint64_t kMagic = 0x5241534E45495441ULL;   // "ATIENSAR" little-endian
constexpr std::uint32_t kWorldParamsVersion = 3;
constexpr std::uint32_t kCommunityVersion = 1;

void writeWorldParams(BinaryWriter& out, const generation::WorldMapParams& p) {
    out.u64(p.seed);
    out.i32(p.width);
    out.i32(p.height);
    out.i32(p.seaPercent);
    out.i32(p.sites);
    out.i32(p.siteSpacing);
    out.i32(p.riverFlow);
    out.u8(static_cast<std::uint8_t>(p.playedBiome));
}
generation::WorldMapParams readWorldParams(BinaryReader& in) {
    generation::WorldMapParams p;
    p.seed = in.u64();
    p.width = in.i32();
    p.height = in.i32();
    p.seaPercent = in.i32();
    p.sites = in.i32();
    p.siteSpacing = in.i32();
    p.riverFlow = in.i32();
    p.playedBiome = static_cast<generation::Biome>(in.u8());
    return p;
}

void writeConfig(BinaryWriter& out, const WorldConfig& c) {
    out.u64(c.seed);
    out.i32(c.mapWidth);
    out.i32(c.mapHeight);
    out.i32(c.worldCells);
    writePos(out, c.localCell);
    out.str(c.ethnos);
    out.i32(c.startingPopulation);
}
WorldConfig readConfig(BinaryReader& in) {
    WorldConfig c;
    c.seed = in.u64();
    c.mapWidth = in.i32();
    c.mapHeight = in.i32();
    c.worldCells = in.i32();
    c.localCell = readPos(in);
    c.ethnos = in.str();
    c.startingPopulation = in.i32();
    return c;
}

// A section: its kind, its version, its length, then its bytes. The length is
// what makes the file survive a section this build does not know.
void writeSection(BinaryWriter& out, std::uint32_t kind, std::uint32_t version,
                  const std::vector<std::uint8_t>& body) {
    out.u32(kind);
    out.u32(version);
    out.u64(static_cast<std::uint64_t>(body.size()));
    for (std::uint8_t b : body) out.u8(b);
}

} // namespace

void World::write(BinaryWriter& out) const {
    out.i64(tick_);
    writeConfig(out, cfg_);
    writePos(out, localCell_);
    writePos(out, localBlock_);
    out.i32(ailing_);
    out.i32(unburied_);
    out.fixed(outdoorTemp_);
    out.i32(exploredCount_);

    // The ground. Written as it stands, tile by tile: it is the one thing in the
    // world that cannot be worked out again from anything smaller.
    out.i32(map_.width());
    out.i32(map_.height());
    for (std::int32_t y = 0; y < map_.height(); ++y)
        for (std::int32_t x = 0; x < map_.width(); ++x) {
            map_.at({x, y}).write(out);
            out.boolean(map_.blocked({x, y}));
        }

    writeVector(out, nodesAtFirst_, [](BinaryWriter& w, std::int32_t v) { w.i32(v); });
    writeVector(out, people_, [](BinaryWriter& w, const Person& p) { p.write(w); });
    writeVector(out, stacks_, [](BinaryWriter& w, const ItemStack& s) { s.write(w); });
    writeVector(out, buildings_, [](BinaryWriter& w, const Building& b) { b.write(w); });
    writeVector(out, nodes_, [](BinaryWriter& w, const ResourceNode& n) { n.write(w); });
    writeVector(out, households_, [](BinaryWriter& w, const Household& h) { h.write(w); });
    writeVector(out, zones_, [](BinaryWriter& w, const Zone& z) { z.write(w); });
    writeVector(out, animals_, [](BinaryWriter& w, const Animal& a) { a.write(w); });
    writeVector(out, settlements_, [](BinaryWriter& w, const Settlement& st) { st.write(w); });
    writeVector(out, freeStacks_, [](BinaryWriter& w, ItemStackId id) { writeId(w, id); });

    // Animal needs live in ECS while the legacy animal record remains the
    // compatibility/save boundary. Keep one entry per stable legacy index so
    // dead slots and recycled ids cannot shift the component state.
    out.u32(static_cast<std::uint32_t>(animals_.size()));
    for (std::uint32_t index = 0; index < animals_.size(); ++index) {
        const entt::entity entity = ecsEntity(ecs::Kind::Animal, index);
        const bool present = entity != entt::null && ecs_.all_of<ecs::Hunger, ecs::Thirst,
                                                                  ecs::Fatigue, ecs::SleepState>(entity);
        out.boolean(present);
        if (!present) continue;
        out.fixed(ecs_.get<ecs::Hunger>(entity).value);
        out.fixed(ecs_.get<ecs::Thirst>(entity).value);
        out.fixed(ecs_.get<ecs::Fatigue>(entity).value);
        out.boolean(ecs_.get<ecs::SleepState>(entity).asleep);
    }

    // Who has laid claim to what, and where every random stream stands. Without
    // the streams a loaded game is a different game from the one that was saved
    // the moment anything rolls a die.
    out.u32(static_cast<std::uint32_t>(reservations_.size()));
    {
        std::vector<std::pair<std::uint64_t, PersonId>> ordered(reservations_.begin(),
                                                                reservations_.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [key, who] : ordered) {
            out.u64(key);
            writeId(out, who);
        }
    }
    out.u32(static_cast<std::uint32_t>(rngs_.size()));
    {
        std::vector<std::pair<std::uint64_t, core::Rng>> ordered(rngs_.begin(), rngs_.end());
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& [stream, rng] : ordered) {
            out.u64(stream);
            out.u64(rng.state());
            out.u64(rng.inc());
        }
    }
}

bool World::read(BinaryReader& in) {
    tick_ = in.i64();
    cfg_ = readConfig(in);
    localCell_ = readPos(in);
    localBlock_ = readPos(in);
    ailing_ = in.i32();
    unburied_ = in.i32();
    outdoorTemp_ = in.fixed();
    exploredCount_ = in.i32();

    const std::int32_t width = in.i32();
    const std::int32_t height = in.i32();
    if (!in.ok() || width <= 0 || height <= 0 || width > 4096 || height > 4096) return false;
    map_.resize(width, height);
    for (std::int32_t y = 0; y < height; ++y)
        for (std::int32_t x = 0; x < width; ++x) {
            map_.at({x, y}).read(in);
            map_.setBlocked({x, y}, in.boolean());
        }

    readVector(in, nodesAtFirst_, [](BinaryReader& r) { return r.i32(); });
    readVector(in, people_, [](BinaryReader& r) { Person p; p.read(r); return p; });
    readVector(in, stacks_, [](BinaryReader& r) { ItemStack s; s.read(r); return s; });
    readVector(in, buildings_, [](BinaryReader& r) { Building b; b.read(r); return b; });
    readVector(in, nodes_, [](BinaryReader& r) { ResourceNode n; n.read(r); return n; });
    readVector(in, households_, [](BinaryReader& r) { Household h; h.read(r); return h; });
    readVector(in, zones_, [](BinaryReader& r) { Zone z; z.read(r); return z; });
    readVector(in, animals_, [](BinaryReader& r) { Animal a; a.read(r); return a; });
    readVector(in, settlements_, [](BinaryReader& r) { Settlement st; st.read(r); return st; });
    readVector(in, freeStacks_, [](BinaryReader& r) { return readId<ItemStackId>(r); });

    struct SavedAnimalNeeds {
        Fixed hunger = core::kOne;
        Fixed thirst = core::kOne;
        Fixed fatigue = core::kOne;
        bool asleep = false;
        bool present = false;
    };
    std::vector<SavedAnimalNeeds> savedAnimalNeeds;
    const std::uint32_t savedAnimalCount = in.u32();
    if (savedAnimalCount > animals_.size() + 1'000'000u) return false;
    savedAnimalNeeds.resize(savedAnimalCount);
    for (auto& saved : savedAnimalNeeds) {
        saved.present = in.boolean();
        if (!saved.present) continue;
        saved.hunger = in.fixed();
        saved.thirst = in.fixed();
        saved.fatigue = in.fixed();
        saved.asleep = in.boolean();
    }

    reservations_.clear();
    const std::uint32_t claims = in.u32();
    for (std::uint32_t i = 0; i < claims && in.ok(); ++i) {
        const std::uint64_t key = in.u64();
        reservations_[key] = readId<PersonId>(in);
    }
    rngs_.clear();
    const std::uint32_t streams = in.u32();
    for (std::uint32_t i = 0; i < streams && in.ok(); ++i) {
        const std::uint64_t stream = in.u64();
        const std::uint64_t state = in.u64();
        const std::uint64_t inc = in.u64();
        core::Rng rng(0, stream);
        rng.setState(state, inc);
        rngs_.emplace(stream, rng);
    }
    if (in.ok()) {
        syncEcs();
        const std::size_t count = std::min(savedAnimalNeeds.size(), animals_.size());
        for (std::size_t index = 0; index < count; ++index) {
            if (!savedAnimalNeeds[index].present || !animals_[index].alive) continue;
            const entt::entity entity = ecsEntity(ecs::Kind::Animal, static_cast<std::uint32_t>(index));
            if (entity == entt::null) continue;
            ecs_.emplace_or_replace<ecs::Hunger>(entity, savedAnimalNeeds[index].hunger, core::kZero);
            ecs_.emplace_or_replace<ecs::Thirst>(entity, savedAnimalNeeds[index].thirst, core::kZero);
            ecs_.emplace_or_replace<ecs::Fatigue>(entity, savedAnimalNeeds[index].fatigue, core::kZero);
            ecs_.emplace_or_replace<ecs::SleepState>(entity, savedAnimalNeeds[index].asleep);
        }
        report_.ticks = tick_;
        report_.population = static_cast<std::int32_t>(ecs_.view<ecs::Person>().size());
    }
    return in.ok();
}

Game newGame(const content::ContentDb& db, const WorldConfig& base,
             const generation::WorldMapParams& worldParams) {
    Game game;
    game.worldParams = worldParams;
    game.base = base;
    *game.country = generation::generateWorldMap(worldParams);

    // Every site on the country gets the same founding band on the same morning.
    // The played one first, so it is community zero whatever the map does.
    for (const auto& site : game.country->sites) {
        WorldConfig cfg = base;
        cfg.seed = worldParams.seed;
        cfg.localCell = site.cell;
        cfg.ethnos = site.ethnos;
        cfg.sharedWorldMap = game.country.get();
        cfg.startingPopulation = site.played ? base.startingPopulation
                                             : generation::kFoundingCommunity;
        auto world = std::make_unique<World>(db, cfg);
        if (site.played) {
            game.communities.insert(game.communities.begin(), std::move(world));
            game.watching = 0;
        } else {
            game.communities.push_back(std::move(world));
        }
    }
    if (game.communities.empty()) {
        WorldConfig cfg = base;
        cfg.seed = worldParams.seed;
        game.communities.push_back(std::make_unique<World>(db, cfg));
    }
    return game;
}

bool saveGame(const Game& game, const std::filesystem::path& file, std::string* error) {
    BinaryWriter out;
    out.u64(kMagic);
    out.u32(kSaveFormatVersion);
    out.u64(game.worldParams.seed);
    out.i64(game.communities.empty() ? 0 : game.communities.front()->tickCount());
    out.u32(static_cast<std::uint32_t>(game.communities.size()));
    out.u32(static_cast<std::uint32_t>(game.watching));

    {
        BinaryWriter body;
        writeWorldParams(body, game.worldParams);
        writeConfig(body, game.base);
        body.i32(game.worldParams.plates);
        body.i32(game.worldParams.erosionPasses);
        body.i32(game.worldParams.rainfallPercent);
        const auto hybrid = game.country ? game.country->hybridTerrain : nullptr;
        body.boolean(bool(hybrid));
        if (hybrid) body.str(generation::saveHybridTerrain(*hybrid));
        const auto foundation = game.country ? game.country->terrainFoundation : nullptr;
        body.boolean(bool(foundation));
        if (foundation) body.str(generation::saveTerrainFoundation(*foundation));
        writeSection(out, 1, kWorldParamsVersion, body.data());
    }
    for (const auto& community : game.communities) {
        BinaryWriter body;
        community->write(body);
        writeSection(out, 2, kCommunityVersion, body.data());
    }

    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    if (!stream) {
        if (error) *error = "cannot open " + file.string() + " for writing";
        return false;
    }
    stream.write(reinterpret_cast<const char*>(out.data().data()),
                 static_cast<std::streamsize>(out.size()));
    if (!stream) {
        if (error) *error = "writing " + file.string() + " failed";
        return false;
    }
    return true;
}

namespace {
bool readWholeFile(const std::filesystem::path& file, std::vector<std::uint8_t>& out) {
    std::ifstream stream(file, std::ios::binary | std::ios::ate);
    if (!stream) return false;
    const std::streamsize size = stream.tellg();
    if (size < 0) return false;
    out.resize(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(stream);
}
} // namespace

bool loadGame(const content::ContentDb& db, const std::filesystem::path& file, Game& out,
              std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!readWholeFile(file, bytes)) {
        if (error) *error = "cannot read " + file.string();
        return false;
    }
    BinaryReader in(bytes);
    if (in.u64() != kMagic) {
        if (error) *error = "not a save file";
        return false;
    }
    const std::uint32_t version = in.u32();
    if (version != kSaveFormatVersion) {
        // No compatibility layer yet, on purpose: while the game is being built,
        // an old save is one you start again from, and the version is here so
        // that a later build can say exactly that rather than crash.
        if (error)
            *error = "save is version " + std::to_string(version) + ", this build writes " +
                     std::to_string(kSaveFormatVersion);
        return false;
    }
    in.u64();                       // seed, repeated in the parameters below
    in.i64();                       // tick, likewise
    const std::uint32_t communities = in.u32();
    const std::uint32_t watching = in.u32();

    Game game;
    game.watching = watching;
    std::uint32_t loaded = 0;
    while (in.ok() && in.remaining() >= 16) {
        const std::uint32_t kind = in.u32();
        const auto sectionVersion = in.u32();
        const std::uint64_t length = in.u64();
        const std::size_t body = in.position();
        if (length > in.remaining()) { in.fail(); break; }
        if (kind == 1) {
            game.worldParams = readWorldParams(in);
            game.base = readConfig(in);
            // v1 predates hybrid terrain: do not silently add mountains to an
            // old game. v2 restores the baked forms before climate/drainage.
            game.worldParams.hybridTerrain = false;
            game.worldParams.stagedTerrain = false;
            try {
                if (sectionVersion < 1 || sectionVersion > kWorldParamsVersion) { in.fail(); break; }
                if (sectionVersion >= 2) {
                    game.worldParams.plates = in.i32();
                    game.worldParams.erosionPasses = in.i32();
                    game.worldParams.rainfallPercent = in.i32();
                    game.worldParams.hybridTerrain = in.boolean();
                    if (game.worldParams.hybridTerrain)
                        game.worldParams.hybridSnapshot = generation::loadHybridTerrain(
                            in.str(), game.worldParams.width, game.worldParams.height);
                }
                if (sectionVersion >= 3) {
                    game.worldParams.stagedTerrain = in.boolean();
                    if (game.worldParams.stagedTerrain)
                        game.worldParams.foundationSnapshot = generation::loadTerrainFoundation(
                            in.str(), game.worldParams.width, game.worldParams.height);
                }
                if (!in.ok() || in.position() > body + length || game.worldParams.width <= 0 ||
                    game.worldParams.height <= 0 || std::int64_t(game.worldParams.width) * game.worldParams.height > 4*1024*1024 ||
                    game.worldParams.seaPercent < 0 || game.worldParams.seaPercent > 100) { in.fail(); break; }
                *game.country = generation::generateWorldMap(game.worldParams);
                game.worldParams.hybridSnapshot.reset();
                game.worldParams.foundationSnapshot.reset();
            } catch (const std::exception&) { in.fail(); break; }
        } else if (kind == 2) {
            WorldConfig cfg = game.base;
            cfg.sharedWorldMap = game.country.get();
            auto world = std::make_unique<World>(db, cfg, World::FromSave{});
            if (!world->read(in)) { in.fail(); break; }
            game.communities.push_back(std::move(world));
            ++loaded;
        }
        in.seek(body + static_cast<std::size_t>(length));
    }
    if (!in.ok() || loaded != communities) {
        if (error) *error = "save is damaged or incomplete";
        return false;
    }
    if (game.watching >= game.communities.size()) game.watching = 0;
    out = std::move(game);
    return true;
}

SaveSummary inspectSave(const std::filesystem::path& file) {
    SaveSummary summary;
    std::vector<std::uint8_t> bytes;
    if (!readWholeFile(file, bytes)) {
        summary.note = "unreadable";
        return summary;
    }
    BinaryReader in(bytes);
    if (in.u64() != kMagic) {
        summary.note = "not a save";
        return summary;
    }
    summary.formatVersion = in.u32();
    summary.seed = in.u64();
    summary.tick = in.i64();
    summary.communities = static_cast<std::int32_t>(in.u32());
    summary.readable = in.ok() && summary.formatVersion == kSaveFormatVersion;
    if (!summary.readable && summary.note.empty())
        summary.note = "version " + std::to_string(summary.formatVersion);
    return summary;
}

} // namespace sim
