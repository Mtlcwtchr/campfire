#include "game/simulation/world.hpp"
#include "game/ecs/life/components.hpp"
#include "game/ecs/resources/components.hpp"

#include "game/ai/components.hpp"
#include "game/ai/planner.hpp"

#include <algorithm>
#include <array>

#include "game/generation/local_map_gen.hpp"
#include "game/simulation/inventory.hpp"
#include "game/simulation/livestock.hpp"
#include "game/simulation/needs.hpp"
#include "game/simulation/population.hpp" // WorldBuilder bootstrap API
#include "game/simulation/zones.hpp"
#include "game/world/height_field.hpp"
#include "game/world/weather_temperature.hpp"

namespace sim {
namespace {

// How far a community can see from the hearth it has just chosen.
constexpr std::int32_t kInitialSightRadius = 20;

constexpr std::array<std::string_view, static_cast<std::size_t>(Terrain::Count)> kTerrainNames{
    "grass", "forest", "dirt", "rock", "water", "sand", "marsh",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(JobKind::Count)> kJobKindNames{
    "none", "harvest", "craft", "construct", "haul_to_store", "haul_to_site",
    "fetch_tool", "fell", "clear", "till", "sow", "reap", "shear", "milk", "slaughter",
    "hunt", "tame", "demolish", "pen", "wear", "scout", "tend", "bury", "eat", "drink", "sleep",
    "wander",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(IdleReason::Count)> kIdleReasonNames{
    "working", "between_jobs", "no_work_available", "no_tool_for_any_job",
    "no_materials_for_any_job", "no_reachable_target", "not_skilled_enough", "unfit",
};

constexpr Fixed kTileSampleOffset = Fixed::ratio(1, 4);

bool resourceAlive(const World& w, const ResourceNode& node) {
    return w.resourceNodeAlive(node.id);
}

bool resourceDepleted(const World& w, const ResourceNode& node) {
    return w.resourceNodeDepleted(node.id);
}

std::vector<WorldPos> polygonOfTile(TilePos tile) {
    const WorldRect box = core::tileBounds(tile);
    return {{box.min.x, box.min.y}, {box.max.x, box.min.y},
            {box.max.x, box.max.y}, {box.min.x, box.max.y}};
}

WorldRect boundsOf(const std::vector<WorldPos>& polygon) {
    if (polygon.empty()) return {{}, {}};
    WorldRect box{polygon.front(), polygon.front()};
    for (WorldPos p : polygon) {
        box.min.x = core::min(box.min.x, p.x);
        box.min.y = core::min(box.min.y, p.y);
        box.max.x = core::max(box.max.x, p.x);
        box.max.y = core::max(box.max.y, p.y);
    }
    return box;
}

bool containsAnyPoint(const std::vector<std::vector<WorldPos>>& polygons, WorldPos p) {
    for (const auto& polygon : polygons)
        if (core::pointInPolygon(polygon, p)) return true;
    return false;
}

bool zoneTileCovered(const Zone& z, TilePos tile) {
    if (z.areas.empty()) return false;
    const WorldRect box = core::tileBounds(tile);
    bool positive = false;
    for (const auto& polygon : z.areas)
        if (core::polygonIntersectsRect(polygon, box)) { positive = true; break; }
    if (!positive) return false;

    const WorldPos c = core::tileCentre(tile);
    const std::array<WorldPos, 9> samples{{
            c,
            {c.x - kTileSampleOffset, c.y}, {c.x + kTileSampleOffset, c.y},
            {c.x, c.y - kTileSampleOffset}, {c.x, c.y + kTileSampleOffset},
            {c.x - kTileSampleOffset, c.y - kTileSampleOffset},
            {c.x + kTileSampleOffset, c.y - kTileSampleOffset},
            {c.x - kTileSampleOffset, c.y + kTileSampleOffset},
            {c.x + kTileSampleOffset, c.y + kTileSampleOffset},
    }};
    for (WorldPos sample : samples)
        if (z.contains(sample)) return true;
    return false;
}

void rebuildZoneCache(World& w, Zone& z) {
    const std::uint64_t bit = std::uint64_t(1) << z.id.value;
    for (TilePos t : z.tiles)
        if (w.map().inBounds(t)) w.map().at(t).zoneMask &= ~bit;

    z.tiles.clear();
    z.bounds = {{0, 0}, {0, 0}};
    z.worldBounds = {{}, {}};
    z.centroid = {};
    if (z.areas.empty()) return;

    bool haveWorldBounds = false;
    bool haveTileBounds = false;
    Fixed sx = core::kZero;
    Fixed sy = core::kZero;
    std::int64_t count = 0;
    for (const auto& polygon : z.areas) {
        for (WorldPos p : polygon) {
            if (!haveWorldBounds) {
                z.worldBounds = {p, p};
                haveWorldBounds = true;
            } else {
                z.worldBounds.min.x = core::min(z.worldBounds.min.x, p.x);
                z.worldBounds.min.y = core::min(z.worldBounds.min.y, p.y);
                z.worldBounds.max.x = core::max(z.worldBounds.max.x, p.x);
                z.worldBounds.max.y = core::max(z.worldBounds.max.y, p.y);
            }
            sx += p.x;
            sy += p.y;
            ++count;
        }
    }
    if (count > 0) z.centroid = {sx / count, sy / count};

    for (std::int32_t y = 0; y < w.map().height(); ++y)
        for (std::int32_t x = 0; x < w.map().width(); ++x) {
            const TilePos tile{x, y};
            if (!zoneTileCovered(z, tile)) continue;
            w.map().at(tile).zoneMask |= bit;
            z.tiles.push_back(tile);
            if (!haveTileBounds) {
                z.bounds = {tile, {tile.x + 1, tile.y + 1}};
                haveTileBounds = true;
            } else {
                z.bounds.min.x = std::min(z.bounds.min.x, tile.x);
                z.bounds.min.y = std::min(z.bounds.min.y, tile.y);
                z.bounds.max.x = std::max(z.bounds.max.x, tile.x + 1);
                z.bounds.max.y = std::max(z.bounds.max.y, tile.y + 1);
            }
        }
}

} // namespace

std::string_view terrainName(Terrain t) {
    const auto i = static_cast<std::size_t>(t);
    return i < kTerrainNames.size() ? kTerrainNames[i] : "unknown";
}

bool terrainPassable(Terrain t) { return t != Terrain::Water; }

std::uint8_t grassFor(Terrain t) {
    switch (t) {
        case Terrain::Grass:  return kGrassFull;
        case Terrain::Marsh:  return kGrassFull * 3 / 4;
        // Floodplain silt carries grazing too - it is what a river people
        // pastures on, and counting only grass starved every flock on the map.
        case Terrain::Dirt:   return kGrassFull / 2;
        case Terrain::Forest: return kGrassFull / 3;
        default:               return 0;
    }
}

Fixed tileMoveCost(const Tile& t) {
    const Fixed base = terrainMoveCost(t.terrain);
    if (t.traffic == 0) return base;
    // A well-trodden way is a third quicker than the ground it was worn into.
    const std::int32_t wear = std::min<std::int32_t>(t.traffic, kPathTraffic);
    return base * (core::kOne - Fixed::ratio(wear, kPathTraffic * 3));
}

Fixed terrainMoveCost(Terrain t) {
    switch (t) {
        case Terrain::Grass:  return core::kOne;
        case Terrain::Dirt:   return core::kOne;
        case Terrain::Sand:   return Fixed::ratio(6, 5);
        case Terrain::Forest: return Fixed::ratio(3, 2);
        case Terrain::Rock:   return Fixed::ratio(7, 5);
        case Terrain::Marsh:  return Fixed::fromInt(2);
        case Terrain::Water:  return Fixed::fromInt(8);
        default:              return core::kOne;
    }
}

std::string_view jobKindName(JobKind k) {
    const auto i = static_cast<std::size_t>(k);
    return i < kJobKindNames.size() ? kJobKindNames[i] : "unknown";
}

std::string_view idleReasonName(IdleReason r) {
    const auto i = static_cast<std::size_t>(r);
    return i < kIdleReasonNames.size() ? kIdleReasonNames[i] : "unknown";
}

// ---------------------------------------------------------------------------

void TileMap::resize(std::int32_t w, std::int32_t h) {
    width_ = w;
    height_ = h;
    tiles_.assign(std::size_t(w) * h, Tile{});
    blocked_.assign(std::size_t(w) * h, 0);
}

bool TileMap::passable(TilePos p) const { return inBounds(p) && !blocked(p); }

void TileMap::setBlocked(TilePos p, bool b) {
    if (!inBounds(p)) return;
    blocked_[index(p)] = b ? 1 : 0;
}

// ---------------------------------------------------------------------------

bool Person::knows(DefId method) const {
    if (!method.valid()) return true;
    return std::find(knownMethods.begin(), knownMethods.end(), method) != knownMethods.end();
}

bool Person::canWork() const {
    if (!alive || asleep) return false;
    // Nor does somebody laid up with a wound or a fever (D98). Half severity is
    // where an ailment stops being something you work through: below it people
    // carry on, above it they are in bed and somebody else has to fetch and
    // carry for them.
    if (ailmentSeverity >= core::Fixed::ratio(1, 2)) return false;
    // Infants have needs and a routine but no work (GDD 6).
    return ageYears >= 5;
}

void setJobStack(const World& w, Job& job, ItemStackId id) {
    job.stack = id;
    job.stackGeneration = id.valid() ? w.stack(id).generation : 0;
}

std::vector<TilePos> footprintOf(const content::ContentDb& db, DefId def, TilePos origin,
                                 std::int32_t margin) {
    const auto& d = db.building(def);
    std::vector<TilePos> out;
    const std::int32_t w = d.footprintWidth + 2 * margin;
    const std::int32_t h = d.footprintDepth + 2 * margin;
    out.reserve(static_cast<std::size_t>(w) * h);
    for (std::int32_t dy = -margin; dy < d.footprintDepth + margin; ++dy)
        for (std::int32_t dx = -margin; dx < d.footprintWidth + margin; ++dx)
            out.push_back({origin.x + dx, origin.y + dy});
    return out;
}

std::vector<TilePos> Building::footprintTiles(const content::ContentDb& db) const {
    const auto& d = db.building(def);
    std::vector<TilePos> out;
    out.reserve(static_cast<std::size_t>(d.footprintWidth) * d.footprintDepth);
    for (std::int32_t dy = 0; dy < d.footprintDepth; ++dy)
        for (std::int32_t dx = 0; dx < d.footprintWidth; ++dx)
            out.push_back({origin.x + dx, origin.y + dy});
    return out;
}

bool Building::covers(const content::ContentDb& db, TilePos p) const {
    const auto& d = db.building(def);
    return p.x >= origin.x && p.x < origin.x + d.footprintWidth && p.y >= origin.y &&
           p.y < origin.y + d.footprintDepth;
}

WorldRect Building::footprintBounds(const content::ContentDb& db) const {
    const auto& d = db.building(def);
    constexpr Fixed half = Fixed::ratio(1, 2);
    return {{Fixed::fromInt(origin.x) - half, Fixed::fromInt(origin.y) - half},
            {Fixed::fromInt(origin.x + d.footprintWidth) - half,
             Fixed::fromInt(origin.y + d.footprintDepth) - half}};
}

std::vector<WorldPos> Building::footprintOutline(const content::ContentDb& db) const {
    const WorldRect box = footprintBounds(db);
    return {{box.min.x, box.min.y}, {box.max.x, box.min.y},
            {box.max.x, box.max.y}, {box.min.x, box.max.y}};
}

WorldPos Building::centreWorld(const content::ContentDb& db) const {
    return footprintBounds(db).centre();
}

bool Building::covers(const content::ContentDb& db, WorldPos p) const {
    return footprintBounds(db).contains(p);
}

bool Zone::appliesTo(WorkCategory c) const {
    if (categories.empty()) return true;
    for (auto x : categories) if (x == c) return true;
    return false;
}

TilePos Zone::centre() const {
    return core::toTile(centreWorld());
}

WorldPos Zone::centreWorld() const {
    if (!areas.empty()) return centroid;
    if (tiles.empty()) return {};
    Fixed sx = core::kZero;
    Fixed sy = core::kZero;
    for (TilePos t : tiles) {
        const WorldPos p = core::tileCentre(t);
        sx += p.x;
        sy += p.y;
    }
    return {sx / static_cast<std::int64_t>(tiles.size()), sy / static_cast<std::int64_t>(tiles.size())};
}

bool Zone::contains(WorldPos p) const {
    if (!areas.empty() && !worldBounds.contains(p)) return false;
    if (!containsAnyPoint(areas, p)) return false;
    return !containsAnyPoint(cutouts, p);
}

bool Zone::covers(TilePos p) const {
    return zoneTileCovered(*this, p);
}

// ---------------------------------------------------------------------------

World::World(const content::ContentDb& db, const WorldConfig& cfg, FromSave)
    : db_(db), cfg_(cfg) {
    // Nothing is generated: every field is about to be read from a save. The
    // country is the one handed in, because a save carries the parameters that
    // made it rather than four million cells of it.
    if (cfg.sharedWorldMap != nullptr) worldMapRef_ = cfg.sharedWorldMap;
}

World::World(const content::ContentDb& db, const WorldConfig& cfg) : db_(db), cfg_(cfg) {
    // The culture names the country it belongs in, and the generator has a
    // preset for it (GDD 5).
    const DefId ethnos = db.ethnosByName(cfg.ethnos);
    const auto biome = generation::parseBiome(ethnos.valid() ? db.ethnos(ethnos).biome : "temperate");

    // The country above the map. One cell of it is this local map: the community
    // lives in a place, and the rest of the world exists whether or not anybody
    // walks there (GDD 2.1, 4.2). It is generated, not simulated - the cells hold
    // their heights, their rivers and the peoples settled in them, and that is
    // what the camera shows when it is pulled all the way back.
    {
        if (cfg.sharedWorldMap != nullptr) {
            // Handed a country that is already built, and read straight from it:
            // the map is a pure function of the seed, and a client simulating
            // every community on it would otherwise hold a dozen identical
            // copies of four million cells.
            worldMapRef_ = cfg.sharedWorldMap;
        } else {
            generation::WorldMapParams wp;
            wp.seed = cfg.seed;
            wp.playedBiome = biome;
            if (cfg.worldCells > 0) {
                wp.width = cfg.worldCells;
                wp.height = cfg.worldCells;
                // How many peoples settle it is a density worked out from the
                // area (siteCountFor), so a smaller world is the same country
                // smaller rather than the same crowd packed into less room.
            }
            ownedWorldMap_ = generation::generateWorldMap(wp);
            worldMapRef_ = &ownedWorldMap_;
        }

        // Which cell of the country this community stands on. Its own, not the
        // map's: the map is shared, and every community is on a different part
        // of it.
        const generation::WorldMapData& country = *worldMapRef_;
        localCell_ = country.inBounds(cfg.localCell) ? cfg.localCell : country.playedCell;
        const std::int32_t half = generation::kCellsPerLocalMap / 2;
        localBlock_ = {std::clamp(localCell_.x - half, 0,
                                  std::max(0, country.width - generation::kCellsPerLocalMap)),
                       std::clamp(localCell_.y - half, 0,
                                  std::max(0, country.height - generation::kCellsPerLocalMap))};
    }

    generation::LocalMapParams params;
    // The local map is seeded from the cell it is, so the same world always
    // grows the same country under the same community.
    params.seed = cfg.seed ^ (std::uint64_t(std::uint32_t(localCell_.x)) << 32) ^
                  std::uint32_t(localCell_.y);
    params.width = cfg.mapWidth;
    params.height = cfg.mapHeight;
    // And this map is that block of the world drawn at play scale (D89).
    params.world = worldMapRef_;
    params.block = localBlock_;
    params.cellsPerSide = generation::kCellsPerLocalMap;

    // What the country says about this exact cell. Without it every local map
    // came out the same shape - a river down the middle, the same scatter, the
    // same hills - whatever the world above it said (D97).
    {
        const generation::WorldMapData& country = *worldMapRef_;
        const generation::WorldCell& cell = country.at(localCell_);
        params.hasRiver = cell.river && !cell.sea;
        params.riverOut = cell.riverOut;
        params.moisture = cell.moisture;
        params.groundFertility = cell.fertility;
        params.elevation = cell.elevation;
        params.temperature = cell.temperature;
        for (std::int32_t dir : core::kCardinalDirections) {
            const TilePos n = core::neighbour(localCell_, dir);
            if (!country.inBounds(n)) continue;
            const generation::WorldCell& other = country.at(n);
            if (other.sea) params.toSea = static_cast<std::int8_t>(dir);
            // Whoever drains into us is where our water comes in from.
            if (other.river && !other.sea && other.riverOut >= 0 &&
                core::neighbour(n, other.riverOut) == localCell_)
                params.riverIn = static_cast<std::int8_t>(dir);
        }
    }
    params = generation::presetFor(biome, params);
    // And then shaped by the country this cell is in, so a taiga map is timber
    // and a desert map is stone and sand (D97).
    params = generation::shapedByCountry(
            static_cast<std::int32_t>(worldMapRef_->at(localCell_).climate), params);
    const TilePos hearth = generation::generateLocalMap(*this, params);

    // Counted before the community has taken anything: the seed stock each
    // species is measured against for the rest of the game.
    nodesAtFirst_.assign(db.resourceNodes().size(), 0);
    for (const auto& n : nodes_)
        if (n.alive) nodesAtFirst_[n.def.value] += 1;

    Settlement st;
    st.id = SettlementId{0};
    st.name = "hearth";
    st.hearth = hearth;
    st.ethnos = ethnos;
    st.priorities.fill(core::kOne);
    settlements_.push_back(std::move(st));

    // What they can see from where they stopped. Everything beyond is unknown
    // ground until somebody walks out and looks.
    explore(hearth, kInitialSightRadius);

    // The community reads the ground it has been given and lays out its own areas
    // - where it lives, where it fells timber, where it will sow, where the flock
    // grazes (GDD 8: in unrestricted mode the settlers choose for themselves).
    layOutSettlementZones(*this, SettlementId{0});

    WorldBuilder::createStartingCommunity(*this, SettlementId{0}, settlements_[0].ethnos,
                                          cfg.startingPopulation);
    updateWeather();
    syncEcs();
    report_.population = static_cast<std::int32_t>(ecs_.view<ecs::Person>().size());
}

void World::syncEcs() {
    std::unordered_set<std::uint64_t> seen;
    seen.reserve(people_.size() + animals_.size() + buildings_.size() + nodes_.size() +
                 stacks_.size());

    const auto add = [this, &seen](ecs::Kind kind, std::uint32_t index, core::WorldPos position,
                            content::DefId definition, std::uint8_t layer, bool selectable) {
        const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(kind)) << 32) | index;
        auto found = ecsEntities_.find(key);
        const entt::entity entity = found == ecsEntities_.end() ? ecs_.create() : found->second;
        if (found == ecsEntities_.end()) {
            ecsEntities_.emplace(key, entity);
            ecs_.emplace<ecs::Identity>(entity, kind, index);
        }
        // After the initial compatibility import, Transform is ECS-owned. Keep
        // the legacy position only as the seed for newly discovered entities;
        // otherwise a final sync must not overwrite movement already committed
        // to the registry.
        core::WorldPos projected = position;
        if (tick_ > 0 && found != ecsEntities_.end() && ecs_.all_of<ecs::Transform>(entity))
            projected = ecs_.get<const ecs::Transform>(entity).value;
        ecs_.emplace_or_replace<ecs::Transform>(entity, projected);
        const ecs::CellId cell = ecs::cellForTile(core::toTile(projected), ecs::kSpatialCellExtent);
        ecs_.emplace_or_replace<ecs::SpatialCell>(entity, cell.x, cell.y);
        ecs_.emplace_or_replace<ecs::Renderable>(entity, definition, layer, true);
        if (kind == ecs::Kind::Person) ecs_.emplace_or_replace<ecs::Person>(entity);
        else ecs_.remove<ecs::Person>(entity);
        if (kind == ecs::Kind::Animal) ecs_.emplace_or_replace<ecs::Animal>(entity);
        else ecs_.remove<ecs::Animal>(entity);
        if (kind == ecs::Kind::Building) ecs_.emplace_or_replace<ecs::Building>(entity);
        else ecs_.remove<ecs::Building>(entity);
        if (kind == ecs::Kind::ResourceNode) ecs_.emplace_or_replace<ecs::ResourceNode>(entity);
        else ecs_.remove<ecs::ResourceNode>(entity);
        if (kind == ecs::Kind::ItemStack) ecs_.emplace_or_replace<ecs::ItemStack>(entity);
        else ecs_.remove<ecs::ItemStack>(entity);
        if (kind == ecs::Kind::Person || kind == ecs::Kind::Animal) {
            if (!ecs_.all_of<ai::Controlled>(entity))
                ecs_.emplace<ai::Controlled>(entity, ai::Lod::Near, 0);
        } else {
            ecs_.remove<ai::Controlled>(entity);
        }
        if (selectable) ecs_.emplace_or_replace<ecs::Selectable>(entity);
        else ecs_.remove<ecs::Selectable>(entity);
        seen.insert(key);
    };

