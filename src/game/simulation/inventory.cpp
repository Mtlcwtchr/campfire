#include "game/simulation/inventory.hpp"

#include <algorithm>
#include <limits>

namespace sim {

ItemStackId carriedStack(const World& w, PersonId person) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.value);
    if (entity != entt::null && w.ecs().valid(entity) && w.ecs().all_of<ecs::Carrying>(entity))
        return ItemStackId{w.ecs().get<const ecs::Carrying>(entity).stack};
    return w.person(person).carrying;
}

ItemStackId equippedToolStack(const World& w, PersonId person) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.value);
    if (entity != entt::null && w.ecs().valid(entity) && w.ecs().all_of<ecs::EquippedTool>(entity))
        return ItemStackId{w.ecs().get<const ecs::EquippedTool>(entity).stack};
    return w.person(person).equippedTool;
}

void setCarriedStack(World& w, PersonId person, ItemStackId stack) {
    w.person(person).carrying = stack;
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.value);
    if (entity != entt::null && w.ecs().valid(entity))
        w.ecs().emplace_or_replace<ecs::Carrying>(entity, stack.value);
}

void setEquippedToolStack(World& w, PersonId person, ItemStackId stack) {
    w.person(person).equippedTool = stack;
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.value);
    if (entity != entt::null && w.ecs().valid(entity))
        w.ecs().emplace_or_replace<ecs::EquippedTool>(entity, stack.value);
}

const ecs::ItemStackState* ecsStackState(const World& w, ItemStackId stack) {
    if (!stack.valid()) return nullptr;
    const entt::entity entity = w.ecsEntity(ecs::Kind::ItemStack, stack.value);
    if (entity == entt::null || !w.ecs().valid(entity) ||
        !w.ecs().all_of<ecs::ItemStackState>(entity))
        return nullptr;
    return &w.ecs().get<const ecs::ItemStackState>(entity);
}

bool stackAlive(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return state->alive;
    return stack.valid() && stack.value < w.stacks().size() && w.stack(stack).alive;
}

std::int32_t stackCount(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return state->count;
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).count : 0;
}

DefId stackDefinition(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return DefId{state->definition};
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).def : DefId{};
}

StackWhere stackWhere(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return static_cast<StackWhere>(state->where);
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).where : StackWhere::Ground;
}

BuildingId stackBuilding(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return BuildingId{state->building};
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).building : BuildingId{};
}

TilePos stackTile(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return {state->tileX, state->tileY};
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).tile : TilePos{};
}

Fixed stackFreshness(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return Fixed::fromRaw(state->freshnessRaw);
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).freshness : core::kZero;
}

std::int32_t stackDurability(const World& w, ItemStackId stack) {
    if (const auto* state = ecsStackState(w, stack)) return state->durabilityLeft;
    return stack.valid() && stack.value < w.stacks().size() ? w.stack(stack).durabilityLeft : 0;
}

bool isAvailable(const World& w, ItemStackId stack) {
    return stackAlive(w, stack) && stackCount(w, stack) > 0 &&
           (stackWhere(w, stack) == StackWhere::Ground ||
            stackWhere(w, stack) == StackWhere::InBuilding);
}

void setStackLocation(World& w, ItemStackId stack, StackWhere where, PersonId holder,
                      BuildingId building, TilePos tile) {
    if (!stack.valid() || stack.value >= w.stacks().size()) return;
    ItemStack& legacy = w.stack(stack);
    legacy.where = where;
    legacy.holder = holder;
    legacy.building = building;
    legacy.tile = tile;
    w.syncEcsStack(stack);
}

void setStackCount(World& w, ItemStackId stack, std::int32_t count) {
    if (!stack.valid() || stack.value >= w.stacks().size()) return;
    w.stack(stack).count = count;
    w.syncEcsStack(stack);
}

bool isLoose(const ItemStack& s) {
    return s.alive && s.count > 0 && (s.where == StackWhere::Ground || s.where == StackWhere::InBuilding);
}

