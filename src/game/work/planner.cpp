#include "game/work/planner.hpp"

#include "game/ecs/construction/components.hpp"
#include "game/ecs/entity.hpp"
#include "game/ecs/resources/components.hpp"
#include "game/ecs/life/components.hpp"

#include <algorithm>
#include <functional>
#include <limits>

#include "game/simulation/inventory.hpp"
#include "game/simulation/needs.hpp"
#include "game/simulation/pathfinder.hpp"
#include "game/simulation/farming.hpp"
#include "game/simulation/livestock.hpp"
#include "game/simulation/zones.hpp"

namespace sim {
namespace work {
namespace {

bool personAlive(const World& w, const Person& person) {
    return w.personAlive(person.id);
}
bool animalAlive(const World& w, const Animal& animal) {
    return w.animalAlive(animal.id);
}


bool resourceAlive(const World& w, const ResourceNode& node) {
    return w.resourceNodeAlive(node.id);
}

bool resourceDepleted(const World& w, const ResourceNode& node) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::ResourceNode, node.id.value);
    if (entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::ResourceState>(entity))
        return w.ecs().get<const ecs::ResourceState>(entity).depleted;
    return node.depleted;
}

bool buildingComplete(const World& w, BuildingId id) {
    return w.buildingComplete(id);
}


Fixed constructionProgressOf(const World& w, const Building& building) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::Building, building.id.value);
    if (entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::ConstructionProgress>(entity))
        return w.ecs().get<const ecs::ConstructionProgress>(entity).done;
    return building.workDone;
}

std::int32_t deliveredMaterial(const World& w, const Building& building, std::size_t index) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::Building, building.id.value);
    if (entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::DeliveredMaterials>(entity)) {
        const auto& values = w.ecs().get<const ecs::DeliveredMaterials>(entity).values;
        if (index < values.size()) return values[index];
        return 0;
    }
    return index < building.delivered.size() ? building.delivered[index] : 0;
}

// A pawn looks for work near itself first. Considering every tree on the map
// costs a path search each and almost never changes the answer, so the planner
// works in two passes: everything within the near radius, and only if that finds
// nothing, everything within the far one.
constexpr std::int32_t kNearWorkRadius = 26;

// How many batches a single stockpile tile can hold.
constexpr std::int32_t kBatchesPerStockpileTile = 4;
// The far pass is deliberately not "the whole map". Ordinary work stays within
// reach of home: a community that sends everybody two hundred metres out to fell
// one tree spends its day walking. Going further than this is the scouts' job,
// and they go alone.
constexpr std::int32_t kFarWorkRadius = 55;

// Ticks a pawn waits before asking again after finding nothing at all to do.
constexpr std::int64_t kIdleReplanDelay = 12;

// How many people a settlement will have out beyond the known edge at once, how
// far it is willing to send them, and how much of the land they take in when they
// get there.
// People served by one more of a production building, beyond the first.
constexpr std::int32_t kPeoplePerWorkshop = 20;

// Days running that a community may want the same building it cannot get before
// that plan steps aside for the next one.
constexpr std::int32_t kPlanAttemptLimit = 6;

// How many stores a settlement keeps before it stops adding them, plus one for
// every four people.
constexpr std::int32_t kStoresPerSettlement = 4;

// Plots of broken but unsown ground the community will carry per pair of hands
// before it stops breaking more.
constexpr std::int32_t kIdleGroundAllowance = 3;

// Days of food in store below which a roof waits and the hands go to the food.
constexpr std::int32_t kRoofYieldsToHungerDays = 8;

// Days of food in store below which a child, and then an elder, is no longer
// fed while there are working adults to feed. Nobody is refused a meal at the
// very edge of death - the last clause of `mayEat` - but the order is real.
const Fixed kChildRationDays = Fixed::ratio(3, 2);
const Fixed kElderRationDays = Fixed::ratio(3, 4);

// Days of food in store at which a community can keep to its trades, and at
// which it drops them and everybody does whatever is short.
// How much a pawn prefers its own trade to anything else.
// Now that a day's food costs a quarter of the work it used to (D49), a
// community can afford to let people keep to their trades: the shepherd is not
// needed on the threshing floor. Hunger still lifts this - see tradeDiscipline.
const Fixed kOwnTradeWeight = Fixed::ratio(5, 2);
const Fixed kOtherTradeWeight = Fixed::ratio(9, 20);

// Batches of waiting input, per workshop already serving the recipe, that say
// the workshop is the bottleneck rather than the harvest.
constexpr std::int32_t kWorkshopBacklogBatches = 24;

// Pulling a building down is a fraction of the work of putting it up.
const Fixed kDemolishShareOfBuilding = Fixed::ratio(1, 4);

// How far out a wild animal is still work worth offering. Beyond this it is
// scenery: on the map, breeding, and nobody's business today.
constexpr std::int32_t kWildWorthLookingAt = 60;

// How close to the byre an animal counts as inside it, and what bringing one in
// is worth against the rest of an evening's work.
constexpr std::int32_t kPennedWithin = 4;
constexpr std::int32_t kPenningWorth = 5;
// How far a flock is worth driving home for the night.
constexpr std::int32_t kDriveHomeReach = 30;

// What a wolf pup is worth to a herding people, and what a wolf hanging about
// the flock is worth dead.
constexpr std::int32_t kTamingWorth = 14;
constexpr std::int32_t kWolfBountyToAFlock = 12;

// What the second leg of a haul is worth on its own account: little enough that
// anybody with work of their own does that instead.
const Fixed kSecondLegWorth = Fixed::ratio(1, 8);

// How many times nearer a roof has to be than the stores before an armful is
// dropped there instead of carried all the way in. Three: a short walk with the
// load and a long one later, by whoever is free, instead of a long walk now.
constexpr std::int32_t kStagingDetour = 3;

// What a building gives up by standing on ground that could be sown.
constexpr std::int64_t kBuildOnFieldCost = 150;
// What a building gives up by standing outside the area it belongs to. Enough to
// outweigh a dozen tiles of walking, so a house goes to the residential quarter
// and a kiln to the craftsmen's; not enough to stop anything being built when
// the right quarter is full or has not been laid out yet.
constexpr std::int64_t kOutsideItsQuarterCost = 60;

// Soil this good needs no channel. The floodplain silt runs to nine tenths and
// better; the dry ground behind it is a fifth, and that is what irrigation is
// for. A channel dug through land that floods anyway is a plot spent for nothing
// (D88).
const Fixed kSoilWorthWatering = Fixed::ratio(4, 5);
// What a length of channel has to be worth to be worth digging: about three
// tiles of desert brought up to silt. In hundredths of fertility, summed over
// everything the length would reach.
constexpr std::int64_t kChannelWorthDigging = 180;
// And how much soil the trunk may spend getting out of the floodplain before the
// community gives up on the idea.
constexpr std::int32_t kTrunkTilesAllowed = 45;

// How far from a family's place a roof still counts as that family's.
constexpr std::int32_t kQuarterRadius = 5;

// A wall is worth the work only for a settlement of some size, and only while
// there is food enough that the season spent on it costs nobody a meal.
constexpr std::int32_t kWallPopulation = 8;
constexpr std::int32_t kWallFoodDays = 14;

constexpr std::int32_t kMaxScouts = 2;
constexpr std::int32_t kScoutRange = 70;
const Fixed kScoutLookWork = Fixed::fromInt(30);

// How far from a work site its inputs may lie and still count as "to hand".
// Inside this radius a crafter is treated as walking between piles, which is not
// worth simulating step by step; beyond it, hauling becomes a real job.

// Demand propagates backwards through recipes with this decay, so an input two
// steps from a real need is wanted, but less than the thing actually needed.
const Fixed kDemandDecay = Fixed::ratio(3, 5);
constexpr int kDemandPropagationRounds = 4;

// How many days of food the community tries to keep standing. GDD 7 asks storage
// and processing to bridge the season of plenty to the season of want, so the
// target is seasonal: in autumn a community that only keeps a week of food is
// already dead, it just does not know it yet.
std::int32_t foodDaysTarget(core::Season season) {
    // The horizon has to reach past the lean season to the next harvest, not just
    // a comfortable few days: a winter target of ten days ran the stores dry on
    // day thirteen of a fifteen-day winter with a spring of nothing behind it.
    switch (season) {
        case core::Season::Spring: return 12;
        case core::Season::Summer: return 16;
        case core::Season::Autumn: return 32;
        case core::Season::Winter: return 22;
    }
    return 16;
}

Fixed distanceWeight(std::int32_t tiles) {
    // 1 / (1 + d/12): near work is preferred, but distance never fully vetoes a
    // job that is genuinely needed.
    return core::kOne / (core::kOne + Fixed::ratio(tiles, 12));
}

// Value alone would have a pawn spend four hours prising flint out of a rock for
// slightly less than a berry bush gives in twenty minutes. What matters is the
// return per unit of effort.
Fixed effortWeight(Fixed workRequired) {
    return core::kOne / (core::kOne + workRequired / Fixed::fromInt(90));
}

// What finishing this building is worth. Like everything else the planner weighs,
// it has to be on the same scale as a meal: a hardcoded constant here had a
// community raising a storage pit while every one of its members starved.
Fixed buildingValue(const World& w, const Settlement& st, const content::BuildingDef& def,
                    std::int32_t population) {
    switch (def.kind) {
        case content::BuildingKind::Hearth:
            // Fire is warmth, cooking and boiled water. The first one is worth a lot.
            return findFireSource(w, st.hearth).valid() ? Fixed::fromInt(2) : Fixed::fromInt(10);
        case content::BuildingKind::Storage: {
            // A roof over the stores is worth what it stops from rotting.
            std::int32_t slots = 0;
            auto buildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                           const ecs::ConstructionProgress, ecs::Building>();
            for (const entt::entity entity : buildings) {
                if (!buildings.get<const ecs::Alive>(entity).value ||
                    !buildings.get<const ecs::ConstructionProgress>(entity).complete) continue;
                const auto& b = w.building(BuildingId{
                    buildings.get<const ecs::Identity>(entity).legacyIndex});
                if (b.settlement == st.id) slots += w.db().building(b.def).storageSlots;
            }
            return slots > 0 ? Fixed::fromInt(2) : Fixed::fromInt(8);
        }
        case content::BuildingKind::Housing: {
            std::int32_t quarters = 0;
            auto buildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                           const ecs::ConstructionProgress, ecs::Building>();
            for (const entt::entity entity : buildings) {
                if (!buildings.get<const ecs::Alive>(entity).value ||
                    !buildings.get<const ecs::ConstructionProgress>(entity).complete) continue;
                const auto& b = w.building(BuildingId{
                    buildings.get<const ecs::Identity>(entity).legacyIndex});
                if (b.settlement == st.id)
                    quarters += w.db().building(b.def).sleepingSlots * kBedQuarters;
            }
            if (quarters >= population * kBedQuarters) return core::kZero;
            // A roof matters, and it matters less than a full larder. Two dozen
            // people arriving on empty ground need twelve reed huts, and the
            // labour that went into them in the first two months was labour that
            // did not go into food: they died in the first winter with the huts
            // standing. Hunger puts the building down and picks it up again.
            return st.foodDays < Fixed::fromInt(kRoofYieldsToHungerDays) ? core::kOne
                                                                        : Fixed::fromInt(6);
        }
        case content::BuildingKind::Workshop:      return Fixed::fromInt(5);
        case content::BuildingKind::Fortification: return Fixed::fromInt(2);
        default:                                   return Fixed::fromInt(3);
    }
}

} // namespace

std::int32_t bedQuartersFor(const Person& p) {
    switch (p.stage) {
        case LifeStage::Child: return 2;
        case LifeStage::Elder: return 3;
        default:               return 4;
    }
}

BuildingId homeForPerson(const World& w, const Person& target) {
    struct Home {
        BuildingId id;
        std::int32_t remaining = 0;
        std::int64_t comfort = 0;
    };
    std::vector<Home> homes;
    for (const auto& building : w.buildings()) {
        if (!buildingComplete(w, building.id) ||
            building.settlement != target.settlement) continue;
        const auto& def = w.db().building(building.def);
        if (def.kind != content::BuildingKind::Housing || def.sleepingSlots <= 0) continue;
        homes.push_back({building.id, def.sleepingSlots,
                         std::int64_t(def.sleepingSlots) * 100 + (def.warmthBonus * 10).roundToInt() +
                                 (def.comfortBonus * 100).roundToInt()});
    }
    std::sort(homes.begin(), homes.end(), [](const Home& a, const Home& b) {
        return a.comfort != b.comfort ? a.comfort > b.comfort : a.id.value < b.id.value;
    });
    if (homes.empty()) return {};

    // Their own family's house first. A house belongs to the family it was
    // raised for, and families do not share one by default: counting beds across
    // the settlement made four houses look like enough for six families.
    if (target.household.valid()) {
        BuildingId own;
        std::int32_t bestComfort = -1;
        for (const auto& building : w.buildings()) {
            if (!buildingComplete(w, building.id)) continue;
            if (building.household != target.household) continue;
            const auto& def = w.db().building(building.def);
            if (def.kind != content::BuildingKind::Housing || def.sleepingSlots <= 0) continue;
            const std::int32_t comfort = (def.comfortBonus * 100).roundToInt();
            if (comfort > bestComfort) { bestComfort = comfort; own = building.id; }
        }
        if (own.valid()) return own;
    }

    // Household ids are created in deterministic order. Whole families claim a
    // house before the next family is considered; ungrouped people are singletons.
    for (std::size_t i = 0; i < w.people().size(); ++i) {
        const auto& representative = w.people()[i];
        if (!personAlive(w, representative) || representative.settlement != target.settlement) continue;

        bool alreadySeen = false;
        if (representative.household.valid()) {
            for (std::size_t j = 0; j < i; ++j) {
                const auto& earlier = w.people()[j];
                if (personAlive(w, earlier) && earlier.settlement == target.settlement &&
                    earlier.household == representative.household) { alreadySeen = true; break; }
            }
        }
        if (alreadySeen) continue;

        std::int32_t members = 0;
        bool containsTarget = false;
        for (const auto& person : w.people()) {
            if (!personAlive(w, person) || person.settlement != target.settlement) continue;
            const bool same = representative.household.valid()
                                      ? person.household == representative.household
                                      : person.id == representative.id;
            if (!same) continue;
            ++members;
            if (person.id == target.id) containsTarget = true;
        }

        std::size_t chosen = homes.size();
        for (std::size_t h = 0; h < homes.size(); ++h) {
            if (homes[h].remaining >= members) { chosen = h; break; }
        }
        if (chosen == homes.size()) {
            // Early shelters may be smaller than a household. Use their places
            // without overfilling; a later, larger house reunites the family.
            for (const auto& person : w.people()) {
                if (!personAlive(w, person) || person.settlement != target.settlement) continue;
                const bool same = representative.household.valid()
                                          ? person.household == representative.household
                                          : person.id == representative.id;
                if (!same) continue;
                std::size_t freeHome = homes.size();
                for (std::size_t h = 0; h < homes.size(); ++h)
                    if (homes[h].remaining > 0) { freeHome = h; break; }
                if (person.id == target.id)
                    return freeHome < homes.size() ? homes[freeHome].id : BuildingId{};
                if (freeHome < homes.size()) --homes[freeHome].remaining;
            }
            continue;
        }
        if (containsTarget) return homes[chosen].id;
        homes[chosen].remaining = std::max(0, homes[chosen].remaining - members);
    }
    return {};
}

bool hasToolEquipped(const World& w, const Person& p, ToolClass cls) {
    if (cls == ToolClass::None) return true;
    const ItemStackId equipped = equippedToolStack(w, p.id);
    if (!equipped.valid()) return false;
    const auto& s = w.stack(equipped);
    const auto* state = ecsStackState(w, equipped);
    if (state ? !state->alive : !s.alive) return false;
    const auto& def = w.db().item(stackDefinition(w, equipped));
    return def.toolClass == cls && (def.durability <= 0 || stackDurability(w, equipped) > 0);
}

std::int32_t desiredGarments(const World& w, const Person& p) {
    // Everybody wants something on. When the body is already losing heat, a second
    // layer is worth the trip.
    std::int32_t want = 1;
    if (p.bodyTempOffset < -Fixed::fromInt(4)) want = 2;
    if (p.bodyTempOffset < -Fixed::fromInt(12)) want = 3;
    (void)w;
    return want;
}

bool skilledEnough(const Person& p, const content::SkillRequirement& required) {
    if (!required.demanded()) return true;
    return p.skills[static_cast<std::size_t>(required.category)].level >= required.level;
}

bool knowsMethod(const World& w, const Person& p, DefId knowledgeDef) {
    if (!knowledgeDef.valid()) return true;   // universally known method
    if (p.knows(knowledgeDef)) return true;
    // A method that became a tradition of the settlement is known to everyone in it.
    if (!p.settlement.valid()) return false;
    const auto& s = w.settlement(p.settlement);
    return std::find(s.traditions.begin(), s.traditions.end(), knowledgeDef) != s.traditions.end();
}

Fixed workRate(const World& w, const Person& p, WorkCategory category, Fixed toolEfficiency) {
    const auto& cfg = w.db().sim();
    const Skill& skill = p.skills[static_cast<std::size_t>(category)];

    Fixed rate = cfg.baseWorkPerHour / std::int64_t(w.db().time().ticksPerHour);
    rate = rate * (core::kOne + cfg.skillWorkBonusPerLevel * std::int64_t(skill.level));
    rate = rate * workCapacity(w, p);
    rate = rate * toolEfficiency;

    // Physical work leans on the body, fine work on the hands.
    switch (category) {
        case WorkCategory::Woodcutting:
        case WorkCategory::Mining:
        case WorkCategory::Construction:
        case WorkCategory::Hauling:
            rate = rate * (Fixed::ratio(1, 2) + p.strength / 2);
            break;
        case WorkCategory::Crafting:
        case WorkCategory::Cooking:
        case WorkCategory::Medical:
            rate = rate * (Fixed::ratio(1, 2) + p.dexterity / 2);
            break;
        default:
            rate = rate * (Fixed::ratio(1, 2) + p.endurance / 2);
            break;
    }
    return core::max(Fixed::ratio(1, 100), rate);
}

