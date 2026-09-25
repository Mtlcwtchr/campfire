#include "game/work/execution.hpp"

#include "game/ecs/entity.hpp"
#include "game/ecs/inventory/components.hpp"
#include "game/ecs/life/components.hpp"
#include "game/ecs/resources/components.hpp"
#include "game/ecs/spatial/components.hpp"

#include <algorithm>

#include "game/simulation/inventory.hpp"
#include "game/simulation/needs.hpp"
#include "game/simulation/pathfinder.hpp"
#include "game/simulation/farming.hpp"
#include "game/simulation/livestock.hpp"
#include "game/work/planner.hpp"
#include "game/simulation/zones.hpp"

namespace sim {
namespace work {
namespace {

// Whether a building of this definition would stand clear at this origin. The
// planner has a richer version of the question (work areas, sown ground, firm
// ground); all that is wanted here is that nothing is already standing in the
// way, so a replacement does not go up over its neighbours.
bool fitsAt(const World& w, DefId def, TilePos origin) {
    for (TilePos t : footprintOf(w.db(), def, origin)) {
        if (!w.map().inBounds(t)) return false;
        const Tile& tile = w.map().at(t);
        if (tile.building.valid() && w.buildingAlive(tile.building)) return false;
        if (!terrainPassable(tile.terrain)) return false;
    }
    return true;
}

bool animalAlive(const World& w, const Animal& animal) {
    return w.animalAlive(animal.id);
}

void finishJob(World& w, Person& p) {
    if (p.job.kind != JobKind::None)
        w.report().jobsCompleted[static_cast<std::size_t>(p.job.kind)]++;
    w.releaseAllBy(p.id);
    p.job = Job{};
    w.syncEcsJob(p.id);
    p.idleReason = IdleReason::BetweenJobs;
}

void abandonJob(World& w, Person& p) {
    w.releaseAllBy(p.id);
    p.job = Job{};
    w.syncEcsJob(p.id);
    p.idleReason = IdleReason::BetweenJobs;
    // Whatever went wrong, do not immediately try the same thing again.
    p.nextPlanTick = w.tickCount() + 1;
}

// Practice is the only way a level goes up (GDD 6).
void gainSkill(World& w, Person& p, WorkCategory c, Fixed workUnits) {
    const auto& cfg = w.db().sim();
    Skill& s = p.skills[static_cast<std::size_t>(c)];
    if (s.level >= cfg.maxSkillLevel) return;

    Fixed rate = cfg.skillGainPerWorkUnit;
    for (DefId t : p.traits) rate = rate * (core::kOne + w.db().trait(t).learnRateMod);
    // Later levels take longer, so a level 15 mason is a real investment.
    rate = rate / (core::kOne + Fixed::ratio(s.level, 4));

    s.progress += rate * workUnits;
    while (s.progress >= core::kOne && s.level < cfg.maxSkillLevel) {
        s.progress -= core::kOne;
        s.level += 1;
    }
}

// GDD 6: discovery is not a separate "research" job. Doing the related work is
// what creates the chance, and the new method belongs to one person first.
void rollDiscovery(World& w, Person& p, WorkCategory category) {
    auto& rng = w.rng(core::stream::kDiscovery);
    for (const auto& k : w.db().knowledge()) {
        if (k.discoveredFrom != category || k.discoveryChanceDenominator <= 0) continue;
        if (p.knows(k.id)) continue;

        bool prereqs = true;
        for (const auto& req : k.requiresKnowledge) {
            const DefId r = w.db().knowledgeByName(req);
            if (!knowsMethod(w, p, r)) { prereqs = false; break; }
        }
        if (!prereqs) continue;

        std::int64_t numerator = 1;
        for (DefId t : p.traits) {
            const Fixed mod = w.db().trait(t).discoveryMod;
            if (mod > core::kZero) numerator += (mod * 4).roundToInt();   // "Great Mind" and friends
        }
        if (rng.chance(numerator, k.discoveryChanceDenominator)) {
            p.knownMethods.push_back(k.id);
            return;   // one discovery at a time
        }
    }
}

void pickUp(World& w, Person& p, ItemStackId id, std::int32_t count) {
    ItemStack& src = w.stack(id);
    const auto* state = ecsStackState(w, id);
    const bool alive = state ? state->alive : src.alive;
    const std::int32_t available = state ? state->count : src.count;
    if (!alive || available <= 0) return;
    const std::int32_t take = std::min(count, available);

    const ItemStackId carried = (take == available) ? id : splitStack(w, id, take);
    if (!carried.valid()) return;

    setStackLocation(w, carried, StackWhere::Carried, p.id, BuildingId{}, p.tile);
    setCarriedStack(w, p.id, carried);
}

void putDownAt(World& w, Person& p, TilePos tile, BuildingId building) {
    const ItemStackId carried = carriedStack(w, p.id);
    if (!carried.valid()) return;
    if (building.valid()) {
        setStackLocation(w, carried, StackWhere::InBuilding, PersonId{}, building,
                         w.building(building).origin);
    } else {
        setStackLocation(w, carried, StackWhere::Ground, PersonId{}, BuildingId{}, tile);
    }
    const ItemStackId id = carried;
    setCarriedStack(w, p.id, ItemStackId{});
    tryMergeAtDestination(w, id);
}

// The tool a pawn holds while working, and how much faster it makes the work.
Fixed toolEfficiencyFor(const World& w, const Person& p, ToolClass required) {
    const ItemStackId equipped = equippedToolStack(w, p.id);
    if (!equipped.valid()) return core::kOne;
    const auto& s = w.stack(equipped);
    const auto* state = ecsStackState(w, equipped);
    if (state ? !state->alive : !s.alive) return core::kOne;
    const auto& def = w.db().item(stackDefinition(w, equipped));
    if (required != ToolClass::None && def.toolClass != required) return core::kOne;
    // A well-made axe cuts better than a poor one. This is what a level of skill
    // buys: not permission to try, but a better result when you do.
    const Fixed quality = state ? Fixed::fromRaw(state->qualityRaw) : s.quality;
    return def.toolEfficiency * quality;
}

// What a person of this skill turns out. Baseline hands make a serviceable thing;
// a master makes a markedly better one.
Fixed craftedQuality(const World& w, const Person& p, WorkCategory category) {
    const std::int32_t level = p.skills[static_cast<std::size_t>(category)].level;
    const std::int32_t cap = std::max(1, w.db().sim().maxSkillLevel);
    return Fixed::ratio(3, 4) + Fixed::ratio(3 * std::min(level, cap), 4 * cap);
}

// Only things made one at a time carry a quality: bulk goods merge, and averaging
// the workmanship of two heaps of grain means nothing.
bool qualityMatters(const content::ItemDef& def) {
    return def.stackLimit <= 1 &&
           (def.toolClass != content::ToolClass::None || def.insulation > core::kZero);
}

// Returns the batch left on the ground that is worth carrying home, if any.
ItemStackId completeHarvest(World& w, Person& p) {
    ResourceNode& node = w.node(p.job.node);
    const entt::entity nodeEntity = w.ecsEntity(ecs::Kind::ResourceNode, node.id.value);
    auto* resourceState = nodeEntity != entt::null && w.ecs().valid(nodeEntity) &&
        w.ecs().all_of<ecs::ResourceState>(nodeEntity)
            ? &w.ecs().get<ecs::ResourceState>(nodeEntity) : nullptr;
    const auto& def = w.db().resourceNode(node.def);

    for (const auto& y : def.harvest.yields) {
        const ItemStackId id = w.spawnStack(y.item, y.count, node.tile, p.settlement);
        w.report().itemsProduced[w.db().item(y.item).name] += y.count;
        tryMergeAtDestination(w, id);
    }

    if (def.consumedOnHarvest) {
        node.depleted = true;
        if (def.regrowDays > 0)
            node.regrowAtTick = w.tickCount() + std::int64_t(def.regrowDays) * w.db().time().ticksPerDay();
        else
            w.setResourceNodeAlive(node.id, false);
        if (def.blocksMovement) {
            w.map().at(node.tile).node = ResourceNodeId{};
            w.refreshBlocked(node.tile);
        }
    } else {
        node.depleted = true;
        node.regrowAtTick = w.tickCount() +
                            std::int64_t(std::max(1, def.regrowDays)) * w.db().time().ticksPerDay();
    }
    if (resourceState) {
        resourceState->depleted = node.depleted;
        resourceState->regrowAtTick = node.regrowAtTick;
        resourceState->workDone = core::kZero;
    }
    node.workDone = core::kZero;

    wearTool(w, equippedToolStack(w, p.id), 1);
    rollDiscovery(w, p, def.harvest.category);

    // The heaviest batch lying here after the merge: what a gatherer would
    // actually pick up before walking back.
    ItemStackId best;
    std::int32_t bestCount = 0;
    auto stacks = w.ecs().view<const ecs::Identity, const ecs::ItemStackState>();
    for (const entt::entity entity : stacks) {
        const auto& identity = stacks.get<const ecs::Identity>(entity);
        const ItemStackId stack{identity.legacyIndex};
        const auto& state = stacks.get<const ecs::ItemStackState>(entity);
        if (!state.alive || state.count <= 0 || static_cast<StackWhere>(state.where) != StackWhere::Ground)
            continue;
        if (TilePos{state.tileX, state.tileY} != node.tile) continue;
        if (w.isReserved(World::stackKey(stack), p.id)) continue;
        if (state.count > bestCount ||
            (state.count == bestCount && best.valid() && stack.value < best.value)) {
            bestCount = state.count;
            best = stack;
        }
    }
    return best;
}

void completeCraft(World& w, Person& p) {
    const auto& r = w.db().recipe(p.job.recipe);

    // Re-check at the moment of consumption: the world moved while we walked.
    for (const auto& in : r.inputs) {
        std::int32_t remaining = in.count;
        for (auto& s : w.stacks()) {
            if (remaining <= 0) break;
            const auto* state = ecsStackState(w, s.id);
            const bool available = state ? state->alive && state->count > 0 &&
                                                 (stackWhere(w, s.id) == StackWhere::Ground ||
                                                  stackWhere(w, s.id) == StackWhere::InBuilding)
                                           : isAvailable(s);
            if (!available || stackDefinition(w, s.id) != in.item) continue;
            const StackWhere where = stackWhere(w, s.id);
            const TilePos at = where == StackWhere::InBuilding
                                       ? w.building(stackBuilding(w, s.id)).origin
                                       : stackTile(w, s.id);
            if (!materialsInReach(w, p.settlement, p.job.target, at)) continue;
            const std::int32_t count = state ? state->count : s.count;
            const std::int32_t take = std::min(remaining, count);
            setStackCount(w, s.id, stackCount(w, s.id) - take);
            remaining -= take;
            if (s.count <= 0) w.destroyStack(s.id);
        }
        if (remaining > 0) {
            // Somebody else used the last of it. The work is lost; re-plan.
            w.report().materialShortageRejections++;
            abandonJob(w, p);
            return;
        }
    }

    const Fixed quality = craftedQuality(w, p, r.category);
    for (const auto& o : r.outputs) {
        const ItemStackId id = w.spawnStack(o.item, o.count, p.job.target, p.settlement);
        if (qualityMatters(w.db().item(o.item))) {
            w.stack(id).quality = quality;
            w.syncEcsStack(id);
        }
        w.report().itemsProduced[w.db().item(o.item).name] += o.count;
        tryMergeAtDestination(w, id);
    }

    wearTool(w, equippedToolStack(w, p.id), 1);
    rollDiscovery(w, p, r.category);
}

void completeConstruction(World& w, Person& p) {
    Building& b = w.building(p.job.building);
    const auto& def = w.db().building(b.def);
    b.state = BuildState::Complete;
    if (const entt::entity entity = w.ecsEntity(ecs::Kind::Building, b.id.value);
        entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::ConstructionProgress>(entity)) {
        auto& progress = w.ecs().get<ecs::ConstructionProgress>(entity);
        progress.done = def.workAmount;
        progress.required = def.workAmount;
        progress.complete = true;
        progress.phase = static_cast<std::uint8_t>(BuildState::Complete);
    }
    w.report().buildingsCompleted[def.name]++;

    for (TilePos t : b.footprintTiles(w.db())) w.refreshBlocked(t);
    notePlanSucceeded(w, b.settlement);

    wearTool(w, equippedToolStack(w, p.id), 1);
    rollDiscovery(w, p, WorkCategory::Construction);
}

} // namespace

// One more pair of feet over this tile. Capped, so a path stops getting quicker
// once it is a path.
void wearPath(World& w, TilePos at) {
    if (!w.map().inBounds(at)) return;
    Tile& t = w.map().at(at);
    if (t.traffic < kPathTraffic * 2) t.traffic += 3;
}

void setPersonPosition(World& w, Person& person, WorldPos position, TilePos tile) {
    person.pos = position;
    person.tile = tile;
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.id.value);
    if (entity == entt::null || !w.ecs().valid(entity)) return;
    if (w.ecs().all_of<ecs::Transform>(entity))
        w.ecs().get<ecs::Transform>(entity).value = position;
    w.ecs().emplace_or_replace<ecs::SpatialCell>(
            entity, ecs::cellForTile(tile, ecs::kSpatialCellExtent).x,
            ecs::cellForTile(tile, ecs::kSpatialCellExtent).y);
}

bool advanceAlongPath(World& w, Person& p) {
    if (p.job.pathIndex >= p.job.path.size()) return true;

    const auto& cfg = w.db().sim();
    Fixed remaining = cfg.walkSpeedTilesPerHour / std::int64_t(w.db().time().ticksPerHour);
    remaining = remaining * moveCapacity(w, p);

    // Movement is budgeted in grass-equivalent metres. A fast pawn may cross
    // several one-metre path nodes in one tick; discarding the remainder capped
    // everybody at ticksPerHour metres/hour regardless of configured speed.
    while (p.job.pathIndex < p.job.path.size() && remaining > core::kZero) {
        const TilePos nextTile = p.job.path[p.job.pathIndex];
        const WorldPos goal = core::tileCentre(nextTile);
        const Fixed dx = goal.x - p.pos.x;
        const Fixed dy = goal.y - p.pos.y;
        const Fixed dist = core::hypot(dx, dy);

        if (dist <= Fixed::ratio(1, 64)) {
            setPersonPosition(w, p, goal, nextTile);
            wearPath(w, nextTile);
            ++p.job.pathIndex;
            continue;
        }

        const Fixed terrainCost = core::max(core::kOne, tileMoveCost(w.map().at(nextTile)));
        const Fixed segmentCost = dist * terrainCost;
        if (segmentCost <= remaining) {
            setPersonPosition(w, p, goal, nextTile);
            wearPath(w, nextTile);
            ++p.job.pathIndex;
            remaining -= segmentCost;
            continue;
        }

        const Fixed distance = remaining / terrainCost;
        WorldPos position = p.pos;
        position.x += dx * distance / dist;
        position.y += dy * distance / dist;
        setPersonPosition(w, p, position, core::toTile(position));
        return false;
    }
    return p.job.pathIndex >= p.job.path.size();
}

void executeJobs(World& w) {
    auto& rep = w.report();

    auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
    for (const entt::entity entity : people) {
        Person& p = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
        if (!p.job.valid()) {
            rep.idleTicks[static_cast<std::size_t>(p.idleReason)]++;
            continue;
        }
        rep.jobTicks[static_cast<std::size_t>(p.job.kind)]++;

        // Going home is the normal meal routine, never a suicide pact. If the
        // journey became critical after planning, eat the physical carried portion
        // immediately rather than waiting for another planner pass.
        const NeedsSnapshot currentNeeds = needsSnapshot(w, p);
        if (p.job.kind == JobKind::Eat && carriedStack(w, p.id).valid() && carriedStack(w, p.id) == p.job.stack &&
            currentNeeds.satiety < Fixed::ratio(1, 5) &&
            w.stackStillIs(p.job.stack, p.job.stackGeneration)) {
            eatFrom(w, p, p.job.stack);
            finishJob(w, p);
            continue;
        }

        // A grazing animal wanders while the shepherd walks to it; re-aim rather
        // than arriving at where it used to be.
        if (p.job.animal.valid() && p.job.animal.value < w.animals().size() &&
            animalAlive(w, w.animal(p.job.animal)) && p.job.phase == JobPhase::Travelling) {
            const TilePos now = w.animal(p.job.animal).tile;
            if (!(now == p.job.target) && core::tileDistance(now, p.tile) > 1) {
                const PathResult again = findPathAdjacent(w.map(), p.tile, now, kPlanningExpansionBudget);
                if (again.found) {
                    p.job.target = now;
                    p.job.path = again.tiles;
                    p.job.pathIndex = 0;
                    if (p.job.path.empty()) p.job.phase = JobPhase::Working;
                } 
            }
        }

        // Work that can hurt you sometimes does (D98). Felling, hunting,
        // butchering and cutting stone are the four that put people in front of
        // an edge or an animal, and a wound from one of them is the commonest
        // reason a bronze age community loses a pair of hands for a fortnight.
        // Rolled per tick of actual work, so it is the amount of dangerous work
        // done that decides the risk, not the number of jobs taken.
        if (p.job.phase == JobPhase::Working && currentNeeds.ailment == Person::Ailment::None) {
            const bool risky = p.job.kind == JobKind::Fell || p.job.kind == JobKind::Hunt ||
                               p.job.kind == JobKind::Slaughter || p.job.kind == JobKind::Clear ||
                               p.job.category == content::WorkCategory::Mining;
            if (risky && w.rng(core::stream::kHealth).chance(1, 3000)) {
                auto& ailment = w.ecs().get<ecs::AilmentState>(entity);
                ailment.kind = static_cast<std::uint8_t>(Person::Ailment::Wound);
                // A tired hand cuts deeper.
                ailment.severity = currentNeeds.rest < Fixed::ratio(1, 3) ? Fixed::ratio(11, 20)
                                                                            : Fixed::ratio(2, 5);
                ailment.sinceTick = w.tickCount();
                p.ailment = Person::Ailment::Wound;
                p.ailmentSeverity = ailment.severity;
                p.ailmentSinceTick = ailment.sinceTick;
                ++w.report().wounded;
                // And the job is dropped: nobody finishes felling a tree with an
                // open gash.
                p.job = Job{};
                continue;
            }
        }

        // --- travel ------------------------------------------------------
        if (p.job.phase == JobPhase::Travelling || p.job.phase == JobPhase::Delivering) {
            if (!advanceAlongPath(w, p)) continue;
            if (p.job.phase == JobPhase::Travelling) {
                p.job.phase = JobPhase::Working;
            } else {
                // Arrived at the drop-off.
                switch (p.job.kind) {
                    case JobKind::HaulToStore:
                        putDownAt(w, p, p.job.deliverTo, p.job.deliverBuilding);
                        finishJob(w, p);
                        continue;
                    case JobKind::HaulToSite: {
                        Building& b = w.building(p.job.building);
                        const auto& def = w.db().building(b.def);
                        if (b.delivered.size() < def.materials.size()) b.delivered.resize(def.materials.size(), 0);
                        bool consumed = false;
                        const ItemStackId carried = carriedStack(w, p.id);
                        if (carried.valid()) {
                            ItemStack& s = w.stack(carried);
                            for (std::size_t i = 0; i < def.materials.size(); ++i) {
                                if (def.materials[i].item != s.def) continue;
                                const std::int32_t room = def.materials[i].count - b.delivered[i];
                                const std::int32_t give = std::min(room, s.count);
                                if (give > 0) {
                                    b.delivered[i] += give;
                                    if (const entt::entity entity = w.ecsEntity(ecs::Kind::Building, b.id.value);
                                        entity != entt::null && w.ecs().valid(entity) &&
                                        w.ecs().all_of<ecs::DeliveredMaterials>(entity))
                                        w.ecs().get<ecs::DeliveredMaterials>(entity).values = b.delivered;
                                    setStackCount(w, s.id, stackCount(w, s.id) - give);
                                    b.lastProgressTick = w.tickCount();
                                    consumed = true;
                                }
                                break;
                            }
                            if (stackCount(w, carried) <= 0) {
                                w.destroyStack(carried);
                                setCarriedStack(w, p.id, ItemStackId{});
                            } else if (!consumed) {
                                // Site does not want it after all; drop it here.
                                putDownAt(w, p, p.tile, BuildingId{});
                            } else {
                                putDownAt(w, p, p.tile, BuildingId{});
                            }
                        }
                        finishJob(w, p);
                        continue;
                    }
                    case JobKind::Eat:
                        // The carried meal has reached the house; consume it in
                        // the normal action phase below.
                        p.job.phase = JobPhase::Working;
                        break;
                    default:
                        finishJob(w, p);
                        continue;
                }
            }
        }

        // --- act ---------------------------------------------------------
        switch (p.job.kind) {
            case JobKind::Harvest: {
                if (!p.job.node.valid() || !w.resourceNodeAlive(p.job.node) ||
                    w.resourceNodeDepleted(p.job.node)) {
                    abandonJob(w, p);
                    break;
                }
                const auto& def = w.db().resourceNode(w.node(p.job.node).def);
                const Fixed rate = workRate(w, p, def.harvest.category,
                                            toolEfficiencyFor(w, p, def.harvest.preferredTool));
                p.job.workDone += rate;
                gainSkill(w, p, def.harvest.category, rate);
                if (p.job.workDone >= p.job.workRequired) {
                    const ItemStackId produced = completeHarvest(w, p);
                    // A gatherer carries their own yield home. Dropping it at the
                    // bush for someone else to fetch doubled every trip and had a
                    // third of the community's waking hours going into hauling.
                    if (produced.valid() && !carriedStack(w, p.id).valid()) {
                        TilePos spot;
                        BuildingId store;
                        const DefId definition = stackDefinition(w, produced);
                        const TilePos sourceTile = stackTile(w, produced);
                        if (findStorageSpot(w, definition, p.tile, spot, store) &&
                            (store.valid() || !(spot == sourceTile))) {
                            w.releaseAllBy(p.id);
                            Job haul;
                            haul.kind = JobKind::HaulToStore;
                            haul.category = WorkCategory::Hauling;
                            setJobStack(w, haul, produced);
                            haul.target = sourceTile;
                            haul.deliverTo = spot;
                            haul.deliverBuilding = store;
                            haul.phase = JobPhase::Working;   // already within arm's reach
                            w.reserve(World::stackKey(produced), p.id);
                            w.report().jobsCompleted[static_cast<std::size_t>(JobKind::Harvest)]++;
                            p.job = haul;
                            break;
                        }
                    }
                    finishJob(w, p);
                }
                break;
            }
            case JobKind::Craft: {
                const auto& r = w.db().recipe(p.job.recipe);
                const Fixed rate = workRate(w, p, r.category, toolEfficiencyFor(w, p, r.requiredTool));
                p.job.workDone += rate;
                gainSkill(w, p, r.category, rate);
                if (p.job.workDone >= p.job.workRequired) {
                    completeCraft(w, p);
                    if (p.job.valid()) finishJob(w, p);
                }
                break;
            }
            case JobKind::Construct: {
                if (!p.job.building.valid() || !w.buildingAlive(p.job.building) ||
                    w.buildingComplete(p.job.building)) {
                    abandonJob(w, p);
                    break;
                }
                Building& b = w.building(p.job.building);
                b.state = BuildState::Building;
                const auto& def = w.db().building(b.def);
                const Fixed rate = workRate(w, p, WorkCategory::Construction,
                                            toolEfficiencyFor(w, p, def.requiredTool));
                Fixed constructionDone = b.workDone;
                ecs::ConstructionProgress* construction = nullptr;
                if (const entt::entity entity = w.ecsEntity(ecs::Kind::Building, b.id.value);
                    entity != entt::null && w.ecs().valid(entity) &&
                    w.ecs().all_of<ecs::ConstructionProgress>(entity)) {
                    construction = &w.ecs().get<ecs::ConstructionProgress>(entity);
                    constructionDone = construction->done;
                }
                constructionDone += rate;
                b.workDone = constructionDone;
                b.lastProgressTick = w.tickCount();
                if (construction) {
                    construction->done = constructionDone;
                    construction->required = def.workAmount;
                    construction->complete = false;
                    construction->phase = static_cast<std::uint8_t>(BuildState::Building);
                }
                gainSkill(w, p, WorkCategory::Construction, rate);
                if (constructionDone >= def.workAmount) {
                    completeConstruction(w, p);
                    finishJob(w, p);
                }
                break;
            }
            case JobKind::HaulToStore: {
                if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                    !isAvailable(w, p.job.stack)) { abandonJob(w, p); break; }
                pickUp(w, p, p.job.stack, w.stack(p.job.stack).count);
                if (!carriedStack(w, p.id).valid()) { abandonJob(w, p); break; }
                const PathResult path = findPath(w.map(), p.tile, p.job.deliverTo);
                if (!path.found) {
                    // Cannot reach the store; leave the batch where it stands.
                    putDownAt(w, p, p.tile, BuildingId{});
                    abandonJob(w, p);
                    break;
                }
                if (path.tiles.empty()) {
                    putDownAt(w, p, p.job.deliverTo, p.job.deliverBuilding);
                    finishJob(w, p);
                    break;
                }
                p.job.path = path.tiles;
                p.job.pathIndex = 0;
                p.job.phase = JobPhase::Delivering;
                break;
            }
            case JobKind::HaulToSite: {
                if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                    !isAvailable(w, p.job.stack)) { abandonJob(w, p); break; }
                const std::int32_t want = std::max(1, p.job.wantedCount);
                pickUp(w, p, p.job.stack, want);
                if (!carriedStack(w, p.id).valid()) { abandonJob(w, p); break; }
                const PathResult path = findPathAdjacent(w.map(), p.tile, p.job.deliverTo);
                if (!path.found) {
                    putDownAt(w, p, p.tile, BuildingId{});
                    abandonJob(w, p);
                    break;
                }
                p.job.path = path.tiles;
                p.job.pathIndex = 0;
                p.job.phase = JobPhase::Delivering;
                break;
            }
            case JobKind::FetchTool: {
                if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                    !isAvailable(w, p.job.stack)) { abandonJob(w, p); break; }
                // Put the old tool down before taking a new one; a person carries one.
                const ItemStackId equipped = equippedToolStack(w, p.id);
                if (equipped.valid() && stackAlive(w, equipped)) {
                    setStackLocation(w, equipped, StackWhere::Ground, PersonId{}, BuildingId{}, p.tile);
                }
                ItemStack& tool = w.stack(p.job.stack);
                const std::int32_t toolCount = ecsStackState(w, p.job.stack)
                                                   ? ecsStackState(w, p.job.stack)->count
                                                   : tool.count;
                const ItemStackId one = toolCount > 1 ? splitStack(w, p.job.stack, 1) : p.job.stack;
                setStackLocation(w, one, StackWhere::Equipped, p.id, BuildingId{}, p.tile);
                setEquippedToolStack(w, p.id, one);
                finishJob(w, p);
                break;
            }
            case JobKind::Tend: {
                // Two legs, like carrying a meal home (D98): reach what answers
                // the hurt, pick a dose off the batch, then take it to whoever
                // is hurt. Arriving is the general travel code's business; by
                // the time this runs the pawn is standing where it was sent.
                if (!p.job.person.valid()) { abandonJob(w, p); break; }
                Person& patient = w.people()[p.job.person.value];
                const entt::entity patientEntity = w.ecsEntity(ecs::Kind::Person, patient.id.value);
                const NeedsSnapshot patientNeeds = needsSnapshot(w, patient);
                const bool patientAlive = patientEntity != entt::null && w.ecs().valid(patientEntity) &&
                    w.ecs().all_of<ecs::Alive>(patientEntity)
                        ? w.ecs().get<const ecs::Alive>(patientEntity).value : patient.alive;
                if (!patientAlive || patientNeeds.ailment == Person::Ailment::None) {
                    if (carriedStack(w, p.id).valid()) putDownAt(w, p, p.tile, BuildingId{});
                    patient.tendedBy = PersonId{};
                    abandonJob(w, p);
                    break;
                }

                if (!carriedStack(w, p.id).valid()) {
                    if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                        !isAvailable(w, p.job.stack)) { abandonJob(w, p); break; }
                    pickUp(w, p, p.job.stack, 1);
                    if (!carriedStack(w, p.id).valid()) { abandonJob(w, p); break; }
                }