    for (Person& p : people_)
        if (p.alive) {
            add(ecs::Kind::Person, p.id.value, p.pos, {}, 0, true);
            const entt::entity entity = ecsEntity(ecs::Kind::Person, p.id.value);
            if (tick_ > 0 && ecs_.all_of<ecs::Transform>(entity)) {
                p.pos = ecs_.get<const ecs::Transform>(entity).value;
                p.tile = core::toTile(p.pos);
            }
            if (ecs_.all_of<ecs::Alive>(entity))
                people_[p.id.value].alive = ecs_.get<const ecs::Alive>(entity).value;
            else
                ecs_.emplace<ecs::Alive>(entity, p.alive);
            if (ecs_.all_of<ecs::Health, ecs::Hunger, ecs::Thirst, ecs::Fatigue,
                           ecs::SleepState, ecs::BodyTemperature, ecs::AilmentState>(entity)) {
                const auto& health = ecs_.get<const ecs::Health>(entity);
                const auto& hunger = ecs_.get<const ecs::Hunger>(entity);
                const auto& thirst = ecs_.get<const ecs::Thirst>(entity);
                const auto& fatigue = ecs_.get<const ecs::Fatigue>(entity);
                const auto& sleep = ecs_.get<const ecs::SleepState>(entity);
                const auto& temperature = ecs_.get<const ecs::BodyTemperature>(entity);
                const auto& ailment = ecs_.get<const ecs::AilmentState>(entity);
                people_[p.id.value].health = health.current;
                people_[p.id.value].satiety = hunger.value;
                people_[p.id.value].hydration = thirst.value;
                people_[p.id.value].rest = fatigue.value;
                people_[p.id.value].asleep = sleep.asleep;
                people_[p.id.value].bodyTempOffset = temperature.offsetC;
                people_[p.id.value].ailment = static_cast<Person::Ailment>(ailment.kind);
                people_[p.id.value].ailmentSeverity = ailment.severity;
                people_[p.id.value].ailmentTended = ailment.tended;
                people_[p.id.value].ailmentSinceTick = ailment.sinceTick;
            } else {
                syncEcsNeeds(p.id);
            }
            syncEcsJob(p.id);
            if (ecs_.all_of<ecs::Carrying>(entity))
                people_[p.id.value].carrying = ItemStackId{ecs_.get<const ecs::Carrying>(entity).stack};
            else
                ecs_.emplace<ecs::Carrying>(entity, p.carrying.value);
            if (ecs_.all_of<ecs::EquippedTool>(entity))
                people_[p.id.value].equippedTool = ItemStackId{ecs_.get<const ecs::EquippedTool>(entity).stack};
            else
                ecs_.emplace<ecs::EquippedTool>(entity, p.equippedTool.value);
            if (ecs_.all_of<ecs::WornItems>(entity)) {
                people_[p.id.value].worn.clear();
                for (const std::uint32_t stack : ecs_.get<const ecs::WornItems>(entity).stacks)
                    people_[p.id.value].worn.emplace_back(stack);
            } else {
                auto& worn = ecs_.emplace<ecs::WornItems>(entity);
                worn.stacks.reserve(p.worn.size());
                for (const ItemStackId stack : p.worn) worn.stacks.push_back(stack.value);
            }
            ecs_.emplace_or_replace<ecs::SettlementMember>(entity, p.settlement.value);
            ecs_.emplace_or_replace<ecs::HouseholdMember>(entity, p.household.value);
            ecs_.emplace_or_replace<ecs::Parentage>(entity, p.mother.value, p.father.value);
            ecs_.emplace_or_replace<ecs::Spouse>(entity, p.spouse.value);
            ecs_.emplace_or_replace<ecs::Profession>(entity,
                                                      static_cast<std::uint8_t>(p.profession));
        }
    for (Animal& a : animals_)
        if (a.alive) {
            add(ecs::Kind::Animal, a.id.value, a.pos, a.def, 1, true);
            const entt::entity entity = ecsEntity(ecs::Kind::Animal, a.id.value);
            if (tick_ > 0 && ecs_.all_of<ecs::Transform>(entity)) {
                a.pos = ecs_.get<const ecs::Transform>(entity).value;
                a.tile = core::toTile(a.pos);
            }
            if (ecs_.all_of<ecs::Health>(entity))
                a.condition = ecs_.get<const ecs::Health>(entity).current;
            else
                ecs_.emplace<ecs::Health>(entity, a.condition, core::kOne);
            if (ecs_.all_of<ecs::Alive>(entity))
                a.alive = ecs_.get<const ecs::Alive>(entity).value;
            else
                ecs_.emplace<ecs::Alive>(entity, a.alive);
            if (ecs_.all_of<ecs::Age>(entity))
                a.ageDays = ecs_.get<const ecs::Age>(entity).days;
            else
                ecs_.emplace<ecs::Age>(entity, a.ageDays);
            if (ecs_.all_of<ecs::GrazingTarget>(entity))
                a.grazeTarget = ecs_.get<const ecs::GrazingTarget>(entity).tile;
            else
                ecs_.emplace<ecs::GrazingTarget>(entity, a.grazeTarget);
            if (ecs_.all_of<ecs::AnimalTimers>(entity)) {
                const auto& timers = ecs_.get<const ecs::AnimalTimers>(entity);
                a.nextShearTick = timers.nextShearTick;
                a.nextMilkTick = timers.nextMilkTick;
                a.nextBreedTick = timers.nextBreedTick;
            } else {
                ecs_.emplace<ecs::AnimalTimers>(entity, a.nextShearTick,
                                                  a.nextMilkTick, a.nextBreedTick);
            }
            if (!ecs_.all_of<ecs::Hunger>(entity))
                ecs_.emplace<ecs::Hunger>(entity, core::kOne, core::kZero);
            if (!ecs_.all_of<ecs::Thirst>(entity))
                ecs_.emplace<ecs::Thirst>(entity, core::kOne, core::kZero);
            if (!ecs_.all_of<ecs::Fatigue>(entity))
                ecs_.emplace<ecs::Fatigue>(entity, core::kOne, core::kZero);
            if (ecs_.all_of<ecs::SleepState>(entity))
                a.penned = ecs_.get<const ecs::SleepState>(entity).asleep;
            else
                ecs_.emplace<ecs::SleepState>(entity, a.penned);
            if (ecs_.all_of<ecs::SettlementMember>(entity))
                a.owner = SettlementId{ecs_.get<const ecs::SettlementMember>(entity).settlement};
            else
                ecs_.emplace<ecs::SettlementMember>(entity, a.owner.value);
        }
    for (Building& b : buildings_)
        if (b.alive) {
            add(ecs::Kind::Building, b.id.value, core::tileCentre(b.origin), b.def, 2, true);
            const entt::entity entity = ecsEntity(ecs::Kind::Building, b.id.value);
            if (!ecs_.all_of<ecs::Health>(entity))
                ecs_.emplace<ecs::Health>(entity, core::kOne, core::kOne);
            if (ecs_.all_of<ecs::Alive>(entity))
                b.alive = ecs_.get<const ecs::Alive>(entity).value;
            else
                ecs_.emplace<ecs::Alive>(entity, b.alive);
            const auto& definition = db_.building(b.def);
            if (ecs_.all_of<ecs::ConstructionProgress>(entity)) {
                b.workDone = ecs_.get<const ecs::ConstructionProgress>(entity).done;
            } else {
                ecs_.emplace<ecs::ConstructionProgress>(
                    entity, b.workDone, definition.workAmount,
                    b.state == BuildState::Complete,
                    static_cast<std::uint8_t>(b.state));
            }
            ecs_.emplace_or_replace<ecs::DeliveredMaterials>(entity, b.delivered);
        }
    for (ResourceNode& n : nodes_)
        if (n.alive) {
            add(ecs::Kind::ResourceNode, n.id.value, core::tileCentre(n.tile), n.def, 1, true);
            const entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, n.id.value);
            if (ecs_.all_of<ecs::Alive>(entity))
                n.alive = ecs_.get<const ecs::Alive>(entity).value;
            else
                ecs_.emplace<ecs::Alive>(entity, n.alive);
            if (ecs_.all_of<ecs::ResourceState>(entity)) {
                const auto& state = ecs_.get<const ecs::ResourceState>(entity);
                n.workDone = state.workDone;
                n.depleted = state.depleted;
                n.regrowAtTick = state.regrowAtTick;
            } else {
                ecs_.emplace<ecs::ResourceState>(entity, n.workDone, n.depleted,
                                                   n.regrowAtTick);
            }
        }
    for (ItemStack& s : stacks_)
        if (s.alive) {
            const entt::entity before = ecsEntity(ecs::Kind::ItemStack, s.id.value);
            add(ecs::Kind::ItemStack, s.id.value, core::tileCentre(s.tile), s.def, 3, false);
            const entt::entity entity = ecsEntity(ecs::Kind::ItemStack, s.id.value);
            // New stacks still enter through the compatibility constructor.
            // Existing stacks are ECS-owned after the first import; copy only
            // the committed component back to the legacy mirror so the end
            // sync cannot undo pickup, merge, spoilage, or tool commands.
            const bool hasState = entity != entt::null && ecs_.valid(entity) &&
                                  ecs_.all_of<ecs::ItemStackState>(entity);
            bool legacyChanged = false;
            if (hasState) {
                const auto& state = ecs_.get<const ecs::ItemStackState>(entity);
                legacyChanged = state.generation != s.generation || state.definition != s.def.value ||
                                 state.count != s.count || state.where != static_cast<std::uint8_t>(s.where) ||
                                 state.tileX != s.tile.x || state.tileY != s.tile.y ||
                                 state.holder != s.holder.value || state.building != s.building.value ||
                                 state.owner != s.owner.value || state.freshnessRaw != s.freshness.raw ||
                                 state.qualityRaw != s.quality.raw || state.durabilityLeft != s.durabilityLeft ||
                                 state.alive != s.alive;
            }
            if (tick_ == 0 || before == entt::null || !hasState || legacyChanged) {
                syncEcsStack(s.id);
            } else {
                const auto& state = ecs_.get<const ecs::ItemStackState>(entity);
                s.generation = state.generation;
                s.def = DefId{state.definition};
                s.count = state.count;
                s.where = static_cast<StackWhere>(state.where);
                s.tile = TilePos{state.tileX, state.tileY};
                s.holder = PersonId{state.holder};
                s.building = BuildingId{state.building};
                s.owner = SettlementId{state.owner};
                s.freshness = Fixed::fromRaw(state.freshnessRaw);
                s.quality = Fixed::fromRaw(state.qualityRaw);
                s.durabilityLeft = state.durabilityLeft;
                s.alive = state.alive;
            }
        }