// ---------------------------------------------------------------------------
// Demand
// ---------------------------------------------------------------------------
Demand computeDemand(const World& w, SettlementId sid) {
    const auto& db = w.db();
    Demand d;
    d.item.assign(db.items().size(), core::kZero);
    d.wanted.assign(db.items().size(), core::kZero);

    std::int32_t population = 0;
    std::int32_t adults = 0;
    std::int32_t hungry = 0;
    std::int32_t cold = 0;
    auto people = w.ecs().view<const ecs::Identity, const ecs::Hunger, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        const auto& hunger = people.get<const ecs::Hunger>(entity);
        const auto& p = w.person(PersonId{identity.legacyIndex});
        if (p.settlement != sid) continue;
        ++population;
        if (p.stage != LifeStage::Child) ++adults;
        if (hunger.value < w.db().sim().hungerEatThreshold) ++hungry;
        if (p.bodyTempOffset < -Fixed::fromInt(6)) ++cold;
    }
    if (population == 0) return d;

    std::vector<std::int32_t> have(db.items().size(), 0);
    auto stackView = w.ecs().view<const ecs::Identity, const ecs::ItemStack,
                                   const ecs::ItemStackState>();
    for (const entt::entity entity : stackView) {
        const auto& state = stackView.get<const ecs::ItemStackState>(entity);
        const DefId definition{state.definition};
        const StackWhere where = static_cast<StackWhere>(state.where);
        const bool alive = state.alive;
        const std::int32_t count = state.count;
        if (!alive || count <= 0) continue;
        const bool available = alive && count > 0 &&
                               (where == StackWhere::Ground || where == StackWhere::InBuilding);
        if ((available || where == StackWhere::Equipped) && count > 0)
            have[definition.value] += count;
    }

    // Demand is the value of acquiring one more unit, on one scale shared by
    // food, materials and tools. Mixing scales - "units of food still wanted"
    // against "3 per missing tool" - made a hungry community incapable of ever
    // stopping to make an axe, which is how it starved next to a forest.
    // Survival food outranks a first tool while the stores are empty; the term
    // collapses to nothing the moment the seasonal target is met, which is when
    // toolmaking and building get their turn.
    static const Fixed kFoodValueScale = Fixed::fromInt(10);
    static const Fixed kMaterialValue = Fixed::fromInt(3);
    // A tool is worth more per unit than any single meal, because it changes
    // every chain downstream - but not more than the meal a starving community
    // needs today. Toolmaking is what surplus time is for: it wins as soon as the
    // seasonal food target is met and food's value collapses to nothing.
    static const Fixed kToolValue = Fixed::fromInt(6);
    static const Fixed kClothingValue = Fixed::fromInt(2);

    // Cloth is worth what the cold makes it worth. A flat two points had a
    // community with a flock in the pasture and no coat on anybody's back leave
    // the fleeces on the sheep and die of exposure in the winter. Unlike hunger
    // pressure this is applied before the chain is propagated: there is no
    // shortcut to a cloak, so the whole wool and flax chain has to be pulled.
    const Fixed coldPressure = core::kOne + Fixed::ratio(2 * cold, std::max(1, population));

    // --- food -----------------------------------------------------------
    // Two numbers, because they answer two questions and answering both with one
    // killed communities. A granary of ears is food security; it is not supper.
    // Counted together, a settlement with seventy thousand ears and no bread saw
    // fifteen days of food in store, stopped valuing food at all, stopped
    // baking - and starved with the granary full. Every death note read the
    // same: satiety nought, nine to twenty-four days of food in store.
    Fixed storedNutrition = core::kZero;   // everything that will feed somebody eventually
    Fixed edibleNutrition = core::kZero;   // what a hungry person could eat now
    Fixed emergencySeedNutrition = core::kZero; // seed stock usable only when the larder is empty
    const auto& seedFund = w.settlement(sid).seedReserve;
    for (const entt::entity entity : stackView) {
        const auto& state = stackView.get<const ecs::ItemStackState>(entity);
        const StackWhere where = static_cast<StackWhere>(state.where);
        const bool available = state.alive && state.count > 0 &&
                               (where == StackWhere::Ground || where == StackWhere::InBuilding);
        if (!available) continue;
        const DefId definition{state.definition};
        const std::int32_t count = state.count;
        const Fixed freshness = Fixed::fromRaw(state.freshnessRaw);
        const auto& def = db.item(definition);
        if (def.category != ItemCategory::Food) continue;
        const Fixed value = def.nutrition * std::int64_t(count) * freshness;
        storedNutrition += value;
        // The same test the hungry themselves apply (inventory.cpp): raw enough
        // to eat, not spoiled, not risky, and not the seed corn.
        if (!def.edibleRaw || def.rawUnsafe) continue;
        if (def.spoilDays > 0 && freshness <= core::kZero) continue;
        if (definition.value < seedFund.size() && seedFund[definition.value] > 0 &&
            countAvailable(w, definition) <= seedFund[definition.value]) {
            emergencySeedNutrition += value;
            continue;
        }
        edibleNutrition += value;
    }
    // A reserve protects next season only while somebody can eat something else.
    // Treating the entire reserve as untouchable when the larder is empty made a
    // settlement die beside its own grain. The planner still prefers non-seed
    // food whenever it exists; this is the famine fallback used by the personal
    // need pass as well.
    if (edibleNutrition <= core::kZero) edibleNutrition = emergencySeedNutrition;
    // One person eats roughly two units of satiety a day.
    const std::int32_t horizonDays = foodDaysTarget(w.now().season);
    const Fixed wantedNutrition =
            Fixed::fromInt(population) * Fixed::fromInt(horizonDays) * Fixed::fromInt(2);
    // What the community is short of is supper, not stock: the gap is measured
    // against what can be eaten, so a full granary and an empty oven still pull
    // the whole chain - reap, thresh, grind, bake - instead of reading as plenty.
    const Fixed nutritionGap = core::max(core::kZero, wantedNutrition - edibleNutrition);
    if (population > 0) {
        d.foodDays = edibleNutrition / Fixed::fromInt(2 * population);
        d.larderDays = storedNutrition / Fixed::fromInt(2 * population);
    }
    const Fixed foodUrgency = wantedNutrition > core::kZero ? core::saturate(nutritionGap / wantedNutrition)
                                                           : core::kZero;

    // What a unit of food is worth is what will still be edible when it is
    // needed. GDD 7 asks storage and processing to carry a community from the
    // season of plenty into the season of want; a berry that keeps five days
    // cannot serve a thirty-day autumn target, however filling it is today.
    // An empty store is a plan going wrong; empty stomachs are a plan already
    // failed. Without this term a community spent its first week making an
    // excellent set of flint tools and then starved holding them, because a
    // store-level shortfall alone never outbid a knife.
    const Fixed hungerPressure =
            core::kOne + Fixed::ratio(4 * hungry, std::max(1, population)) * std::int64_t(1);

    const Fixed immediateShare = Fixed::ratio(std::min(2, horizonDays), horizonDays);
    if (foodUrgency > core::kZero) {
        for (const auto& it : db.items()) {
            if (it.category != ItemCategory::Food || it.nutrition <= core::kZero) continue;
            const Fixed keeps = it.spoilDays <= 0
                                        ? core::kOne
                                        : core::min(core::kOne, Fixed::ratio(it.spoilDays, horizonDays));
            const Fixed weight = immediateShare + (core::kOne - immediateShare) * keeps;
            d.item[it.id.value] += foodUrgency * it.nutrition * kFoodValueScale * weight;
            // Units of this food that would close the whole nutritional gap.
            d.wanted[it.id.value] = core::max(d.wanted[it.id.value],
                                              Fixed::fromInt(have[it.id.value]) + nutritionGap / it.nutrition);
        }
    }

    // --- construction ---------------------------------------------------
    auto constructionBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                               const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity entity : constructionBuildings) {
        if (!constructionBuildings.get<const ecs::Alive>(entity).value ||
            constructionBuildings.get<const ecs::ConstructionProgress>(entity).complete) continue;
        const auto& b = w.building(BuildingId{
            constructionBuildings.get<const ecs::Identity>(entity).legacyIndex});
        if (b.settlement != sid) continue;
        const auto& def = db.building(b.def);
        for (std::size_t i = 0; i < def.materials.size(); ++i) {
            const std::int32_t need = def.materials[i].count -
                                      deliveredMaterial(w, b, i);
            if (need <= 0) continue;
            const DefId item = def.materials[i].item;
            d.item[item.value] = core::max(d.item[item.value], kMaterialValue);
            d.wanted[item.value] += Fixed::fromInt(need);
        }
    }

    // --- tools and clothing ---------------------------------------------
    std::array<std::int32_t, static_cast<std::size_t>(ToolClass::Count)> owned{};
    for (const auto& it : db.items())
        if (it.toolClass != ToolClass::None) owned[static_cast<std::size_t>(it.toolClass)] += have[it.id.value];

    for (const auto& it : db.items()) {
        if (it.toolClass == ToolClass::None) continue;
        const auto cls = static_cast<std::size_t>(it.toolClass);
        // Roughly one tool of each useful class per two workers.
        const std::int32_t want = std::max(1, adults / 2);
        if (owned[cls] >= want) continue;
        d.item[it.id.value] = core::max(d.item[it.id.value], kToolValue);
        d.wanted[it.id.value] = core::max(d.wanted[it.id.value], Fixed::fromInt(want));
        d.tool[cls] += kToolValue;
    }
    for (const auto& it : db.items()) {
        if (it.category == ItemCategory::Clothing && have[it.id.value] < population) {
            // A warmer garment is worth more when it is cold, which is what sends
            // them after hides rather than only linen in autumn.
            const Fixed warmth = core::kOne + it.insulation / 8;
            d.item[it.id.value] =
                    core::max(d.item[it.id.value], kClothingValue * coldPressure * warmth);
            d.wanted[it.id.value] = core::max(d.wanted[it.id.value], Fixed::fromInt(population));
        }
        if (it.containerCapacity > core::kZero && have[it.id.value] < population / 2) {
            d.item[it.id.value] = core::max(d.item[it.id.value], kMaterialValue);
            d.wanted[it.id.value] = core::max(d.wanted[it.id.value], Fixed::fromInt(population / 2));
        }

        // Medicine (D98). Two parts to what a community wants: a standing store,
        // because the wound comes first and the poultice cannot be made after
        // it, and a sharp rise for every person actually laid up right now.
        // Without the first, nobody ever gathers a herb in a good year; without
        // the second, a settlement with three people down carries on making
        // baskets.
        if (it.healsWound > core::kZero || it.healsSickness > core::kZero) {
            std::int32_t needing = 0;
            auto people = w.ecs().view<const ecs::Identity, const ecs::SettlementMember, ecs::Person>();
            for (const entt::entity entity : people) {
                const auto& identity = people.get<const ecs::Identity>(entity);
                const auto& social = people.get<const ecs::SettlementMember>(entity);
                const auto& q = w.person(PersonId{identity.legacyIndex});
                if (social.settlement != sid.value) continue;
                const NeedsSnapshot qNeeds = needsSnapshot(w, q);
                if (qNeeds.ailment == Person::Ailment::None) continue;
                const bool wound = qNeeds.ailment == Person::Ailment::Wound;
                // Only the remedy that answers this hurt: knowing the difference
                // is the whole of herbcraft, and a demand table that does not
                // know it has the community brewing fever draughts for a gash.
                if (wound == (it.healsWound > core::kZero)) ++needing;
            }
            const std::int32_t keep = std::max(2, population / 6);
            const std::int32_t want = keep + needing * 2;
            if (have[it.id.value] < want) {
                // Somebody down is worth more than any tool: a pair of hands
                // lost for a fortnight costs the settlement more than anything
                // else it could be making this hour.
                const Fixed urgency = needing > 0 ? Fixed::fromInt(2 + needing) : core::kOne;
                d.item[it.id.value] = core::max(d.item[it.id.value], kToolValue * urgency);
                d.wanted[it.id.value] = core::max(d.wanted[it.id.value], Fixed::fromInt(want));
            }
        }
    }

    // --- propagate backwards along recipes -------------------------------
    // What a recipe's inputs are worth is what its outputs are worth, discounted
    // for the work still between them; how many are wanted is however many runs
    // of the recipe would close the deficit on its outputs. This is what turns
    // "we want bread" into "someone should be reaping" - and, just as important,
    // what stops the reaping once there is more grain in the ear than the
    // community could ever thresh.
    for (int round = 0; round < kDemandPropagationRounds; ++round) {
        std::vector<Fixed> addValue(d.item.size(), core::kZero);
        std::vector<Fixed> addWanted(d.item.size(), core::kZero);

        for (const auto& r : db.recipes()) {
            Fixed outputValue = core::kZero;
            Fixed runsNeeded = core::kZero;
            for (const auto& o : r.outputs) {
                const Fixed deficit = core::max(core::kZero, d.wanted[o.item.value] - Fixed::fromInt(have[o.item.value]));
                if (deficit <= core::kZero) continue;
                outputValue += d.item[o.item.value] * std::int64_t(o.count);
                runsNeeded = core::max(runsNeeded, deficit / std::int64_t(o.count));
            }
            if (outputValue <= core::kZero || runsNeeded <= core::kZero) continue;

            std::int32_t inputUnits = 0;
            for (const auto& in : r.inputs) inputUnits += in.count;
            if (inputUnits <= 0) continue;

            const Fixed perInputUnit = outputValue * kDemandDecay / std::int64_t(inputUnits);
            for (const auto& in : r.inputs) {
                addValue[in.item.value] = core::max(addValue[in.item.value], perInputUnit);
                addWanted[in.item.value] = core::max(addWanted[in.item.value],
                                                     runsNeeded * std::int64_t(in.count));
            }
        }
        for (std::size_t i = 0; i < d.item.size(); ++i) {
            d.item[i] = core::max(d.item[i], addValue[i]);
            d.wanted[i] = core::max(d.wanted[i], addWanted[i]);
        }
    }

    // Hunger pressure is applied here, after propagation, and only to what can
    // actually be eaten. Applying it before would raise the whole chain equally -
    // and a starving community would go on reaping einkorn past a barn holding
    // nine hundred ears, because a sheaf in the field scored as high as the
    // threshing that would have turned the ones it already had into food.
    for (const auto& it : db.items()) {
        if (it.category != ItemCategory::Food || it.nutrition <= core::kZero) continue;
        if (!it.edibleRaw) continue;
        d.item[it.id.value] = d.item[it.id.value] * hungerPressure;
    }

    // Nothing is worth fetching once there is already more of it standing about
    // than anything downstream can use.
    for (const auto& it : db.items()) {
        Fixed& value = d.item[it.id.value];
        if (value <= core::kZero) continue;
        const Fixed deficit = d.wanted[it.id.value] - Fixed::fromInt(have[it.id.value]);
        if (deficit <= core::kZero) { value = core::kZero; continue; }
        // Interest tapers as the pile approaches what is actually wanted.
        if (d.wanted[it.id.value] > core::kZero)
            value = value * core::saturate(deficit / d.wanted[it.id.value] + Fixed::ratio(1, 4));
    }
    return d;
}

// ---------------------------------------------------------------------------
// Candidate jobs
// ---------------------------------------------------------------------------
namespace {


// ---------------------------------------------------------------------------
// Per-tick index
//
// Every pawn planning in the same tick asks the same questions of the same world:
// where is item X, which storage tiles have room, what is lying loose. Answering
// those from scratch per pawn per candidate made the planner quadratic in the
// number of batches. The index is built once per tick and thrown away, so nothing
// can go stale between ticks.
// ---------------------------------------------------------------------------
struct TickCache {
    // Available batches (ground or stored) grouped by item def.
    std::vector<std::vector<ItemStackId>> byItem;
    // Batches lying where nobody put them on purpose. A batch already sitting in
    // a stockpile is NOT loose: counting it as such had haulers carrying the same
    // berries between two tiles of the same pile forever.
    std::vector<ItemStackId> loose;
    // Storage tiles with room, and storage buildings with a free slot.
    std::vector<TilePos> freeStorageTiles;
    std::vector<BuildingId> freeStorageBuildings;
    // Batches sitting in an open stockpile while a roofed store has room: worth
    // moving, but only once such a store exists.
    std::vector<ItemStackId> shelterable;
    // For each item, one pile that still has room. Keyed by item rather than by
    // (item, tile) so the lookup is a single find: iterating a hash map to search
    // for a match would also make the choice depend on bucket order, which is
    // exactly the kind of thing that desyncs a lockstep game.
    std::unordered_map<std::uint32_t, TilePos> topUpTile;
    // The wild animals, and how many of each kind there are. Gathered once a
    // tick rather than once per person: offering every animal on the map to
    // every settler every tick cost half the tick budget.
    std::vector<AnimalId> wild;
    std::vector<std::int32_t> wildHead;
    // How many of each wild plant, tree and bank are still standing, so the
    // community can be asked to leave a seed stock the way it leaves a breeding
    // herd.
    std::vector<std::int32_t> standingNodes;
    // Whether any settlement has a dog with its flock. One question, asked once.
    bool anyGuardDog = false;
};

TickCache buildTickCache(const World& w) {
    TickCache c;
    c.byItem.assign(w.db().items().size(), {});

    auto inStockpile = [&](TilePos t) { return insideAnyZoneOfKind(w, ZoneKind::Storage, t); };

    // How many batches sit on each tile, and whether one of them can still take
    // more of a given item.
    std::unordered_map<std::uint64_t, std::int32_t> batchesOnTile;
    std::unordered_map<std::uint32_t, TilePos> roomFor;

    auto stacks = w.ecs().view<const ecs::Identity, const ecs::ItemStackState>();
    for (const entt::entity entity : stacks) {
        const auto& identity = stacks.get<const ecs::Identity>(entity);
        const ItemStackId stack{identity.legacyIndex};
        const auto& state = stacks.get<const ecs::ItemStackState>(entity);
        const StackWhere where = static_cast<StackWhere>(state.where);
        if (!state.alive || state.count <= 0 ||
            (where != StackWhere::Ground && where != StackWhere::InBuilding)) continue;
        const DefId definition{state.definition};
        const std::int32_t count = state.count;
        const TilePos tile{state.tileX, state.tileY};
        c.byItem[definition.value].push_back(stack);
        if (where != StackWhere::Ground) continue;
        const bool stored = inStockpile(tile);
        if (stored) c.shelterable.push_back(stack);
        else c.loose.push_back(stack);
        batchesOnTile[std::hash<core::TilePos>{}(tile)] += 1;
        if (count < w.db().item(definition).stackLimit) {
            // Ties resolved by coordinate, so the pile chosen never depends on
            // iteration order.
            auto it = roomFor.find(definition.value);
            if (it == roomFor.end() ||
                std::pair(tile.x, tile.y) < std::pair(it->second.x, it->second.y))
                roomFor[definition.value] = tile;
        }
    }

    auto buildingView = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                      const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity entity : buildingView) {
        const auto& identity = buildingView.get<const ecs::Identity>(entity);
        const Building& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildingView.get<const ecs::Alive>(entity).value ||
            !buildingView.get<const ecs::ConstructionProgress>(entity).complete) continue;
        const auto& def = w.db().building(b.def);
        if (def.storageSlots <= 0) continue;
        if (stacksInBuilding(w, b.id) < def.storageSlots) c.freeStorageBuildings.push_back(b.id);
    }

    for (const auto& z : w.zones()) {
        if (!z.alive || z.kind != ZoneKind::Storage || z.mode == ZoneMode::Forbidden) continue;
        for (TilePos t : z.tiles) {
            if (!w.map().inBounds(t) || w.map().blocked(t)) continue;
            auto it = batchesOnTile.find(std::hash<core::TilePos>{}(t));
            // A stockpile tile holds a small pile, not a single batch: at one batch
            // per tile the default stockpile filled before the harvest did, and the
            // surplus was left standing in the fields.
            if (it != batchesOnTile.end() && it->second >= kBatchesPerStockpileTile) continue;
            if (w.isReserved(World::tileKey(t))) continue;
            c.freeStorageTiles.push_back(t);
        }
    }

    c.wildHead.assign(w.db().animals().size(), 0);
    c.standingNodes.assign(w.db().resourceNodes().size(), 0);
    auto nodeView = w.ecs().view<const ecs::Identity, const ecs::ResourceNode,
                                  const ecs::Alive, const ecs::ResourceState>();
    for (const entt::entity entity : nodeView) {
        const auto& identity = nodeView.get<const ecs::Identity>(entity);
        const auto& n = w.node(ResourceNodeId{identity.legacyIndex});
        if (!nodeView.get<const ecs::Alive>(entity).value ||
            nodeView.get<const ecs::ResourceState>(entity).depleted) continue;
        c.standingNodes[n.def.value] += 1;
    }
    auto animalView = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                    const ecs::Alive, const ecs::Transform>();
    for (const entt::entity entity : animalView) {
        const auto& identity = animalView.get<const ecs::Identity>(entity);
        const auto& a = w.animal(AnimalId{identity.legacyIndex});
        if (!animalView.get<const ecs::Alive>(entity).value) continue;
        if (w.db().animal(a.def).guardsFlock && a.owner.valid()) c.anyGuardDog = true;
        if (a.owner.valid() || !w.db().animal(a.def).wild) continue;
        c.wildHead[a.def.value] += 1;
        // Only the ones anybody could reach today go in the list. The rest are
        // still on the map and still breeding; they are simply not work yet.
        bool nearAnybody = false;
        const TilePos animalTile = core::toTile(animalView.get<const ecs::Transform>(entity).value);
        for (const auto& st : w.settlements())
            if (st.alive && core::tileDistance(animalTile, st.hearth) <= kWildWorthLookingAt)
                nearAnybody = true;
        if (nearAnybody) c.wild.push_back(a.id);
    }

    c.topUpTile = std::move(roomFor);
    return c;
}

// Where a batch of this item should go, given the index. Buildings first, then a
// pile of the same item that still has room, then any free stockpile tile.
// Whether this building is where that item is worked: something it takes in or
// something it turns out.
bool worksWith(const World& w, const content::BuildingDef& def, DefId item) {
    for (const auto& r : w.db().recipes()) {
        if (r.workplaceDefs.empty()) continue;
        bool here = false;
        for (DefId place : r.workplaceDefs)
            if (w.db().building(place).function == def.function) here = true;
        if (!here) continue;
        for (const auto& in : r.inputs)
            if (in.item == item) return true;
        for (const auto& out : r.outputs)
            if (out.item == item) return true;
    }
    return false;
}