bool isAvailable(const ItemStack& s) { return isLoose(s); }

std::int32_t countAvailable(const World& w, DefId item) {
    std::int32_t total = 0;
    for (const auto& s : w.stacks())
        if (isAvailable(s) && s.def == item) total += s.count;
    return total;
}

std::int32_t countAvailable(const World& w, ItemCategory category) {
    std::int32_t total = 0;
    for (const auto& s : w.stacks())
        if (isAvailable(s) && w.db().item(s.def).category == category) total += s.count;
    return total;
}

namespace {

// Common shape for "nearest available batch matching a predicate, unclaimed".
template <typename Pred>
ItemStackId nearestMatching(const World& w, TilePos from, PersonId forWho, Pred pred) {
    ItemStackId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    for (const auto& s : w.stacks()) {
        if (!isAvailable(s) || !pred(s)) continue;
        if (w.isReserved(World::stackKey(s.id), forWho)) continue;
        const std::int32_t d = core::chebyshev(from, s.tile);
        // Ties broken by id so the choice does not depend on vector ordering
        // after slot recycling.
        if (d < bestDist || (d == bestDist && best.valid() && s.id.value < best.value)) {
            bestDist = d;
            best = s.id;
        }
    }
    return best;
}

} // namespace

ItemStackId findNearestAvailable(const World& w, TilePos from, DefId item, PersonId forWho) {
    return nearestMatching(w, from, forWho, [&](const ItemStack& s) { return s.def == item; });
}

const content::HarvestSpec& fellSpecOf(const content::ContentDb& db,
                                       const content::ResourceNodeDef& def) {
    static const content::HarvestSpec kNone;
    if (!def.fell.yields.empty()) return def.fell;
    // A tamarisk or an oak has no second action: what it gives, it gives by
    // being cut down.
    if (def.consumedOnHarvest && def.harvest.category == content::WorkCategory::Woodcutting)
        return def.harvest;
    (void)db;
    return kNone;
}

bool isGathered(const content::ResourceNodeDef& def) {
    if (def.harvest.yields.empty()) return false;
    return !(def.consumedOnHarvest && def.harvest.category == content::WorkCategory::Woodcutting);
}

ItemStackId findNearestEdible(const World& w, const Person& p, BuildingId preferredBuilding) {
    const bool starving = p.satiety < Fixed::ratio(1, 5);
    const bool preferHome = preferredBuilding.valid() && !starving;

    // Grain is food, and that is exactly the problem: left to itself a hungry
    // community eats the seed jar, breaks a hundred and fifty plots in the
    // spring and sows a dozen. So the search runs twice - once past the seed
    // fund, and only if that turns up nothing at all does the community eat
    // next year's harvest.
    for (int pass = 0; pass < 2; ++pass) {
        const bool spendSeed = pass == 1;
        const ItemStackId found = findEdibleStack(w, p, preferredBuilding, starving, preferHome, spendSeed);
        if (found.valid()) return found;
    }
    return {};
}