    for (auto it = ecsEntities_.begin(); it != ecsEntities_.end();) {
        if (seen.count(it->first) != 0) {
            ++it;
            continue;
        }
        if (ecs_.valid(it->second)) ecs_.destroy(it->second);
        it = ecsEntities_.erase(it);
    }
}

entt::entity World::ecsEntity(ecs::Kind kind, std::uint32_t legacyIndex) const {
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(kind)) << 32) | legacyIndex;
    const auto it = ecsEntities_.find(key);
    return it == ecsEntities_.end() ? entt::null : it->second;
}

void World::syncEcsStack(ItemStackId id) {
    if (!id.valid() || id.value >= stacks_.size()) return;
    const ItemStack& stack = stacks_[id.value];
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(ecs::Kind::ItemStack)) << 32) |
                              id.value;
    entt::entity entity = ecsEntity(ecs::Kind::ItemStack, id.value);
    if (entity == entt::null || !ecs_.valid(entity)) {
        entity = ecs_.create();
        ecsEntities_[key] = entity;
        ecs_.emplace<ecs::Identity>(entity, ecs::Kind::ItemStack, id.value);
        ecs_.emplace<ecs::ItemStack>(entity);
        ecs_.emplace<ecs::Transform>(entity, core::tileCentre(stack.tile));
        ecs_.emplace<ecs::Renderable>(entity, stack.def, static_cast<std::uint8_t>(3), true);
        ecs_.emplace<ecs::Selectable>(entity);
    }
    ecs_.get<ecs::Transform>(entity).value = core::tileCentre(stack.tile);
    const auto stackCell = ecs::cellForTile(stack.tile, ecs::kSpatialCellExtent);
    ecs_.emplace_or_replace<ecs::SpatialCell>(entity, stackCell.x, stackCell.y);
    ecs_.get<ecs::Renderable>(entity).sprite = stack.def;
    ecs_.emplace_or_replace<ecs::ItemStackState>(
            entity, stack.generation, stack.def.value, stack.count,
            static_cast<std::uint8_t>(stack.where), stack.tile.x, stack.tile.y,
            stack.holder.value, stack.building.value, stack.owner.value,
            stack.freshness.raw, stack.quality.raw, stack.durabilityLeft, stack.alive);
}