bool cachedStorageSpot(const World& w, const TickCache& cache, DefId item, TilePos near,
                       TilePos& outTile, BuildingId& outBuilding, HouseholdId carrier) {
    const auto& itemDef = w.db().item(item);
    (void)carrier;

    // How far the nearest proper store is, so a roof can be judged against it.
    std::int32_t bestDistToStore = std::numeric_limits<std::int32_t>::max();
    for (BuildingId b : cache.freeStorageBuildings)
        if (w.db().building(w.building(b).def).kind != content::BuildingKind::Housing)
            bestDistToStore = std::min(bestDistToStore, core::chebyshev(near, w.building(b).origin));
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    int bestPriority = std::numeric_limits<int>::max();
    outBuilding = BuildingId{};
    bool found = false;

    for (BuildingId b : cache.freeStorageBuildings) {
        const auto& def = w.db().building(w.building(b).def);
        if (!def.acceptsCategories.empty() &&
            std::find(def.acceptsCategories.begin(), def.acceptsCategories.end(), itemDef.category) ==
                    def.acceptsCategories.end())
            continue;
        const std::int32_t d = core::chebyshev(near, w.building(b).origin);
        // Two legs. What somebody gathered out in the fields goes to the
        // nearest roof when a roof is much nearer than the stores - that is a
        // short walk with an armful instead of a long one - and moving it on
        // from there is a separate job that free hands take (see the second
        // pass in addHaulCandidates). Carrying home unconditionally was tried
        // and reverted: everything ended up in the houses, the common stores
        // never filled, and hauling collapsed to a twentieth of what it was.
        // Three layers, in the order the work actually wants them. First the
        // place the thing is worked: grain belongs at the mill and flour at the
        // oven, because that is where the next pair of hands will look for it.
        // Then a roof, for what somebody carried in from the fields. The big
        // stores are last - they are where a surplus goes to keep, not where a
        // day's work is staged.
        const bool isHome = def.kind == content::BuildingKind::Housing;
        const bool worksThis = def.kind == content::BuildingKind::Workshop &&
                               !def.function.empty() && worksWith(w, def, item);
        int priority = 2;
        if (worksThis) priority = 0;
        else if (isHome && (itemDef.category == ItemCategory::Food ||
                            d * kStagingDetour < bestDistToStore))
            priority = 1;
        else if (!isHome) priority = 2;
        else priority = 3;
        if (priority < bestPriority || (priority == bestPriority && d < bestDist)) {
            bestPriority = priority;
            bestDist = d;
            outBuilding = b;
            outTile = w.building(b).origin;
            found = true;
        }
    }
    if (found) return true;

    if (auto it = cache.topUpTile.find(item.value); it != cache.topUpTile.end()) {
        outTile = it->second;
        return true;
    }

    for (TilePos t : cache.freeStorageTiles) {
        const std::int32_t d = core::chebyshev(near, t);
        if (d < bestDist) { bestDist = d; outTile = t; found = true; }
    }
    return found;
}

struct Candidate {
    Job job;
    Fixed score = core::kZero;
};

// Why one pawn's planning pass failed. Counting per candidate instead would report
// one rejection per standing tree per tick, which says nothing.
struct PlanningBlockers {
    bool missingTool = false;
    bool missingMaterial = false;
    bool underSkilled = false;
};

bool seasonAllows(std::string_view season, const content::ResourceNodeDef& def) {
    if (def.seasons.empty()) return true;
    for (const auto& s : def.seasons) if (s == season) return true;
    return false;
}

// How many units of `item` are available within reach of a site.
// The same question asked of a plain radius, for work out on the land where the
// settlement's stores are not to hand: a sower carries seed out to the furrow.
std::int32_t availableWithin(const World& w, const TickCache& cache, DefId item, TilePos site,
                             std::int32_t radius, PersonId forWho) {
    std::int32_t total = 0;
    for (ItemStackId id : cache.byItem[item.value]) {
        const auto& s = w.stack(id);
        const auto* state = ecsStackState(w, id);
        const bool alive = state ? state->alive : s.alive;
        const std::int32_t count = state ? state->count : s.count;
        if (!alive || count <= 0) continue;
        if (w.isReserved(World::stackKey(s.id), forWho)) continue;
        const StackWhere where = stackWhere(w, id);
        const BuildingId building = stackBuilding(w, id);
        const TilePos tile = stackTile(w, id);
        const TilePos at = where == StackWhere::InBuilding ? w.building(building).origin : tile;
        if (core::chebyshev(at, site) <= radius) total += count;
    }
    return total;
}

std::int32_t availableNear(const World& w, const TickCache& cache, DefId item, TilePos site,
                           SettlementId sid, PersonId forWho) {
    std::int32_t total = 0;
    for (ItemStackId id : cache.byItem[item.value]) {
        const auto& s = w.stack(id);
        const auto* state = ecsStackState(w, id);
        const bool alive = state ? state->alive : s.alive;
        const std::int32_t count = state ? state->count : s.count;
        if (!alive || count <= 0) continue;
        if (w.isReserved(World::stackKey(s.id), forWho)) continue;
        const StackWhere where = stackWhere(w, id);
        const BuildingId building = stackBuilding(w, id);
        const TilePos tile = stackTile(w, id);
        const TilePos at = where == StackWhere::InBuilding ? w.building(building).origin : tile;
        if (materialsInReach(w, sid, site, at)) total += count;
    }
    return total;
}

// The player's category priority for this settlement, defaulting to 1.
Fixed categoryPriority(const Settlement& s, WorkCategory c) {
    const Fixed v = s.priorities[static_cast<std::size_t>(c)];
    return v <= core::kZero ? core::kOne : v;
}

Fixed traitAffinity(const World& w, const Person& p, WorkCategory c) {
    Fixed bonus = core::kZero;
    for (DefId t : p.traits)
        for (const auto& [cat, val] : w.db().trait(t).workAffinity)
            if (cat == c) bonus += val;
    return core::max(Fixed::ratio(1, 4), core::kOne + bonus);
}

// A pawn leans toward its main profession and toward what it is already good at,
// but neither closes off other work (GDD 6).
Fixed personalWeight(const World& w, const Person& p, WorkCategory c) {
    Fixed weight = traitAffinity(w, p, c);
    // A trade is what somebody does, not a slight preference. At three halves
    // every pawn simply chased whatever the ledger wanted most, so the whole
    // community moved from task to task as one and no single person mattered:
    // anybody could replace anybody. A shepherd herds; he will bake if there is
    // nobody to bake and nothing to herd, and that is what the penalty is - a
    // penalty, not a bar.
    if (p.profession == c) {
        weight = weight * kOwnTradeWeight;
    } else {
        // The penalty for working outside one's trade, relaxed by hunger. Held
        // at full strength through a lean winter it killed a whole community:
        // flour keeps forty days, and two bakers cannot thresh, grind and bake
        // for twenty people, so they died with three thousand units of spoiled
        // flour in the granary. When the larder is short, everybody threshes.
        const Fixed discipline =
                p.settlement.valid() ? w.settlement(p.settlement).tradeDiscipline : core::kOne;
        weight = weight * (kOtherTradeWeight * discipline + (core::kOne - discipline));
    }
    const std::int32_t level = p.skills[static_cast<std::size_t>(c)].level;
    weight = weight * (core::kOne + Fixed::ratio(level, 20));
    return weight;
}

// Whether taking this one would cut into the stock the country needs to seed
// itself again. Measured as a share of what grew here before anybody touched it,
// so it means the same thing on a small map as on a large one.
bool belowSeedStock(const World& w, const TickCache& cache, DefId nodeDef,
                    const content::ResourceNodeDef& def) {
    if (def.keepStandingPercent <= 0) return false;
    if (nodeDef.value >= cache.standingNodes.size()) return false;
    const std::int32_t floorCount =
            std::max(3, w.nodesAtFirst(nodeDef) * def.keepStandingPercent / 100);
    return cache.standingNodes[nodeDef.value] <= floorCount;
}

// Timber. The woodcutters work the area the community set aside for it and move
// that area when the trees there are gone (D83), and they take what nobody eats
// first: a tamarisk before a date palm, because the palm is supper as well as
// timber. Ground being cleared for a field or a building is a different job -
// that one happens wherever the ground is wanted.
void addFellingCandidates(const World& w, const TickCache& cache, const Person& p,
                          const Settlement& st, const Demand& demand, std::int32_t radius,
                          std::vector<Candidate>& out, PlanningBlockers& blocked) {
    for (const auto& node : w.nodes()) {
        if (!resourceAlive(w, node) || resourceDepleted(w, node)) continue;
        if (core::chebyshev(p.tile, node.tile) > radius) continue;
        const auto& def = w.db().resourceNode(node.def);
        const auto& spec = fellSpecOf(w.db(), def);
        if (spec.yields.empty()) continue;
        if (!insideZoneOfKind(w, st.id, ZoneKind::Timber, node.tile)) continue;
        if (!workAllowedAt(w, st.id, spec.category, node.tile)) continue;
        if (!knowsMethod(w, p, spec.knowledgeDef)) continue;
        if (w.isReserved(World::nodeKey(node.id), p.id)) continue;
        if (belowSeedStock(w, cache, node.def, def)) continue;

        Fixed value = core::kZero;
        for (const auto& y : spec.yields) value += demand.forItem(y.item) * std::int64_t(y.count);
        if (value <= core::kZero) continue;
        // A tree that also feeds people is worth less as firewood than one that
        // does not, whatever its timber is worth.
        if (isGathered(def)) value = value / 2;

        Fixed penalty = core::kOne;
        if (spec.preferredTool != ToolClass::None && !hasToolEquipped(w, p, spec.preferredTool))
            penalty = spec.bareHandPenalty;
        const Fixed effectiveWork = spec.workAmount / core::max(penalty, Fixed::ratio(1, 10));
        const Fixed jobScore =
                value * effortWeight(effectiveWork) *
                zoneWeightAt(w, st.id, spec.category, node.tile) *
                categoryPriority(st, spec.category) * personalWeight(w, p, spec.category) *
                distanceWeight(core::chebyshev(p.tile, node.tile));

        if (!hasToolEquipped(w, p, spec.requiredTool)) {
            const ItemStackId tool = findNearestTool(w, p.tile, spec.requiredTool, p.id);
            if (!tool.valid()) { blocked.missingTool = true; continue; }
            const StackWhere where = stackWhere(w, tool);
            const TilePos at = where == StackWhere::InBuilding
                                       ? w.building(stackBuilding(w, tool)).origin
                                       : stackTile(w, tool);
            if (!workAllowedAt(w, st.id, WorkCategory::Hauling, at)) continue;

            Candidate fetch;
            fetch.job.kind = JobKind::FetchTool;
            fetch.job.category = WorkCategory::Hauling;
            setJobStack(w, fetch.job, tool);
            fetch.job.target = at;
            fetch.job.workRequired = core::kZero;
            fetch.score = jobScore * Fixed::ratio(4, 5) * distanceWeight(core::chebyshev(p.tile, at));
            out.push_back(std::move(fetch));
            continue;
        }

        Candidate c;
        c.job.kind = JobKind::Fell;
        c.job.category = spec.category;
        c.job.node = node.id;
        c.job.target = node.tile;
        c.job.workRequired = effectiveWork;
        c.score = jobScore;
        out.push_back(std::move(c));
    }
}

void addHarvestCandidates(const World& w, const TickCache& cache, const Person& p,
                          const Settlement& st, const Demand& demand, std::int32_t radius,
                          std::vector<Candidate>& out, PlanningBlockers& blocked) {
    const std::string_view season = core::seasonName(w.now().season);
    // How hungry it has to get before the community will take the last of
    // something. A famine overrides the seed stock: people who are starving
    // today will fell the last palm, and should.
    const bool famished = st.foodDays < core::kOne;
    const Fixed faminePriority = famished ? Fixed::fromInt(100) : core::kOne;
    for (const auto& node : w.nodes()) {
        if (!resourceAlive(w, node) || resourceDepleted(w, node)) continue;
        if (core::chebyshev(p.tile, node.tile) > radius) continue;
        const auto& def = w.db().resourceNode(node.def);
        // Picking, not felling. A tree taken for its timber is offered by the
        // woodcutters' rule below, in the timber area and nowhere else.
        if (!isGathered(def)) continue;
        // Leave enough standing to seed the next crop of it. Nothing grows back
        // out of its own stump (D83), so without this the country is simply
        // eaten: a community stripped every stand of wild emmer on the map and
        // then met a winter with nothing left to gather.
        if (!famished && belowSeedStock(w, cache, node.def, def)) continue;
        const auto& h = def.harvest;
        if (h.yields.empty()) continue;

        const WorkCategory cat = h.category;
        if (!workAllowedAt(w, st.id, cat, node.tile)) continue;
        if (!seasonAllows(season, def)) continue;
        if (!knowsMethod(w, p, h.knowledgeDef)) continue;
        if (w.isReserved(World::nodeKey(node.id), p.id)) continue;

        Fixed value = core::kZero;
        for (const auto& y : h.yields) value += demand.forItem(y.item) * std::int64_t(y.count);
        if (value <= core::kZero) continue;

        // Work out what this job would be worth before deciding whether the tool
        // is worth going to get.
        Fixed penalty = core::kOne;
        if (h.preferredTool != ToolClass::None && !hasToolEquipped(w, p, h.preferredTool))
            penalty = h.bareHandPenalty;
        const Fixed effectiveWork = h.workAmount / core::max(penalty, Fixed::ratio(1, 10));
        const Fixed jobScore = value * effortWeight(effectiveWork) *
                               zoneWeightAt(w, st.id, cat, node.tile) * categoryPriority(st, cat) *
                               personalWeight(w, p, cat) * distanceWeight(core::chebyshev(p.tile, node.tile)) *
                               faminePriority;

        if (!hasToolEquipped(w, p, h.requiredTool)) {
            // The method is known and the target is standing there; only the tool
            // is missing. Fetching it is itself a job, and it competes on score
            // like any other - otherwise a settlement with four axes in the store
            // never cuts a tree, because there is always some tool-free work to do
            // instead.
            const ItemStackId tool = findNearestTool(w, p.tile, h.requiredTool, p.id);
            if (!tool.valid()) { blocked.missingTool = true; continue; }
            const StackWhere where = stackWhere(w, tool);
            const TilePos at = where == StackWhere::InBuilding
                                       ? w.building(stackBuilding(w, tool)).origin
                                       : stackTile(w, tool);
            if (!workAllowedAt(w, st.id, WorkCategory::Hauling, at)) continue;

            Candidate fetch;
            fetch.job.kind = JobKind::FetchTool;
            fetch.job.category = WorkCategory::Hauling;
            setJobStack(w, fetch.job, tool);
            fetch.job.target = at;
            fetch.job.workRequired = core::kZero;
            // Slightly discounted: the trip is real, and the work still follows.
            fetch.score = jobScore * Fixed::ratio(4, 5) * distanceWeight(core::chebyshev(p.tile, at));
            out.push_back(std::move(fetch));
            continue;
        }

        Candidate c;
        c.job.kind = JobKind::Harvest;
        c.job.category = cat;
        c.job.node = node.id;
        c.job.target = node.tile;
        // Working without the preferred tool is slower, not impossible.
        c.job.workRequired = effectiveWork;
        c.score = jobScore;
        out.push_back(std::move(c));
    }
}

void addConstructionCandidates(const World& w, const Person& p, const Settlement& st, const Demand& demand,
                               std::vector<Candidate>& out, PlanningBlockers& blocked) {
    std::int32_t population = 0;
    auto people = w.ecs().view<const ecs::Identity, const ecs::SettlementMember, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& social = people.get<const ecs::SettlementMember>(entity);
        if (social.settlement == st.id.value) ++population;
    }

    auto buildingView = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                      const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity entity : buildingView) {
        const auto& identity = buildingView.get<const ecs::Identity>(entity);
        const Building& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildingView.get<const ecs::Alive>(entity).value || b.settlement != st.id) continue;
        // A finished building is work only when it is coming down to make room
        // for its own replacement.
        const bool buildingDone = buildingView.get<const ecs::ConstructionProgress>(entity).complete;
        if (buildingDone && !b.replacedBy.valid()) continue;
        const auto& def = w.db().building(b.def);
        if (!workAllowedAt(w, st.id, WorkCategory::Construction, b.origin)) continue;

        // Something marked for replacement comes down first, and that is work
        // like any other.
        if (buildingDone && b.replacedBy.valid()) {
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, b.origin)) continue;
            Candidate c;
            c.job.kind = JobKind::Demolish;
            c.job.category = WorkCategory::Construction;
            c.job.building = b.id;
            c.job.target = b.origin;
            c.job.workRequired = def.workAmount * kDemolishShareOfBuilding;
            c.score = buildingValue(w, st, w.db().building(b.replacedBy), population) *
                      effortWeight(c.job.workRequired) *
                      categoryPriority(st, WorkCategory::Construction) *
                      personalWeight(w, p, WorkCategory::Construction) *
                      distanceWeight(core::chebyshev(p.tile, b.origin));
            out.push_back(std::move(c));
            continue;
        }

        // Anything standing on the site comes down first. Until it does, no
        // amount of delivered brick raises a wall through a tamarisk.
        {
            ResourceNodeId standing;
            for (TilePos t : b.footprintTiles(w.db())) {
                if (!w.map().inBounds(t)) continue;
                const ResourceNodeId n = w.map().at(t).node;
                if (n.valid() && w.resourceNodeAlive(n)) { standing = n; break; }
            }
            if (standing.valid()) {
                const auto& nodeDef = w.db().resourceNode(w.node(standing).def);
                const Fixed work = nodeDef.clearWork > core::kZero ? nodeDef.clearWork
                                                                   : nodeDef.harvest.workAmount;
                Candidate c;
                c.job.kind = JobKind::Clear;
                c.job.category = WorkCategory::Construction;
                c.job.node = standing;
                c.job.target = w.node(standing).tile;
                c.job.workRequired = work;
                c.score = buildingValue(w, st, def, population) * effortWeight(work) *
                          zoneWeightAt(w, st.id, WorkCategory::Construction, b.origin) *
                          categoryPriority(st, WorkCategory::Construction) *
                          personalWeight(w, p, WorkCategory::Construction) *
                          distanceWeight(core::chebyshev(p.tile, w.node(standing).tile));
                out.push_back(std::move(c));
                continue;
            }
        }

        // Everything delivered? Then the site needs hands, not materials.
        bool complete = true;
        std::size_t missingIndex = 0;
        for (std::size_t i = 0; i < def.materials.size(); ++i) {
            const std::int32_t have = deliveredMaterial(w, b, i);
            if (have < def.materials[i].count) { complete = false; missingIndex = i; break; }
        }

        if (complete) {
            if (!knowsMethod(w, p, def.knowledgeDef)) continue;
            if (!skilledEnough(p, def.requiredSkill)) { blocked.underSkilled = true; continue; }
            if (!hasToolEquipped(w, p, def.requiredTool)) {
                if (findNearestTool(w, p.tile, def.requiredTool, p.id).valid()) continue;
                blocked.missingTool = true;
                continue;
            }
            Candidate c;
            c.job.kind = JobKind::Construct;
            c.job.category = WorkCategory::Construction;
            c.job.building = b.id;
            c.job.target = b.origin;
            c.job.workRequired = def.workAmount;
            // Effort counts what is left to do, so a site that is nearly finished
            // pulls harder than one just begun.
            const Fixed remaining = core::max(core::kOne, def.workAmount - constructionProgressOf(w, b));
            c.score = buildingValue(w, st, def, population) * effortWeight(remaining) *
                      zoneWeightAt(w, st.id, WorkCategory::Construction, b.origin) *
                      categoryPriority(st, WorkCategory::Construction) *
                      personalWeight(w, p, WorkCategory::Construction) *
                      distanceWeight(core::chebyshev(p.tile, b.origin));
            out.push_back(std::move(c));
            continue;
        }

        // Otherwise the next useful act is carrying the missing material there.
        if (carriedStack(w, p.id).valid()) continue;
        const DefId wanted = def.materials[missingIndex].item;
        const ItemStackId src = findNearestAvailable(w, p.tile, wanted, p.id);
        if (!src.valid()) { blocked.missingMaterial = true; continue; }

        Candidate c;
        c.job.kind = JobKind::HaulToSite;
        c.job.category = WorkCategory::Hauling;
        setJobStack(w, c.job, src);
        c.job.building = b.id;
        c.job.wantedItem = wanted;
        c.job.wantedCount = def.materials[missingIndex].count -
                            deliveredMaterial(w, b, missingIndex);
        c.job.target = w.stack(src).where == StackWhere::InBuilding
                               ? w.building(w.stack(src).building).origin
                               : w.stack(src).tile;
        c.job.deliverTo = b.origin;
        c.job.workRequired = core::kZero;
        // Carrying a material to a site is worth what the material is worth to
        // that site, on the same scale as everything else.
        c.score = (demand.forItem(wanted) + core::kOne) *
                  buildingValue(w, st, def, population) / Fixed::fromInt(3) *
                  categoryPriority(st, WorkCategory::Hauling) *
                  personalWeight(w, p, WorkCategory::Hauling) *
                  distanceWeight(core::chebyshev(p.tile, c.job.target));
        out.push_back(std::move(c));
    }
}