ItemStackId findEdibleStack(const World& w, const Person& p, BuildingId preferredBuilding,
                            bool starving, bool preferHome, bool spendSeed) {
    const std::vector<std::int32_t>* reserve = nullptr;
    if (!spendSeed && p.settlement.valid()) reserve = &w.settlement(p.settlement).seedReserve;

    ItemStackId best;
    std::int32_t bestScore = std::numeric_limits<std::int32_t>::max();
    for (const auto& s : w.stacks()) {
        if (!isAvailable(s)) continue;
        if (w.isReserved(World::stackKey(s.id), p.id)) continue;
        if (reserve && s.def.value < reserve->size() && (*reserve)[s.def.value] > 0 &&
            countAvailable(w, s.def) <= (*reserve)[s.def.value])
            continue;
        const auto& def = w.db().item(s.def);
        if (def.category != ItemCategory::Food || def.nutrition <= core::kZero) continue;
        if (!def.edibleRaw) continue;                            // needs cooking first
        if (def.spoilDays > 0 && s.freshness <= core::kZero) continue;
        // GDD 7: having a product in store does not mean an autonomous pawn will
        // consider it acceptable. Risky food is a last resort.
        if (def.rawUnsafe && !starving) continue;

        // GDD 7 again: eat what will spoil first. Days of shelf life left dominate
        // the choice, with distance as the tie-break, so a community does not let
        // berries rot beside it while it works through the grain that would have
        // carried it through winter.
        const std::int32_t daysLeft =
                def.spoilDays <= 0 ? 999
                                   : static_cast<std::int32_t>((s.freshness * def.spoilDays).toInt());
        const std::int32_t distance = core::chebyshev(p.tile, s.tile);
        const bool atHome = preferHome && s.where == StackWhere::InBuilding &&
                            s.building == preferredBuilding;
        const std::int32_t score = starving
                                           ? distance
                                           : (preferHome && !atHome ? 1'000'000 : 0) +
                                                     std::min(daysLeft, 60) * 64 + std::min(distance, 60);
        if (score < bestScore || (score == bestScore && best.valid() && s.id.value < best.value)) {
            bestScore = score;
            best = s.id;
        }
    }
    return best;
}

ItemStackId findNearestWearable(const World& w, const Person& p) {
    ItemStackId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    Fixed bestWarmth = core::kZero;
    for (const auto& s : w.stacks()) {
        if (!isAvailable(s)) continue;
        const auto& def = w.db().item(s.def);
        if (def.category != ItemCategory::Clothing || def.insulation <= core::kZero) continue;
        if (w.isReserved(World::stackKey(s.id), p.id)) continue;
        const std::int32_t d = core::tileDistance(p.tile, s.tile);
        // Warmth decides between two garments the same distance away, so the
        // cloak goes to whoever reaches it first rather than the tunic.
        if (d < bestDist || (d == bestDist && def.insulation > bestWarmth)) {
            bestDist = d;
            bestWarmth = def.insulation;
            best = s.id;
        }
    }
    return best;
}

ItemStackId findNearestTool(const World& w, TilePos from, ToolClass cls, PersonId forWho) {
    if (cls == ToolClass::None) return {};
    ItemStackId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    Fixed bestEff = core::kZero;
    for (const auto& s : w.stacks()) {
        if (!isAvailable(s)) continue;
        const auto& def = w.db().item(s.def);
        if (def.toolClass != cls) continue;
        if (def.durability > 0 && s.durabilityLeft <= 0) continue;
        if (w.isReserved(World::stackKey(s.id), forWho)) continue;
        const std::int32_t d = core::chebyshev(from, s.tile);
        if (d < bestDist || (d == bestDist && def.toolEfficiency > bestEff)) {
            bestDist = d;
            bestEff = def.toolEfficiency;
            best = s.id;
        }
    }
    return best;
}

std::int32_t stacksInBuilding(const World& w, BuildingId b) {
    std::int32_t n = 0;
    for (const auto& s : w.stacks())
        if (s.alive && s.count > 0 && s.where == StackWhere::InBuilding && s.building == b) ++n;
    return n;
}

BuildingId findStorageFor(const World& w, DefId item, TilePos near) {
    const auto& itemDef = w.db().item(item);
    BuildingId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    for (const auto& b : w.buildings()) {
        if (!b.alive || b.state != BuildState::Complete) continue;
        const auto& def = w.db().building(b.def);
        if (def.storageSlots <= 0) continue;
        if (!def.acceptsCategories.empty()) {
            const bool ok = std::find(def.acceptsCategories.begin(), def.acceptsCategories.end(),
                                      itemDef.category) != def.acceptsCategories.end();
            if (!ok) continue;
        }
        if (stacksInBuilding(w, b.id) >= def.storageSlots) continue;
        const std::int32_t d = core::chebyshev(near, b.origin);
        if (d < bestDist) { bestDist = d; best = b.id; }
    }
    return best;
}