void World::syncEcsBuilding(BuildingId id) {
    if (!id.valid() || id.value >= buildings_.size()) return;
    const Building& building = buildings_[id.value];
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(ecs::Kind::Building)) << 32) |
                              id.value;
    entt::entity entity = ecsEntity(ecs::Kind::Building, id.value);
    if (entity == entt::null || !ecs_.valid(entity)) {
        entity = ecs_.create();
        ecsEntities_[key] = entity;
        ecs_.emplace<ecs::Identity>(entity, ecs::Kind::Building, id.value);
        ecs_.emplace<ecs::Building>(entity);
        ecs_.emplace<ecs::Transform>(entity, core::tileCentre(building.origin));
        ecs_.emplace<ecs::Renderable>(entity, building.def, static_cast<std::uint8_t>(2), true);
        ecs_.emplace<ecs::Selectable>(entity);
        ecs_.emplace<ecs::Health>(entity, core::kOne, core::kOne);
    }
    ecs_.get<ecs::Transform>(entity).value = core::tileCentre(building.origin);
    const auto buildingCell = ecs::cellForTile(building.origin, ecs::kSpatialCellExtent);
    ecs_.emplace_or_replace<ecs::SpatialCell>(entity, buildingCell.x, buildingCell.y);
    ecs_.get<ecs::Renderable>(entity).sprite = building.def;
    if (!ecs_.all_of<ecs::Alive>(entity))
        ecs_.emplace<ecs::Alive>(entity, building.alive);
    if (!ecs_.all_of<ecs::ConstructionProgress>(entity))
        ecs_.emplace<ecs::ConstructionProgress>(entity, building.workDone,
                                                  db_.building(building.def).workAmount,
                                                  building.state == BuildState::Complete,
                                                  static_cast<std::uint8_t>(building.state));
    if (!ecs_.all_of<ecs::DeliveredMaterials>(entity))
        ecs_.emplace<ecs::DeliveredMaterials>(entity, building.delivered);
    else
        ecs_.get<ecs::DeliveredMaterials>(entity).values = building.delivered;
}

void World::syncEcsResourceNode(ResourceNodeId id) {
    if (!id.valid() || id.value >= nodes_.size()) return;
    const ResourceNode& node = nodes_[id.value];
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(ecs::Kind::ResourceNode)) << 32) |
                              id.value;
    entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, id.value);
    if (entity == entt::null || !ecs_.valid(entity)) {
        entity = ecs_.create();
        ecsEntities_[key] = entity;
        ecs_.emplace<ecs::Identity>(entity, ecs::Kind::ResourceNode, id.value);
        ecs_.emplace<ecs::ResourceNode>(entity);
        ecs_.emplace<ecs::Transform>(entity, core::tileCentre(node.tile));
        const auto nodeCell = ecs::cellForTile(node.tile, ecs::kSpatialCellExtent);
        ecs_.emplace<ecs::SpatialCell>(entity, nodeCell.x, nodeCell.y);
        ecs_.emplace<ecs::Renderable>(entity, node.def, static_cast<std::uint8_t>(1), true);
        ecs_.emplace<ecs::Selectable>(entity);
        ecs_.emplace<ecs::Alive>(entity, node.alive);
        ecs_.emplace<ecs::ResourceState>(entity, node.workDone, node.depleted, node.regrowAtTick);
    } else {
        ecs_.get<ecs::Transform>(entity).value = core::tileCentre(node.tile);
        const auto nodeCell = ecs::cellForTile(node.tile, ecs::kSpatialCellExtent);
        ecs_.emplace_or_replace<ecs::SpatialCell>(entity, nodeCell.x, nodeCell.y);
        ecs_.get<ecs::Renderable>(entity).sprite = node.def;
        ecs_.get<ecs::Alive>(entity).value = node.alive;
        ecs_.get<ecs::ResourceState>(entity) = {node.workDone, node.depleted, node.regrowAtTick};
    }
}

void World::syncEcsAnimal(AnimalId id) {
    if (!id.valid() || id.value >= animals_.size()) return;
    const Animal& animal = animals_[id.value];
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(ecs::Kind::Animal)) << 32) |
                              id.value;
    entt::entity entity = ecsEntity(ecs::Kind::Animal, id.value);
    if (entity == entt::null || !ecs_.valid(entity)) {
        entity = ecs_.create();
        ecsEntities_[key] = entity;
        ecs_.emplace<ecs::Identity>(entity, ecs::Kind::Animal, id.value);
        ecs_.emplace<ecs::Animal>(entity);
        ecs_.emplace<ecs::Transform>(entity, animal.pos);
        const auto animalCell = ecs::cellForTile(animal.tile, ecs::kSpatialCellExtent);
        ecs_.emplace<ecs::SpatialCell>(entity, animalCell.x, animalCell.y);
        ecs_.emplace<ecs::Renderable>(entity, animal.def, static_cast<std::uint8_t>(1), true);
        ecs_.emplace<ecs::Selectable>(entity);
        ecs_.emplace<ecs::Health>(entity, animal.condition, core::kOne);
        ecs_.emplace<ecs::Alive>(entity, animal.alive);
        ecs_.emplace<ecs::Age>(entity, animal.ageDays);
        ecs_.emplace<ecs::GrazingTarget>(entity, animal.grazeTarget);
        ecs_.emplace<ecs::AnimalTimers>(entity, animal.nextShearTick, animal.nextMilkTick,
                                         animal.nextBreedTick);
        ecs_.emplace<ecs::Hunger>(entity, core::kOne, core::kZero);
        ecs_.emplace<ecs::Thirst>(entity, core::kOne, core::kZero);
        ecs_.emplace<ecs::Fatigue>(entity, core::kOne, core::kZero);
        ecs_.emplace<ecs::SleepState>(entity, animal.penned);
        ecs_.emplace<ecs::SettlementMember>(entity, animal.owner.value);
        ecs_.emplace<ai::Controlled>(entity, ai::Lod::Near, 0);
    } else {
        ecs_.get<ecs::Transform>(entity).value = animal.pos;
        const auto animalCell = ecs::cellForTile(animal.tile, ecs::kSpatialCellExtent);
        ecs_.emplace_or_replace<ecs::SpatialCell>(entity, animalCell.x, animalCell.y);
        ecs_.get<ecs::Renderable>(entity).sprite = animal.def;
        ecs_.get<ecs::Health>(entity).current = animal.condition;
        ecs_.get<ecs::Alive>(entity).value = animal.alive;
        ecs_.get<ecs::Age>(entity).days = animal.ageDays;
        ecs_.get<ecs::GrazingTarget>(entity).tile = animal.grazeTarget;
        auto& timers = ecs_.get<ecs::AnimalTimers>(entity);
        timers.nextShearTick = animal.nextShearTick;
        timers.nextMilkTick = animal.nextMilkTick;
        timers.nextBreedTick = animal.nextBreedTick;
        ecs_.get<ecs::SleepState>(entity).asleep = animal.penned;
        if (ecs_.all_of<ecs::SettlementMember>(entity))
            ecs_.get<ecs::SettlementMember>(entity).settlement = animal.owner.value;
        else
            ecs_.emplace<ecs::SettlementMember>(entity, animal.owner.value);
        if (!ecs_.all_of<ai::Controlled>(entity))
            ecs_.emplace<ai::Controlled>(entity, ai::Lod::Near, 0);
    }
}

void World::syncEcsPerson(PersonId id) {
    if (!id.valid() || id.value >= people_.size()) return;
    const Person& person = people_[id.value];
    const std::uint64_t key = (std::uint64_t(static_cast<std::uint8_t>(ecs::Kind::Person)) << 32) |
                              id.value;
    entt::entity entity = ecsEntity(ecs::Kind::Person, id.value);
    if (entity == entt::null || !ecs_.valid(entity)) {
        entity = ecs_.create();
        ecsEntities_[key] = entity;
        ecs_.emplace<ecs::Identity>(entity, ecs::Kind::Person, id.value);
        ecs_.emplace<ecs::Person>(entity);
        ecs_.emplace<ecs::Transform>(entity, person.pos);
        const auto cell = ecs::cellForTile(person.tile, ecs::kSpatialCellExtent);
        ecs_.emplace<ecs::SpatialCell>(entity, cell.x, cell.y);
        ecs_.emplace<ecs::Renderable>(entity, DefId{}, static_cast<std::uint8_t>(0), true);
        ecs_.emplace<ecs::Selectable>(entity);
        ecs_.emplace<ecs::Alive>(entity, person.alive);
        ecs_.emplace<ai::Controlled>(entity, ai::Lod::Near, 0);
        ecs_.emplace<ecs::Carrying>(entity, person.carrying.value);
        ecs_.emplace<ecs::EquippedTool>(entity, person.equippedTool.value);
        auto& worn = ecs_.emplace<ecs::WornItems>(entity);
        worn.stacks.reserve(person.worn.size());
        for (const ItemStackId stack : person.worn) worn.stacks.push_back(stack.value);
    }
    syncEcsNeeds(id);
    syncEcsJob(id);
    ecs_.emplace_or_replace<ecs::SettlementMember>(entity, person.settlement.value);
    ecs_.emplace_or_replace<ecs::HouseholdMember>(entity, person.household.value);
    ecs_.emplace_or_replace<ecs::Parentage>(entity, person.mother.value, person.father.value);
    ecs_.emplace_or_replace<ecs::Spouse>(entity, person.spouse.value);
    ecs_.emplace_or_replace<ecs::Profession>(entity,
                                              static_cast<std::uint8_t>(person.profession));
}