void addCraftCandidates(const World& w, const TickCache& cache, const Person& p, const Settlement& st,
                        const Demand& demand, std::vector<Candidate>& out, PlanningBlockers& blocked) {
    const bool famished = demand.foodDays < core::kOne;
    for (const auto& r : w.db().recipes()) {
        Fixed value = core::kZero;
        for (const auto& o : r.outputs) value += demand.forItem(o.item) * std::int64_t(o.count);
        if (value <= core::kZero) continue;
        if (!knowsMethod(w, p, r.knowledgeDef)) continue;
        if (!skilledEnough(p, r.requiredSkill)) { blocked.underSkilled = true; continue; }

        // Where would this happen?
        TilePos site = st.hearth;
        BuildingId workplace;
        if (!r.workplaceDefs.empty()) {
            workplace = findFreeWorkplace(w, r.workplaceDefs, p.tile, p.id);
            if (!workplace.valid()) continue;              // no workshop, no craft
            site = w.building(workplace).origin;
        } else {
            const Zone* store = nearestStorageZone(w, st.id, p.tile);
            if (store) site = store->centre();
        }
        if (!workAllowedAt(w, st.id, r.category, site)) continue;

        // Are the inputs actually to hand at that site?
        bool inputsPresent = true;
        for (const auto& in : r.inputs) {
            // The seed fund is off limits while there is anything else to eat.
            // Eating the seed corn is a real thing a starving community does,
            // and it should cost it next year's harvest, not this recipe's.
            if (!famished && in.item.value < st.seedReserve.size() &&
                st.seedReserve[in.item.value] > 0 &&
                countAvailable(w, in.item) - st.seedReserve[in.item.value] < in.count) {
                inputsPresent = false;
                break;
            }
            if (availableNear(w, cache, in.item, site, st.id, p.id) < in.count) { inputsPresent = false; break; }
        }
        if (!inputsPresent) { blocked.missingMaterial = true; continue; }

        const Fixed recipeScore = value * effortWeight(r.workAmount) *
                                  zoneWeightAt(w, st.id, r.category, site) * categoryPriority(st, r.category) *
                                  personalWeight(w, p, r.category) *
                                  distanceWeight(core::chebyshev(p.tile, site));

        if (!hasToolEquipped(w, p, r.requiredTool)) {
            const ItemStackId tool = findNearestTool(w, p.tile, r.requiredTool, p.id);
            if (!tool.valid()) { blocked.missingTool = true; continue; }
            const StackWhere where = stackWhere(w, tool);
            const TilePos at = where == StackWhere::InBuilding
                                       ? w.building(stackBuilding(w, tool)).origin
                                       : stackTile(w, tool);
            Candidate fetch;
            fetch.job.kind = JobKind::FetchTool;
            fetch.job.category = WorkCategory::Hauling;
            setJobStack(w, fetch.job, tool);
            fetch.job.target = at;
            fetch.job.workRequired = core::kZero;
            fetch.score = recipeScore * Fixed::ratio(4, 5) * distanceWeight(core::chebyshev(p.tile, at));
            out.push_back(std::move(fetch));
            continue;
        }

        // Cooking and boiling need an actual fire.
        if (r.category == WorkCategory::Cooking && !findFireSource(w, site).valid()) continue;

        Candidate c;
        c.job.kind = JobKind::Craft;
        c.job.category = r.category;
        c.job.recipe = r.id;
        c.job.building = workplace;
        c.job.target = site;
        c.job.workRequired = r.workAmount;
        c.score = recipeScore;
        out.push_back(std::move(c));
    }
}

void addHaulCandidates(const World& w, const TickCache& cache, const Person& p, const Settlement& st,
                       const Demand& demand, std::int32_t radius, std::vector<Candidate>& out) {
    if (carriedStack(w, p.id).valid()) return;
    std::vector<ItemStackId> sources = cache.loose;
    if (!cache.freeStorageBuildings.empty())
        sources.insert(sources.end(), cache.shelterable.begin(), cache.shelterable.end());

    for (ItemStackId id : sources) {
        const auto& s = w.stack(id);
        const auto* state = ecsStackState(w, id);
        const bool alive = state ? state->alive : s.alive;
        const std::int32_t count = state ? state->count : s.count;
        const StackWhere where = stackWhere(w, id);
        const TilePos tile = stackTile(w, id);
        if (!alive || count <= 0 || where != StackWhere::Ground) continue;
        if (core::chebyshev(p.tile, tile) > radius) continue;
        if (w.isReserved(World::stackKey(s.id), p.id)) continue;

        TilePos spot;
        BuildingId destBuilding;
        const DefId definition = stackDefinition(w, id);
        if (!cachedStorageSpot(w, cache, definition, tile, spot, destBuilding, p.household)) continue;
        if (spot == tile && !destBuilding.valid()) continue;   // already stored where it lies
        if (!workAllowedAt(w, st.id, WorkCategory::Hauling, tile)) continue;

        Candidate c;
        c.job.kind = JobKind::HaulToStore;
        c.job.category = WorkCategory::Hauling;
        setJobStack(w, c.job, s.id);
        c.job.target = tile;
        c.job.deliverTo = spot;
        c.job.deliverBuilding = destBuilding;
        c.job.workRequired = core::kZero;
        // Moving a batch is worth what the batch is worth. A flat tidying bonus
        // had the community carrying its five hundredth branch to the pile while
        // it starved.
        const Fixed cargoValue = demand.forItem(definition) * std::int64_t(std::min(count, 20));
        c.score = (Fixed::ratio(1, 4) + cargoValue) * categoryPriority(st, WorkCategory::Hauling) *
                  personalWeight(w, p, WorkCategory::Hauling) *
                  distanceWeight(core::chebyshev(p.tile, tile));
        out.push_back(std::move(c));
    }

    // The second leg: what has piled up under somebody's roof, moved on to the
    // common stores. This is its own job on purpose - a farmer who has sown and
    // reaped and has nothing growing yet takes it, and so does anybody else with
    // no work of their own - which is why it is worth so little on its own
    // account that it loses to any real work.
    auto housingStacks = w.ecs().view<const ecs::Identity, const ecs::ItemStackState>();
    for (const entt::entity entity : housingStacks) {
        const auto& identity = housingStacks.get<const ecs::Identity>(entity);
        const ItemStackId stack{identity.legacyIndex};
        const auto& state = housingStacks.get<const ecs::ItemStackState>(entity);
        const bool alive = state.alive;
        const std::int32_t count = state.count;
        const StackWhere where = static_cast<StackWhere>(state.where);
        const BuildingId building{state.building};
        if (!alive || count <= 0 || where != StackWhere::InBuilding) continue;
        if (!building.valid()) continue;
        const auto& from = w.building(building);
        if (!w.buildingAlive(from.id) || from.settlement != st.id) continue;
        if (w.db().building(from.def).kind != content::BuildingKind::Housing) continue;
        if (w.isReserved(World::stackKey(stack), p.id)) continue;
        if (core::chebyshev(p.tile, from.origin) > radius) continue;

        // A household keeps its own food; everything else moves on, and food
        // moves on too once there is more of it than the pantry is for.
        const auto& itemDef = w.db().item(DefId{state.definition});
        const bool pantryFood = itemDef.category == ItemCategory::Food &&
                                stacksInBuilding(w, from.id) * 2 <=
                                        w.db().building(from.def).storageSlots;
        if (pantryFood) continue;

        BuildingId destination;
        std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
        for (BuildingId b : cache.freeStorageBuildings) {
            const auto& def = w.db().building(w.building(b).def);
            if (def.kind == content::BuildingKind::Housing) continue;
            if (!def.acceptsCategories.empty() &&
                std::find(def.acceptsCategories.begin(), def.acceptsCategories.end(),
                          itemDef.category) == def.acceptsCategories.end())
                continue;
            const std::int32_t d = core::chebyshev(from.origin, w.building(b).origin);
            if (d < bestDist) { bestDist = d; destination = b; }
        }
        if (!destination.valid()) continue;

        Candidate c;
        c.job.kind = JobKind::HaulToStore;
        c.job.category = WorkCategory::Hauling;
        setJobStack(w, c.job, stack);
        c.job.target = from.origin;
        c.job.deliverTo = w.building(destination).origin;
        c.job.deliverBuilding = destination;
        c.job.workRequired = core::kZero;
        c.score = kSecondLegWorth * categoryPriority(st, WorkCategory::Hauling) *
                  personalWeight(w, p, WorkCategory::Hauling) *
                  distanceWeight(core::chebyshev(p.tile, from.origin));
        out.push_back(std::move(c));
    }
}

// A field is worth working for what it will eventually yield, discounted because
// the yield is weeks away. GDD 7 asks production chains to arise from the
// community's needs; sowing is the longest such chain it has.
void addFieldCandidates(const World& w, const TickCache& cacheFor, const Person& p, const Settlement& st,
                        const Demand& demand, std::int32_t radius, std::vector<Candidate>& out,
                        PlanningBlockers& blocked) {
    const Zone* field = nearestZoneOfKind(w, st.id, ZoneKind::Farm, p.tile);
    if (!field) return;

    std::int32_t population = 0;
    auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& q = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
        if (q.settlement == st.id) ++population;
    }

    for (TilePos tile : field->tiles) {
        if (core::tileDistance(p.tile, tile) > radius) continue;
        if (!w.map().at(tile).explored) continue;
        if (w.isReserved(World::tileKey(tile), p.id)) continue;
        if (!workAllowedAt(w, st.id, WorkCategory::Farming, tile)) continue;

        DefId cropId;
        const JobKind action = nextFieldAction(w, st.id, tile, cropId);
        if (action == JobKind::None) continue;

        if (action == JobKind::Clear) {
            const ResourceNodeId nodeId = w.map().at(tile).node;
            if (!nodeId.valid()) continue;
            const DefId crop = chooseCropToSow(w, st.id);
            if (!crop.valid()) continue;
            const auto& nodeDef = w.db().resourceNode(w.node(nodeId).def);
            const auto& wantedCrop = w.db().crop(crop);
            const Fixed work = nodeDef.clearWork > core::kZero ? nodeDef.clearWork
                                                               : nodeDef.harvest.workAmount;
            // Three steps stand between clear ground and grain, and whatever the
            // clearing itself yields is worth having too.
            Fixed value = demand.forItem(wantedCrop.harvestItem) *
                          std::int64_t(wantedCrop.harvestCount) * kDemandDecay * kDemandDecay *
                          kDemandDecay;
            for (const auto& y : nodeDef.harvest.yields)
                value += demand.forItem(y.item) * std::int64_t(y.count);
            if (value <= core::kZero) continue;

            Candidate c;
            c.job.kind = JobKind::Clear;
            c.job.category = WorkCategory::Construction;
            c.job.node = nodeId;
            c.job.target = tile;
            c.job.workRequired = work;
            c.score = value * effortWeight(work) *
                      zoneWeightAt(w, st.id, WorkCategory::Farming, tile) *
                      categoryPriority(st, WorkCategory::Farming) *
                      personalWeight(w, p, WorkCategory::Construction) *
                      distanceWeight(core::tileDistance(p.tile, tile));
            out.push_back(std::move(c));
            continue;
        }

        // Tilling has no crop chosen yet; value it by the best thing this culture
        // could put in the ground.
        DefId valued = cropId;
        if (!valued.valid()) valued = chooseCropToSow(w, st.id);
        if (!valued.valid()) continue;

        const auto& crop = w.db().crop(valued);
        if (!knowsMethod(w, p, crop.knowledgeDef)) continue;
        // No point walking out to sow with nothing to sow.
        if (action == JobKind::Sow &&
            availableWithin(w, cacheFor, crop.seedItem, tile, kSeedCarryRadius, p.id) < crop.seedCount) {
            blocked.missingMaterial = true;
            continue;
        }

        Fixed value = demand.forItem(crop.harvestItem) * std::int64_t(crop.harvestCount);
        Fixed work = crop.harvestWork;
        switch (action) {
            case JobKind::Till:
                // Not while there is already more broken ground than the
                // community can put seed in. A field is worth what is sown in
                // it, and hands spent breaking the five hundredth plot are
                // hands not spent sowing the two hundredth.
                if (st.brokenUnsown > kIdleGroundAllowance * population) continue;
                work = crop.tillWork;
                // Two more steps stand between broken ground and grain.
                value = value * kDemandDecay * kDemandDecay;
                break;
            case JobKind::Sow:
                work = crop.sowWork;
                value = value * kDemandDecay;
                // Seed eaten is seed not sown; the planner must see the cost.
                value = value - demand.forItem(crop.seedItem) * std::int64_t(crop.seedCount);
                break;
            case JobKind::ReapCrop:
                work = crop.harvestWork;
                // The cost is already sunk and the field is already ripe. A
                // community does not leave standing grain because its ledger says
                // it has enough - and if it does leave it, the crop is lost.
                value = core::max(value, Fixed::fromInt(4));
                break;
            default:
                continue;
        }
        if (value <= core::kZero) continue;

        // A hoe is not required to break ground, only to break it quickly.
        Fixed penalty = core::kOne;
        if (crop.tillTool != ToolClass::None && !hasToolEquipped(w, p, crop.tillTool) &&
            action == JobKind::Till)
            penalty = crop.bareHandPenalty;
        const Fixed effectiveWork = work / core::max(penalty, Fixed::ratio(1, 10));

        Candidate c;
        c.job.kind = action;
        c.job.category = WorkCategory::Farming;
        c.job.target = tile;
        c.job.crop = valued;
        c.job.workRequired = effectiveWork;
        c.score = value * effortWeight(effectiveWork) *
                  zoneWeightAt(w, st.id, WorkCategory::Farming, tile) *
                  categoryPriority(st, WorkCategory::Farming) *
                  personalWeight(w, p, WorkCategory::Farming) *
                  distanceWeight(core::tileDistance(p.tile, tile));
        out.push_back(std::move(c));
    }
}

// Driving the flock in for the night. This is the shepherd's evening work, and
// it is why a dog is worth having: with one, a shepherd brings in the whole
// flock at once instead of fetching them one at a time.
void addPenningCandidates(const World& w, const Person& p, const Settlement& st,
                          std::int32_t radius, std::vector<Candidate>& out) {
    // At dusk, not in the dark. Offering this once the light had gone meant
    // competing with sleep, and sleep won every time: a byre stood empty for a
    // decade while the flock strayed two thousand times.
    if (w.now().hour < kPenningHour) return;
    // The shepherds' work, and only theirs while there are any: eighty head
    // against forty people is three thousand candidates a tick otherwise, and
    // the whole planner slows to a crawl for a job one person does. But a flock
    // is not left out because the last shepherd died - then it is everybody's.
    // A flock with no shepherd is not left out for the wolves: the profession
    // review sees the work going undone - a head of stock is work every day -
    // and somebody takes the trade up. Letting everybody consider penning
    // instead put every animal in front of every person every night tick, and
    // the whole simulation slowed by half for a job one person does.
    if (p.profession != WorkCategory::Herding) return;

    // The byre with room in it. Counting what is already inside by distance is
    // enough: a penned animal is standing at the door.
    BuildingId byre;
    std::int32_t room = 0;
    auto animals = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                const ecs::Alive, const ecs::SleepState,
                                const ecs::Transform>();
    auto buildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                   const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity buildingEntity : buildings) {
        const auto& identity = buildings.get<const ecs::Identity>(buildingEntity);
        const Building& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildings.get<const ecs::Alive>(buildingEntity).value ||
            !buildings.get<const ecs::ConstructionProgress>(buildingEntity).complete ||
            b.settlement != st.id) continue;
        const std::int32_t slots = w.db().building(b.def).animalSlots;
        if (slots <= 0) continue;
        std::int32_t inside = 0;
        for (const entt::entity entity : animals) {
            const auto& a = w.animal(AnimalId{animals.get<const ecs::Identity>(entity).legacyIndex});
            const TilePos tile = core::toTile(animals.get<const ecs::Transform>(entity).value);
            if (animals.get<const ecs::Alive>(entity).value &&
                animals.get<const ecs::SleepState>(entity).asleep && a.owner == st.id &&
                core::tileDistance(tile, b.origin) <= kPennedWithin)
                ++inside;
        }
        if (slots - inside > room) { room = slots - inside; byre = b.id; }
    }
    if (!byre.valid() || room <= 0) return;
    // And it has to be a byre the flock can actually be walked to before the
    // night is out. A pasture that has moved across the map leaves its byre
    // behind, and driving sheep twenty tiles in the dark is not penning them.
    if (core::tileDistance(p.tile, w.building(byre).origin) > kDriveHomeReach) return;

    // The nearest one that is still out: with a dog the rest follow it in, and
    // without one this is the next of many trips.
    AnimalId nearest;
    std::int32_t bestDist = radius + 1;
    for (const entt::entity entity : animals) {
        const auto& a = w.animal(AnimalId{animals.get<const ecs::Identity>(entity).legacyIndex});
        if (!animals.get<const ecs::Alive>(entity).value ||
            animals.get<const ecs::SleepState>(entity).asleep || a.owner != st.id) continue;
        if (w.isReserved(World::animalKey(a.id), p.id)) continue;
        const std::int32_t d = core::tileDistance(
                p.tile, core::toTile(animals.get<const ecs::Transform>(entity).value));
        if (d < bestDist) { bestDist = d; nearest = a.id; }
    }
    if (nearest.valid()) {
        const auto& a = w.animal(nearest);
        Candidate c;
        c.job.kind = JobKind::Pen;
        c.job.category = WorkCategory::Herding;
        c.job.animal = a.id;
        c.job.building = byre;
        c.job.target = a.tile;
        c.job.deliverTo = w.building(byre).origin;
        c.job.workRequired = core::kOne;
        c.score = Fixed::fromInt(kPenningWorth) * categoryPriority(st, WorkCategory::Herding) *
                  personalWeight(w, p, WorkCategory::Herding) *
                  distanceWeight(core::tileDistance(p.tile, a.tile));
        out.push_back(std::move(c));
    }
}