                // The patient may have moved: they are ill, not dead.
                if (p.tile != patient.tile) {
                    const PathResult path = findPathAdjacent(w.map(), p.tile, patient.tile);
                    if (!path.found) {
                        putDownAt(w, p, p.tile, BuildingId{});
                        patient.tendedBy = PersonId{};
                        abandonJob(w, p);
                        break;
                    }
                    if (!path.tiles.empty()) {
                        p.job.deliverTo = patient.tile;
                        p.job.path = path.tiles;
                        p.job.pathIndex = 0;
                        p.job.phase = JobPhase::Delivering;
                        break;
                    }
                }

                // Standing over them. The remedy is used up whether or not it
                // was the right one - but only the right one helps, which is
                // what knowing herbcraft is worth.
                const ItemStackId held = carriedStack(w, p.id);
                const auto& def = w.db().item(stackDefinition(w, held));
                const bool wound = patientNeeds.ailment == Person::Ailment::Wound;
                const Fixed answers = wound ? def.healsWound : def.healsSickness;
                setCarriedStack(w, p.id, ItemStackId{});
                consume(w, held, 1);
                ecs::AilmentState* patientAilment =
                    patientEntity != entt::null && w.ecs().valid(patientEntity) &&
                    w.ecs().all_of<ecs::AilmentState>(patientEntity)
                        ? &w.ecs().get<ecs::AilmentState>(patientEntity) : nullptr;
                if (answers > core::kZero) {
                    // Skill decides how much of it takes: the same poultice in a
                    // practised hand closes a wound the novice only cleans.
                    const Fixed hand = workRate(w, p, content::WorkCategory::Medical, core::kOne);
                    if (patientAilment) {
                        patientAilment->tended = core::min(core::kOne,
                                                           patientAilment->tended + answers * hand);
                        patientAilment->severity =
                            core::max(core::kZero, patientAilment->severity - answers / 3);
                        patient.ailmentTended = patientAilment->tended;
                        patient.ailmentSeverity = patientAilment->severity;
                    } else {
                        patient.ailmentTended = core::min(core::kOne, patient.ailmentTended + answers * hand);
                        patient.ailmentSeverity =
                            core::max(core::kZero, patient.ailmentSeverity - answers / 3);
                    }
                    ++w.report().treatments;
                    gainSkill(w, p, content::WorkCategory::Medical, Fixed::fromInt(2));
                }
                patient.tendedBy = PersonId{};
                finishJob(w, p);
                break;
            }
            case JobKind::Bury: {
                if (!p.job.person.valid()) { p.job = Job{}; break; }
                Person& dead = w.people()[p.job.person.value];
                if (w.personAlive(dead.id) || dead.buried) { abandonJob(w, p); break; }
                if (core::tileDistance(p.tile, dead.tile) > 1) {
                    const PathResult path = findPathAdjacent(w.map(), p.tile, dead.tile);
                    if (!path.found) { abandonJob(w, p); break; }
                    if (!path.tiles.empty()) {
                        p.job.path = path.tiles;
                        p.job.pathIndex = 0;
                        p.job.phase = JobPhase::Travelling;
                        break;
                    }
                }
                p.job.workDone = p.job.workDone +
                                 workRate(w, p, content::WorkCategory::Ritual, core::kOne);
                if (p.job.workDone < p.job.workRequired) break;
                dead.buried = true;
                // The ground they lay on is cleaned by the burial, which is the
                // material half of the observance (D99).
                for (core::TilePos q : core::tilesWithin(dead.tile, 2)) {
                    if (!w.map().inBounds(q)) continue;
                    Tile& tile = w.map().at(q);
                    tile.pollution = core::max(core::kZero, tile.pollution - Fixed::ratio(1, 2));
                }
                ++w.report().burials;
                gainSkill(w, p, content::WorkCategory::Ritual, Fixed::fromInt(3));
                finishJob(w, p);
                break;
            }
            case JobKind::Scout: {
                // Standing and looking: the work is the looking, and what it buys
                // is ground the rest of the community may now work.
                const Fixed rate = workRate(w, p, WorkCategory::Scouting, core::kOne);
                p.job.workDone += rate;
                gainSkill(w, p, WorkCategory::Scouting, rate);
                if (p.job.workDone < p.job.workRequired) break;
                w.explore(p.tile, kScoutRevealRadius);
                rollDiscovery(w, p, WorkCategory::Scouting);
                finishJob(w, p);
                break;
            }

            case JobKind::Wear: {
                if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                    !isAvailable(w, p.job.stack)) {
                    abandonJob(w, p);
                    break;
                }
                // One garment out of the batch goes on the body; the rest stays
                // where it was for whoever comes next.
                const std::int32_t garmentCount = stackCount(w, p.job.stack);
                const ItemStackId one = garmentCount > 1
                                                ? splitStack(w, p.job.stack, 1)
                                                : p.job.stack;
                if (!one.valid()) { abandonJob(w, p); break; }
                setStackLocation(w, one, StackWhere::Equipped, p.id, BuildingId{}, p.tile);
                p.worn.push_back(one);
                finishJob(w, p);
                break;
            }

            case JobKind::Eat: {
                // Eating is the one case where the batch may legitimately be in
                // the pawn's own hands rather than lying available.
                if (!w.stackStillIs(p.job.stack, p.job.stackGeneration) ||
                    (!isAvailable(w, p.job.stack) && carriedStack(w, p.id) != p.job.stack)) {
                    abandonJob(w, p);
                    break;
                }

                if (carriedStack(w, p.id) != p.job.stack && p.job.deliverBuilding.valid()) {
                    const ItemStackId source = p.job.stack;
                    pickUp(w, p, source, mealPortions(w, p, source));
                    if (!carriedStack(w, p.id).valid()) { abandonJob(w, p); break; }

                    w.release(World::stackKey(source));
                    const ItemStackId carried = carriedStack(w, p.id);
                    setJobStack(w, p.job, carried);
                    w.reserve(World::stackKey(carried), p.id);
                    p.job.target = p.job.deliverTo;
                    const PathResult home = findPath(w.map(), p.tile, p.job.deliverTo,
                                                     kPlanningExpansionBudget);
                    if (!home.found) {
                        // The portion remains physical in the pawn's hands; a
                        // later job will put it down rather than deleting it.
                        abandonJob(w, p);
                        break;
                    }
                    p.job.path = home.tiles;
                    p.job.pathIndex = 0;
                    p.job.phase = home.tiles.empty() ? JobPhase::Working : JobPhase::Delivering;
                    if (!home.tiles.empty()) break;
                }

                eatFrom(w, p, p.job.stack);
                finishJob(w, p);
                break;
            }
            case JobKind::Drink: {
                // Drinking straight from an open source carries whatever the water
                // carries (GDD 7); purity is a property of the tile, not a flag.
                w.ecs().get<ecs::Thirst>(entity).value = core::kOne;
                p.hydration = core::kOne;
                const Fixed pollution = w.map().at(p.job.target).pollution;
                if (pollution > core::kZero &&
                    w.rng(core::stream::kHealth).chance((pollution * 100).roundToInt(), 2000))
                    w.ecs().get<ecs::Health>(entity).current =
                            core::max(Fixed::ratio(1, 10), w.ecs().get<ecs::Health>(entity).current -
                                                                       Fixed::ratio(1, 10));
                p.health = w.ecs().get<const ecs::Health>(entity).current;
                finishJob(w, p);
                break;
            }
            case JobKind::Sleep: {
                // Sleep owns the wake decision: fully rested, or first light with
                // enough rest to work on.
                const auto date = w.now();
                const NeedsSnapshot needs = needsSnapshot(w, p);
                const bool rested = needs.rest >= core::kOne ||
                                    (date.isDaylight && needs.rest > Fixed::ratio(4, 5));
                if (rested) {
                    w.ecs().get<ecs::SleepState>(entity).asleep = false;
                    p.asleep = false;
                    finishJob(w, p);
                } else {
                    w.ecs().get<ecs::SleepState>(entity).asleep = true;
                    p.asleep = true;
                }
                break;
            }
            // --- the fields --------------------------------------------
            case JobKind::Hunt:
            case JobKind::Tame: {
                if (!p.job.animal.valid() || !animalAlive(w, w.animal(p.job.animal))) { abandonJob(w, p); break; }
                Animal& quarry = w.animal(p.job.animal);
                const auto& def = w.db().animal(quarry.def);
                const bool taming = p.job.kind == JobKind::Tame;
                const auto category =
                        taming ? content::WorkCategory::Herding : content::WorkCategory::Hunting;
                const Fixed rate = workRate(w, p, category, toolEfficiencyFor(w, p, def.huntTool));
                p.job.workDone += rate;
                gainSkill(w, p, category, rate);
                if (p.job.workDone < p.job.workRequired) break;

                if (taming) {
                    // The pup changes hands and kind: what it becomes is what the
                    // content says a tamed one is.
                    const DefId became = w.db().animalByName(def.tamesInto);
                    const TilePos at = quarry.tile;
                    const Sex sex = quarry.sex;
                    w.setAnimalAlive(quarry.id, false);
                    w.release(World::animalKey(quarry.id));
                    if (became.valid()) w.spawnAnimal(became, at, p.settlement, sex, 0);
                    w.report().animalsTamed++;
                } else {
                    for (const auto& y : def.huntYields) {
                        const ItemStackId id = w.spawnStack(y.item, y.count, quarry.tile, p.settlement);
                        w.report().itemsProduced[w.db().item(y.item).name] += y.count;
                        tryMergeAtDestination(w, id);
                    }
                    w.setAnimalAlive(quarry.id, false);
                    w.release(World::animalKey(quarry.id));
                    w.report().animalsHunted++;
                    wearTool(w, equippedToolStack(w, p.id), 1);
                    rollDiscovery(w, p, content::WorkCategory::Hunting);
                }
                finishJob(w, p);
                break;
            }

            case JobKind::Demolish: {
                if (!p.job.building.valid() || !w.buildingAlive(p.job.building)) {
                    abandonJob(w, p);
                    break;
                }
                Building& old = w.building(p.job.building);
                const auto& oldDef = w.db().building(old.def);
                const Fixed rate = workRate(w, p, content::WorkCategory::Construction, core::kOne);
                p.job.workDone += rate;
                gainSkill(w, p, content::WorkCategory::Construction, rate);
                if (p.job.workDone < p.job.workRequired) break;

                // What comes out of it. Reed and mats are reed and mats again;
                // the share is the replacement's, because it is the replacement
                // that decides how carefully the old one is taken apart.
                const DefId replacement = old.replacedBy;
                const Fixed share = replacement.valid() ? w.db().building(replacement).salvageShare
                                                        : oldDef.salvageShare;
                for (const auto& m : oldDef.materials) {
                    const std::int32_t back = (Fixed::fromInt(m.count) * share).roundToInt();
                    if (back <= 0) continue;
                    const ItemStackId id = w.spawnStack(m.item, back, old.origin, old.settlement);
                    w.report().itemsProduced[w.db().item(m.item).name] += back;
                    tryMergeAtDestination(w, id);
                }

                const TilePos where = old.origin;
                const SettlementId owner = old.settlement;
                const HouseholdId family = old.household;
                for (TilePos t : old.footprintTiles(w.db()))
                    if (w.map().inBounds(t) && w.map().at(t).building == old.id)
                        w.map().at(t).building = BuildingId{};
                w.setBuildingAlive(old.id, false);
                w.release(World::buildingKey(old.id));
                w.report().buildingsDemolished++;

                // And the new one goes up where the old one stood - if it still
                // fits there. The planner checks before committing to the
                // upgrade, and this is the belt to that brace: nothing gets
                // built over ground that filled up while the old one came down.
                if (replacement.valid() && owner.valid() && fitsAt(w, replacement, where))
                    w.placeBlueprint(replacement, where, owner, family);
                for (TilePos t : old.footprintTiles(w.db())) w.refreshBlocked(t);
                finishJob(w, p);
                break;
            }

            case JobKind::Pen: {
                if (!p.job.animal.valid() || !animalAlive(w, w.animal(p.job.animal))) { abandonJob(w, p); break; }
                if (!p.job.building.valid() || !w.buildingAlive(p.job.building)) {
                    abandonJob(w, p);
                    break;
                }
                Animal& beast = w.animal(p.job.animal);
                // Driven, not carried: the animal is set down at the byre and
                // shut in. With a dog the whole flock follows, so a shepherd
                // brings in every animal standing near this one too.
                const TilePos door = w.building(p.job.building).origin;
                bool dog = false;
                auto dogs = w.ecs().view<const ecs::Identity, const ecs::Alive, ecs::Animal>();
                for (const entt::entity entity : dogs) {
                    if (!dogs.get<const ecs::Alive>(entity).value) continue;
                    const auto& d = w.animal(AnimalId{dogs.get<const ecs::Identity>(entity).legacyIndex});
                    if (d.owner == beast.owner && w.db().animal(d.def).guardsFlock) {
                        dog = true;
                        break;
                    }
                }
                const std::int32_t sweep = dog ? kDogDrivesWithin : 0;
                for (auto& q : w.animals()) {
                    const entt::entity qEntity = w.ecsEntity(ecs::Kind::Animal, q.id.value);
                    const bool ecsAlive = qEntity != entt::null && w.ecs().all_of<ecs::Alive>(qEntity)
                        ? w.ecs().get<const ecs::Alive>(qEntity).value : q.alive;
                    const bool ecsPenned = qEntity != entt::null && w.ecs().all_of<ecs::SleepState>(qEntity)
                        ? w.ecs().get<const ecs::SleepState>(qEntity).asleep : q.penned;
                    if (!ecsAlive || ecsPenned || q.owner != beast.owner) continue;
                    if (q.id != beast.id && core::tileDistance(q.tile, beast.tile) > sweep) continue;
                    q.penned = true;
                    if (qEntity != entt::null && w.ecs().all_of<ecs::SleepState>(qEntity))
                        w.ecs().get<ecs::SleepState>(qEntity).asleep = true;
                    q.tile = door;
                    q.pos = core::tileCentre(door);
                    q.grazeTarget = door;
                    const entt::entity targetEntity = w.ecsEntity(ecs::Kind::Animal, q.id.value);
                    if (targetEntity != entt::null && w.ecs().all_of<ecs::GrazingTarget>(targetEntity))
                        w.ecs().get<ecs::GrazingTarget>(targetEntity).tile = door;
                    w.report().animalsPenned++;
                }
                gainSkill(w, p, content::WorkCategory::Herding, p.job.workRequired);
                finishJob(w, p);
                break;
            }

            case JobKind::Fell: {
                // Taking the tree itself. Unlike clearing, this is done for the
                // timber and needs the axe the content asks for; unlike picking,
                // the tree does not come back - what replaces it is seed falling
                // near the ones still standing (D83).
                if (!p.job.node.valid() || !w.resourceNodeAlive(p.job.node)) { abandonJob(w, p); break; }
                ResourceNode& node = w.node(p.job.node);
                const entt::entity nodeEntity = w.ecsEntity(ecs::Kind::ResourceNode, node.id.value);
                auto* resourceState = nodeEntity != entt::null && w.ecs().valid(nodeEntity) &&
                    w.ecs().all_of<ecs::ResourceState>(nodeEntity)
                        ? &w.ecs().get<ecs::ResourceState>(nodeEntity) : nullptr;
                const auto& spec = fellSpecOf(w.db(), w.db().resourceNode(node.def));
                if (spec.yields.empty()) { abandonJob(w, p); break; }
                const Fixed rate = workRate(w, p, spec.category,
                                            toolEfficiencyFor(w, p, spec.preferredTool));
                p.job.workDone += rate;
                gainSkill(w, p, spec.category, rate);
                if (p.job.workDone < p.job.workRequired) break;

                for (const auto& y : spec.yields) {
                    const ItemStackId id = w.spawnStack(y.item, y.count, node.tile, p.settlement);
                    w.report().itemsProduced[w.db().item(y.item).name] += y.count;
                    tryMergeAtDestination(w, id);
                }
                wearTool(w, equippedToolStack(w, p.id), 1);
                rollDiscovery(w, p, spec.category);

                const TilePos where = node.tile;
                w.setResourceNodeAlive(node.id, false);
                node.depleted = true;
                if (nodeEntity != entt::null && w.ecs().valid(nodeEntity) &&
                    w.ecs().all_of<ecs::Alive>(nodeEntity))
                    w.ecs().get<ecs::Alive>(nodeEntity).value = false;
                if (resourceState) {
                    resourceState->depleted = true;
                    resourceState->workDone = core::kZero;
                }
                if (w.map().inBounds(where) && w.map().at(where).node == p.job.node) {
                    w.map().at(where).node = ResourceNodeId{};
                    w.refreshBlocked(where);
                }
                w.report().treesFelled++;
                finishJob(w, p);
                break;
            }

            case JobKind::Clear: {
                // Getting something off ground that is wanted for a field or a
                // building: felling the tree, breaking up the outcrop, pulling
                // out the thicket. Unlike harvesting it does not wait for the
                // right tool - a community with no axe still clears its ground,
                // it just gets no timber out of it.
                if (!p.job.node.valid() || !w.resourceNodeAlive(p.job.node)) { abandonJob(w, p); break; }
                ResourceNode& node = w.node(p.job.node);
                const entt::entity nodeEntity = w.ecsEntity(ecs::Kind::ResourceNode, node.id.value);
                auto* resourceState = nodeEntity != entt::null && w.ecs().valid(nodeEntity) &&
                    w.ecs().all_of<ecs::ResourceState>(nodeEntity)
                        ? &w.ecs().get<ecs::ResourceState>(nodeEntity) : nullptr;
                const auto& def = w.db().resourceNode(node.def);
                const Fixed rate = workRate(w, p, def.harvest.category,
                                            toolEfficiencyFor(w, p, def.harvest.preferredTool));
                p.job.workDone += rate;
                gainSkill(w, p, def.harvest.category, rate);
                if (p.job.workDone < p.job.workRequired) break;

                // What comes of it, if the worker had what it takes to take it.
                const bool depleted = resourceState ? resourceState->depleted : node.depleted;
                if (!depleted && hasToolEquipped(w, p, def.harvest.requiredTool)) {
                    for (const auto& y : def.harvest.yields) {
                        const ItemStackId id = w.spawnStack(y.item, y.count, node.tile, p.settlement);
                        w.report().itemsProduced[w.db().item(y.item).name] += y.count;
                        tryMergeAtDestination(w, id);
                    }
                    wearTool(w, equippedToolStack(w, p.id), 1);
                }
                const TilePos where = node.tile;
                w.setResourceNodeAlive(node.id, false);
                node.depleted = true;
                if (nodeEntity != entt::null && w.ecs().valid(nodeEntity) &&
                    w.ecs().all_of<ecs::Alive>(nodeEntity))
                    w.ecs().get<ecs::Alive>(nodeEntity).value = false;
                if (resourceState) {
                    resourceState->depleted = true;
                    resourceState->workDone = core::kZero;
                }
                if (w.map().inBounds(where) && w.map().at(where).node == p.job.node) {
                    w.map().at(where).node = ResourceNodeId{};
                    w.refreshBlocked(where);
                }
                w.report().nodesCleared++;
                finishJob(w, p);
                break;
            }

            case JobKind::Till:
            case JobKind::Sow:
            case JobKind::ReapCrop: {
                if (!w.map().inBounds(p.job.target)) { abandonJob(w, p); break; }
                const Fixed rate = workRate(w, p, WorkCategory::Farming,
                                            toolEfficiencyFor(w, p, content::ToolClass::Hoe));
                p.job.workDone += rate;
                gainSkill(w, p, WorkCategory::Farming, rate);
                if (p.job.workDone < p.job.workRequired) break;

                Tile& tile = w.map().at(p.job.target);
                if (p.job.kind == JobKind::Till) {
                    tile.tilled = true;
                } else if (p.job.kind == JobKind::Sow) {
                    // Seed comes out of the stores: sowing is the community
                    // choosing not to eat something today.
                    const auto& crop = w.db().crop(p.job.crop);
                    if (!consumeFromStores(w, crop.seedItem, crop.seedCount, p.job.target, kSeedCarryRadius)) {
                        w.report().materialShortageRejections++;
                        abandonJob(w, p);
                        break;
                    }
                    tile.crop = p.job.crop;
                    tile.cropGrowth = core::kZero;
                    w.report().cropsSown++;
                } else {
                    const auto& crop = w.db().crop(tile.crop);
                    // What the tile yields, not what the crop yields. Ground
                    // decides the harvest as much as the seed does, and until it
                    // did, leading water to a field bought a community nothing
                    // it could count: fertility only made the ears come sooner.
                    const Fixed share = Fixed::ratio(1, 2) + effectiveFertility(w, p.job.target) / 2;
                    const std::int32_t yield = std::max<std::int32_t>(
                            1, (Fixed::fromInt(crop.harvestCount) * share).roundToInt());
                    const ItemStackId id = w.spawnStack(crop.harvestItem, yield,
                                                        p.job.target, p.settlement);
                    w.report().itemsProduced[w.db().item(crop.harvestItem).name] += yield;
                    w.report().cropsReaped++;
                    tryMergeAtDestination(w, id);
                    tile.crop = DefId{};
                    tile.cropGrowth = core::kZero;
                    // Stubble still counts as broken ground for one more sowing.
                    tile.tilled = true;
                }
                wearTool(w, equippedToolStack(w, p.id), 1);
                rollDiscovery(w, p, WorkCategory::Farming);
                finishJob(w, p);
                break;
            }

            // --- the flock ---------------------------------------------
            case JobKind::Shear:
            case JobKind::Milk:
            case JobKind::Slaughter: {
                if (!p.job.animal.valid() || p.job.animal.value >= w.animals().size() ||
                    !animalAlive(w, w.animal(p.job.animal))) {
                    abandonJob(w, p);
                    break;
                }
                const Fixed rate = workRate(w, p, WorkCategory::Herding, core::kOne);
                p.job.workDone += rate;
                gainSkill(w, p, WorkCategory::Herding, rate);
                if (p.job.workDone < p.job.workRequired) break;

                Animal& a = w.animal(p.job.animal);
                const auto& adef = w.db().animal(a.def);
                const auto& time = w.db().time();
                const entt::entity animalEntity = w.ecsEntity(ecs::Kind::Animal, a.id.value);
                auto* timers = animalEntity != entt::null && w.ecs().all_of<ecs::AnimalTimers>(animalEntity)
                    ? &w.ecs().get<ecs::AnimalTimers>(animalEntity) : nullptr;
                const std::vector<content::IngredientSpec>* yields = nullptr;
                if (p.job.kind == JobKind::Shear) {
                    yields = &adef.shearYields;
                    a.nextShearTick = w.tickCount() + std::int64_t(adef.shearIntervalDays) * time.ticksPerDay();
                    if (timers) timers->nextShearTick = a.nextShearTick;
                } else if (p.job.kind == JobKind::Milk) {
                    yields = &adef.milkYields;
                    a.nextMilkTick = w.tickCount() + std::int64_t(adef.milkIntervalDays) * time.ticksPerDay();
                    if (timers) timers->nextMilkTick = a.nextMilkTick;
                } else {
                    yields = &adef.slaughterYields;
                    w.setAnimalAlive(a.id, false);
                    if (animalEntity != entt::null && w.ecs().all_of<ecs::Alive>(animalEntity))
                        w.ecs().get<ecs::Alive>(animalEntity).value = false;
                    w.release(World::animalKey(a.id));
                    w.report().animalsLost++;
                }
                for (const auto& y : *yields) {
                    const ItemStackId id = w.spawnStack(y.item, y.count, p.tile, p.settlement);
                    w.report().itemsProduced[w.db().item(y.item).name] += y.count;
                    tryMergeAtDestination(w, id);
                }
                rollDiscovery(w, p, WorkCategory::Herding);
                finishJob(w, p);
                break;
            }

            case JobKind::Wander:
            case JobKind::None:
            case JobKind::Count:
                finishJob(w, p);
                break;
    }

    }

    // Publish the complete job progress slice after all per-person writers
    // have committed. This keeps the ECS component coherent even for branches
    // that finish or abandon a job early.
    auto progressView = w.ecs().view<const ecs::Identity, ecs::Person,
                                      ecs::JobState, ecs::JobProgress>();
    for (const entt::entity entity : progressView) {
        const auto& identity = progressView.get<const ecs::Identity>(entity);
        const Person& p = w.person(PersonId{identity.legacyIndex});
        auto& job = progressView.get<ecs::JobState>(entity);
        auto& progress = progressView.get<ecs::JobProgress>(entity);
        job.kind = static_cast<std::uint8_t>(p.job.kind);
        job.phase = static_cast<std::uint8_t>(p.job.phase);
        job.active = p.job.valid();
        progress.done = p.job.workDone;
        progress.required = p.job.workRequired;
    }

}

} // namespace work
} // namespace sim