void World::syncEcsNeeds(PersonId person) {
    if (!person.valid() || person.value >= people_.size()) return;
    const entt::entity entity = ecsEntity(ecs::Kind::Person, person.value);
    if (entity == entt::null || !ecs_.valid(entity)) return;
    const Person& p = people_[person.value];
    ecs_.emplace_or_replace<ecs::Health>(entity, p.health, core::kOne);
    ecs_.emplace_or_replace<ecs::Hunger>(entity, p.satiety, core::kZero);
    ecs_.emplace_or_replace<ecs::Thirst>(entity, p.hydration, core::kZero);
    ecs_.emplace_or_replace<ecs::Fatigue>(entity, p.rest, core::kZero);
    ecs_.emplace_or_replace<ecs::SleepState>(entity, p.asleep);
    ecs_.emplace_or_replace<ecs::BodyTemperature>(entity, p.bodyTempOffset);
    ecs_.emplace_or_replace<ecs::AilmentState>(
            entity, static_cast<std::uint8_t>(p.ailment), p.ailmentSeverity,
            p.ailmentTended, p.ailmentSinceTick);
}

void World::syncEcsJob(PersonId person) {
    if (!person.valid() || person.value >= people_.size()) return;
    const entt::entity entity = ecsEntity(ecs::Kind::Person, person.value);
    if (entity == entt::null || !ecs_.valid(entity)) return;
    const Job& job = people_[person.value].job;
    const bool hadJobState = ecs_.all_of<ecs::JobState>(entity);
    const bool hadJobLinks = ecs_.all_of<ecs::JobLinks>(entity);
    const ecs::JobState previousState = hadJobState
        ? ecs_.get<const ecs::JobState>(entity) : ecs::JobState{};
    const ecs::JobLinks previousLinks = hadJobLinks
        ? ecs_.get<const ecs::JobLinks>(entity) : ecs::JobLinks{};
    const bool linksMatch = hadJobLinks &&
        previousLinks.stack == job.stack.value &&
        previousLinks.building == job.building.value &&
        previousLinks.node == job.node.value &&
        previousLinks.animal == job.animal.value &&
        previousLinks.person == job.person.value &&
        previousLinks.target == job.target;
    const bool sameAssignment = hadJobState &&
        previousState.kind == static_cast<std::uint8_t>(job.kind) &&
        previousState.active == job.valid() && linksMatch;
    ecs_.emplace_or_replace<ecs::JobState>(
        entity, static_cast<std::uint8_t>(job.kind), static_cast<std::uint8_t>(job.phase), job.valid());
    ecs_.emplace_or_replace<ecs::JobCategory>(
        entity, static_cast<std::uint8_t>(job.category));
    if (ecs_.all_of<ecs::JobProgress>(entity)) {
        auto& progress = ecs_.get<ecs::JobProgress>(entity);
        // A changed required amount means a new assignment; initialize it from
        // legacy. Otherwise ECS progress is authoritative and legacy mirrors it.
        if (sameAssignment && progress.required == job.workRequired) {
            people_[person.value].job.workDone = progress.done;
            people_[person.value].job.workRequired = progress.required;
        } else {
            progress.done = job.workDone;
            progress.required = job.workRequired;
        }
    } else {
        ecs_.emplace<ecs::JobProgress>(entity, job.workDone, job.workRequired);
    }
    ecs_.emplace_or_replace<ecs::JobLinks>(
        entity, job.stack.value, job.building.value, job.node.value,
        job.animal.value, job.person.value, job.target);
}

ecs::render::Frame World::extractPresentationFrame() const {
    return ecs::render::extract(ecs_);
}

std::int32_t World::explore(TilePos centre, std::int32_t radius) {
    std::int32_t revealed = 0;
    for (TilePos p : core::tilesWithin(centre, radius)) {
        if (!map_.inBounds(p)) continue;
        Tile& t = map_.at(p);
        if (t.explored) continue;
        t.explored = true;
        ++revealed;
    }
    exploredCount_ += revealed;
    report_.tilesExplored += revealed;
    return revealed;
}

bool World::stackStillIs(ItemStackId id, std::uint32_t generation) const {
    if (!id.valid() || id.value >= stacks_.size()) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::ItemStack, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::ItemStackState>(entity)) {
        const auto& state = ecs_.get<const ecs::ItemStackState>(entity);
        return state.alive && state.generation == generation;
    }
    return stacks_[id.value].alive && stacks_[id.value].generation == generation;
}

bool World::resourceNodeAlive(ResourceNodeId id) const {
    if (!id.valid() || id.value >= nodes_.size()) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::Alive>(entity))
        return ecs_.get<const ecs::Alive>(entity).value;
    return nodes_[id.value].alive;
}

bool World::resourceNodeDepleted(ResourceNodeId id) const {
    if (!id.valid() || id.value >= nodes_.size()) return true;
    const entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::ResourceState>(entity))
        return ecs_.get<const ecs::ResourceState>(entity).depleted;
    return nodes_[id.value].depleted;
}

bool World::buildingAlive(BuildingId id) const {
    if (!id.valid() || id.value >= buildings_.size()) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::Building, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::Alive>(entity))
        return ecs_.get<const ecs::Alive>(entity).value;
    return buildings_[id.value].alive;
}

bool World::buildingComplete(BuildingId id) const {
    if (!buildingAlive(id)) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::Building, id.value);
    if (entity != entt::null && ecs_.valid(entity) &&
        ecs_.all_of<ecs::ConstructionProgress>(entity))
        return ecs_.get<const ecs::ConstructionProgress>(entity).complete;
    return buildings_[id.value].state == BuildState::Complete;
}

std::uint8_t World::buildingPhase(BuildingId id) const {
    if (!id.valid() || id.value >= buildings_.size()) return static_cast<std::uint8_t>(BuildState::Blueprint);
    const entt::entity entity = ecsEntity(ecs::Kind::Building, id.value);
    if (entity != entt::null && ecs_.valid(entity) &&
        ecs_.all_of<ecs::ConstructionProgress>(entity))
        return ecs_.get<const ecs::ConstructionProgress>(entity).phase;
    return static_cast<std::uint8_t>(buildings_[id.value].state);
}

bool World::personAlive(PersonId id) const {
    if (!id.valid() || id.value >= people_.size()) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::Person, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::Alive>(entity))
        return ecs_.get<const ecs::Alive>(entity).value;
    return people_[id.value].alive;
}

bool World::animalAlive(AnimalId id) const {
    if (!id.valid() || id.value >= animals_.size()) return false;
    const entt::entity entity = ecsEntity(ecs::Kind::Animal, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::Alive>(entity))
        return ecs_.get<const ecs::Alive>(entity).value;
    return animals_[id.value].alive;
}

void World::setPersonAlive(PersonId id, bool value) {
    if (!id.valid() || id.value >= people_.size()) return;
    people_[id.value].alive = value;
    const entt::entity entity = ecsEntity(ecs::Kind::Person, id.value);
    if (entity != entt::null && ecs_.valid(entity))
        ecs_.emplace_or_replace<ecs::Alive>(entity, value);
}

void World::setAnimalAlive(AnimalId id, bool value) {
    if (!id.valid() || id.value >= animals_.size()) return;
    animals_[id.value].alive = value;
    const entt::entity entity = ecsEntity(ecs::Kind::Animal, id.value);
    if (entity != entt::null && ecs_.valid(entity))
        ecs_.emplace_or_replace<ecs::Alive>(entity, value);
}

void World::setBuildingAlive(BuildingId id, bool value) {
    if (!id.valid() || id.value >= buildings_.size()) return;
    buildings_[id.value].alive = value;
    const entt::entity entity = ecsEntity(ecs::Kind::Building, id.value);
    if (entity != entt::null && ecs_.valid(entity))
        ecs_.emplace_or_replace<ecs::Alive>(entity, value);
}

void World::setResourceNodeAlive(ResourceNodeId id, bool value) {
    if (!id.valid() || id.value >= nodes_.size()) return;
    nodes_[id.value].alive = value;
    const entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, id.value);
    if (entity != entt::null && ecs_.valid(entity))
        ecs_.emplace_or_replace<ecs::Alive>(entity, value);
}

void World::setStackAlive(ItemStackId id, bool value) {
    if (!id.valid() || id.value >= stacks_.size()) return;
    stacks_[id.value].alive = value;
    const entt::entity entity = ecsEntity(ecs::Kind::ItemStack, id.value);
    if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::ItemStackState>(entity))
        ecs_.get<ecs::ItemStackState>(entity).alive = value;
}

core::Rng& World::rng(std::uint64_t stream) {
    auto it = rngs_.find(stream);
    if (it == rngs_.end()) it = rngs_.emplace(stream, core::Rng(cfg_.seed, stream)).first;
    return it->second;
}

void World::refreshBlocked(TilePos p) {
    if (!map_.inBounds(p)) return;
    const Tile& t = map_.at(p);
    bool blocked = !terrainPassable(t.terrain);
    if (t.node.valid() && resourceAlive(*this, nodes_[t.node.value]) &&
        !resourceDepleted(*this, nodes_[t.node.value]))
        blocked = blocked || db_.resourceNode(nodes_[t.node.value].def).blocksMovement;
    if (t.building.valid() && buildingComplete(t.building))
        blocked = blocked || db_.building(buildings_[t.building.value].def).blocksMovement;
    map_.setBlocked(p, blocked);
}