// The wild animals: hunted for meat and hide, and their young taken to be
// tamed. Hunting takes the animal away for good, so what a hunter has to think
// about is not how hungry the settlement is but how many are left to breed - a
// country hunted out stays hunted out.
void addWildCandidates(const World& w, const TickCache& cache, const Person& p,
                       const Settlement& st, const Demand& demand, std::int32_t radius,
                       std::vector<Candidate>& out, PlanningBlockers& blocked) {
    const auto& head = cache.wildHead;

    const bool haveGuard = cache.anyGuardDog;

    for (AnimalId id : cache.wild) {
        const auto& a = w.animal(id);
        if (!animalAlive(w, a) || a.owner.valid()) continue;
        const auto& def = w.db().animal(a.def);
        if (core::tileDistance(p.tile, a.tile) > radius) continue;
        if (w.isReserved(World::animalKey(a.id), p.id)) continue;
        if (!workAllowedAt(w, st.id, WorkCategory::Hunting, a.tile)) continue;

        // A wolf pup is worth more alive to a herding people than dead.
        if (!def.tamesInto.empty() && !haveGuard && !a.adult(w.db()) &&
            knowsMethod(w, p, w.db().animal(w.db().animalByName(def.tamesInto)).knowledgeDef)) {
            Candidate c;
            c.job.kind = JobKind::Tame;
            c.job.category = WorkCategory::Herding;
            c.job.animal = a.id;
            c.job.target = a.tile;
            c.job.workRequired = def.huntWork;
            c.score = Fixed::fromInt(kTamingWorth) * effortWeight(def.huntWork) *
                      categoryPriority(st, WorkCategory::Herding) *
                      personalWeight(w, p, WorkCategory::Herding) *
                      distanceWeight(core::tileDistance(p.tile, a.tile));
            out.push_back(std::move(c));
            continue;
        }

        if (def.huntYields.empty()) continue;
        // Leave enough to breed. A predator is the exception: fewer wolves is
        // the point of hunting one.
        if (!def.predator && head[a.def.value] <= def.breedingStock) continue;

        Fixed value = core::kZero;
        for (const auto& y : def.huntYields) value += demand.forItem(y.item) * std::int64_t(y.count);
        // A wolf near the flock is worth killing whatever its hide fetches.
        if (def.predator) {
            for (const auto& q : w.animals())
                if (animalAlive(w, q) && q.owner == st.id && core::tileDistance(q.tile, a.tile) <= kWolfReach)
                    value = core::max(value, Fixed::fromInt(kWolfBountyToAFlock));
        }
        if (value <= core::kZero) continue;

        if (!hasToolEquipped(w, p, def.huntTool)) {
            if (findNearestTool(w, p.tile, def.huntTool, p.id).valid()) continue;
            blocked.missingTool = true;
            continue;
        }

        Candidate c;
        c.job.kind = JobKind::Hunt;
        c.job.category = WorkCategory::Hunting;
        c.job.animal = a.id;
        c.job.target = a.tile;
        c.job.workRequired = def.huntWork;
        c.score = value * effortWeight(def.huntWork) *
                  zoneWeightAt(w, st.id, WorkCategory::Hunting, a.tile) *
                  categoryPriority(st, WorkCategory::Hunting) *
                  personalWeight(w, p, WorkCategory::Hunting) *
                  distanceWeight(core::tileDistance(p.tile, a.tile));
        out.push_back(std::move(c));
    }
}

// Wool, milk, and - when the flock is large enough to spare one, or the community
// is desperate - meat.
void addLivestockCandidates(const World& w, const Person& p, const Settlement& st, const Demand& demand,
                            std::int32_t radius, std::vector<Candidate>& out, PlanningBlockers& blocked) {
    // How many of each kind there are decides whether one can be spared.
    std::vector<std::int32_t> headcount(w.db().animals().size(), 0);
    auto animalView = w.ecs().view<const ecs::Identity, const ecs::Animal, const ecs::Alive>();
    for (const entt::entity entity : animalView) {
        const Animal& a = w.animal(AnimalId{animalView.get<const ecs::Identity>(entity).legacyIndex});
        if (animalView.get<const ecs::Alive>(entity).value && a.owner == st.id) headcount[a.def.value] += 1;
    }

    // A ledger shortfall is the normal state of a settlement building up stores,
    // and somebody waking up hungry is normal too; neither is a reason to eat the
    // breeding flock. Only an empty larder is - and measuring it as anything
    // looser than this is what ate a starting flock of eight down to two.
    const bool larderEmpty = demand.foodDays < core::kOne;
    std::int32_t population = 0;
    auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& q = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
        if (q.settlement == st.id) population += 1;
    }

    for (const entt::entity entity : animalView) {
        const Animal& a = w.animal(AnimalId{animalView.get<const ecs::Identity>(entity).legacyIndex});
        if (!animalView.get<const ecs::Alive>(entity).value || a.owner != st.id) continue;
        if (core::tileDistance(p.tile, a.tile) > radius) continue;
        if (w.isReserved(World::animalKey(a.id), p.id)) continue;
        if (!workAllowedAt(w, st.id, WorkCategory::Herding, a.tile)) continue;

        const auto& def = w.db().animal(a.def);
        if (!knowsMethod(w, p, def.knowledgeDef)) continue;

        JobKind action = nextAnimalAction(w, a);
        const std::vector<content::IngredientSpec>* yields = nullptr;
        Fixed work = core::kZero;

        if (action == JobKind::Shear) { yields = &def.shearYields; work = def.shearWork; }
        else if (action == JobKind::Milk) { yields = &def.milkYields; work = def.milkWork; }

        if (action == JobKind::None && !def.slaughterYields.empty() && a.adult(w.db())) {
            // A flock is capital before it is meat. Only the surplus above the
            // breeding herd is eaten; the herd itself is touched only when people
            // are genuinely starving, and even then not to the last pair.
            // The herd a community wants grows with the mouths it feeds: the
            // breeding stock it must never eat, plus a beast for every second
            // person. Everything above that is meat, and nothing below the
            // breeding stock is touched unless the larder is bare.
            const std::int32_t head = headcount[a.def.value];
            const bool surplus = head > def.breedingStock + population / 2;
            const bool lastResort = larderEmpty && head > def.breedingStock / 2;
            if (surplus || lastResort) {
                action = JobKind::Slaughter;
                yields = &def.slaughterYields;
                work = def.slaughterWork;
            }
        }
        if (action == JobKind::None || !yields) continue;

        Fixed value = core::kZero;
        for (const auto& y : *yields) value += demand.forItem(y.item) * std::int64_t(y.count);
        // An animal standing ready is the same case as a ripe field: the fleece
        // grew whether or not anyone takes it, the milk spoils in the ewe, and the
        // work is small. A herding culture does not leave its flock untouched for
        // a season because the ledger says the granary is full.
        if (action == JobKind::Shear || action == JobKind::Milk)
            value = core::max(value, Fixed::fromInt(3));
        if (value <= core::kZero) continue;

        Candidate c;
        c.job.kind = action;
        c.job.category = WorkCategory::Herding;
        c.job.animal = a.id;
        c.job.target = a.tile;
        c.job.workRequired = work;
        // Which animal goes first, when one has to. A herding people keeps its
        // ewes and eats the surplus rams and the old, which is the difference
        // between a flock that grows and one that dwindles.
        if (action == JobKind::Slaughter) {
            Fixed preference = a.sex == Sex::Male ? Fixed::ratio(3, 2) : Fixed::ratio(1, 2);
            if (def.maxAgeDays > 0)
                preference = preference * (core::kOne + Fixed::ratio(a.ageDays, def.maxAgeDays));
            value = value * preference;
        }
        c.score = value * effortWeight(work) *
                  zoneWeightAt(w, st.id, WorkCategory::Herding, a.tile) *
                  categoryPriority(st, WorkCategory::Herding) *
                  personalWeight(w, p, WorkCategory::Herding) *
                  distanceWeight(core::tileDistance(p.tile, a.tile));
        out.push_back(std::move(c));
    }
    (void)blocked;
}

// Walking out past the known edge. GDD 9 has the player steering by area rather
// than by order; before an area can mean anything, somebody has to have been
// there. A settlement keeps a couple of people doing this and no more - it is
// worth knowing the land, but not at the cost of the harvest.
void addScoutCandidates(const World& w, const Person& p, const Settlement& st,
                        std::vector<Candidate>& out) {
    std::int32_t scoutsOut = 0;
    auto people = w.ecs().view<const ecs::Identity, const ecs::JobState, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        const auto& jobState = people.get<const ecs::JobState>(entity);
        const auto& q = w.person(PersonId{identity.legacyIndex});
        if (q.settlement == st.id && jobState.active &&
            jobState.kind == static_cast<std::uint8_t>(JobKind::Scout)) ++scoutsOut;
    }
    if (scoutsOut >= kMaxScouts) return;

    // The frontier: unknown tiles that touch known ground. Walking to one of
    // those is the cheapest way to learn something new.
    TilePos best;
    std::int32_t bestScore = -1;
    // Ring by ring outward, stopping at the first that has any unknown ground
    // touching known ground. Stepping two rings at a time stepped straight over
    // the frontier - every candidate was unknown but so were all its neighbours,
    // so nothing ever qualified and nobody ever went looking.
    for (std::int32_t ring = 1; ring <= kScoutRange; ++ring) {
        for (TilePos candidate : core::tileRing(st.hearth, ring)) {
            if (!w.map().inBounds(candidate)) continue;
            if (w.map().at(candidate).explored) continue;
            if (w.map().blocked(candidate)) continue;
            if (w.isReserved(World::tileKey(candidate), p.id)) continue;

            bool touchesKnown = false;
            for (const auto& d : core::neighbourOffsets(candidate)) {
                const TilePos n{candidate.x + d.x, candidate.y + d.y};
                if (w.map().inBounds(n) && w.map().at(n).explored) { touchesKnown = true; break; }
            }
            if (!touchesKnown) continue;

            const std::int32_t score = 100 - core::tileDistance(p.tile, candidate);
            if (score > bestScore) { bestScore = score; best = candidate; }
        }
        if (bestScore >= 0) break;   // the innermost unknown edge is the one to walk to
    }
    if (bestScore < 0) return;

    Candidate c;
    c.job.kind = JobKind::Scout;
    c.job.category = WorkCategory::Scouting;
    c.job.target = best;
    c.job.workRequired = kScoutLookWork;
    // Worth doing, never worth doing instead of eating. Scouting scores like a
    // modest craft: the community sends one or two people, not everybody.
    // Knowing the land is worth real effort - it is what makes any of it workable
    // at all - and only two people are ever out doing it.
    c.score = Fixed::fromInt(8) * categoryPriority(st, WorkCategory::Scouting) *
              personalWeight(w, p, WorkCategory::Scouting) *
              distanceWeight(core::tileDistance(p.tile, best));
    out.push_back(std::move(c));
}

// If the best work needs a tool the pawn lacks but one exists, going to get it is
// itself the job.
bool tryFetchTool(World& w, Person& p, const Settlement& st, const Demand& demand) {
    std::array<Fixed, static_cast<std::size_t>(ToolClass::Count)> want = demand.tool;

    // What tool would unlock the most valuable work right here?
    ToolClass best = ToolClass::None;
    Fixed bestValue = core::kZero;
    for (const auto& node : w.nodes()) {
        if (!resourceAlive(w, node) || resourceDepleted(w, node)) continue;
        const auto& def = w.db().resourceNode(node.def);
        const ToolClass cls = def.harvest.requiredTool != ToolClass::None ? def.harvest.requiredTool
                                                                         : def.harvest.preferredTool;
        if (cls == ToolClass::None || hasToolEquipped(w, p, cls)) continue;
        if (!knowsMethod(w, p, def.harvest.knowledgeDef)) continue;
        Fixed value = core::kZero;
        for (const auto& y : def.harvest.yields) value += demand.forItem(y.item) * std::int64_t(y.count);
        value = value * distanceWeight(core::chebyshev(p.tile, node.tile));
        if (value > bestValue) { bestValue = value; best = cls; }
    }
    for (std::size_t i = 0; i < want.size(); ++i)
        if (want[i] > bestValue && !hasToolEquipped(w, p, static_cast<ToolClass>(i))) {
            bestValue = want[i];
            best = static_cast<ToolClass>(i);
        }
    if (best == ToolClass::None || bestValue <= core::kZero) return false;

    const ItemStackId tool = findNearestTool(w, p.tile, best, p.id);
    if (!tool.valid()) return false;
    if (!workAllowedAt(w, st.id, WorkCategory::Hauling, w.stack(tool).tile)) return false;

    Job j;
    j.kind = JobKind::FetchTool;
    j.category = WorkCategory::Hauling;
    setJobStack(w, j, tool);
    j.target = w.stack(tool).where == StackWhere::InBuilding ? w.building(w.stack(tool).building).origin
                                                             : w.stack(tool).tile;
    j.workRequired = core::kZero;
    p.job = j;
    return true;
}

// Urgent bodily needs pre-empt work and ignore zones and priorities: a thirsty
// person walks to water whatever the player has set.
bool tryPersonalNeed(World& w, Person& p) {
    const auto& cfg = w.db().sim();
    const auto date = w.now();
    const NeedsSnapshot needs = needsSnapshot(w, p);

    if (needs.hydration < cfg.thirstDrinkThreshold) {
        TilePos water;
        if (findWaterTile(w, p.tile, water)) {
            Job j;
            j.kind = JobKind::Drink;
            j.category = WorkCategory::WaterCarrying;
            j.target = water;
            j.workRequired = core::kOne;
            p.job = j;
            return true;
        }
    }

    // Who eats when there is not enough. No brake on how fast a community
    // grows - it grows as it grows - but when the larder runs down the food goes
    // to the people who fill it again. Children go hungry first, then the old,
    // then everybody. This is what a community actually does, and it is the
    // difference between a settlement that shrinks through a bad year and one
    // that dies out to the last person.
    const Fixed larder = p.settlement.valid() ? w.settlement(p.settlement).foodDays : core::kOne;
    const bool mayEat = p.stage == LifeStage::Adult || larder >= (p.stage == LifeStage::Child
                                                                          ? kChildRationDays
                                                                          : kElderRationDays) ||
                        needs.satiety < Fixed::ratio(1, 10);
    if (mayEat && needs.satiety < cfg.hungerEatThreshold) {
        const bool starving = needs.satiety < Fixed::ratio(1, 5);
        const BuildingId home = homeForPerson(w, p);
        const bool homeMeal = home.valid() && !starving && (date.hour >= 17 || date.hour < 6);
        // A hungry gatherer carrying a basket of berries eats from the basket.
        // Without this they walked the whole load back to the stockpile and then
        // walked out again to fetch a meal, and spent a quarter of their waking
        // hours doing it.
        const ItemStackId carriedId = carriedStack(w, p.id);
        if (carriedId.valid()) {
            const auto& carried = w.stack(carriedId);
            const auto* state = ecsStackState(w, carriedId);
            const DefId definition = stackDefinition(w, carriedId);
            const std::int32_t count = state ? state->count : carried.count;
            const bool alive = state ? state->alive : carried.alive;
            const Fixed freshness = stackFreshness(w, carriedId);
            const auto& def = w.db().item(definition);
            // A hauler carrying grain to the store is carrying seed, not lunch.
            const auto& seedFund = w.settlement(p.settlement).seedReserve;
            const bool isSeed = definition.value < seedFund.size() &&
                                seedFund[definition.value] > 0 &&
                                countAvailable(w, definition) <= seedFund[definition.value];
            const bool edible = alive && count > 0 && !isSeed &&
                                def.category == ItemCategory::Food && def.nutrition > core::kZero &&
                                def.edibleRaw && (def.spoilDays <= 0 || freshness > core::kZero) &&
                                (!def.rawUnsafe || needs.satiety < Fixed::ratio(1, 5));
            if (edible) {
                Job j;
                j.kind = JobKind::Eat;
                j.category = WorkCategory::Hauling;
                setJobStack(w, j, carriedId);
                j.target = homeMeal ? w.building(home).origin : p.tile;
                j.building = homeMeal ? home : BuildingId{};
                j.workRequired = core::kOne;
                p.job = j;
                return true;
            }
        }

        const ItemStackId food = findNearestEdible(w, p, homeMeal ? home : BuildingId{});
        if (food.valid()) {
            Job j;
            j.kind = JobKind::Eat;
            j.category = WorkCategory::Hauling;
            setJobStack(w, j, food);
            const StackWhere where = stackWhere(w, food);
            j.target = where == StackWhere::InBuilding
                               ? w.building(stackBuilding(w, food)).origin
                               : stackTile(w, food);
            if (homeMeal) {
                j.deliverBuilding = home;
                j.deliverTo = w.building(home).origin;
            }
            j.workRequired = core::kOne;
            p.job = j;
            return true;
        }
    }

    // Somebody who is ill is somebody else's work (D98). Taken before the day's
    // own trade, because an untended wound costs the settlement a pair of hands
    // for a fortnight and sometimes for good - it is the highest-value hour
    // anybody can spend, and it is invisible on a demand table because the
    // thing being produced is a person.
    if (w.ailing() > 0 && needs.ailment == Person::Ailment::None && needs.health > Fixed::ratio(1, 2)) {
        const Person* patient = nullptr;
        Fixed worst = Fixed::ratio(1, 8);       // a scratch is left to heal itself
        auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& q = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
            if (q.settlement != p.settlement || q.id == p.id) continue;
            const NeedsSnapshot qNeeds = needsSnapshot(w, q);
            if (qNeeds.ailment == Person::Ailment::None) continue;
            if (qNeeds.ailmentSeverity <= worst) continue;
            // One healer at a time: without this the whole settlement walks to
            // the same sickbed and nothing else gets done.
            if (q.tendedBy.valid() && q.tendedBy != p.id) {
                const Person& other = w.person(q.tendedBy);
                if (personAlive(w, other) && other.job.kind == JobKind::Tend) continue;
            }
            worst = qNeeds.ailmentSeverity;
            patient = &q;
        }
        if (patient != nullptr) {
            // What answers this hurt: a poultice for a wound, a draught for a
            // fever. Knowing the difference is the whole of herbcraft, so a
            // person who does not know it cannot do this job at all.
            const bool wound = patient->ailment == Person::Ailment::Wound;
            const DefId herbcraft = w.db().knowledgeByName("herbcraft");
            if (knowsMethod(w, p, herbcraft)) {
                ItemStackId remedy;
                Fixed best = core::kZero;
                for (const auto& stack : w.stacks()) {
                    const auto* state = ecsStackState(w, stack.id);
                    const bool alive = state ? state->alive : stack.alive;
                    const std::int32_t count = state ? state->count : stack.count;
                    const StackWhere where = stackWhere(w, stack.id);
                    if (!alive || count <= 0) continue;
                    if (where == StackWhere::Equipped || where == StackWhere::Carried) continue;
                    const auto& def = w.db().item(stackDefinition(w, stack.id));
                    const Fixed answers = wound ? def.healsWound : def.healsSickness;
                    if (answers <= core::kZero) continue;
                    if (!materialsInReach(w, p.settlement, p.tile, stackTile(w, stack.id))) continue;
                    if (answers > best) { best = answers; remedy = stack.id; }
                }
                if (remedy.valid()) {
                    Job j;
                    j.kind = JobKind::Tend;
                    j.category = WorkCategory::Medical;
                    setJobStack(w, j, remedy);
                    j.person = patient->id;
                    const StackWhere where = stackWhere(w, remedy);
                    j.target = where == StackWhere::InBuilding
                                       ? w.building(stackBuilding(w, remedy)).origin
                                       : stackTile(w, remedy);
                    j.deliverTo = patient->tile;
                    j.workRequired = core::kOne;
                    p.job = j;
                    w.people()[patient->id.value].tendedBy = p.id;
                    return true;
                }
            }
        }
    }

    // And the dead are somebody's work too (D99). A body left where it fell is
    // not a matter of feeling: it fouls the ground it lies on, and the ground
    // people walk over is where the flux comes from. Burial is the oldest
    // observance there is and the first one that pays for itself.
    if (w.unburied() > 0) {
        const Person* unburied = nullptr;
        std::int32_t nearest = 1 << 20;
        for (const auto& q : w.people()) {
            if (personAlive(w, q) || q.buried || q.settlement != p.settlement) continue;
            const std::int32_t d = core::tileDistance(p.tile, q.tile);
            if (d < nearest) { nearest = d; unburied = &q; }
        }
        if (unburied != nullptr) {
            Job j;
            j.kind = JobKind::Bury;
            j.category = WorkCategory::Ritual;
            j.person = unburied->id;
            j.target = unburied->tile;
            j.workRequired = Fixed::fromInt(3);   // digging, and the words over it
            p.job = j;
            return true;
        }
    }

    // Getting dressed comes before the day's work: a person with nothing on in
    // autumn is on a slow path to dying of exposure, and the cloth is already
    // made and lying in the store.
    if (static_cast<std::int32_t>(p.worn.size()) < desiredGarments(w, p)) {
        const ItemStackId garment = findNearestWearable(w, p);
        if (garment.valid()) {
            Job j;
            j.kind = JobKind::Wear;
            j.category = WorkCategory::Hauling;
            setJobStack(w, j, garment);
            const StackWhere where = stackWhere(w, garment);
            j.target = where == StackWhere::InBuilding
                               ? w.building(stackBuilding(w, garment)).origin
                               : stackTile(w, garment);
            j.workRequired = core::kOne;
            p.job = j;
            return true;
        }
    }

    const bool night = !date.isDaylight;
    if (needs.rest < cfg.fatigueSleepThreshold || (night && needs.rest < Fixed::ratio(4, 5))) {
        Job j;
        j.kind = JobKind::Sleep;
        j.category = WorkCategory::Hauling;
        j.target = p.tile;
        // Housing capacity belongs to the whole building. No bed objects exist.
        const BuildingId home = homeForPerson(w, p);
        if (home.valid()) {
            j.target = w.building(home).origin;
            j.building = home;
        }
        p.job = j;
        return true;
    }
    return false;
}