BuildingId findFreeWorkplace(const World& w, const std::vector<DefId>& buildingDefs, TilePos near,
                             PersonId forWho) {
    BuildingId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    for (const auto& b : w.buildings()) {
        if (!b.alive || b.state != BuildState::Complete) continue;
        if (std::find(buildingDefs.begin(), buildingDefs.end(), b.def) == buildingDefs.end()) continue;
        // A mill has room for three; a kiln for one. Counting the people already
        // at it, rather than treating any workplace as taken by the first comer,
        // is what lets a settlement have one mill instead of four hand-querns.
        std::int32_t busy = 0;
        auto people = w.ecs().view<const ecs::Identity, const ecs::JobState,
                                    const ecs::JobLinks, ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& identity = people.get<const ecs::Identity>(entity);
            const auto& jobState = people.get<const ecs::JobState>(entity);
            const auto& links = people.get<const ecs::JobLinks>(entity);
            if (identity.legacyIndex != forWho.value && jobState.active &&
                links.building == b.id.value) ++busy;
        }
        if (busy >= w.db().building(b.def).workerSlots) continue;
        const std::int32_t d = core::chebyshev(near, b.origin);
        if (d < bestDist) { bestDist = d; best = b.id; }
    }
    return best;
}

BuildingId findFireSource(const World& w, TilePos near) {
    BuildingId best;
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    for (const auto& b : w.buildings()) {
        if (!b.alive || b.state != BuildState::Complete) continue;
        if (!w.db().building(b.def).providesFire) continue;
        const std::int32_t d = core::chebyshev(near, b.origin);
        if (d < bestDist) { bestDist = d; best = b.id; }
    }
    return best;
}

bool findWaterTile(const World& w, TilePos from, TilePos& out) {
    // Water is not exhausted by everyday drawing (GDD 7), so this only has to
    // find the nearest shore tile, not track a volume.
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    bool found = false;
    const auto& m = w.map();
    for (std::int32_t y = 0; y < m.height(); ++y) {
        for (std::int32_t x = 0; x < m.width(); ++x) {
            const TilePos p{x, y};
            if (m.at(p).terrain != Terrain::Water) continue;
            const std::int32_t d = core::chebyshev(from, p);
            if (d < bestDist) { bestDist = d; out = p; found = true; }
        }
    }
    return found;
}

ItemStackId splitStack(World& w, ItemStackId from, std::int32_t count) {
    {
        const ItemStack& src = w.stack(from);
        if (!src.alive || src.count < count || count <= 0) return {};
        if (src.count == count) return from;
    }

    // Copy everything needed out of the source first. spawnStack can grow the
    // batch vector, and a reference taken before it is dangling afterwards -
    // writing the decremented count through one corrupted the store silently and
    // only surfaced as a pawn holding a tool that did not think it was held.
    const DefId def = w.stack(from).def;
    const TilePos tile = w.stack(from).tile;
    const SettlementId owner = w.stack(from).owner;
    const Fixed freshness = w.stack(from).freshness;
    const std::int32_t durability = w.stack(from).durabilityLeft;

    const ItemStackId nid = w.spawnStack(def, count, tile, owner);
    {
        ItemStack& dst = w.stack(nid);
        dst.freshness = freshness;
        dst.durabilityLeft = durability;
    }
    w.stack(from).count -= count;
    return nid;
}