ItemStackId World::spawnStack(DefId item, std::int32_t count, TilePos at, SettlementId owner) {
    ItemStackId id;
    if (!freeStacks_.empty()) {
        id = freeStacks_.back();
        freeStacks_.pop_back();
    } else {
        id = ItemStackId{static_cast<std::uint32_t>(stacks_.size())};
        stacks_.emplace_back();
    }
    ItemStack& s = stacks_[id.value];
    const std::uint32_t generation = s.generation + 1;
    s = ItemStack{};
    s.id = id;
    s.generation = generation;
    s.def = item;
    s.count = count;
    s.where = StackWhere::Ground;
    s.tile = at;
    s.owner = owner;
    setStackAlive(id, true);
    s.durabilityLeft = db_.item(item).durability;
    syncEcsStack(id);
    return id;
}

void World::destroyStack(ItemStackId id) {
    if (!id.valid() || id.value >= stacks_.size()) return;
    ItemStack& s = stacks_[id.value];
    if (!s.alive) return;
    if (s.holder.valid() && s.holder.value < people_.size()) {
        Person& p = people_[s.holder.value];
        const entt::entity holder = ecsEntity(ecs::Kind::Person, s.holder.value);
        if (p.carrying == id) {
            p.carrying = ItemStackId{};
            if (holder != entt::null && ecs_.valid(holder) && ecs_.all_of<ecs::Carrying>(holder))
                ecs_.get<ecs::Carrying>(holder).stack = core::Handle<core::ItemStackTag>::kInvalid;
        }
        if (p.equippedTool == id) {
            p.equippedTool = ItemStackId{};
            if (holder != entt::null && ecs_.valid(holder) && ecs_.all_of<ecs::EquippedTool>(holder))
                ecs_.get<ecs::EquippedTool>(holder).stack = core::Handle<core::ItemStackTag>::kInvalid;
        }
        p.worn.erase(std::remove(p.worn.begin(), p.worn.end(), id), p.worn.end());
        if (holder != entt::null && ecs_.valid(holder) && ecs_.all_of<ecs::WornItems>(holder)) {
            auto& worn = ecs_.get<ecs::WornItems>(holder).stacks;
            worn.erase(std::remove(worn.begin(), worn.end(), id.value), worn.end());
        }
    }
    setStackAlive(id, false);
    s.count = 0;
    syncEcsStack(id);
    release(stackKey(id));
    freeStacks_.push_back(id);
}

AnimalId World::spawnAnimal(DefId def, TilePos at, SettlementId owner, Sex sex, std::int32_t ageDays) {
    AnimalId id;
    for (std::size_t i = 0; i < animals_.size(); ++i)
        if (!animals_[i].alive) { id = AnimalId{static_cast<std::uint32_t>(i)}; break; }
    if (!id.valid()) {
        id = AnimalId{static_cast<std::uint32_t>(animals_.size())};
        animals_.emplace_back();
    }

    const auto& adef = db_.animal(def);
    Animal& a = animals_[id.value];
    a = Animal{};
    a.id = id;
    a.def = def;
    a.owner = owner;
    a.tile = at;
    a.grazeTarget = at;
    a.pos = core::tileCentre(at);
    a.sex = sex;
    a.ageDays = ageDays;
    setAnimalAlive(id, true);
    // A newborn is not milked or shorn until it is grown.
    const std::int64_t toAdult =
            std::int64_t(std::max(0, adef.adultAgeDays - ageDays)) * db_.time().ticksPerDay();
    a.nextShearTick = tick_ + toAdult;
    a.nextMilkTick = tick_ + toAdult;
    a.nextBreedTick = tick_ + toAdult;
    syncEcsAnimal(id);
    return id;
}

HouseholdId World::createHousehold(SettlementId s, content::WorkCategory trade) {
    const HouseholdId id{static_cast<std::uint32_t>(households_.size())};
    Household h;
    h.id = id;
    h.settlement = s;
    h.trade = trade;
    households_.push_back(h);
    return id;
}

ResourceNodeId World::spawnNode(DefId def, TilePos at) {
    const ResourceNodeId id{static_cast<std::uint32_t>(nodes_.size())};
    ResourceNode n;
    n.id = id;
    n.def = def;
    n.tile = at;
    nodes_.push_back(n);
    syncEcsResourceNode(id);
    map_.at(at).node = id;
    refreshBlocked(at);
    return id;
}

BuildingId World::placeBlueprint(DefId def, TilePos origin, SettlementId s,
                                HouseholdId forHousehold) {
    const BuildingId id{static_cast<std::uint32_t>(buildings_.size())};
    Building b;
    b.id = id;
    b.def = def;
    b.origin = origin;
    b.settlement = s;
    b.household = forHousehold;
    b.state = BuildState::Blueprint;
    b.lastProgressTick = tick_;
    b.delivered.assign(db_.building(def).materials.size(), 0);
    buildings_.push_back(std::move(b));
    syncEcsBuilding(id);

    for (TilePos p : buildings_.back().footprintTiles(db_))
        if (map_.inBounds(p)) {
            map_.at(p).building = id;
            // Ground being dug into a channel stops being a plot the moment the
            // digging starts.
            if (db_.building(def).irrigationRadius > 0) map_.at(p).tilled = false;
        }
    return id;
}

ZoneId World::createZone(const std::string& label, ZoneKind kind, ZoneMode mode, SettlementId s,
                         bool playerDrawn) {
    // A zone's id is its bit in every tile's mask, so a dead zone's slot is reused
    // rather than growing the mask past what a 64-bit word can hold.
    ZoneId id;
    for (std::size_t i = 0; i < zones_.size(); ++i)
        if (!zones_[i].alive) { id = ZoneId{static_cast<std::uint32_t>(i)}; break; }
    if (!id.valid()) {
        if (zones_.size() >= kMaxZones) return {};
        id = ZoneId{static_cast<std::uint32_t>(zones_.size())};
        zones_.emplace_back();
    }

    Zone& z = zones_[id.value];
    z = Zone{};
    z.id = id;
    z.label = label;
    z.kind = kind;
    z.mode = mode;
    z.settlement = s;
    z.playerDrawn = playerDrawn;
    z.alive = true;

    if (s.valid() && s.value < settlements_.size()) settlements_[s.value].zones.push_back(id);
    return id;
}

std::uint64_t World::zoneMaskAt(WorldPos p) const {
    std::uint64_t mask = 0;
    for (const auto& z : zones_)
        if (z.alive && z.contains(p)) mask |= std::uint64_t(1) << z.id.value;
    return mask;
}

void World::addAreaToZone(ZoneId id, const std::vector<WorldPos>& polygon) {
    if (!id.valid() || id.value >= zones_.size() || !zones_[id.value].alive) return;
    if (polygon.size() < 3) return;
    Zone& z = zones_[id.value];
    const WorldRect addedBounds = boundsOf(polygon);
    z.cutouts.erase(std::remove_if(z.cutouts.begin(), z.cutouts.end(), [&](const auto& cut) {
                        return core::polygonIntersectsRect(cut, addedBounds);
                    }),
                    z.cutouts.end());
    z.areas.push_back(polygon);
    rebuildZoneCache(*this, z);
}

void World::addDiscToZone(ZoneId id, WorldPos centre, Fixed radius) {
    addAreaToZone(id, core::discOutline(centre, radius));
}

void World::removeAreaFromZone(ZoneId id, const std::vector<WorldPos>& polygon) {
    if (!id.valid() || id.value >= zones_.size() || !zones_[id.value].alive) return;
    if (polygon.size() < 3) return;
    Zone& z = zones_[id.value];
    z.cutouts.push_back(polygon);
    rebuildZoneCache(*this, z);
}

void World::removeDiscFromZone(ZoneId id, WorldPos centre, Fixed radius) {
    removeAreaFromZone(id, core::discOutline(centre, radius));
}

void World::addTilesToZone(ZoneId id, const std::vector<TilePos>& tiles) {
    for (TilePos t : tiles)
        if (map_.inBounds(t)) addAreaToZone(id, polygonOfTile(t));
}

void World::removeTilesFromZone(ZoneId id, const std::vector<TilePos>& tiles) {
    for (TilePos t : tiles)
        if (map_.inBounds(t)) removeAreaFromZone(id, polygonOfTile(t));
}

void World::destroyZone(ZoneId id) {
    if (!id.valid() || id.value >= zones_.size() || !zones_[id.value].alive) return;
    Zone& z = zones_[id.value];
    const std::uint64_t bit = std::uint64_t(1) << id.value;
    for (TilePos t : z.tiles)
        if (map_.inBounds(t)) map_.at(t).zoneMask &= ~bit;

    for (auto& st : settlements_)
        st.zones.erase(std::remove(st.zones.begin(), st.zones.end(), id), st.zones.end());

    z.alive = false;
    z.areas.clear();
    z.cutouts.clear();
    z.tiles.clear();
}

bool World::reserve(std::uint64_t key, PersonId by) {
    ecs::CommandBuffer intent;
    intent.push(ecs::Command{ecs::ReserveIntent{key, by}});
    applyReservationIntents(intent);
    const auto it = reservations_.find(key);
    return it != reservations_.end() && it->second == by;
}

bool World::reserveAll(const std::vector<std::uint64_t>& keys, PersonId by) {
    if (!by.valid()) return false;
    std::vector<std::uint64_t> unique = keys;
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    for (const std::uint64_t key : unique)
        if (isReserved(key, by)) return false;
    ecs::CommandBuffer intents;
    for (const std::uint64_t key : unique) intents.push(ecs::ReserveIntent{key, by});
    applyReservationIntents(intents);
    for (const std::uint64_t key : unique)
        if (!isReserved(key)) return false;
    return true;
}

void World::release(std::uint64_t key) { reservations_.erase(key); }

bool World::isReserved(std::uint64_t key, PersonId ignoring) const {
    auto it = reservations_.find(key);
    if (it == reservations_.end()) return false;
    return !(ignoring.valid() && it->second == ignoring);
}

void World::releaseAllBy(PersonId by) {
    for (auto it = reservations_.begin(); it != reservations_.end();)
        it = (it->second == by) ? reservations_.erase(it) : std::next(it);
}

void World::applyReservationIntents(const ecs::CommandBuffer& intents) {
    std::vector<ecs::ReleaseReservationIntent> releases;
    std::vector<ecs::ReserveIntent> reservations;
    for (const ecs::Command& command : intents.commands()) {
        if (const auto* release = std::get_if<ecs::ReleaseReservationIntent>(&command))
            releases.push_back(*release);
        else if (const auto* reserve = std::get_if<ecs::ReserveIntent>(&command))
            reservations.push_back(*reserve);
    }

    // Releases precede new claims, and both phases have a stable tie-break.
    std::sort(releases.begin(), releases.end(), [](const auto& a, const auto& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.by.value < b.by.value;
    });
    for (const auto& release : releases) {
        const auto it = reservations_.find(release.key);
        if (it != reservations_.end() && it->second == release.by) reservations_.erase(it);
    }

    std::sort(reservations.begin(), reservations.end(), [](const auto& a, const auto& b) {
        if (a.key != b.key) return a.key < b.key;
        return a.by.value < b.by.value;
    });
    for (const auto& reservation : reservations) {
        const auto it = reservations_.find(reservation.key);
        if (it == reservations_.end()) reservations_.emplace(reservation.key, reservation.by);
    }
}