bool routePersonalNeed(World& w, Person& p) {
    if (!p.job.valid()) return false;
    if (p.job.stack.valid() && !w.reserve(World::stackKey(p.job.stack), p.id)) return false;

    // Water is drunk from the bank, and anything standing on ground nobody can
    // walk onto is reached from beside it. A batch of food left lying where a
    // building later went up is exactly that case: the meal is three tiles away,
    // the tile it lies on is a wall, and the path onto it does not exist. A
    // community starved to the last person that way, standing in a heap with
    // seven days of food in the settlement.
    const bool ontoBlockedGround = !w.map().passable(p.job.target);
    const PathResult path =
            (p.job.kind == JobKind::Drink || ontoBlockedGround)
                    ? findPathAdjacent(w.map(), p.tile, p.job.target, kPlanningExpansionBudget)
                    : findPath(w.map(), p.tile, p.job.target, kPlanningExpansionBudget);
    if (!path.found) {
        w.releaseAllBy(p.id);
        p.job = Job{};
        return false;
    }
    p.job.path = path.tiles;
    p.job.pathIndex = 0;
    p.job.phase = path.tiles.empty() ? JobPhase::Working : JobPhase::Travelling;
    return true;
}

} // namespace


void assignJobs(World& w) {
    // GDD 7: an urgent bodily state can interrupt ordinary work, not merely lose a
    // tie-break against it. A pawn on the edge drops what it is doing - but only
    // if there is actually something to drop it for. Interrupting unconditionally
    // meant that a community with no food left abandoned every job every tick and
    // thrashed itself to death instead of going out and gathering some.
    auto people = w.ecs().view<const ecs::Identity, const ecs::JobState, ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        const auto& jobState = people.get<const ecs::JobState>(entity);
        Person& p = w.person(PersonId{identity.legacyIndex});
        if (!jobState.active) continue;
        if (jobState.kind == static_cast<std::uint8_t>(JobKind::Eat) ||
            jobState.kind == static_cast<std::uint8_t>(JobKind::Drink) ||
            jobState.kind == static_cast<std::uint8_t>(JobKind::Sleep)) continue;
        // Interrupt at the need's own threshold, not at the point of collapse.
        // Waiting until a quarter satiety left the whole community working at half
        // capacity all day, which halved everything it produced and made the
        // shortfall permanent.
        const auto& cfg = w.db().sim();
        const NeedsSnapshot needs = needsSnapshot(w, p);
        const bool needsAttention = needs.satiety < cfg.hungerEatThreshold ||
                                    needs.hydration < cfg.thirstDrinkThreshold ||
                                    needs.rest < cfg.fatigueSleepThreshold;
        if (!needsAttention) continue;
        // Finishing something almost done costs less than losing the work in it.
        if (p.job.workRequired > core::kZero && p.job.workDone > p.job.workRequired * Fixed::ratio(3, 4)) continue;

        const Job current = p.job;
        p.job = Job{};
        if (!tryPersonalNeed(w, p)) {
            p.job = current;                    // nothing to eat, nowhere to rest: keep working
            continue;
        }
        // The need job replaces the old one; put down whatever was being carried
        // rather than deleting it.
        const Job needJob = p.job;
        p.job = current;
        const ItemStackId carriedId = carriedStack(w, p.id);
        if (carriedId.valid()) {
            setStackLocation(w, carriedId, StackWhere::Ground, PersonId{}, BuildingId{}, p.tile);
            setCarriedStack(w, p.id, ItemStackId{});
        }
        w.releaseAllBy(p.id);
        p.job = needJob;
        if (!routePersonalNeed(w, p)) {
            // Nothing to eat that can be reached is still a named reason; an idle
            // pawn must never be left without one.
            p.idleReason = IdleReason::NoReachableTarget;
            continue;
        }
    }

    // One demand computation per settlement per tick; every pawn in it reads the
    // same picture, so their choices are consistent with each other.
    std::vector<Demand> demands;
    demands.reserve(w.settlements().size());
    for (const auto& s : w.settlements()) demands.push_back(computeDemand(w, s.id));

    const TickCache cache = buildTickCache(w);

    // Shuffle the order in which pawns choose so a low entity id does not win
    // every contested target run after run.
    std::vector<PersonId> order;
    auto peopleForOrder = w.ecs().view<const ecs::Identity, ecs::Person>();
    for (const entt::entity entity : peopleForOrder) {
        const Person& p = w.person(PersonId{peopleForOrder.get<const ecs::Identity>(entity).legacyIndex});
        if (!p.job.valid()) order.push_back(p.id);
    }
    w.rng(core::stream::kWork).shuffle(order);
    std::stable_partition(order.begin(), order.end(), [&](PersonId id) {
        const auto& person = w.person(id);
        const auto& cfg = w.db().sim();
        const NeedsSnapshot needs = needsSnapshot(w, person);
        return needs.satiety < cfg.hungerEatThreshold ||
               needs.hydration < cfg.thirstDrinkThreshold ||
               needs.rest < cfg.fatigueSleepThreshold;
    });

    for (PersonId pid : order) {
        Person& p = w.person(pid);
        if (!personAlive(w, p) || p.job.valid()) continue;
        // A body in real need re-plans immediately; only fruitless work search waits.
        const NeedsSnapshot needs = needsSnapshot(w, p);
        const bool urgent = needs.satiety < w.db().sim().hungerEatThreshold ||
                            needs.hydration < w.db().sim().thirstDrinkThreshold ||
                            needs.rest < w.db().sim().fatigueSleepThreshold;
        if (!urgent && w.tickCount() < p.nextPlanTick) continue;

        if (!canWork(w, p)) {
            if (needs.asleep) { p.idleReason = IdleReason::Unfit; continue; }
        }

        if (tryPersonalNeed(w, p)) {
            if (routePersonalNeed(w, p)) {
                p.idleReason = IdleReason::Working;
                p.nextPlanTick = 0;
            } else {
                p.idleReason = IdleReason::NoReachableTarget;
            }
            continue;
        }
        if (!canWork(w, p)) { p.idleReason = IdleReason::Unfit; continue; }
        if (!p.settlement.valid()) { p.idleReason = IdleReason::NoWorkAvailable; continue; }

        const Settlement& st = w.settlement(p.settlement);
        const Demand& demand = demands[p.settlement.value];
        PlanningBlockers blocked;
        std::vector<Candidate> candidates;

        auto gather = [&](std::int32_t radius) {
            candidates.clear();
            addConstructionCandidates(w, p, st, demand, candidates, blocked);
            addCraftCandidates(w, cache, p, st, demand, candidates, blocked);
            addHarvestCandidates(w, cache, p, st, demand, radius, candidates, blocked);
            addFellingCandidates(w, cache, p, st, demand, radius, candidates, blocked);
            addFieldCandidates(w, cache, p, st, demand, radius, candidates, blocked);
            addLivestockCandidates(w, p, st, demand, radius, candidates, blocked);
            addWildCandidates(w, cache, p, st, demand, radius, candidates, blocked);
            addPenningCandidates(w, p, st, radius, candidates);
            addHaulCandidates(w, cache, p, st, demand, radius, candidates);
            addScoutCandidates(w, p, st, candidates);
            // Highest score first; ties broken deterministically by job shape.
            std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
                if (a.score != b.score) return a.score > b.score;
                if (a.job.kind != b.job.kind) return a.job.kind < b.job.kind;
                if (a.job.target.x != b.job.target.x) return a.job.target.x < b.job.target.x;
                return a.job.target.y < b.job.target.y;
            });
        };

        gather(kNearWorkRadius);
        if (candidates.empty()) gather(kFarWorkRadius);

        bool assigned = false;
        // Try the best few in order; a candidate can still fail on the route,
        // which is only knowable after an actual path search.
        const std::size_t attempts = std::min<std::size_t>(candidates.size(), 3);
        for (std::size_t i = 0; i < attempts; ++i) {
            Job job = candidates[i].job;
            const PathResult path = findPathAdjacent(w.map(), p.tile, job.target, kPlanningExpansionBudget);
            if (!path.found) continue;
            job.path = path.tiles;
            job.pathIndex = 0;
            job.phase = job.path.empty() ? JobPhase::Working : JobPhase::Travelling;

            std::vector<std::uint64_t> reservationKeys;
            if (job.node.valid()) reservationKeys.push_back(World::nodeKey(job.node));
            if (job.animal.valid()) reservationKeys.push_back(World::animalKey(job.animal));
            if (job.kind == JobKind::Till || job.kind == JobKind::Sow ||
                job.kind == JobKind::ReapCrop || job.kind == JobKind::Scout)
                reservationKeys.push_back(World::tileKey(job.target));
            if (job.stack.valid()) reservationKeys.push_back(World::stackKey(job.stack));
            if (job.kind == JobKind::Craft && job.building.valid())
                reservationKeys.push_back(World::buildingKey(job.building));
            if (job.kind == JobKind::HaulToStore && !job.deliverBuilding.valid())
                reservationKeys.push_back(World::tileKey(job.deliverTo));
            if (!w.reserveAll(reservationKeys, p.id)) continue;

            p.job = job;
            w.syncEcsJob(p.id);
            p.idleReason = IdleReason::Working;
            assigned = true;
            break;
        }
        if (assigned) continue;

        if (tryFetchTool(w, p, st, demand)) {
            const PathResult path = findPathAdjacent(w.map(), p.tile, p.job.target);
            if (path.found && w.reserve(World::stackKey(p.job.stack), p.id)) {
                p.job.path = path.tiles;
                p.job.phase = p.job.path.empty() ? JobPhase::Working : JobPhase::Travelling;
                w.syncEcsJob(p.id);
                p.idleReason = IdleReason::Working;
                continue;
            }
            p.job = Job{};
        }

        // Nothing to do. Name the reason so a stalled economy explains itself.
        auto& rep = w.report();
        if (!candidates.empty()) {
            p.idleReason = IdleReason::NoReachableTarget;
        } else if (blocked.missingTool) {
            p.idleReason = IdleReason::NoToolForAnyJob;
            rep.toolShortageRejections++;
        } else if (blocked.missingMaterial) {
            p.idleReason = IdleReason::NoMaterialsForAnyJob;
            rep.materialShortageRejections++;
        } else if (blocked.underSkilled) {
            p.idleReason = IdleReason::NotSkilledEnough;
        } else {
            p.idleReason = IdleReason::NoWorkAvailable;
        }
        p.nextPlanTick = w.tickCount() + kIdleReplanDelay;
    }
}


// ---------------------------------------------------------------------------
// Autonomous construction planning
// ---------------------------------------------------------------------------
namespace {

// More than this many open sites and nothing ever gets finished.
constexpr std::int32_t kMaxConcurrentProjects = 3;

// Whether a tile is part of the building itself rather than of its margin.
bool coversFootprint(const content::ContentDb& db, DefId def, TilePos origin, TilePos p) {
    const auto& d = db.building(def);
    return p.x >= origin.x && p.x < origin.x + d.footprintWidth && p.y >= origin.y &&
           p.y < origin.y + d.footprintDepth;
}

bool footprintClear(const World& w, DefId buildingDef, TilePos origin,
                    BuildingId ignore = BuildingId{}) {
    const auto& def = w.db().building(buildingDef);
    // A channel is the exception to both rules below. It is meant to touch the
    // next length of channel and to run through the ground it waters, and while
    // it obeyed them a community could dig only single unconnected pits along
    // the bank: every neighbour of a canal was rejected for having a building in
    // its margin, and every tile of the field was rejected for being field.
    const bool channel = def.irrigationRadius > 0;
    // So is the wall, and for the same reason: a length of wall is meant to
    // touch the next one. While it obeyed the margin rule the community could
    // raise only single posts - three to seven of them in ten years, each with a
    // tile of daylight either side - and the gate, which has one tile of the
    // line set aside for it, could never be built at all, because something in
    // the settlement it rings always stood within a tile of that one.
    const bool line = def.kind == content::BuildingKind::Fortification;
    const std::int32_t margin = (channel || line) ? 0 : 1;
    for (TilePos p : footprintOf(w.db(), buildingDef, origin, margin)) {
        if (!w.map().inBounds(p)) return false;
        const Tile& t = w.map().at(p);
        // The one building that may stand in the way is the one being replaced:
        // an upgrade goes up where the old one stood, and the old one comes down
        // first.
        if (t.building.valid() && t.building != ignore) return false;
        // Beyond the footprint itself only the "nothing built here" rule applies:
        // the margin keeps the settlement walkable, it is not part of the house.
        if (!coversFootprint(w.db(), buildingDef, origin, p)) continue;
        if (!terrainPassable(t.terrain)) return false;
        // A mud-brick house does not stand on a bog, and a pit dug in one fills
        // with water. Light things - a reed shelter, a channel - are fine there.
        if (def.firmGround && t.terrain == Terrain::Marsh) return false;
        // Nor across the river. A crossing is shallow water with stones in it;
        // a house built on one looked exactly as odd as it sounds.
        if (t.ford) return false;
        // Something standing here is a cost, not a refusal: the community clears
        // it. Only what cannot be cleared at all stops the site.
        if (channel) continue;
        // Nobody builds on ground already broken for sowing - but a farmstead
        // does stand on the edge of its own fields, so unbroken ground inside a
        // farm area is fair game. The siting score is what keeps buildings off
        // it while there is anywhere else to go.
        if (t.tilled || t.crop.valid()) return false;
        if (insideAnyZoneOfKind(w, ZoneKind::Storage, p)) return false;
    }
    return true;
}

// Which area a building wants to be near. GDD 8 has the settlers choosing their
// own placement; what they choose is a spot close to the work it serves.
ZoneKind relatedZone(const content::BuildingDef& def) {
    // What the content says first: a dairy belongs by the flock and a kiln by
    // the clay, whatever kind of building they are.
    if (!def.nearArea.empty()) {
        if (def.nearArea == "pasture") return ZoneKind::Pasture;
        if (def.nearArea == "farm") return ZoneKind::Farm;
        if (def.nearArea == "extraction") return ZoneKind::Extraction;
        if (def.nearArea == "fishing") return ZoneKind::Fishing;
        if (def.nearArea == "hunting") return ZoneKind::Hunting;
    }
    // Otherwise the area is decided by what the building is: a roof belongs in a
    // residential quarter, a bench in the craftsmen's, the common stores and the
    // fire on the common ground, the wall on the line (D79).
    switch (def.kind) {
        case content::BuildingKind::Housing:       return ZoneKind::Residential;
        case content::BuildingKind::Workshop:      return ZoneKind::Craft;
        case content::BuildingKind::Storage:       return ZoneKind::Storage;
        case content::BuildingKind::Hearth:        return ZoneKind::Civic;
        case content::BuildingKind::Fortification: return ZoneKind::Fortification;
        default:                                    return ZoneKind::Settlement;
    }
}

// Search outward from the hearth so the settlement grows as a settlement rather
// than as scattered huts, and prefer a spot near the area the building serves.
bool findBuildSpot(const World& w, const Settlement& st, DefId buildingDef, TilePos& out,
                   TilePos around = TilePos{}, bool haveAnchor = false) {
    const auto& def = w.db().building(buildingDef);
    const Zone* related = nearestZoneOfKind(w, st.id, relatedZone(def), st.hearth);
    TilePos anchor = related && !related->tiles.empty() ? related->centre() : st.hearth;
    // A family's own house and its own store go where the family lives, which is
    // near the ground it works. Without this every roof went up against the
    // hearth, and the settlement was one heap of huts whatever anybody in it did
    // for a living.
    TilePos from = st.hearth;
    if (haveAnchor) {
        anchor = around;
        from = around;
    }

    std::int64_t bestScore = -1;
    bool found = false;

    // A channel has to hold water, so it starts at the river or continues one
    // that already does. Given that, it is dug where it will water the most dry
    // ground the community actually works. Scoring it by nearness to the driest
    // furrow instead packed two hundred channels along one edge of the field:
    // hundreds of tiles are all "one away" from something dry, so the community
    // dug the same place over and over instead of pushing the water inland.
    if (def.irrigationRadius > 0) {
        // Ground worth watering: farm ground whose own soil is poor. Silt on the
        // floodplain is already as good as water can make it - a channel there
        // costs a plot and buys nothing, and the community was digging a hundred
        // and seventy of them through land that flooded every spring anyway.
        // What irrigation is for is carrying the fertile country out into the
        // dry ground behind the levee, where the soil is worth a fifth of the
        // silt and a channel is the whole difference.
        std::vector<TilePos> dry;
        for (const Zone& z : w.zones()) {
            if (!z.alive || z.kind != ZoneKind::Farm || z.settlement != st.id) continue;
            for (TilePos t : z.tiles) {
                if (!w.map().inBounds(t)) continue;
                const Tile& tile = w.map().at(t);
                if (tile.irrigated) continue;
                if (tile.fertility >= kSoilWorthWatering) continue;
                dry.push_back(t);
            }
        }
        if (dry.empty()) return false;

        // Where the water is worth most: the dry tile with the most dry ground
        // around it. The trunk is dug towards that, and not towards whatever dry
        // plot happens to be nearest - the outer end of every furrow is dry, so
        // "nearest" was always two tiles away in some direction or other, the
        // trunk had no gradient to follow, and it wandered along the bank until
        // it had spent its whole allowance of soil without leaving the
        // floodplain (D88).
        const auto worthOf = [&](TilePos candidate) {
            std::int64_t worth = 0;
            for (TilePos q : core::tilesWithin(candidate, def.irrigationRadius)) {
                if (!w.map().inBounds(q)) continue;
                const Tile& reached = w.map().at(q);
                if (reached.irrigated) continue;
                if (!insideZoneOfKind(w, st.id, ZoneKind::Farm, q)) continue;
                const Fixed deficit = kSoilWorthWatering - reached.fertility;
                if (deficit > core::kZero) worth += (deficit * 100).roundToInt();
            }
            return worth;
        };
        std::sort(dry.begin(), dry.end(), [&](TilePos a, TilePos b) {
            const std::int64_t wa = worthOf(a), wb = worthOf(b);
            if (wa != wb) return wa > wb;
            return a.y != b.y ? a.y < b.y : a.x < b.x;
        });

        // And on the bank the water can actually reach. Water runs over ground,
        // not across the river and not over a ford, so the ground a channel can
        // occupy is whatever is land-connected to the intake. The field on this
        // seed lies on the east bank and the community was cutting its intake
        // into the west one: the trunk could never have arrived, so it wandered
        // until it had spent every tile of soil it was allowed.
        TilePos target = dry.front();
        std::vector<std::uint8_t> reachable;
        bool haveBank = false;
        const std::int32_t mapW = w.map().width(), mapH = w.map().height();
        const auto indexOf = [&](TilePos t) { return static_cast<std::size_t>(t.y) * mapW + t.x; };
        for (std::size_t attempt = 0; attempt < dry.size() && attempt < 6 && !haveBank; ++attempt) {
            target = dry[attempt];
            reachable.assign(static_cast<std::size_t>(mapW) * mapH, 0);
            std::vector<TilePos> open{target};
            reachable[indexOf(target)] = 1;
            bool touchesWater = false;
            std::size_t spread = 0;
            while (!open.empty() && spread < 6000) {
                const TilePos at = open.back();
                open.pop_back();
                ++spread;
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour(at, dir);
                    if (!w.map().inBounds(n)) continue;
                    const Tile& tile = w.map().at(n);
                    if (tile.terrain == Terrain::Water) { touchesWater = true; continue; }
                    if (tile.ford) continue;             // a crossing carries no canal
                    auto& mark = reachable[indexOf(n)];
                    if (mark) continue;
                    mark = 1;
                    open.push_back(n);
                }
            }
            haveBank = touchesWater;
        }
        if (!haveBank) return false;

        // Does this settlement already have its intake from the river?
        bool intakeStands = false;
        auto irrigationBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive, ecs::Building>();
        for (const entt::entity entity : irrigationBuildings) {
            if (!irrigationBuildings.get<const ecs::Alive>(entity).value) continue;
            const auto& b = w.building(BuildingId{
                irrigationBuildings.get<const ecs::Identity>(entity).legacyIndex});
            if (b.settlement != st.id) continue;
            if (w.db().building(b.def).irrigationRadius <= 0) continue;
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(b.origin, dir);
                if (w.map().inBounds(n) && w.map().at(n).terrain == Terrain::Water) {
                    intakeStands = true;
                    break;
                }
            }
            if (intakeStands) break;
        }