void tryMergeAtDestination(World& w, ItemStackId id) {
    ItemStack& s = w.stack(id);
    if (!s.alive || s.count <= 0) return;
    const auto& def = w.db().item(s.def);
    if (def.category == ItemCategory::Tool || def.category == ItemCategory::Weapon) return;  // individual items

    for (auto& other : w.stacks()) {
        if (other.id == s.id || !other.alive || other.count <= 0) continue;
        if (other.def != s.def || other.where != s.where) continue;
        if (s.where == StackWhere::Ground && !(other.tile == s.tile)) continue;
        if (s.where == StackWhere::InBuilding && other.building != s.building) continue;
        if (other.count >= def.stackLimit) continue;

        const std::int32_t room = def.stackLimit - other.count;
        const std::int32_t moved = std::min(room, s.count);
        // Freshness of the merged batch is the weighted mean, so mixing a fresh
        // delivery into an old one does not silently rejuvenate it.
        if (def.spoilDays > 0) {
            const std::int64_t total = other.count + moved;
            other.freshness = Fixed::fromRaw((other.freshness.raw * other.count + s.freshness.raw * moved) / total);
        }
        other.count += moved;
        s.count -= moved;
        if (s.count <= 0) { w.destroyStack(s.id); return; }
    }
}

void consume(World& w, ItemStackId id, std::int32_t count) {
    ItemStack& s = w.stack(id);
    s.count -= count;
    if (s.count <= 0) w.destroyStack(id);
}

bool consumeFromStores(World& w, DefId item, std::int32_t count, TilePos near, std::int32_t radius) {
    // Look before taking: a half-consumed sowing would destroy seed and plant
    // nothing.
    std::int32_t found = 0;
    for (const auto& s : w.stacks()) {
        if (!isAvailable(s) || s.def != item) continue;
        const TilePos at = s.where == StackWhere::InBuilding ? w.building(s.building).origin : s.tile;
        if (core::tileDistance(at, near) > radius) continue;
        found += s.count;
        if (found >= count) break;
    }
    if (found < count) return false;

    std::int32_t remaining = count;
    for (auto& s : w.stacks()) {
        if (remaining <= 0) break;
        if (!isAvailable(s) || s.def != item) continue;
        const TilePos at = s.where == StackWhere::InBuilding ? w.building(s.building).origin : s.tile;
        if (core::tileDistance(at, near) > radius) continue;
        const std::int32_t take = std::min(remaining, s.count);
        s.count -= take;
        remaining -= take;
        if (s.count <= 0) w.destroyStack(s.id);
    }
    return true;
}

void wearTool(World& w, ItemStackId tool, std::int32_t amount) {
    if (!tool.valid()) return;
    ItemStack& s = w.stack(tool);
    if (!s.alive) return;
    const auto& def = w.db().item(s.def);
    if (def.durability <= 0) return;
    s.durabilityLeft -= amount;
    if (s.durabilityLeft <= 0) w.destroyStack(tool);
}

void tickSpoilage(World& w) {
    const auto& cfg = w.db().time();
    // Once a day is enough resolution for food that lasts days, and keeps the
    // per-tick cost of the whole item store at zero on 99% of ticks.
    if (w.tickCount() % cfg.ticksPerDay() != 0) return;

    std::vector<ItemStackId> rotted;
    for (auto& s : w.stacks()) {
        if (!s.alive || s.count <= 0) continue;
        const auto& def = w.db().item(s.def);
        if (def.spoilDays <= 0) continue;

        Fixed rate = Fixed::ratio(1, def.spoilDays);
        if (s.where == StackWhere::InBuilding && w.building(s.building).state == BuildState::Complete)
            rate = rate * w.db().building(w.building(s.building).def).spoilRateMultiplier;
        rate = rate * def.cookedSpoilMultiplier;

        s.freshness = s.freshness - rate;
        if (s.freshness <= core::kZero) rotted.push_back(s.id);
    }
    for (auto id : rotted) {
        // Rot is not deletion: it becomes a pollution source at its location
        // (GDD 7, hybrid sanitation model).
        const ItemStack& s = w.stack(id);
        if (s.where == StackWhere::Ground && w.map().inBounds(s.tile))
            w.map().at(s.tile).pollution = core::min(core::kOne, w.map().at(s.tile).pollution + Fixed::ratio(1, 4));
        w.destroyStack(id);
    }
}

} // namespace sim