void World::applyPlacementIntents(const ecs::CommandBuffer& intents) {
    std::vector<ecs::PlaceBlueprintIntent> placements;
    for (const ecs::Command& command : intents.commands())
        if (const auto* placement = std::get_if<ecs::PlaceBlueprintIntent>(&command))
            placements.push_back(*placement);
    std::sort(placements.begin(), placements.end(), [](const auto& a, const auto& b) {
        if (a.settlement.value != b.settlement.value) return a.settlement.value < b.settlement.value;
        if (a.origin.x != b.origin.x) return a.origin.x < b.origin.x;
        if (a.origin.y != b.origin.y) return a.origin.y < b.origin.y;
        return a.definition.value < b.definition.value;
    });
    for (const auto& placement : placements) {
        if (!placement.definition.valid()) continue;
        const auto& definition = db_.building(placement.definition);
        bool clear = true;
        for (std::int32_t y = 0; y < definition.footprintDepth && clear; ++y)
            for (std::int32_t x = 0; x < definition.footprintWidth; ++x) {
                const TilePos tile{placement.origin.x + x, placement.origin.y + y};
                if (!map_.inBounds(tile) || map_.at(tile).building.valid()) {
                    clear = false;
                    break;
                }
            }
        if (!clear) continue;
        placeBlueprint(placement.definition, placement.origin, placement.settlement,
                       placement.household);
    }
}

// ---------------------------------------------------------------------------

void World::ensureWeatherClimate() const {
    if (weatherClimate_) return;
    std::array<Fixed,7> baseline{Fixed::ratio(65,100),core::kZero,Fixed::ratio(1,2),
                               core::kZero,core::kZero,Fixed::ratio(1,2),core::kZero};
    const auto* country=worldMapRef_;
    std::optional<generation::WorldMapData> reconstructed;
    if (!country || country->cells.empty()) {
        generation::WorldMapParams wp;
        wp.seed=cfg_.seed;
        const auto ethnos=db_.ethnosByName(cfg_.ethnos);
        wp.playedBiome=generation::parseBiome(ethnos.valid()?db_.ethnos(ethnos).biome:"temperate");
        if (cfg_.worldCells>0) wp.width=wp.height=cfg_.worldCells;
        reconstructed=generation::generateWorldMap(wp);
        country=&*reconstructed;
    }
    if (country->inBounds(localCell_)) {
        const WorldPos p{Fixed::fromInt(localCell_.x*generation::kMetresPerCell)+Fixed::ratio(generation::kMetresPerCell,2),
                         Fixed::fromInt(localCell_.y*generation::kMetresPerCell)+Fixed::ratio(generation::kMetresPerCell,2)};
        world::HeightField field(country,cfg_.seed);
        const auto climate=field.surfaceClimateAt(p).environment;
        std::copy(climate.begin(),climate.end(),baseline.begin());
        baseline[6]=field.heightAt(p);
    }
    weatherClimate_=baseline;
}

world::weather::Sample World::weather() const {
    ensureWeatherClimate();
    std::array<float,4> mids{};
    for (std::size_t i=0;i<4;++i) mids[i]=static_cast<float>(db_.sim().seasonMidC[i].toDouble());
    const auto state=world::weather::snapshot(cfg_.seed,world::weather::dayAt(tick_,db_.time()),db_.time().daysPerSeason,mids);
    const auto& c=*weatherClimate_;
    return state.at(static_cast<float>(c[0].toDouble()),static_cast<float>(c[2].toDouble()),
        static_cast<float>(c[6].toDouble()),(localCell_.x+0.5f)*generation::kMetresPerCell,
        (localCell_.y+0.5f)*generation::kMetresPerCell,static_cast<float>(c[5].toDouble()));
}

void World::updateWeather() {
    ensureWeatherClimate();
    const auto date = now();
    const auto& time = db_.time();
    const WorldPos p{Fixed::fromInt(localCell_.x*generation::kMetresPerCell)+Fixed::ratio(generation::kMetresPerCell,2),
                     Fixed::fromInt(localCell_.y*generation::kMetresPerCell)+Fixed::ratio(generation::kMetresPerCell,2)};
    const Fixed temp=world::weather::temperatureAt(cfg_.seed,tick_,time,db_.sim().seasonMidC,
                                                  (*weatherClimate_)[0],(*weatherClimate_)[6],p);

    // Night is colder; midday is warmest.
    const Fixed dayPhase = Fixed::ratio(date.hour, std::max(1, time.hoursPerDay));
    const Fixed swing =
            date.isDaylight ? db_.sim().dayWarmthC * dayPhase : core::kZero - db_.sim().nightChillC;
    outdoorTemp_ = temp + swing;
}

void World::updateResourceRegrowth() {
    for (auto& n : nodes_) {
        if (!resourceAlive(*this, n)) continue;
        const entt::entity entity = ecsEntity(ecs::Kind::ResourceNode, n.id.value);
        ecs::ResourceState* state = entity != entt::null && ecs_.valid(entity) &&
            ecs_.all_of<ecs::ResourceState>(entity)
                ? &ecs_.get<ecs::ResourceState>(entity) : nullptr;
        const bool depleted = state ? state->depleted : n.depleted;
        const std::int64_t regrowAt = state ? state->regrowAtTick : n.regrowAtTick;
        if (!depleted || regrowAt <= 0) continue;
        if (tick_ < regrowAt) continue;
        // Not onto ground somebody works. A field does not grow a tamarisk back
        // between the furrows, and a granary does not sprout one through itself.
        if (map_.inBounds(n.tile)) {
            const Tile& t = map_.at(n.tile);
            if (t.tilled || t.crop.valid() || t.building.valid()) continue;
        }
        n.depleted = false;
        n.regrowAtTick = 0;
        n.workDone = core::kZero;
        if (state) {
            state->depleted = false;
            state->regrowAtTick = 0;
            state->workDone = core::kZero;
        }
        refreshBlocked(n.tile);
    }
}

// Grass grows back. Slowly, and only where grass grows at all, so ground grazed
// bare stays bare for a while and the flock has to be somewhere else.
void World::updateGrass() {
    const auto& time = db_.time();
    if (tick_ == 0 || tick_ % time.ticksPerDay() != 0) return;
    const bool growing = now().season != core::Season::Winter;
    if (!growing) return;
    for (std::int32_t y = 0; y < map_.height(); ++y)
        for (std::int32_t x = 0; x < map_.width(); ++x) {
            Tile& t = map_.at(TilePos{x, y});
            const std::uint8_t cap = grassFor(t.terrain);
            if (cap == 0 || t.grass >= cap) continue;
            t.grass = static_cast<std::uint8_t>(
                    std::min<std::int32_t>(cap, t.grass + kGrassGrowsPerDay));
        }
}

// A wood comes back by spreading, not by growing out of its own stumps: seed
// falls near a parent that is still standing and takes root somewhere else. So
// felling a stand really does clear it, and the land fills in again from its
// edges - which is what makes a forest zone a place rather than a respawner.
void World::updateWildSpread() {
    const auto& time = db_.time();
    if (tick_ == 0 || tick_ % time.ticksPerDay() != 0) return;
    core::Rng& rng = this->rng(core::stream::kWildlife);

    // Collected first: spawning appends to nodes_, and this morning's seedling
    // should not seed again the same morning.
    std::vector<std::pair<DefId, TilePos>> taking;
    for (const auto& n : nodes_) {
        if (!resourceAlive(*this, n) || resourceDepleted(*this, n)) continue;
        const auto& def = db_.resourceNode(n.def);
        if (def.spreadOneIn <= 0) continue;
        if (!rng.chance(1, def.spreadOneIn)) continue;

        // Somewhere near the parent, on ground of the kind it grows on, and not
        // on anything anybody is using.
        const auto around = core::tilesWithin(n.tile, kSpreadReach);
        const TilePos at = around[rng.below(static_cast<std::uint32_t>(around.size()))];
        if (!map_.inBounds(at) || at == n.tile) continue;
        const Tile& t = map_.at(at);
        if (t.node.valid() || t.building.valid()) continue;
        if (t.tilled || t.crop.valid() || t.ford) continue;
        if (!terrainPassable(t.terrain)) continue;
        if (t.terrain != map_.at(n.tile).terrain) continue;
        // Ground decides whether the seed takes. Bare silt at the edge of the
        // desert carries a wood no better than it carries a field, so the wild
        // comes back thickest where the land is richest - which is also the
        // ground the community wants for itself, and that is the tension.
        if (!rng.chance((t.fertility * 100).roundToInt() + 10, 110)) continue;
        taking.emplace_back(n.def, at);
    }
    for (const auto& [def, at] : taking)
        if (map_.inBounds(at) && !map_.at(at).node.valid()) spawnNode(def, at);
}

// Branches do not grow out of the ground: they fall off trees. A gathered pile
// is gone for good, and a living tree drops a new one nearby now and then. This
// is also what makes a wood worth keeping and clearing one cost something: fell
// the trees and the deadfall stops.
void World::updateDeadfall() {
    const auto& time = db_.time();
    if (tick_ == 0 || tick_ % time.ticksPerDay() != 0) return;

    const DefId branchDef = db_.resourceNodeByName("fallen_branch");
    if (!branchDef.valid()) return;

    core::Rng& wildlife = rng(core::stream::kWildlife);
    // Collected first, because spawning appends to nodes_ and would otherwise
    // let this morning's new pile drop a pile of its own.
    std::vector<TilePos> dropAt;
    for (const auto& n : nodes_) {
        if (!resourceAlive(*this, n) || resourceDepleted(*this, n)) continue;
        if (db_.resourceNode(n.def).kind != content::ResourceKind::Tree) continue;
        if (!wildlife.chance(1, kDeadfallOneIn)) continue;

        // Only if there is not already deadfall under this tree: a wood carries
        // as much loose wood as it has trees, not as much as it has days.
        bool already = false;
        TilePos spot;
        bool found = false;
        for (TilePos t : core::tilesWithin(n.tile, 1)) {
            if (!map_.inBounds(t)) continue;
            const Tile& tile = map_.at(t);
            if (tile.node.valid() && nodes_[tile.node.value].alive &&
                nodes_[tile.node.value].def == branchDef) { already = true; break; }
            if (found || t == n.tile) continue;
            if (tile.node.valid() || tile.building.valid()) continue;
            if (tile.tilled || tile.crop.valid()) continue;
            if (!terrainPassable(tile.terrain)) continue;
            spot = t;
            found = true;
        }
        if (already || !found) continue;
        dropAt.push_back(spot);
    }
    for (TilePos t : dropAt)
        if (map_.inBounds(t) && !map_.at(t).node.valid()) spawnNode(branchDef, t);
}