        // How much soil has already gone into channel that waters nothing: the
        // trunk on its way out of the floodplain.
        std::int32_t trunkTiles = 0;
        for (const entt::entity entity : irrigationBuildings) {
            if (!irrigationBuildings.get<const ecs::Alive>(entity).value) continue;
            const auto& b = w.building(BuildingId{
                irrigationBuildings.get<const ecs::Identity>(entity).legacyIndex});
            if (b.settlement != st.id) continue;
            if (w.db().building(b.def).irrigationRadius <= 0) continue;
            if (w.map().inBounds(b.origin) && w.map().at(b.origin).fertility >= kSoilWorthWatering)
                ++trunkTiles;
        }

        std::int64_t bestGain = 0;   // meaningful only once `found`
        for (TilePos p : core::tilesWithin(st.hearth, kFarWorkRadius)) {
            if (!w.map().inBounds(p)) continue;
            const Tile& t = w.map().at(p);
            if (!t.explored || !terrainPassable(t.terrain)) continue;
            if (!reachable[indexOf(p)]) continue;            // the wrong bank
            if (t.crop.valid()) continue;                    // never dig up a standing crop
            // Through the field where it has to be: the channel takes the plot
            // it runs through, and that is the price of watering the two dozen
            // around it. Standing ground on the way costs nothing, field costs
            // something, broken field costs more - and a plot with a crop
            // already in it is never dug up at all.
            std::int64_t cost = 0;
            if (insideZoneOfKind(w, st.id, ZoneKind::Farm, p)) cost += t.tilled ? 3 : 1;

            // Six neighbour lookups before the two expensive questions: only a
            // handful of tiles on the map can hold water at all, and asking the
            // costly ones of all nine thousand cost a third of the tick budget.
            // Water runs along the sides of a tile, never across a corner, and
            // it has to run from somewhere: either the river or the next length
            // of channel. Checking six of the eight directions - a leftover from
            // the hex grid - left two of the four sides unchecked, so a channel
            // could be laid with nothing joining it and the fields filled up
            // with separate square puddles.
            bool fromRiver = false;
            bool fromChannel = false;
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(p, dir);
                if (!w.map().inBounds(n)) continue;
                if (w.map().at(n).terrain == Terrain::Water) { fromRiver = true; continue; }
                const BuildingId b = w.map().at(n).building;
                if (!b.valid()) continue;
                const Building& next = w.buildings()[b.value];
                // Finished channel only. Digging from a length that was merely
                // planned left orphans behind whenever that plan was abandoned,
                // and an orphan is exactly the square puddle in the middle of a
                // field that this rule exists to prevent.
                if (buildingComplete(w, next.id) &&
                    w.db().building(next.def).irrigationRadius > 0)
                    fromChannel = true;
            }
            if (!fromRiver && !fromChannel) continue;
            // One intake, and one only. Every tile beside the bank is a possible
            // head, so unchecked the community cut a dozen leads out of the
            // river and a hundred and twenty-four lengths of channel ended up
            // running through silt that floods every spring anyway. A river
            // people digs one canal out of the bank and branches it where the
            // water is wanted (D88).
            if (fromRiver && !fromChannel && intakeStands) continue;
            if (!footprintClear(w, buildingDef, p)) continue;
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, p)) continue;

            // What this length would actually add: the fertility it would make
            // up on poor ground it reaches, not the number of plots it touches.
            // Counting plots had a channel through the floodplain scoring as
            // well as one out in the desert, and the floodplain is where the
            // community already was.
            std::int64_t gain = 0;
            for (TilePos q : core::tilesWithin(p, def.irrigationRadius)) {
                if (!w.map().inBounds(q)) continue;
                const Tile& reached = w.map().at(q);
                if (reached.irrigated) continue;
                if (!insideZoneOfKind(w, st.id, ZoneKind::Farm, q)) continue;
                const Fixed deficit = kSoilWorthWatering - reached.fertility;
                if (deficit <= core::kZero) continue;
                gain += (deficit * 100).roundToInt();
            }
            // A length that waters nothing is a length of the trunk on its way
            // somewhere, and a trunk is a line, not a delta: it may only carry on
            // from a single neighbour. Without that the community dug a fan of
            // parallel leads out of the bank, all of them one tile from dry
            // ground and none of them reaching it. Where the water does pay,
            // any tile of the chain may branch - that is what a distributary is.
            // A length has to pay for itself. Any dry plot within three tiles
            // counts as a gain, so once the good ground was watered the
            // community went on digging for the sake of one plot at a time -
            // a hundred and sixty-seven lengths to add a hundred fertility.
            // Anything short of that is a length of the trunk on its way
            // somewhere - including one that happens to catch a plot or two in
            // passing. Rejecting those outright broke the chain: the trunk could
            // not carry on through them and nothing was allowed to replace them,
            // so seven lengths were dug and the water never arrived.
            if (gain < kChannelWorthDigging) {
                // And the trunk is not allowed to wander for ever looking for
                // the desert: this much soil spent reaching it, and no more.
                if (trunkTiles >= kTrunkTilesAllowed) continue;
                std::int32_t neighbours = 0;
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour(p, dir);
                    if (!w.map().inBounds(n)) continue;
                    const BuildingId b = w.map().at(n).building;
                    if (b.valid() && w.buildingAlive(BuildingId{b.value}) &&
                        w.db().building(w.buildings()[b.value].def).irrigationRadius > 0)
                        ++neighbours;
                }
                if (neighbours > 1) continue;
            }

            // What it waters comes first; failing that, how much nearer it gets
            // the water to ground that needs it. Demanding a gain outright dug
            // nothing at all, because the first lengths of a channel out of the
            // river water nothing: only the last one reaches the field.
            const std::int64_t toTarget = core::tileDistance(p, target);
            const std::int64_t score =
                    gain * 100000 - toTarget * 100 - cost * 40 - core::tileDistance(p, st.hearth);
            if (!found || score > bestGain) { bestGain = score; out = p; found = true; }
        }
        return found;
    }

    // A wall goes on the fortification line and nowhere else. Scoring it by
    // distance from the hearth is not enough: the search stops at the innermost
    // ring that has room, so a wall scored that way is built through the middle
    // of the village. It also grows from whatever is already standing, so the
    // community raises a wall rather than a scattering of posts.
    if (def.kind == content::BuildingKind::Fortification) {
        const Zone* line = nearestZoneOfKind(w, st.id, ZoneKind::Fortification, st.hearth);
        if (!line) return false;
        // The gate goes in the tile the line set aside for it, and the wall
        // never does: the way out has to survive the wall being finished.
        const bool isGate = !def.blocksMovement;
        if (isGate && !st.hasGateway) return false;
        if (isGate) {
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, st.gateway)) return false;
            if (!footprintClear(w, buildingDef, st.gateway)) return false;
            out = st.gateway;
            return true;
        }
        for (TilePos p : line->tiles) {
            if (st.hasGateway && p.x == st.gateway.x && p.y == st.gateway.y) continue;
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, p)) continue;
            if (!footprintClear(w, buildingDef, p)) continue;
            std::int64_t score = 1;
            for (int dir : core::kCardinalDirections) {
                const TilePos n = core::neighbour(p, dir);
                if (!w.map().inBounds(n)) continue;
                const BuildingId b = w.map().at(n).building;
                if (b.valid() && w.buildingAlive(BuildingId{b.value}) &&
                    w.db().building(w.buildings()[b.value].def).kind ==
                            content::BuildingKind::Fortification)
                    score += 10;
            }
            if (score > bestScore) { bestScore = score; out = p; found = true; }
        }
        return found;
    }

    for (std::int32_t radius = 0; radius <= 16; ++radius) {
        for (TilePos p : core::tileRing(from, radius)) {
            if (!w.map().inBounds(p) || !w.map().at(p).explored) continue;
            // Always inside the settled ground, quarter or not: work draws on
            // the settlement's stores, and a granary outside the settlement
            // cannot see them. Room for quarters comes from the settlement
            // growing, not from building outside it.
            if (!insideSettlementZone(w, st.id, p)) continue;
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, p)) continue;
            if (!footprintClear(w, buildingDef, p)) continue;

            // Close to the hearth, and closer still to whatever the building is
            // for. Ground that has to be cleared first is worth less, and
            // building over stone the community quarries costs more than
            // felling a tree.
            // Sown ground is not building land. Unbroken ground inside a farm
            // area is allowed - a farmstead stands on the edge of its own
            // fields - but it costs enough that anything else wins while there
            // is anything else, or the whole village ends up standing among the
            // crops with the kiln in the middle of them.
            std::int64_t clearing = 0;
            if (insideAnyZoneOfKind(w, ZoneKind::Farm, p)) clearing += kBuildOnFieldCost;
            // Inside the area this building belongs to is worth a good walk: it
            // is what keeps the quarters quarters instead of one heap of roofs,
            // kilns and granaries around the fire. Worth, not a requirement - a
            // community with nowhere left in its craft quarter still builds.
            if (!insideAnyZoneOfKind(w, relatedZone(def), p)) clearing += kOutsideItsQuarterCost;
            for (TilePos q : footprintOf(w.db(), buildingDef, p)) {
                if (!w.map().inBounds(q)) continue;
                const ResourceNodeId n = w.map().at(q).node;
                if (!n.valid() || !w.resourceNodeAlive(n)) continue;
                const auto kind = w.db().resourceNode(w.node(n).def).kind;
                // A vein or a bank is worth keeping; a tree or a thicket is not.
                clearing += kind == content::ResourceKind::Rock ? 60 : 12;
            }
            const std::int64_t score = 200 - core::tileDistance(p, from) * 4 -
                                       core::tileDistance(p, anchor) * 6 - clearing;
            if (score > bestScore) { bestScore = score; out = p; found = true; }
        }
    }
    // Every ring is scored, not only the first one with room in it. Stopping at
    // the innermost ring that worked meant the score decided nothing: a building
    // went wherever there happened to be a gap nearest the fire, so no area
    // could draw it - one workshop in six stood in the craftsmen's quarter - and
    // starting the search inside the quarter instead only traded that for a mill
    // ten tiles from the granary it grinds. One score over the whole reach
    // weighs both: its own quarter, and the walk.
    return found;
}

bool anyBuildingOfKind(const World& w, SettlementId sid, content::BuildingKind kind, bool includePlanned) {
    auto buildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                   const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity entity : buildings) {
        const auto& identity = buildings.get<const ecs::Identity>(entity);
        const Building& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildings.get<const ecs::Alive>(entity).value || b.settlement != sid) continue;
        if (!includePlanned && !buildings.get<const ecs::ConstructionProgress>(entity).complete) continue;
        if (w.db().building(b.def).kind == kind) return true;
    }
    return false;
}

// Whether this settlement could actually get hold of what the building is made
// of: the material is in store, or something it knows how to make produces it and
// the place that work needs is standing.
//
// Without this a community that wanted a brick house wanted it for ever. Housing
// is decided before workshops are, so every day it chose the house it could not
// supply, the site stalled and was abandoned, and the kiln that would have fired
// the brick was never even considered.
// Whether the settlement already has as many of these as it will keep. A store
// answers "is there room" with "dig another pit", and raw material piles up for
// ever, so a community with no limit dug fifty-four of them.
// Has the community tried this and failed often enough to try something else?
bool planExhausted(const Settlement& st, DefId buildingDef) {
    if (!buildingDef.valid() || buildingDef.value >= st.planAttempts.size()) return false;
    return st.planAttempts[buildingDef.value] >= kPlanAttemptLimit;
}

void notePlanFailed(World& w, const Settlement& st, DefId buildingDef) {
    if (!buildingDef.valid()) return;
    auto& attempts = w.settlement(st.id).planAttempts;
    if (attempts.size() <= buildingDef.value) attempts.resize(buildingDef.value + 1, 0);
    attempts[buildingDef.value] += 1;
}


bool roomForAnother(const World& w, const Settlement& st, DefId buildingDef) {
    const auto& def = w.db().building(buildingDef);
    if (def.maxPerSettlement <= 0) return true;

    // Counted by what the building is for, not by which culture's version of it
    // this is. A mill is a mill: capping each definition separately let a
    // settlement that knew two kinds of quern build twice as many.
    std::int32_t have = 0;
    auto buildings = w.ecs().view<const ecs::Identity, const ecs::Alive, ecs::Building>();
    for (const entt::entity entity : buildings) {
        const auto& identity = buildings.get<const ecs::Identity>(entity);
        const Building& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildings.get<const ecs::Alive>(entity).value || b.settlement != st.id) continue;
        const auto& other = w.db().building(b.def);
        const bool sameThing = def.function.empty() ? b.def == buildingDef
                                                    : other.function == def.function;
        if (sameThing) ++have;
    }

    // And the ceiling rises with the mouths: one mill and one oven feed about
    // twenty people between them, so a settlement of forty needs two of each -
    // it starved with a full granary otherwise, unable to grind fast enough.
    std::int32_t population = 0;
    for (PersonId id : st.members)
        if (personAlive(w, w.person(id))) ++population;
    const std::int32_t limit = def.maxPerSettlement + population / kPeoplePerWorkshop;
    return have < limit;
}

bool canSupply(const World& w, const Settlement& st, DefId buildingDef) {
    for (const auto& need : w.db().building(buildingDef).materials) {
        if (countAvailable(w, need.item) >= need.count) continue;

        bool makeable = false;
        for (const auto& r : w.db().recipes()) {
            bool makes = false;
            for (const auto& o : r.outputs)
                if (o.item == need.item) makes = true;
            if (!makes) continue;

            bool anybodyKnows = false;
            for (PersonId id : st.members)
                if (personAlive(w, w.person(id)) && knowsMethod(w, w.person(id), r.knowledgeDef))
                    anybodyKnows = true;
            if (!anybodyKnows) continue;

            if (!r.workplaceDefs.empty()) {
                bool placeStands = false;
                auto workplaces = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                                const ecs::ConstructionProgress, ecs::Building>();
                for (const entt::entity entity : workplaces) {
                    const auto& identity = workplaces.get<const ecs::Identity>(entity);
                    const Building& b = w.building(BuildingId{identity.legacyIndex});
                    if (workplaces.get<const ecs::Alive>(entity).value &&
                        workplaces.get<const ecs::ConstructionProgress>(entity).complete &&
                        b.settlement == st.id &&
                        std::find(r.workplaceDefs.begin(), r.workplaceDefs.end(), b.def) !=
                                r.workplaceDefs.end())
                        placeStands = true;
                }
                if (!placeStands) continue;
            }
            makeable = true;
            break;
        }
        // Or gathered. A reed bed is not a recipe and a palm is not a workshop,
        // but a community with either standing in its country can certainly get
        // hold of reed and timber - and while this only asked about recipes, any
        // building made of gathered material was unbuildable whenever the store
        // happened to be empty. That is what left a flock of forty with nowhere
        // to be shut in: the byre is made of reed, and the answer to "can we get
        // reed" was no every time the last bundle had just been used.
        if (!makeable) {
            for (const auto& node : w.nodes()) {
                if (!resourceAlive(w, node) || resourceDepleted(w, node)) continue;
                if (core::tileDistance(node.tile, st.hearth) > kMaxWorkDistance) continue;
                const auto& nodeDef = w.db().resourceNode(node.def);
                const auto yieldsIt = [&](const content::HarvestSpec& spec) {
                    for (const auto& y : spec.yields)
                        if (y.item == need.item) return true;
                    return false;
                };
                if (!yieldsIt(nodeDef.harvest) && !yieldsIt(nodeDef.fell)) continue;
                makeable = true;
                break;
            }
        }
        if (!makeable) return false;
    }
    return true;
}

bool knowsHowToBuild(const World& w, const Settlement& st, DefId buildingDef) {
    if (!buildingDef.valid()) return false;
    const auto& def = w.db().building(buildingDef);
    for (PersonId id : st.members)
        if (personAlive(w, w.person(id)) && knowsMethod(w, w.person(id), def.knowledgeDef)) return true;
    return false;
}

// The best building of a kind this settlement can raise, by whatever measure
// suits the kind. A shelter is judged by how little it costs; a house by how many
// it sleeps and how warm it keeps them.
DefId bestBuildableOfKind(const World& w, const Settlement& st, content::BuildingKind kind,
                          const std::function<std::int64_t(const content::BuildingDef&)>& score) {
    DefId best;
    std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
    for (const auto& def : w.db().buildings()) {
        if (def.kind != kind) continue;
        if (!knowsHowToBuild(w, st, def.id)) continue;
        if (!canSupply(w, st, def.id)) continue;
        if (!roomForAnother(w, st, def.id)) continue;
        if (planExhausted(st, def.id)) continue;
        const std::int64_t s = score(def);
        if (s > bestScore) { bestScore = s; best = def.id; }
    }
    return best;
}

// The cheapest building of a kind that this settlement can actually raise.
// Picking the cheapest outright and then discovering nobody knows how to build it
// stopped the whole day's planning: a Mesopotamian village kept choosing a
// thatched pit it had never heard of and so never got as far as housing.
DefId cheapestBuildableOfKind(const World& w, const Settlement& st, content::BuildingKind kind) {
    DefId best;
    Fixed bestWork = Fixed::fromInt(1 << 20);
    for (const auto& def : w.db().buildings()) {
        if (def.kind != kind) continue;
        if (!knowsHowToBuild(w, st, def.id)) continue;
        if (!canSupply(w, st, def.id)) continue;
        if (!roomForAnother(w, st, def.id)) continue;
        if (planExhausted(st, def.id)) continue;
        if (def.workAmount < bestWork) { bestWork = def.workAmount; best = def.id; }
    }
    return best;
}

} // namespace

void notePlanSucceeded(World& w, SettlementId sid) {
    // A plan *finished* means the ones waiting behind it deserve another look:
    // whatever was blocking the settlement has moved. Laying a blueprint is not
    // finishing one - counting it as success let a plan nobody could supply
    // clear its own failures every time it was laid again, and a community
    // pegged out thirty sites it never built.
    if (!sid.valid()) return;
    for (auto& count : w.settlement(sid).planAttempts) count = 0;
}

