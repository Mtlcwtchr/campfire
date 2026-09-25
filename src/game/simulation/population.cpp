#include "game/simulation/population.hpp"

#include <algorithm>

#include "game/simulation/zones.hpp"

namespace sim {
namespace {

// How many settlement members must hold a method before it stops being one
// person's knowledge and becomes a tradition everyone grows up with (GDD 6).
std::int32_t traditionThreshold(std::int32_t adults) { return std::max(3, adults * 2 / 5); }

// A profession only changes after the new conditions have held for a while, so a
// single bad season does not turn every farmer into a woodcutter (GDD 6, the
// "profession inertia" the document leaves open - DECISIONS.md D8).
constexpr std::int32_t kProfessionReviewDays = 10;

const char* kGivenNames[] = {
    "Alkaios", "Bria", "Demos", "Eirene", "Gelon", "Hera", "Iason", "Kleio",
    "Lykos", "Melia", "Nestor", "Ophis", "Pelias", "Rhoda", "Selene", "Thera",
    "Xanthos", "Zoe", "Argos", "Byrsa", "Chryse", "Doros", "Elpis", "Phorbas",
};
constexpr std::size_t kGivenNameCount = sizeof(kGivenNames) / sizeof(kGivenNames[0]);

LifeStage stageForAge(const content::SimConfig& cfg, std::int32_t years) {
    if (years < cfg.adultAge) return LifeStage::Child;
    if (years >= cfg.elderAge) return LifeStage::Elder;
    return LifeStage::Adult;
}

void applyTraits(const World& w, Person& p) {
    p.strength = core::kOne;
    p.endurance = core::kOne;
    p.dexterity = core::kOne;
    for (DefId t : p.traits) {
        const auto& def = w.db().trait(t);
        p.strength += def.strengthMod;
        p.endurance += def.enduranceMod;
        p.dexterity += def.dexterityMod;
    }
    // Children and elders are not adults with a flat penalty; their bodies differ.
    switch (p.stage) {
        case LifeStage::Child: p.strength = p.strength * Fixed::ratio(1, 2);
                               p.endurance = p.endurance * Fixed::ratio(3, 5); break;
        case LifeStage::Elder: p.strength = p.strength * Fixed::ratio(7, 10);
                               p.endurance = p.endurance * Fixed::ratio(3, 5); break;
        case LifeStage::Adult: break;
    }
    p.strength = core::max(Fixed::ratio(1, 5), p.strength);
    p.endurance = core::max(Fixed::ratio(1, 5), p.endurance);
    p.dexterity = core::max(Fixed::ratio(1, 5), p.dexterity);
}

void rollInnateTraits(World& w, Person& p, const Person* mother, const Person* father) {
    auto& rng = w.rng(core::stream::kTraits);
    for (const auto& def : w.db().traits()) {
        bool excluded = false;
        for (DefId have : p.traits)
            for (const auto& x : w.db().trait(have).excludes)
                if (x == def.name) { excluded = true; break; }
        if (excluded) continue;

        bool got = false;
        if (def.heritable) {
            // Partial probabilistic inheritance from each carrying parent.
            for (const Person* parent : {mother, father}) {
                if (!parent) continue;
                if (std::find(parent->traits.begin(), parent->traits.end(), def.id) == parent->traits.end()) continue;
                if (rng.chance(def.heritableChanceNumerator, def.heritableChanceDenominator)) { got = true; break; }
            }
        }
        if (!got && def.innateChanceDenominator > 0 && rng.chance(1, def.innateChanceDenominator)) got = true;
        if (got) p.traits.push_back(def.id);
    }
}

// Deals the culture's trades out one after another, so a community of ten covers
// ten trades rather than ten people all being good at the same thing. GDD 6 has
// professions chosen autonomously; this is the competence they arrive with, not a
// posting.
void grantStartingSkills(World& w, Person& p, const content::EthnosDef& eth,
                         content::WorkCategory speciality) {
    if (eth.trades.empty()) return;
    const bool grown = p.stage != LifeStage::Child;

    for (const auto& name : eth.trades) {
        content::WorkCategory c;
        if (!content::parseWorkCategory(name, c)) continue;
        // Children have watched the work but not done it (GDD 6).
        p.skills[static_cast<std::size_t>(c)].level = grown ? eth.baseSkill : eth.baseSkill / 2;
    }
    // The family's own trade, which is the one it is actually good at. A child
    // of the house is already ahead in it without having worked a day: it grew
    // up watching.
    const std::size_t own = static_cast<std::size_t>(speciality);
    p.skills[own].level = grown ? eth.specialistSkill : (eth.specialistSkill + 1) / 2;
}

PersonId addPerson(World& w, SettlementId sid, Sex sex, std::int32_t ageYears,
                   PersonId mother, PersonId father, TilePos at) {
    const auto& cfg = w.db().sim();
    auto& rng = w.rng(core::stream::kPopulation);

    Person p;
    p.id = core::PersonId{static_cast<std::uint32_t>(w.people().size())};
    p.sex = sex;
    p.ageYears = ageYears;
    p.birthTick = w.tickCount() - std::int64_t(ageYears) * w.db().time().ticksPerYear();
    p.stage = stageForAge(cfg, ageYears);
    p.settlement = sid;
    p.mother = mother;
    p.father = father;
    p.tile = at;
    p.pos = core::tileCentre(at);
    p.name = kGivenNames[rng.below(kGivenNameCount)];

    w.people().push_back(std::move(p));
    Person& ref = w.people().back();

    const Person* m = mother.valid() ? &w.person(mother) : nullptr;
    const Person* f = father.valid() ? &w.person(father) : nullptr;
    rollInnateTraits(w, ref, m, f);
    applyTraits(w, ref);

    // Born into a house and into its trade. This is the whole of how a trade
    // stays in a family: the child grows up in the work, and by the time it is
    // grown the work is what it is good at.
    const Person* parent = m ? m : f;
    if (parent && parent->household.valid()) {
        ref.household = parent->household;
        ref.profession = w.household(parent->household).trade;
    }

    // Everyone born into the ethnos knows what the ethnos knows.
    if (sid.valid()) {
        const auto& st = w.settlement(sid);
        if (st.ethnos.valid())
            for (const auto& kn : w.db().ethnos(st.ethnos).commonKnowledge) {
                const DefId id = w.db().knowledgeByName(kn);
                if (id.valid()) ref.knownMethods.push_back(id);
            }
        w.settlement(sid).members.push_back(ref.id);
    }
    return ref.id;
}

} // namespace

// --- labour, in units of work a day -----------------------------------------
//
// Everything below is derived from the content, not guessed, and the derivation
// is worth keeping written down because the whole shape of a settlement follows
// from it. Feeding one person for a day means two units of satiety, which is
// 3.6 loaves. Working back through bake (10 flour -> 10 bread, 80), grind (15
// grain -> 10 flour, 70), thresh (20 ears -> 15 grain, 55) and the field
// itself (till 55 + sow 22 + reap 38 for 22 ears every 25 days):
//
//   field and threshing   58 work a day a head   <- the farmer's trade
//   grinding and baking   53 work a day a head   <- the miller's and baker's
//
// An adult delivers about three hundred units of work a day once sleeping,
// eating, walking and hauling are taken out. So two adults on the field feed
// about ten people, and two at the quern and the oven serve about eleven. That
// is the scale the rest of this is built on: one farm, one mill, ten mouths,
// and everything above that in proportion.

// Years somebody stays in a trade before they are open to another. A trade is a
// life, not a rota: at every review people swapped to whatever the ledger was
// shortest of that morning, and nothing was ever done properly.
constexpr std::int32_t kYearsInATrade = 3;

// What each trade is on the hook for, per day. Per plot for the farmer, per head
// of population for the trades that serve people, per animal for the herdsman.
constexpr std::int32_t kFieldWorkPerPlotDay = 7;
// The field does not feed everybody on its own: fish, game, milk and what is
// gathered carry part of it, and counting the whole of a person's food against
// farming had every settlement permanently short of farmers.
constexpr std::int32_t kHerdWorkPerAnimalDay = 8;
constexpr std::int32_t kCraftWorkPerHeadDay = 10;
constexpr std::int32_t kHaulWorkPerHeadDay = 20;
constexpr std::int32_t kWoodWorkPerHeadDay = 6;
constexpr std::int32_t kStoneWorkPerHeadDay = 3;
constexpr std::int32_t kGatherWorkPerHeadDay = 12;
constexpr std::int32_t kBuildDaysAllowed = 30;
// How much work a day it takes to be worth having somebody on a trade at all.
// A fifth of one person's day: below that the community is better off with the
// work picked up by whoever is passing.
constexpr std::int32_t kWorkToWarrantATrade = 60;

// How much work this trade needs doing in this settlement each day.
Fixed tradeNeed(const World& w, const Settlement& st, WorkCategory c) {
    std::int32_t mouths = 0;
    for (PersonId id : st.members)
        if (w.person(id).alive) ++mouths;

    switch (c) {
        case WorkCategory::Farming: {
            // What the mouths need grown, and never less than what the ground
            // already broken needs working. Counting only the broken ground was
            // a trap with no way out: a settlement with no field yet needed no
            // farmer, so it never got one and never got a field.
            std::int32_t plots = 0;
            for (const auto& z : w.zones()) {
                if (!z.alive || z.kind != ZoneKind::Farm || z.settlement != st.id) continue;
                for (TilePos t : z.tiles) {
                    if (!w.map().inBounds(t)) continue;
                    const Tile& tile = w.map().at(t);
                    if (tile.tilled || tile.crop.valid()) ++plots;
                }
            }
            return core::max(Fixed::fromInt(mouths * kFieldWorkPerHeadDay),
                             Fixed::fromInt(plots * kFieldWorkPerPlotDay));
        }
        case WorkCategory::Cooking:
            return Fixed::fromInt(mouths * kMealWorkPerHeadDay);
        case WorkCategory::Herding: {
            std::int32_t head = 0;
            for (const auto& a : w.animals())
                if (a.alive && a.owner == st.id) ++head;
            return Fixed::fromInt(head * kHerdWorkPerAnimalDay);
        }
        case WorkCategory::Crafting:     return Fixed::fromInt(mouths * kCraftWorkPerHeadDay);
        case WorkCategory::Hauling:      return Fixed::fromInt(mouths * kHaulWorkPerHeadDay);
        case WorkCategory::Woodcutting:  return Fixed::fromInt(mouths * kWoodWorkPerHeadDay);
        case WorkCategory::Mining:       return Fixed::fromInt(mouths * kStoneWorkPerHeadDay);
        case WorkCategory::Foraging:
        case WorkCategory::Hunting:      return Fixed::fromInt(mouths * kGatherWorkPerHeadDay);
        case WorkCategory::Construction: {
            Fixed remaining = core::kZero;
            for (const auto& b : w.buildings()) {
                if (!b.alive || b.settlement != st.id || b.state == BuildState::Complete) continue;
                remaining += core::max(core::kZero, w.db().building(b.def).workAmount - b.workDone);
            }
            return remaining / Fixed::fromInt(kBuildDaysAllowed);
        }
        default:
            return core::kZero;
    }
}

// And how much work the hands already on that trade can do in a day.
Fixed tradeHands(const World& w, const Settlement& st, WorkCategory c) {
    std::int32_t adults = 0;
    for (PersonId id : st.members) {
        const Person& q = w.person(id);
        if (q.alive && q.stage != LifeStage::Child && q.profession == c) ++adults;
    }
    return Fixed::fromInt(adults * kWorkPerAdultDay);
}

// How much the trade of one's own house outweighs whatever the ledger happens to
// want this morning.
const Fixed kFamilyTradeStickiness = Fixed::fromInt(3);

// A grown child of the house who is not its heir sets up on its own: same trade,
// its own place, its own roof, its own store. This is how a farming village
// grows into two farms and then four instead of piling everybody into the
// father's house, and it is why the population does not stall as it rises.
void foundNewHouseholds(World& w) {
    for (std::size_t i = 0; i < w.people().size(); ++i) {
        Person& p = w.people()[i];
        if (!p.alive || p.stage != LifeStage::Adult || !p.household.valid()) continue;
        if (!p.settlement.valid()) continue;
        // Somebody has to be at home to inherit: the eldest adult of the house
        // keeps it, and the eldest is the one nobody in the house is older than.
        bool heir = true;
        std::int32_t adults = 0;
        for (const auto& other : w.people()) {
            if (!other.alive || other.household != p.household) continue;
            if (other.stage == LifeStage::Child) continue;
            ++adults;
            if (other.id != p.id && other.ageYears > p.ageYears) heir = false;
        }
        if (heir) continue;
        // A house of one's own is something two people set up. Letting a single
        // grown adult found one made a household of every non-heir in the
        // settlement, each wanting a roof of its own: forty-eight huts for
        // thirty people, a shanty town rather than a village.
        if (!p.spouse.valid()) continue;
        if (!w.person(p.spouse).alive || w.person(p.spouse).stage != LifeStage::Adult) continue;

        // And only when the family's own work no longer needs the hands. A farm
        // whose field has grown keeps its grown children on it; a farm that is
        // already worked to its size sends them out to break their own ground or
        // to take up a trade nobody is covering. Which of the two happens is
        // decided by the profession review, in work units, not here.
        const Settlement& st = w.settlement(p.settlement);
        const content::WorkCategory family = w.household(p.household).trade;
        if (tradeNeed(w, st, family) > tradeHands(w, st, family)) continue;

        const content::WorkCategory trade = family;
        const HouseholdId house = w.createHousehold(p.settlement, trade);
        p.household = house;
        p.profession = trade;
        // A married pair leaves together, and so do their children.
        if (p.spouse.valid() && w.person(p.spouse).alive) {
            Person& spouse = w.person(p.spouse);
            spouse.household = house;
            spouse.profession = trade;
        }
        for (auto& child : w.people()) {
            if (!child.alive || child.stage != LifeStage::Child) continue;
            if (child.mother == p.id || child.father == p.id) {
                child.household = house;
                child.profession = trade;
            }
        }
        // One new house per review: a village does not split four ways in a day.
        return;
    }
}

bool reconsiderProfession(World& w, Person& p) {
    if (p.stage == LifeStage::Child || !p.settlement.valid()) return false;
    const Settlement& st = w.settlement(p.settlement);

    // Leaving a trade is a decision with a condition, not a score to be
    // outbid. Weighing trades by how badly each was short of hands made people
    // swap every review: a trade with nobody on it scored highest, so somebody
    // left theirs for it, which left theirs short, so somebody came back.
    //
    // The condition is the one the work itself imposes: there has to be at
    // least a pair of hands' worth of work going undone in the other trade, and
    // one's own has to still be covered after leaving it.
    const Fixed hands = Fixed::fromInt(kWorkPerAdultDay);
    const Fixed ownShortfall = tradeNeed(w, st, p.profession) - tradeHands(w, st, p.profession);

    WorkCategory best = p.profession;
    Fixed bestScore = core::kZero;
    // Whether the trade chosen is one nobody at all is doing. Such a trade is
    // taken up at once, without waiting out the years a trade normally holds
    // somebody: a community whose every cook walked into the fields on the same
    // morning had nobody left to bake, and the lock meant nobody could come back
    // for three years. They starved in the fourth with grain in the granary.
    WorkCategory unmannedWant = p.profession;
    for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
        const auto c = static_cast<WorkCategory>(i);
        if (c == p.profession) continue;
        // Where the hands are needed *more* than where they are. Requiring one's
        // own trade to be covered before leaving it was a deadlock: the need for
        // farming is counted per mouth, so a growing settlement is always short
        // of farmers, so nobody could ever become the shepherd - and a flock of
        // seventy-seven roamed a decade with nobody watching it.
        const Fixed need = tradeNeed(w, st, c);
        const Fixed shortfall = need - tradeHands(w, st, c);
        // A trade with nobody at all on it, and real work waiting, gets somebody
        // whatever the arithmetic says. Comparing shortfalls alone (D76) had a
        // hidden consequence: farming and cooking are counted per mouth, so a
        // growing settlement is always short of both, and no other trade could
        // ever show a bigger shortfall than the one the person was already in.
        // Twenty-two people came out as sixteen farmers and six cooks - no
        // shepherd, no woodcutter, nobody hauling - and a flock of forty strayed
        // away for want of anyone to bring it in.
        const bool unmanned = tradeHands(w, st, c) <= core::kZero &&
                              need >= Fixed::fromInt(kWorkToWarrantATrade);
        if (unmanned) unmannedWant = c;
        if (!unmanned && shortfall < ownShortfall + hands) continue;
        // But nobody leaves their own trade unmanned to do it.
        if (unmanned && tradeHands(w, st, p.profession) <= hands &&
            tradeNeed(w, st, p.profession) > core::kZero)
            continue;

        // Among the trades that need somebody, the one this person is best
        // suited to, with the trade of their own house counting for a lot: a
        // child of the house takes up the family work unless the family work
        // does not need them.
        Fixed score = shortfall * (core::kOne + Fixed::ratio(p.skills[i].level, 2));
        for (DefId t : p.traits)
            for (const auto& [cat, val] : w.db().trait(t).workAffinity)
                if (cat == c) score = score * core::max(Fixed::ratio(1, 4), core::kOne + val);
        if (p.household.valid() && c == w.household(p.household).trade)
            score = score * kFamilyTradeStickiness;
        if (score > bestScore) { bestScore = score; best = c; }
    }