// Paths fade when nobody uses them. Without this the first year's wandering is
// printed on the map for ever.
void World::updatePaths() {
    const auto& time = db_.time();
    if (tick_ == 0 || tick_ % time.ticksPerDay() != 0) return;
    for (std::int32_t y = 0; y < map_.height(); ++y)
        for (std::int32_t x = 0; x < map_.width(); ++x) {
            Tile& t = map_.at(TilePos{x, y});
            if (t.traffic > 0) t.traffic -= 1;
        }
}

// Plans the community gave up on get another chance as the season turns: the
// clay it could not reach may be reachable now, or the hands it lacked grown.
void World::forgetOldPlanFailures() {
    const auto& time = db_.time();
    if (tick_ == 0 || tick_ % (time.ticksPerDay() * time.daysPerSeason) != 0) return;
    for (auto& st : settlements_)
        for (auto& count : st.planAttempts) count /= 2;
}

void World::updateSpoilage() {
    // Inventory/spoilage is intentionally outside the current runtime. The
    // inventory model is being replaced wholesale and must not mutate the
    // terrain-backed world until its new pipeline is introduced.
}

// What a day of accounting is worth, for a community nobody is watching.
//
// Derived from the planner's own labour table rather than guessed at, because a
// guess here is a guess about whether a community lives: feeding one mouth for a
// day costs field work and cooking (kFieldWorkPerHeadDay + kMealWorkPerHeadDay),
// and one adult's day is kWorkPerAdultDay of work. So a pair of hands covers
// three mouths and a bit, and the food that comes of it is that many mouths'
// worth of satiety.
//
// The first version of this said "one pair of hands feeds two" - two and a
// quarter units of satiety against the two a mouth eats - and since children
// count two fifths of a hand and elders three fifths, a community's hands are
// always fewer than its mouths. Production was therefore below consumption for
// every community on the map, always: every neighbour starved to death while
// nobody was looking, which is exactly what a player saw (D109).
void World::tick() {
    if (detail_ == Detail::Abstract) {
        tickAbstract();
        return;
    }
    // Fixed order. Every system reads the state the previous one left, so the
    // sequence itself is part of the determinism contract. Actor runtime state
    // is already present in ECS; world/terrain data stays in its own pipeline.
    updateWeather();

    // Actor systems run here only when their state is component-owned. Work
    // planning/execution is an independent pipeline under game/work; it is
    // invoked by its own driver and is not an ECS system. Farming, construction,
    // inventory and population mechanics are intentionally absent until their
    // replacement pipelines are designed.
    tickNeeds(*this);
    tickLivestock(*this);

    ++tick_;

    // Lifecycle cleanup is a commit phase of the detailed tick. Dead actor
    // entities are removed before AI LOD assignment, so the planner never sees
    // a stale controlled handle when the full projection bridge is skipped.
    for (auto it = ecsEntities_.begin(); it != ecsEntities_.end();) {
        const entt::entity entity = it->second;
        const bool deadActor = ecs_.valid(entity) &&
                               ((ecs_.all_of<ecs::Person, ecs::Alive>(entity) &&
                                 !ecs_.get<const ecs::Alive>(entity).value) ||
                                (ecs_.all_of<ecs::Animal, ecs::Alive>(entity) &&
                                 !ecs_.get<const ecs::Alive>(entity).value));
        if (!deadActor) {
            ++it;
            continue;
        }
        ecs_.destroy(entity);
        it = ecsEntities_.erase(it);
    }

    ai::Planner aiPlanner;
    std::vector<ai::Intent> aiIntents;
    const ecs::CellId aiFocus = aiFocus_.value_or(
            ecs::cellForTile(localCell(), ecs::kSpatialCellExtent));
    ai::Planner::assignLod(ecs_.writeRegistry(), aiFocus, 1, 3, 8, tick_);
    aiPlanner.planParallel(ecs_.snapshot(), {tick_, 1}, aiFocus, 8,
                           std::min<std::size_t>(8, std::max<std::size_t>(1, std::thread::hardware_concurrency())),
                           aiIntents);
    aiPlanner.execute(ecs_.writeRegistry(), aiIntents, tick_);
    report_.ticks = tick_;
    report_.population = static_cast<std::int32_t>(ecs_.view<ecs::Person>().size());
}

// A community nobody is looking at, in one day of accounting.
//
// The expensive half of a tick is deciding and doing: every pawn scoring every
// job it could reach, every path searched, every step walked. None of that is
// visible from another valley, and none of it has to happen for the community to
// go on being one. What does have to happen is that the day costs food, that
// work gets done, and that people are born and die - so that is what this does,
// on the same people and the same stores the detailed loop uses.
//
// Nothing is thrown away and nothing is rebuilt: turning the detail back on
// (setDetail) hands the planner a world it understands, standing where the
// accounting left it.
void World::tickAbstract() {
    // The former aggregate fast-forward is retained below as a reference for
    // the eventual coarse ECS implementation, but it is intentionally not a
    // runtime path after the ECS cut-over. Abstract worlds currently advance
    // only component-owned needs/livestock state; work/economy replacement
    // pipelines are excluded instead of silently reintroducing old mechanics.
    updateWeather();
    tickNeeds(*this);
    tickLivestock(*this);
    ++tick_;
    report_.ticks = tick_;
    report_.population = static_cast<std::int32_t>(ecs_.view<ecs::Person>().size());
}

std::uint64_t World::checksum() const {
    core::Checksum h;
    h.add(tick_);
    h.add(exploredCount_);
    for (const auto& p : people_) {
        if (!personAlive(p.id)) { h.add(std::int64_t(-1)); continue; }
        const entt::entity personEntity = ecsEntity(ecs::Kind::Person, p.id.value);
        if (personEntity != entt::null && ecs_.valid(personEntity) &&
            ecs_.all_of<ecs::Transform>(personEntity)) {
            const auto& transform = ecs_.get<const ecs::Transform>(personEntity).value;
            h.add(transform.x.raw);
            h.add(transform.y.raw);
        } else {
            h.add(p.pos.x.raw);
            h.add(p.pos.y.raw);
        }
        const NeedsSnapshot needs = needsSnapshot(*this, p);
        h.add(needs.satiety.raw);
        h.add(needs.hydration.raw);
        h.add(needs.rest.raw);
        h.add(needs.health.raw);
        h.add(needs.asleep ? 1 : 0);
        h.add(static_cast<std::uint8_t>(needs.ailment));
        h.add(needs.ailmentSeverity.raw);
        if (personEntity != entt::null && ecs_.valid(personEntity) &&
            ecs_.all_of<ecs::JobState, ecs::JobProgress>(personEntity)) {
            h.add(static_cast<std::int32_t>(ecs_.get<const ecs::JobState>(personEntity).kind));
            h.add(ecs_.get<const ecs::JobProgress>(personEntity).done.raw);
        } else {
            h.add(static_cast<std::int32_t>(p.job.kind));
            h.add(p.job.workDone.raw);
        }
        h.add(p.ageYears);
        h.add(p.nextPlanTick);
    }
    for (const auto& s : stacks_) {
        const auto* state = ecsStackState(*this, s.id);
        if (state ? !state->alive : !s.alive) { h.add(std::int64_t(-2)); continue; }
        h.add(std::int32_t(state ? state->definition : s.def.value));
        h.add(state ? state->count : s.count);
        h.add(state ? state->tileX : s.tile.x);
        h.add(state ? state->tileY : s.tile.y);
        h.add(state ? state->freshnessRaw : s.freshness.raw);
        h.add(state ? state->qualityRaw : s.quality.raw);
        h.add(std::int32_t(state ? state->generation : s.generation));
    }
    for (const auto& b : buildings_) {
        h.add(std::int32_t(b.def.value));
        const entt::entity buildingEntity = ecsEntity(ecs::Kind::Building, b.id.value);
        if (buildingEntity != entt::null && ecs_.valid(buildingEntity) &&
            ecs_.all_of<ecs::ConstructionProgress>(buildingEntity))
        {
            const auto& progress = ecs_.get<const ecs::ConstructionProgress>(buildingEntity);
            h.add(progress.complete ? static_cast<std::int32_t>(BuildState::Complete)
                                     : static_cast<std::int32_t>(BuildState::Building));
            h.add(progress.done.raw);
        } else {
            h.add(static_cast<std::int32_t>(b.state));
            h.add(b.workDone.raw);
        }
        if (buildingEntity != entt::null && ecs_.valid(buildingEntity) &&
            ecs_.all_of<ecs::DeliveredMaterials>(buildingEntity)) {
            for (auto d : ecs_.get<const ecs::DeliveredMaterials>(buildingEntity).values) h.add(d);
        } else {
            for (auto d : b.delivered) h.add(d);
        }
        h.add(b.origin.x);
        h.add(b.origin.y);
    }
    for (const auto& n : nodes_) {
        const entt::entity nodeEntity = ecsEntity(ecs::Kind::ResourceNode, n.id.value);
        if (nodeEntity != entt::null && ecs_.valid(nodeEntity) &&
            ecs_.all_of<ecs::ResourceState>(nodeEntity)) {
            const auto& state = ecs_.get<const ecs::ResourceState>(nodeEntity);
            h.add(state.depleted ? 1 : 0);
            h.add(state.regrowAtTick);
            h.add(state.workDone.raw);
        } else {
            h.add(n.depleted ? 1 : 0);
            h.add(n.regrowAtTick);
            h.add(n.workDone.raw);
        }
    }
    for (const auto& a : animals_) {
        if (!a.alive) { h.add(std::int64_t(-3)); continue; }
        h.add(a.pos.x.raw);
        h.add(a.pos.y.raw);
        h.add(a.ageDays);
        const entt::entity entity = ecsEntity(ecs::Kind::Animal, a.id.value);
        if (entity != entt::null && ecs_.valid(entity) && ecs_.all_of<ecs::Health>(entity))
            h.add(ecs_.get<const ecs::Health>(entity).current.raw);
        else
            h.add(a.condition.raw);
    }
    // Sown ground is state the player can lose, so it belongs in the fingerprint.
    for (std::int32_t y = 0; y < map_.height(); ++y)
        for (std::int32_t x = 0; x < map_.width(); ++x) {
            const Tile& t = map_.at({x, y});
            if (!t.crop.valid() && !t.tilled) continue;
            h.add(x);
            h.add(y);
            h.add(std::int32_t(t.crop.value));
            h.add(t.cropGrowth.raw);
        }
    return h.value();
}

} // namespace sim