void planSettlementProjects(World& w) {
    // Once a day is plenty; nothing here changes between ticks.
    if (w.tickCount() % w.db().time().ticksPerDay() != 0) return;

    // Abandon sites nobody has been able to supply. Without this, a hut whose
    // timber needs an axe the community has not yet made occupies a project slot
    // for the rest of the game.
    const std::int64_t stallLimit = w.db().time().ticksPerDay() * 20;
    for (auto& b : w.buildings()) {
        if (!w.buildingAlive(b.id) || buildingComplete(w, b.id)) continue;
        if (w.tickCount() - b.lastProgressTick < stallLimit) continue;
        for (TilePos t : b.footprintTiles(w.db()))
            if (w.map().inBounds(t)) w.map().at(t).building = BuildingId{};
        w.setBuildingAlive(b.id, false);
        // Nobody could supply it. That is the plan failing, not the site.
        if (b.settlement.valid()) notePlanFailed(w, w.settlement(b.settlement), b.def);
    }

    ecs::CommandBuffer placementIntents;
    for (const auto& st : w.settlements()) {
        if (!st.alive) continue;

        // Food accounting must run even while the construction queue is full.
        // Skipping it with the project early-out left young settlements at
        // foodDays == 0 forever, so their edible stores never informed needs.
        const Demand demand = computeDemand(w, st.id);
        w.settlement(st.id).foodDays = demand.foodDays;
        w.settlement(st.id).larderDays = demand.larderDays;

        std::int32_t open = 0;
        auto openBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                           const ecs::ConstructionProgress, ecs::Building>();
        for (const entt::entity entity : openBuildings) {
            const auto& identity = openBuildings.get<const ecs::Identity>(entity);
            const Building& b = w.building(BuildingId{identity.legacyIndex});
            if (openBuildings.get<const ecs::Alive>(entity).value && b.settlement == st.id &&
                !openBuildings.get<const ecs::ConstructionProgress>(entity).complete) ++open;
        }
        if (open >= kMaxConcurrentProjects) continue;

        std::int32_t population = 0;
        auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& p = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
            if (p.settlement == st.id) ++population;
        }

        DefId wanted;

        // A fire first: it is what makes cooking, warmth and boiling possible.
        if (!anyBuildingOfKind(w, st.id, content::BuildingKind::Hearth, true))
            wanted = cheapestBuildableOfKind(w, st, content::BuildingKind::Hearth);

        // Then somewhere dry to keep food.
        if (!wanted.valid() && !anyBuildingOfKind(w, st.id, content::BuildingKind::Storage, true))
            wanted = cheapestBuildableOfKind(w, st, content::BuildingKind::Storage);

        // Then a roof over the stores, once they are outgrowing the ones there
        // are. A settlement whose granary is full stops storing: the surplus is
        // left where it was made and rots there.
        if (!wanted.valid()) {
            std::int32_t slots = 0;
            auto storageBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                                  const ecs::ConstructionProgress, ecs::Building>();
            for (const entt::entity entity : storageBuildings) {
                const auto& identity = storageBuildings.get<const ecs::Identity>(entity);
                const Building& b = w.building(BuildingId{identity.legacyIndex});
                if (!storageBuildings.get<const ecs::Alive>(entity).value || b.settlement != st.id) continue;
                const auto& bd = w.db().building(b.def);
                if (bd.storageSlots <= 0) continue;
                // Counting what is still going up as well keeps the community
                // from queueing a second granary while the first is unfinished.
                slots += bd.storageSlots;
            }
            // A store for each kind of thing, and whichever is short of room
            // gets the next one. The granary takes the harvest; the pit takes
            // reed, clay, brick, flint and tools. Asking only "is there room"
            // without asking "room for what" had a community answer a granary
            // full of grain by digging a pit that could not hold any.
            // A ceiling that grows with the settlement rather than a fixed one.
            // A flat cap looked reasonable and killed communities outright: when
            // the stores were full and no more were allowed, hauling stopped and
            // the harvest rotted where it was reaped.
            std::int32_t stores = 0;
            for (const entt::entity entity : storageBuildings) {
                const auto& identity = storageBuildings.get<const ecs::Identity>(entity);
                const Building& b = w.building(BuildingId{identity.legacyIndex});
                if (storageBuildings.get<const ecs::Alive>(entity).value && b.settlement == st.id &&
                    w.db().building(b.def).storageSlots > 0)
                    ++stores;
            }
            const bool roomToBuild = stores < kStoresPerSettlement + population / 4;

            if (!roomToBuild) {
                // nothing
            } else if (slots == 0) {
                wanted = bestBuildableOfKind(w, st, content::BuildingKind::Storage,
                                             [](const content::BuildingDef& d) {
                                                 return std::int64_t(d.storageSlots);
                                             });
            } else {
                for (const auto category : {ItemCategory::Food, ItemCategory::Raw}) {
                    if (wanted.valid()) break;
                    std::int32_t roomFor = 0;
                    std::int32_t usedFor = 0;
                    for (const entt::entity entity : storageBuildings) {
                        const auto& identity = storageBuildings.get<const ecs::Identity>(entity);
                        const Building& b = w.building(BuildingId{identity.legacyIndex});
                        if (!storageBuildings.get<const ecs::Alive>(entity).value || b.settlement != st.id) continue;
                        const auto& bd = w.db().building(b.def);
                        if (bd.storageSlots <= 0) continue;
                        if (!bd.acceptsCategories.empty() &&
                            std::find(bd.acceptsCategories.begin(), bd.acceptsCategories.end(),
                                      category) == bd.acceptsCategories.end())
                            continue;
                        roomFor += bd.storageSlots;
                        if (storageBuildings.get<const ecs::ConstructionProgress>(entity).complete)
                            usedFor += stacksInBuilding(w, b.id);
                    }
                    if (roomFor > 0 && usedFor * 5 < roomFor * 4) continue;
                    wanted = bestBuildableOfKind(
                            w, st, content::BuildingKind::Storage,
                            [&](const content::BuildingDef& d) -> std::int64_t {
                                const bool takes =
                                        d.acceptsCategories.empty() ||
                                        std::find(d.acceptsCategories.begin(),
                                                  d.acceptsCategories.end(),
                                                  category) != d.acceptsCategories.end();
                                if (!takes) return -1;
                                return std::int64_t(d.storageSlots);
                            });
                }
            }
        }

        // Then a roof for whichever family has the least of one. Housing is
        // decided family by family and built at the family's own place: a
        // shepherd's house goes up by the pasture and a farmer's by the field,
        // and a family does not move into somebody else's house because the
        // settlement's bed count happens to add up.
        TilePos buildAt;
        bool atSeat = false;
        HouseholdId buildFor;
        if (!wanted.valid()) {
            std::int32_t worst = 0;
            for (const auto& house : w.households()) {
                if (!house.alive || house.settlement != st.id || !house.seated) continue;

                std::int32_t mouths = 0;
                for (PersonId id : st.members)
                    if (personAlive(w, w.person(id)) && w.person(id).household == house.id)
                        mouths += bedQuartersFor(w.person(id));
                if (mouths == 0) continue;

                // Only its own roofs count. A house with no family attached -
                // the first shelter a settlement throws up - counts for whoever
                // is nearest it.
                std::int32_t beds = 0;
                for (const auto& b : w.buildings()) {
                    if (!w.buildingAlive(b.id) || b.settlement != st.id) continue;
                    const auto& bd = w.db().building(b.def);
                    if (bd.sleepingSlots <= 0) continue;
                    if (w.buildingPhase(b.id) != static_cast<std::uint8_t>(BuildState::Complete) &&
                        w.buildingPhase(b.id) != static_cast<std::uint8_t>(BuildState::Building) &&
                        w.buildingPhase(b.id) != static_cast<std::uint8_t>(BuildState::Blueprint))
                        continue;
                    const bool ours = b.household.valid()
                                              ? b.household == house.id
                                              : core::tileDistance(b.origin, house.seat) <= kQuarterRadius;
                    if (!ours) continue;
                    beds += bd.sleepingSlots * kBedQuarters;
                }
                if (mouths - beds > worst) { worst = mouths - beds; buildFor = house.id; }
            }

            // A roof for each family, decided family by family. This is what a
            // reed hut is for: eight bundles of reed and a week's work, so a
            // family of four raises two of them and rebuilds in brick later.
            // It only became affordable once the hut was priced like a hut -
            // when it cost a season, eight families wanted nine brick houses at
            // once and the community starved.
            // Is there something of this family's to build over? A family that
            // has outgrown its hut rebuilds on the same ground and uses what the
            // hut gives back, rather than putting a second building beside it
            // and leaving the first standing empty.
            if (buildFor.valid()) {
                buildAt = w.household(buildFor).seat;
                atSeat = true;
                // The first roof is whatever goes up quickest, because sleeping
                // outside is the immediate problem. After that a family builds
                // the best house it can supply: a reed hut is what you throw up
                // in a week, a brick house is what you live in.
                // Has this family any roof at all yet? A family with none takes
                // whatever goes up quickest - a reed hut in a week - and moves
                // into brick later. Giving every family a brick house at once
                // cost a settlement of eight families nine houses' worth of
                // brick and labour in its first years, and it starved.
                std::int32_t roofs = 0;
                for (const auto& b : w.buildings()) {
                    if (!w.buildingAlive(b.id) || b.settlement != st.id) continue;
                    if (w.db().building(b.def).sleepingSlots <= 0) continue;
                    const bool ours = b.household.valid()
                                              ? b.household == buildFor
                                              : core::tileDistance(b.origin,
                                                                   w.household(buildFor).seat) <=
                                                        kQuarterRadius;
                    if (ours) ++roofs;
                }
                // The house that fits this family, not the largest one going. A
                // family of three that builds a house for six spends thirty-four
                // bricks and a season on three empty beds - and a settlement
                // where every family did that built seventeen large houses and
                // starved.
                const std::int32_t mouths = worst;
                (void)roofs;
                wanted = bestBuildableOfKind(
                        w, st, content::BuildingKind::Housing,
                        [mouths](const content::BuildingDef& d) {
                            const std::int32_t beds = d.sleepingSlots * kBedQuarters;
                            // Nearest fit, and a roof too small is worse than one
                            // a little too big.
                            const std::int32_t over = beds - mouths;
                            const std::int64_t miss = over >= 0 ? over : -over * 3;
                            return -miss * 10 + (d.comfortBonus * 8).roundToInt();
                        });
            }
        }

        // Then a workshop, but only once something the community wants actually
        // needs one - the building follows the demand, not a tech tree.
        if (!wanted.valid()) {
            for (const auto& r : w.db().recipes()) {
                if (r.workplaceDefs.empty()) continue;
                Fixed value = core::kZero;
                for (const auto& o : r.outputs) value += demand.forItem(o.item);
                if (value <= core::kZero) continue;
                bool exists = false;
                for (const auto& b : w.buildings())
                    if (w.buildingAlive(b.id) && b.settlement == st.id &&
                        std::find(r.workplaceDefs.begin(), r.workplaceDefs.end(), b.def) !=
                                r.workplaceDefs.end()) { exists = true; break; }
                if (exists) continue;
                // Of the buildings that would serve, take the one this settlement
                // actually knows how to raise.
                // Of the buildings that serve, the one that costs least work and
                // that this settlement knows how to raise.
                Fixed cheapest = Fixed::fromInt(1 << 20);
                for (DefId candidate : r.workplaceDefs) {
                    if (!knowsHowToBuild(w, st, candidate)) continue;
                    if (planExhausted(st, candidate)) continue;
                    const Fixed work = w.db().building(candidate).workAmount;
                    if (work < cheapest) { cheapest = work; wanted = candidate; }
                }
                if (wanted.valid()) break;
            }
        }

        // Then a second one of whatever is congested. The inputs of a recipe are
        // piling up in store while its output is still wanted, which means the
        // one workshop the community has cannot chew through what the fields
        // bring in. Without this a village of seventeen starved to the last
        // person inside a granary holding three thousand ears of emmer: it had
        // grain, flour, a quern and an oven, and no reason to build a second of
        // either.
        if (!wanted.valid()) {
            std::vector<std::int64_t> inStore(w.db().items().size(), 0);
            for (const auto& stack : w.stacks()) {
                const auto* state = ecsStackState(w, stack.id);
                const StackWhere where = stackWhere(w, stack.id);
                const std::int32_t count = state ? state->count : stack.count;
                const bool available = state ? state->alive && count > 0 &&
                                                     (where == StackWhere::Ground || where == StackWhere::InBuilding)
                                               : isAvailable(stack);
                if (available) inStore[stackDefinition(w, stack.id).value] += count;
            }

            const std::int32_t cap = 1 + population / 6;
            std::int64_t worst = 0;
            for (const auto& r : w.db().recipes()) {
                if (r.workplaceDefs.empty()) continue;
                Fixed value = core::kZero;
                for (const auto& o : r.outputs) value += demand.forItem(o.item);
                if (value <= core::kZero) continue;

                std::int32_t serving = 0;
                for (const auto& b : w.buildings())
                    if (w.buildingAlive(b.id) && b.settlement == st.id &&
                        std::find(r.workplaceDefs.begin(), r.workplaceDefs.end(), b.def) !=
                                r.workplaceDefs.end())
                        serving += 1;
                if (serving == 0 || serving >= cap) continue;

                // Batches of this recipe that could be worked right now. The
                // scarcest input decides, the way it decides in the job planner.
                std::int64_t batches = kWorkshopBacklogBatches * 1000;
                for (const auto& in : r.inputs)
                    if (in.count > 0)
                        batches = std::min(batches, inStore[in.item.value] / in.count);
                if (batches < std::int64_t(kWorkshopBacklogBatches) * serving) continue;
                if (batches <= worst) continue;

                for (DefId candidate : r.workplaceDefs) {
                    if (!knowsHowToBuild(w, st, candidate)) continue;
                    if (planExhausted(st, candidate)) continue;
                    // Some things a settlement builds one of, however busy it is.
                    const std::int32_t limit = w.db().building(candidate).maxPerSettlement;
                    if (limit > 0) {
                        std::int32_t have = 0;
                        for (const auto& b : w.buildings())
                            if (w.buildingAlive(b.id) && b.settlement == st.id && b.def == candidate) ++have;
                        if (have >= limit) continue;
                    }
                    worst = batches;
                    wanted = candidate;
                    break;
                }
            }
        }

        // Then somewhere to shut the flock in at night. A byre is what turns a
        // flock from something that has to be watched every night into something
        // that is simply brought in - and a flock nobody brings in strays and
        // gets taken.
        if (!wanted.valid()) {
            std::int32_t head = 0;
            auto herd = w.ecs().view<const ecs::Identity, const ecs::Animal, const ecs::Alive>();
            for (const entt::entity entity : herd) {
                const Animal& a = w.animal(AnimalId{herd.get<const ecs::Identity>(entity).legacyIndex});
                if (herd.get<const ecs::Alive>(entity).value && a.owner == st.id &&
                    !w.db().animal(a.def).guardsFlock) ++head;
            }
            std::int32_t shelter = 0;
            auto shelters = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                          const ecs::ConstructionProgress, ecs::Building>();
            for (const entt::entity entity : shelters) {
                const auto& identity = shelters.get<const ecs::Identity>(entity);
                const Building& b = w.building(BuildingId{identity.legacyIndex});
                const auto phase = shelters.get<const ecs::ConstructionProgress>(entity).phase;
                if (shelters.get<const ecs::Alive>(entity).value && b.settlement == st.id &&
                    (phase == static_cast<std::uint8_t>(BuildState::Complete) ||
                     phase == static_cast<std::uint8_t>(BuildState::Building) ||
                     phase == static_cast<std::uint8_t>(BuildState::Blueprint)))
                    shelter += w.db().building(b.def).animalSlots;
            }
            if (head > shelter) {
                Fixed cheapest = Fixed::fromInt(1 << 20);
                for (const auto& d : w.db().buildings()) {
                    if (d.animalSlots <= 0) continue;
                    if (!knowsHowToBuild(w, st, d.id)) continue;
                    if (!canSupply(w, st, d.id)) continue;
                    if (!roomForAnother(w, st, d.id)) continue;
                    if (planExhausted(st, d.id)) continue;
                    if (d.workAmount < cheapest) { cheapest = d.workAmount; wanted = d.id; }
                }
            }
        }

        // Then water to the fields. For a river people this is the technology
        // that decides what the land is worth, so it comes before the wall and
        // before anything else the community merely wants.
        if (!wanted.valid()) {
            // Ground the community has actually broken, and how much of it is
            // already watered. Both halves matter: watering land it has only
            // drawn an area around would have it digging for ever, and chasing
            // the last dry tile of a field that keeps growing had it dig three
            // hundred channels for three hundred plots.
            std::int32_t worked = 0;
            std::int32_t watered = 0;
            for (const Zone& z : w.zones()) {
                if (!z.alive || z.kind != ZoneKind::Farm || z.settlement != st.id) continue;
                for (TilePos t : z.tiles) {
                    if (!w.map().inBounds(t)) continue;
                    const Tile& tile = w.map().at(t);
                    if (!tile.tilled && !tile.crop.valid()) continue;
                    ++worked;
                    if (tile.irrigated) ++watered;
                }
            }
            // A budget, so that a dry tile nothing can reach does not have the
            // community digging across the map after it for ever. It is generous
            // because the scoring already refuses to dig where nothing would be
            // watered; this is only the backstop.
            std::int32_t channels = 0;
            bool digging = false;
            auto irrigationBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                                    const ecs::ConstructionProgress, ecs::Building>();
            for (const entt::entity entity : irrigationBuildings) {
                const auto& identity = irrigationBuildings.get<const ecs::Identity>(entity);
                const Building& b = w.building(BuildingId{identity.legacyIndex});
                if (!irrigationBuildings.get<const ecs::Alive>(entity).value || b.settlement != st.id) continue;
                if (w.db().building(b.def).irrigationRadius <= 0) continue;
                ++channels;
                if (!irrigationBuildings.get<const ecs::ConstructionProgress>(entity).complete) digging = true;
            }
            // One length of channel at a time. Laying out several at once had
            // people swapping between them every time a nearer one appeared:
            // a channel is cheap, so every unfinished length outbid whatever
            // its digger was already standing in.
            if (worked > watered && !digging && channels < worked / 2 + 8) {
                Fixed cheapest = Fixed::fromInt(1 << 20);
                for (const auto& d : w.db().buildings()) {
                    if (d.irrigationRadius <= 0) continue;
                    if (!knowsHowToBuild(w, st, d.id)) continue;
                    if (planExhausted(st, d.id)) continue;
                    if (d.workAmount < cheapest) { cheapest = d.workAmount; wanted = d.id; }
                }
            }
        }

        // Last, a wall. It is the first thing the community builds for tomorrow
        // rather than for today, so it waits until there is a settlement worth
        // walling and food enough that a season spent on earthworks costs
        // nobody a meal.
        if (!wanted.valid() && population >= kWallPopulation &&
            demand.foodDays >= Fixed::fromInt(kWallFoodDays))
            wanted = bestBuildableOfKind(w, st, content::BuildingKind::Fortification,
                                         [](const content::BuildingDef& d) {
                                             // The gate first, cheapest after
                                             // that. It is what the line is laid
                                             // out around, and a wall that goes
                                             // up before it is a wall built
                                             // across the way out.
                                             const std::int64_t gate =
                                                     d.blocksMovement ? 0 : 1'000'000;
                                             return gate - std::int64_t(d.workAmount.roundToInt());
                                         });

        if (!wanted.valid()) continue;

        // A workshop that belongs beside an area - a dairy by the flock - is
        // built in the quarter of the families that work there, not at the edge
        // of the village nearest to it.
        const auto& wantedDef = w.db().building(wanted);
        if (!atSeat && !wantedDef.nearArea.empty()) {
            const ZoneKind wants = relatedZone(wantedDef);
            for (const auto& house : w.households()) {
                if (!house.alive || house.settlement != st.id || !house.seated) continue;
                if (zoneOfTrade(house.trade) != wants) continue;
                buildAt = house.seat;
                atSeat = true;
                break;
            }
        }

        // Houses and the stores that serve a quarter go to the quarter; the
        // hearth, the walls and the waterworks belong to the whole settlement.
        const auto kind = w.db().building(wanted).kind;
        const bool toQuarter = atSeat && (kind == content::BuildingKind::Housing ||
                                          kind == content::BuildingKind::Storage ||
                                          !wantedDef.nearArea.empty());

        // If this is the better version of something that already stands, it goes
        // up where that stands: the old one is pulled down first and most of it
        // goes into the new one.
        if (wantedDef.replacesDef.valid()) {
            BuildingId worn;
            std::int32_t nearest = std::numeric_limits<std::int32_t>::max();
            for (const auto& b : w.buildings()) {
                if (!w.buildingAlive(b.id) || b.settlement != st.id) continue;
                if (b.def != wantedDef.replacesDef || !buildingComplete(w, b.id)) continue;
                if (b.replacedBy.valid()) continue;                 // already coming down
                if (buildFor.valid() && b.household.valid() && b.household != buildFor) continue;
                // And the better version has to fit where the old one stood. A
                // manor is bigger than the house it replaces, and unchecked it
                // went up over whatever was beside it - a finished irrigation
                // channel was swallowed on all eight sides and left holding no
                // water, which is a canal nobody can use and a building nobody
                // meant to put there.
                if (!footprintClear(w, wanted, b.origin, b.id)) continue;
                const std::int32_t d = atSeat ? core::tileDistance(b.origin, buildAt)
                                              : core::tileDistance(b.origin, st.hearth);
                if (d < nearest) { nearest = d; worn = b.id; }
            }
            if (worn.valid()) {
                w.building(worn).replacedBy = wanted;
                notePlanSucceeded(w, st.id);
                continue;
            }
        }

        TilePos spot;
        if (!findBuildSpot(w, st, wanted, spot, buildAt, toQuarter)) {
            notePlanFailed(w, st, wanted);
            continue;
        }
        placementIntents.push(ecs::PlaceBlueprintIntent{
                wanted, spot, st.id,
                kind == content::BuildingKind::Housing ? buildFor : HouseholdId{}});
    }
    w.applyPlacementIntents(placementIntents);
}

} // namespace work
} // namespace sim