    if (best == p.profession) return false;
    // And not again for years. Somebody who took up a trade last season is not
    // available for another: at every review, people swapped to wherever the
    // ledger was shortest that morning and nothing was ever done properly.
    if (best != unmannedWant && p.tradeTakenUpTick > 0 &&
        w.tickCount() - p.tradeTakenUpTick < std::int64_t(kYearsInATrade) *
                                                     w.db().time().ticksPerYear())
        return false;

    p.profession = best;
    p.tradeTakenUpTick = w.tickCount();
    // A family changes trade as a family: if the head of the house takes up
    // something else, the house has taken it up. Children carry that forward.
    if (p.household.valid()) {
        Household& house = w.household(p.household);
        bool senior = true;
        for (const auto& other : w.people())
            if (other.alive && other.id != p.id && other.household == house.id &&
                other.stage != LifeStage::Child && other.ageYears > p.ageYears)
                senior = false;
        if (senior) {
            house.trade = best;
            for (auto& member : w.people())
                if (member.alive && member.household == house.id) {
                    member.profession = best;
                    member.tradeTakenUpTick = w.tickCount();
                }
        }
    }
    return true;
}

void tickPopulation(World& w) {
    const auto& cfg = w.db().sim();
    const auto& time = w.db().time();

    // --- yearly: ageing, stage transitions, natural death ----------------
    if (w.tickCount() % time.ticksPerYear() == 0 && w.tickCount() > 0) {
        auto& rng = w.rng(core::stream::kPopulation);
        auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : people) {
            Person& p = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
            p.ageYears += 1;
            const LifeStage next = stageForAge(cfg, p.ageYears);
            if (next != p.stage) {
                p.stage = next;
                applyTraits(w, p);
            }
            if (p.stage == LifeStage::Elder) {
                // Natural death is a probabilistic outcome of being old, not a
                // timer started when the stage begins (GDD 6).
                const std::int32_t over = p.ageYears - cfg.elderAge;
                const std::int64_t chanceIn = std::max<std::int64_t>(3, 40 - over);
                if (p.ageYears >= cfg.maxAge || rng.chance(1, chanceIn)) {
                    p.alive = false;
                    p.deathTick = w.tickCount();
                    p.deathCause = "old age";
                    w.report().deaths++;
                    w.report().deathCauses["old age"]++;
                    w.releaseAllBy(p.id);
                }
            }
        }
    }

    // --- periodically: new houses, profession review, tradition fixing ---
    // Not on the first tick: a community that has just arrived has had no time
    // to reconsider anything, and reviewing then scattered the founding families
    // out of their own trades before the first day was out.
    if (w.tickCount() > 0 && w.tickCount() % (time.ticksPerDay() * kProfessionReviewDays) == 0) {
        foundNewHouseholds(w);

        // One departure a trade a review. A trade is not abandoned by everybody
        // on it at once: at one review thirteen cooks all walked into the fields
        // together - the fields were short of hands and the kitchen was not -
        // and the community starved in the year that followed.
        std::vector<std::int32_t> leaving(content::kWorkCategoryCount, 0);
        auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : people) {
            Person& p = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
            const auto was = p.profession;
            if (leaving[static_cast<std::size_t>(was)] >= 1) continue;
            if (reconsiderProfession(w, p)) leaving[static_cast<std::size_t>(was)] += 1;
        }

        for (auto& st : w.settlements()) {
            if (!st.alive) continue;
            std::int32_t adults = 0;
            for (PersonId id : st.members)
                if (w.person(id).alive && w.person(id).stage != LifeStage::Child) ++adults;
            const std::int32_t threshold = traditionThreshold(adults);

            for (const auto& k : w.db().knowledge()) {
                if (std::find(st.traditions.begin(), st.traditions.end(), k.id) != st.traditions.end()) continue;
                std::int32_t holders = 0;
                for (PersonId id : st.members)
                    if (w.person(id).alive && w.person(id).knows(k.id)) ++holders;
                if (holders >= threshold) st.traditions.push_back(k.id);
            }
        }
    }

    // --- daily: pairing and conception -----------------------------------
    if (w.tickCount() % time.ticksPerDay() != 0) return;
    auto& rng = w.rng(core::stream::kSocial);

    // Pairing. The player never picks the couples (GDD 6).
    for (std::size_t i = 0; i < w.people().size(); ++i) {
        Person& a = w.people()[i];
        if (!a.alive || a.stage != LifeStage::Adult || a.spouse.valid()) continue;
        for (std::size_t j = i + 1; j < w.people().size(); ++j) {
            Person& b = w.people()[j];
            if (!b.alive || b.stage != LifeStage::Adult || b.spouse.valid()) continue;
            if (b.settlement != a.settlement || b.sex == a.sex) continue;
            // Close kin are excluded; the rest is up to the two of them.
            if (a.mother.valid() && (a.mother == b.mother || a.mother == b.id)) continue;
            if (a.father.valid() && (a.father == b.father || a.father == b.id)) continue;
            if (b.mother == a.id || b.father == a.id) continue;
            if (!rng.chance(1, 40)) continue;
            a.spouse = b.id;
            b.spouse = a.id;
            const entt::entity aEntity = w.ecsEntity(ecs::Kind::Person, a.id.value);
            const entt::entity bEntity = w.ecsEntity(ecs::Kind::Person, b.id.value);
            if (aEntity != entt::null && w.ecs().valid(aEntity))
                w.ecs().emplace_or_replace<ecs::Spouse>(aEntity, a.spouse.value);
            if (bEntity != entt::null && w.ecs().valid(bEntity))
                w.ecs().emplace_or_replace<ecs::Spouse>(bEntity, b.spouse.value);
            break;
        }
    }

    // Conception requires two partners who can actually meet (GDD 6).
    std::vector<std::pair<PersonId, PersonId>> conceptions;
    auto peopleForConception = w.ecs().view<const ecs::Identity, const ecs::Health,
                                             const ecs::Hunger, const ecs::SettlementMember,
                                             const ecs::Spouse, ecs::Person>();
    for (const entt::entity entity : peopleForConception) {
        const auto& identity = peopleForConception.get<const ecs::Identity>(entity);
        const auto& health = peopleForConception.get<const ecs::Health>(entity);
        const auto& hunger = peopleForConception.get<const ecs::Hunger>(entity);
        const auto& settlement = peopleForConception.get<const ecs::SettlementMember>(entity);
        const auto& spouse = peopleForConception.get<const ecs::Spouse>(entity);
        const auto& p = w.person(PersonId{identity.legacyIndex});
        if (p.sex != Sex::Female || p.stage != LifeStage::Adult) continue;
        const PersonId spouseId{spouse.person};
        if (!spouseId.valid()) continue;
        const Person& partner = w.person(spouseId);
        if (!partner.alive) continue;
        if (settlement.settlement != partner.settlement.value) continue; // not in the same place
        if (core::chebyshev(p.tile, partner.tile) > 40) continue;      // cannot meet today

        // The couple weighs their own circumstances rather than breeding on every
        // physical opportunity: health, food, shelter and children already born.
        if (health.current < Fixed::ratio(3, 5) || hunger.value < Fixed::ratio(1, 2)) continue;
        std::int32_t children = 0;
        auto childrenPeople = w.ecs().view<const ecs::Identity, const ecs::Parentage, ecs::Person>();
        for (const entt::entity childEntity : childrenPeople) {
            const auto& social = childrenPeople.get<const ecs::Parentage>(childEntity);
            if (social.mother == p.id.value) ++children;
        }

        std::int32_t sleepingPlaces = 0;
        for (const auto& b : w.buildings())
            if (b.alive && b.state == BuildState::Complete)
                sleepingPlaces += w.db().building(b.def).sleepingSlots;
        std::int32_t population = 0;
        auto socialPeople = w.ecs().view<const ecs::Identity, const ecs::SettlementMember, ecs::Person>();
        for (const entt::entity entity : socialPeople) {
            const auto& social = socialPeople.get<const ecs::SettlementMember>(entity);
            if (social.settlement == settlement.settlement) ++population;
        }

        std::int64_t chanceIn = 60 + std::int64_t(children) * 60;
        if (sleepingPlaces < population) chanceIn *= 2;               // nowhere to house another child
        if (p.ageYears > 40) chanceIn *= 4;

        if (rng.chance(1, chanceIn)) conceptions.emplace_back(p.id, spouseId);
    }
    for (const auto& [mother, father] : conceptions) {
        const TilePos at = w.person(mother).tile;
        const Sex sex = rng.chance(1, 2) ? Sex::Female : Sex::Male;
        // Gestation is modelled as the delay before the child appears; the slice
        // does not yet carry a pregnancy state on the mother (DECISIONS.md D9).
        addPerson(w, w.person(mother).settlement, sex, 0, mother, father, at);
        w.report().births++;
    }
}

void WorldBuilder::createStartingCommunity(World& w, SettlementId sid, DefId ethnosId, std::int32_t size) {
    auto& rng = w.rng(core::stream::kPopulation);
    Settlement& st = w.settlement(sid);
    st.ethnos = ethnosId;
    const std::size_t firstPerson = w.people().size();

    // Several already-existing families, with adults, children and elders from the
    // first tick - not a founding event that happens after the map loads.
    std::int32_t remaining = size;
    const TilePos centre = st.hearth;

    // Smaller families, more of them: each is meant to occupy a niche, and a
    // community of four large households has four trades rather than eight.
    std::int32_t familyTrade = 0;
    while (remaining > 0) {
        const std::int32_t familySize = std::min(remaining, rng.range(2, 3));

        // The trade this family lives by. Dealt per family, not per adult: a
        // husband and wife of the same house work the same land, and their
        // children learn it from them.
        content::WorkCategory trade = content::WorkCategory::Foraging;
        HouseholdId house;
        if (ethnosId.valid()) {
            const auto& eth = w.db().ethnos(ethnosId);
            if (!eth.trades.empty()) {
                const std::string& name =
                        eth.trades[static_cast<std::size_t>(familyTrade) % eth.trades.size()];
                content::parseWorkCategory(name, trade);
            }
        }
        house = w.createHousehold(sid, trade);
        ++familyTrade;
        const std::size_t firstOfFamily = w.people().size();
        const auto nearby = core::tilesWithin(centre, 3);
        const TilePos at = nearby[rng.below(static_cast<std::uint32_t>(nearby.size()))];
        const TilePos spot = w.map().inBounds(at) && !w.map().blocked(at) ? at : centre;

        PersonId mother, father;
        std::int32_t made = 0;
        if (familySize >= 2) {
            mother = addPerson(w, sid, Sex::Female, rng.range(20, 38), PersonId{}, PersonId{}, spot);
            father = addPerson(w, sid, Sex::Male, rng.range(20, 40), PersonId{}, PersonId{}, spot);
            w.person(mother).spouse = father;
            w.person(father).spouse = mother;
            made = 2;
        } else {
            // A lone elder attached to the group.
            addPerson(w, sid, rng.chance(1, 2) ? Sex::Female : Sex::Male,
                      rng.range(w.db().sim().elderAge, w.db().sim().elderAge + 12),
                      PersonId{}, PersonId{}, spot);
            made = 1;
        }
        for (std::int32_t i = made; i < familySize; ++i) {
            addPerson(w, sid, rng.chance(1, 2) ? Sex::Female : Sex::Male,
                      rng.range(1, w.db().sim().adultAge - 1), mother, father, spot);
        }

        for (std::size_t i = firstOfFamily; i < w.people().size(); ++i) {
            w.people()[i].household = house;
            w.people()[i].profession = trade;
        }

        // Each family carries its own private craft traditions (GDD 6): losing the
        // only bearer of one has real consequences until it spreads.
        if (ethnosId.valid()) {
            const auto& eth = w.db().ethnos(ethnosId);
            std::vector<std::string> pool = eth.familyKnowledgePool;
            for (std::int32_t d = 0; d < eth.familyKnowledgeDraws && !pool.empty(); ++d) {
                const std::size_t pick = rng.below(static_cast<std::uint32_t>(pool.size()));
                const DefId id = w.db().knowledgeByName(pool[pick]);
                pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(pick));
                if (!id.valid()) continue;
                for (std::int32_t i = 0; i < familySize; ++i) {
                    const std::size_t idx = w.people().size() - static_cast<std::size_t>(familySize) + i;
                    Person& member = w.people()[idx];
                    if (member.stage != LifeStage::Child && !member.knows(id))
                        member.knownMethods.push_back(id);
                }
            }
        }
        remaining -= familySize;
    }

    if (ethnosId.valid()) {
        const auto& eth = w.db().ethnos(ethnosId);

        // A society of 2000 BC, not a band that has to discover flintknapping:
        // every adult arrives competent across the culture's trades, and the
        // specialities are dealt round so the community covers all of them.
        for (std::size_t i = firstPerson; i < w.people().size(); ++i) {
            Person& p = w.people()[i];
            grantStartingSkills(w, p, eth, p.profession);
        }

        // What they carried here. Set down by the fire, in the open, for the
        // haulers to put away: it is stock, not an inventory.
        for (const auto& [itemName, count] : eth.startingGoods) {
            const DefId item = w.db().itemByName(itemName);
            if (!item.valid() || count <= 0) continue;
            const std::int32_t limit = std::max(1, w.db().item(item).stackLimit);
            std::int32_t left = count;
            while (left > 0) {
                const std::int32_t batch = std::min(left, limit);
                TilePos at = st.hearth;
                const auto nearby = core::tilesWithin(st.hearth, 3);
                for (int tries = 0; tries < 8; ++tries) {
                    const TilePos p = nearby[rng.below(static_cast<std::uint32_t>(nearby.size()))];
                    if (w.map().inBounds(p) && !w.map().blocked(p)) { at = p; break; }
                }
                w.spawnStack(item, batch, at, sid);
                left -= batch;
            }
        }

        // The flock walks in with them. GDD 6 has the founders carrying no goods;
        // livestock is the deliberate exception, recorded in DECISIONS.md D22.
        const Zone* pasture = nearestZoneOfKind(w, sid, ZoneKind::Pasture, st.hearth);
        for (const auto& [animalName, count] : eth.startingLivestock) {
            const DefId def = w.db().animalByName(animalName);
            if (!def.valid()) continue;
            for (std::int32_t i = 0; i < count; ++i) {
                TilePos at = st.hearth;
                if (pasture && !pasture->tiles.empty())
                    at = pasture->tiles[rng.below(static_cast<std::uint32_t>(pasture->tiles.size()))];
                // A starting flock is grown and mixed, or it will never breed.
                const Sex sex = (i % 3 == 0) ? Sex::Male : Sex::Female;
                const std::int32_t age = w.db().animal(def).adultAgeDays + rng.range(0, 300);
                w.spawnAnimal(def, at, sid, sex, age);
            }
        }
    }

    // No review on the day they arrive. The families come with their trades -
    // that is what a family trade is - and reviewing them before the first
    // morning scattered every one of them out of it.
}

} // namespace sim
