#include "game/simulation/report.hpp"

#include "game/ecs/construction/components.hpp"
#include "game/ecs/entity.hpp"
#include "game/ecs/resources/components.hpp"
#include "game/ecs/life/components.hpp"

#include "game/simulation/population.hpp"

#include <map>

#include <algorithm>
#include <sstream>

#include "game/simulation/inventory.hpp"
#include "game/simulation/needs.hpp"
#include "game/work/planner.hpp"

namespace sim {
namespace {
bool personAlive(const World& w, const Person& person) {
    return w.personAlive(person.id);
}


std::string pct(std::int64_t part, std::int64_t whole) {
    if (whole <= 0) return "0%";
    return std::to_string(part * 100 / whole) + "%";
}

template <typename Map>
std::vector<std::pair<std::string, std::int32_t>> sortedDesc(const Map& m) {
    std::vector<std::pair<std::string, std::int32_t>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    return v;
}

struct ReportNeeds {
    Fixed satiety;
    Fixed hydration;
    Fixed rest;
    Fixed health;
};

ReportNeeds needsForReport(const World& w, const Person& person) {
    const NeedsSnapshot needs = needsSnapshot(w, person);
    return {needs.satiety, needs.hydration, needs.rest, needs.health};
}

} // namespace

std::string formatStatusLine(const World& w) {
    const auto date = w.now();
    const auto& rep = w.report();
    std::ostringstream o;
    o << "y" << date.year << " " << core::seasonName(date.season) << " d" << (date.dayOfSeason + 1)
      << " h" << date.hour
      << " | pop " << rep.population
      << " | temp " << core::toString(w.outdoorTemperature(), 1) << "C";

    std::int32_t working = 0, idle = 0;
    Fixed foodNutrition = core::kZero;
    auto people = w.ecs().view<const ecs::JobState>();
    for (const entt::entity entity : people) {
        if (people.get<const ecs::JobState>(entity).active) ++working;
        else ++idle;
    }
    auto stackView = w.ecs().view<const ecs::Identity, const ecs::ItemStack,
                                   const ecs::ItemStackState>();
    for (const entt::entity entity : stackView) {
        const auto& state = stackView.get<const ecs::ItemStackState>(entity);
        const StackWhere where = static_cast<StackWhere>(state.where);
        const bool available = state.alive && state.count > 0 &&
                               (where == StackWhere::Ground || where == StackWhere::InBuilding);
        if (!available) continue;
        const DefId definition{state.definition};
        const auto& def = w.db().item(definition);
        const std::int32_t count = state.count;
        if (def.category == ItemCategory::Food) foodNutrition += def.nutrition * std::int64_t(count);
    }
    o << " | working " << working << "/" << (working + idle)
      << " | food " << core::toString(foodNutrition, 0)
      << " | checksum " << std::hex << w.checksum() << std::dec;
    return o.str();
}

std::vector<std::string> checkInvariants(const World& w) {
    std::vector<std::string> problems;
    auto stackView = w.ecs().view<const ecs::Identity, const ecs::ItemStack,
                                   const ecs::ItemStackState>();

    auto people = w.ecs().view<const ecs::Identity, const ecs::Person,
                                const ecs::Health, const ecs::Hunger,
                                const ecs::Carrying, const ecs::EquippedTool,
                                const ecs::Transform>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        const auto& p = w.person(PersonId{identity.legacyIndex});
        const auto& health = people.get<const ecs::Health>(entity);
        const auto& hunger = people.get<const ecs::Hunger>(entity);
        const auto& carrying = people.get<const ecs::Carrying>(entity);
        const auto& equipped = people.get<const ecs::EquippedTool>(entity);
        const TilePos position = core::toTile(people.get<const ecs::Transform>(entity).value);
        if (!w.map().inBounds(position))
            problems.push_back(p.name + " is outside the map");
        if (hunger.value < core::kZero || hunger.value > core::kOne)
            problems.push_back(p.name + " has satiety out of range");
        if (health.current > health.maximum)
            problems.push_back(p.name + " has health above 1");
        const ItemStackId carried{carrying.stack};
        if (carried.valid()) {
            const auto& s = w.stack(carried);
            const auto* state = ecsStackState(w, carried);
            const bool alive = state ? state->alive : s.alive;
            const StackWhere where = stackWhere(w, carried);
            const PersonId holder = state ? PersonId{state->holder} : s.holder;
            if (!alive || where != StackWhere::Carried || holder != p.id)
                problems.push_back(p.name + " carries a batch that does not think it is carried");
        }
        const ItemStackId tool{equipped.stack};
        if (tool.valid()) {
            const auto& s = w.stack(tool);
            const auto* state = ecsStackState(w, tool);
            const bool alive = state ? state->alive : s.alive;
            const StackWhere where = stackWhere(w, tool);
            const PersonId holder = state ? PersonId{state->holder} : s.holder;
            const DefId definition = stackDefinition(w, tool);
            const std::int32_t count = state ? state->count : s.count;
            if (!alive || where != StackWhere::Equipped || holder != p.id) {
                static const char* kWhere[] = {"ground", "carried", "in_building", "equipped"};
                problems.push_back(p.name + " holds a tool that does not think it is held"
                                   " (batch " + std::to_string(tool.value) +
                                   " " + w.db().item(definition).name +
                                   " alive=" + std::to_string(alive) +
                                   " where=" + kWhere[static_cast<std::size_t>(where)] +
                                   " count=" + std::to_string(count) +
                                   " holder=" + (holder.valid() ? std::to_string(holder.value) : "none") +
                                   " me=" + std::to_string(p.id.value) + ")");
            }
        }
    }

    for (const entt::entity entity : stackView) {
        const auto& state = stackView.get<const ecs::ItemStackState>(entity);
        const bool alive = state.alive;
        const std::int32_t count = state.count;
        const StackWhere where = static_cast<StackWhere>(state.where);
        const TilePos tile{state.tileX, state.tileY};
        const PersonId holder{state.holder};
        const BuildingId building{state.building};
        if (!alive) continue;
        if (count <= 0) problems.push_back("live batch with no units");
        if (where == StackWhere::Ground && !w.map().inBounds(tile))
            problems.push_back("batch on the ground outside the map");
        if ((where == StackWhere::Carried || where == StackWhere::Equipped) && !holder.valid())
            problems.push_back("held batch with no holder");
        if (where == StackWhere::InBuilding && !building.valid())
            problems.push_back("stored batch with no building");
    }

    auto buildingView = w.ecs().view<const ecs::Identity, const ecs::Building,
                                      const ecs::Alive, const ecs::DeliveredMaterials>();
    for (const entt::entity entity : buildingView) {
        const auto& identity = buildingView.get<const ecs::Identity>(entity);
        const auto& b = w.building(BuildingId{identity.legacyIndex});
        if (!buildingView.get<const ecs::Alive>(entity).value) continue;
        const auto& def = w.db().building(b.def);
        const auto& delivered = buildingView.get<const ecs::DeliveredMaterials>(entity).values;
        for (std::size_t i = 0; i < delivered.size() && i < def.materials.size(); ++i)
            if (delivered[i] > def.materials[i].count)
                problems.push_back(def.name + " site holds more material than it needs");
    }
    return problems;
}

std::string formatReport(const World& w) {
    const auto& rep = w.report();
    const auto date = w.now();
    std::ostringstream o;

    o << "=== run report ===\n";
    o << "ticks           " << rep.ticks << "  (" << date.year << "y "
      << core::seasonName(date.season) << " d" << (date.dayOfSeason + 1) << ")\n";
    o << "seed            " << w.config().seed << "\n";
    o << "final checksum  " << std::hex << w.checksum() << std::dec << "\n\n";

    o << "-- population --\n";
    o << "alive           " << rep.population << "\n";
    o << "births          " << rep.births << "\n";
    o << "deaths          " << rep.deaths << "\n";
    for (const auto& [cause, n] : sortedDesc(rep.deathCauses))
        o << "  " << cause << ": " << n << "\n";

    std::int64_t personTicks = 0;
    for (auto n : rep.idleTicks) personTicks += n;
    for (auto n : rep.jobTicks) personTicks += n;

    o << "\n-- where the time went (person-ticks, " << personTicks << " total) --\n";
    for (std::size_t i = 1; i < static_cast<std::size_t>(JobKind::Count); ++i) {
        if (rep.jobTicks[i] == 0) continue;
        o << "  " << jobKindName(static_cast<JobKind>(i)) << ": " << rep.jobTicks[i]
          << " (" << pct(rep.jobTicks[i], personTicks) << "), completed "
          << rep.jobsCompleted[i] << "\n";
    }

    o << "\n-- idleness, by cause --\n";
    std::int64_t idleTotal = 0;
    for (std::size_t i = 1; i < static_cast<std::size_t>(IdleReason::Count); ++i) idleTotal += rep.idleTicks[i];
    if (idleTotal == 0) {
        o << "  none\n";
    } else {
        for (std::size_t i = 1; i < static_cast<std::size_t>(IdleReason::Count); ++i) {
            if (rep.idleTicks[i] == 0) continue;
            o << "  " << idleReasonName(static_cast<IdleReason>(i)) << ": " << rep.idleTicks[i]
              << " (" << pct(rep.idleTicks[i], personTicks) << " of all time)\n";
        }
    }

    o << "\n-- hunger and thirst --\n";
    o << "  person-ticks below the eat threshold:   " << rep.hungryPersonTicks
      << " (" << pct(rep.hungryPersonTicks, personTicks) << ")\n";
    o << "  person-ticks below a quarter satiety:   " << rep.starvingPersonTicks
      << " (" << pct(rep.starvingPersonTicks, personTicks) << ")\n";
    o << "  person-ticks below the drink threshold: " << rep.thirstyPersonTicks
      << " (" << pct(rep.thirstyPersonTicks, personTicks) << ")\n";

    o << "\n-- the sick and the dead --\n";
    {
        std::int32_t ill = 0, laidUp = 0, unburied = 0;
        auto people = w.ecs().view<const ecs::Identity, const ecs::AilmentState, const ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& ailment = people.get<const ecs::AilmentState>(entity);
            if (ailment.kind != static_cast<std::uint8_t>(Person::Ailment::None)) ++ill;
            if (ailment.severity >= core::Fixed::ratio(1, 2)) ++laidUp;
        }
        auto lifecycle = w.ecs().view<const ecs::Identity, const ecs::Person,
                                       const ecs::Alive>();
        for (const entt::entity entity : lifecycle) {
            if (lifecycle.get<const ecs::Alive>(entity).value) continue;
            const auto& p = w.person(PersonId{
                    lifecycle.get<const ecs::Identity>(entity).legacyIndex});
            if (!p.buried) ++unburied;
        }
        o << "  wounds " << rep.wounded << ", sicknesses " << rep.fellIll << ", recovered "
          << rep.recovered << ", treatments given " << rep.treatments << "\n";
        o << "  ill now " << ill << " (" << laidUp << " of them laid up)\n";
        o << "  buried " << rep.burials << ", still lying where they fell " << unburied << "\n";
    }

    o << "\n-- the land --\n";
    std::int32_t flock = 0, grown = 0;
    auto animalView = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                    const ecs::Alive, const ecs::Age,
                                    const ecs::SettlementMember>();
    for (const entt::entity entity : animalView) {
        const auto& identity = animalView.get<const ecs::Identity>(entity);
        const auto& a = w.animal(AnimalId{identity.legacyIndex});
        if (!animalView.get<const ecs::Alive>(entity).value) continue;
        ++flock;
        if (animalView.get<const ecs::Age>(entity).days >= w.db().animal(a.def).adultAgeDays) ++grown;
    }
    {
        // Households, because the shape of a settlement follows from them: a
        // roof is built per family, so twice as many families is twice as many
        // roofs whatever the population does.
        std::int32_t houses = 0;
        std::int32_t housed = 0;
        for (const auto& h : w.households()) {
            if (!h.alive) continue;
            std::int32_t members = 0;
            auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
            for (const entt::entity entity : people) {
                const auto& q = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
                if (q.household == h.id) ++members;
            }
            if (members == 0) continue;
            ++houses;
            housed += members;
        }
        {
        std::int32_t wild = 0;
        std::int32_t wolves = 0;
        std::int32_t dogs = 0;
        for (const entt::entity entity : animalView) {
            const auto& identity = animalView.get<const ecs::Identity>(entity);
            const auto& a = w.animal(AnimalId{identity.legacyIndex});
            if (!animalView.get<const ecs::Alive>(entity).value) continue;
            const auto& def = w.db().animal(a.def);
            if (animalView.get<const ecs::SettlementMember>(entity).settlement ==
                core::Handle<core::SettlementTag>::kInvalid) {
                ++wild;
                if (def.predator) ++wolves;
            }
            if (def.guardsFlock) ++dogs;
        }
        o << "  wild animals:     " << wild << " (" << wolves << " wolves), hunted "
          << rep.animalsHunted << ", taken by wolves " << rep.animalsTakenByWolves << "\n";
        std::int32_t penned = 0;
        auto animals = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                     const ecs::Alive, const ecs::SleepState>();
        for (const entt::entity entity : animals) {
            if (animals.get<const ecs::Alive>(entity).value &&
                animals.get<const ecs::SleepState>(entity).asleep) ++penned;
        }
        o << "  shepherd dogs:    " << dogs << ", tamed " << rep.animalsTamed << "\n";
        o << "  flock:            " << penned << " penned now, " << rep.animalsPenned
          << " brought in all told, " << rep.animalsStrayed << " strayed\n";
    }
    o << "  households:       " << houses << " (" << (houses ? housed / houses : 0)
          << " to a house)\n";
    }
    o << "  livestock alive:  " << flock << " (" << grown << " grown), born " << rep.animalsBorn
      << ", lost " << rep.animalsLost << "\n";
    std::int32_t sown = 0, ripe = 0, tilled = 0, watered = 0;
    for (std::int32_t y = 0; y < w.map().height(); ++y)
        for (std::int32_t x = 0; x < w.map().width(); ++x) {
            const auto& t = w.map().at({x, y});
            if (t.tilled) ++tilled;
            if (t.irrigated && (t.tilled || t.crop.valid())) ++watered;
            if (!t.crop.valid()) continue;
            ++sown;
            if (t.cropGrowth >= core::kOne) ++ripe;
    }
    std::int32_t channels = 0;
    auto irrigationBuildings = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                            const ecs::ConstructionProgress, ecs::Building>();
    for (const entt::entity entity : irrigationBuildings) {
        if (!irrigationBuildings.get<const ecs::Alive>(entity).value ||
            !irrigationBuildings.get<const ecs::ConstructionProgress>(entity).complete) continue;
        const auto& b = w.building(BuildingId{
            irrigationBuildings.get<const ecs::Identity>(entity).legacyIndex});
        if (w.db().building(b.def).irrigationRadius > 0) ++channels;
    }
    o << "  fields:           " << tilled << " broken, " << sown << " sown, " << ripe
      << " ripe, " << watered << " watered\n";
    // Where the channels run matters as much as how many: a length dug through
    // silt that floods anyway is a plot spent for nothing (D88).
    std::int32_t throughGoodSoil = 0;
    Fixed wateredGain = core::kZero;
    for (const entt::entity entity : irrigationBuildings) {
        if (!irrigationBuildings.get<const ecs::Alive>(entity).value) continue;
        const auto& b = w.building(BuildingId{
            irrigationBuildings.get<const ecs::Identity>(entity).legacyIndex});
        if (w.db().building(b.def).irrigationRadius <= 0) continue;
        if (w.map().inBounds(b.origin) && w.map().at(b.origin).fertility >= Fixed::ratio(4, 5))
            ++throughGoodSoil;
    }
    for (std::int32_t y = 0; y < w.map().height(); ++y)
        for (std::int32_t x = 0; x < w.map().width(); ++x) {
            const auto& t = w.map().at({x, y});
            if (!t.irrigated || !(t.tilled || t.crop.valid())) continue;
            wateredGain += core::max(core::kZero, Fixed::ratio(4, 5) - t.fertility);
        }
    std::int32_t fieldGoodSoil = 0, fieldPoorSoil = 0;
    for (const auto& z : w.zones()) {
        if (!z.alive || z.kind != ZoneKind::Farm) continue;
        for (const auto& t : z.tiles) {
            if (!w.map().inBounds(t)) continue;
            if (w.map().at(t).fertility >= Fixed::ratio(4, 5)) ++fieldGoodSoil;
            else ++fieldPoorSoil;
        }
    }
    o << "  field soil:       " << fieldGoodSoil << " tiles need no water, " << fieldPoorSoil
      << " would gain by it\n";
    o << "  channel dug:      " << channels << " tiles (" << throughGoodSoil
      << " through soil that needed none), fertility added "
      << core::toString(wateredGain, 0) << "\n";
    // What is still standing in the wild. Trees are felled and never grow back
    // out of their own stumps (D83); what replaces them is seed falling near the
    // ones still standing, so this is the number that says whether the community
    // is living off the country or eating it.
    {
        std::map<std::string, std::int32_t> standing;
        auto nodeView = w.ecs().view<const ecs::Identity, const ecs::ResourceNode,
                                     const ecs::Alive, const ecs::ResourceState>();
        for (const entt::entity entity : nodeView) {
            const auto& identity = nodeView.get<const ecs::Identity>(entity);
            const auto& n = w.node(ResourceNodeId{identity.legacyIndex});
            if (!nodeView.get<const ecs::Alive>(entity).value ||
                nodeView.get<const ecs::ResourceState>(entity).depleted) continue;
            standing[std::string(w.db().resourceNode(n.def).name)] += 1;
        }
        o << "  still standing:   ";
        if (standing.empty()) o << "nothing";
        bool first = true;
        for (const auto& [name, n] : standing) {
            o << (first ? "" : ", ") << name << " " << n;
            first = false;
        }
        o << "\n";
    }
o << "  known land:       " << w.exploredCount() << " tiles ("
      << rep.tilesExplored << " walked to since the start)\n";
    o << "  sown all told:    " << rep.cropsSown << ", reaped " << rep.cropsReaped
      << ", left to rot " << rep.cropsLost << "\n";

    // The areas the settlement is made of, which are what drives where anything
    // gets built (D79). Printed because a quarter that never appears is a driver
    // that never fired, and that is invisible in every other number here.
    if (!rep.deathNotes.empty()) {
        o << "\n-- how they died --\n";
        for (const auto& n : rep.deathNotes) {
            o << "  day " << n.day << " " << n.cause << ", age " << n.age
              << ": food " << core::toString(n.satiety, 2) << ", water "
              << core::toString(n.hydration, 2) << ", felt "
              << core::toString(n.feltTempC, 1) << "C (outdoors "
              << core::toString(n.outdoorTempC, 1) << "C), " << n.garments << " worn, "
              << (n.underARoof ? "under a roof" : "in the open") << ", "
              << core::toString(n.foodDaysInStore, 1) << " days of food ready to eat ("
              << core::toString(n.larderDaysInStore, 1) << " counting the granary), doing "
              << n.doing << ", nearest meal "
              << (n.foodWithin < 0 ? std::string("none anywhere")
                                   : std::to_string(n.foodWithin) + " tiles off")
              << "\n";
        }
    }

    // Who does what. The same question the trades panel answers in the client,
    // and headless the only way to see that a settlement with forty head of
    // livestock has nobody herding them.
    {
        std::map<std::string, std::int32_t> byTrade;
        auto people = w.ecs().view<const ecs::Identity, const ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& identity = people.get<const ecs::Identity>(entity);
            const auto& p = w.person(PersonId{identity.legacyIndex});
            byTrade[std::string(content::workCategoryName(p.profession))] += 1;
        }
        o << "\n-- who does what --\n";
        if (byTrade.empty()) o << "  nobody\n";
        for (const auto& [name, n] : byTrade) o << "  " << name << ": " << n << "\n";
        // And what the ledger the community reads says about each trade: the
        // work it wants doing a day, against the hands on it. This is what
        // decides who changes trade, and reading it is the only way to see why
        // forty head of livestock ended up with nobody herding them.
        if (!w.settlements().empty()) {
            const auto& st = w.settlements().front();
            o << "  trade ledger (work wanted a day / hands on it):\n";
            for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
                const auto c = static_cast<content::WorkCategory>(i);
                const Fixed need = tradeNeed(w, st, c);
                const Fixed hands = tradeHands(w, st, c);
                if (need <= core::kZero && hands <= core::kZero) continue;
                o << "    " << content::workCategoryName(c) << ": " << core::toString(need, 0)
                  << " / " << core::toString(hands, 0) << "\n";
            }
        }
    }

    o << "\n-- the quarters --\n";
    std::int32_t listed = 0;
    for (const auto& z : w.zones()) {
        if (!z.alive || z.tiles.empty()) continue;
        ++listed;
        o << "  " << z.label << " (" << zoneKindName(z.kind) << "): " << z.tiles.size()
          << " tiles\n";
    }
    if (listed == 0) o << "  none laid out\n";

    o << "\n-- rejected work --\n";
    o << "  jobs rejected for want of a tool:      " << rep.toolShortageRejections << "\n";
    o << "  jobs rejected for want of materials:   " << rep.materialShortageRejections << "\n";

    o << "\n-- produced --\n";
    for (const auto& [name, n] : sortedDesc(rep.itemsProduced)) o << "  " << name << ": " << n << "\n";

    o << "\n-- built --\n";
    if (rep.buildingsCompleted.empty()) o << "  nothing\n";
    for (const auto& [name, n] : sortedDesc(rep.buildingsCompleted)) o << "  " << name << ": " << n << "\n";

    o << "\n-- standing stores --\n";
    std::unordered_map<std::string, std::int32_t> stored;
    auto stackView = w.ecs().view<const ecs::Identity, const ecs::ItemStack,
                                   const ecs::ItemStackState>();
    for (const entt::entity entity : stackView) {
        const auto& state = stackView.get<const ecs::ItemStackState>(entity);
        const bool alive = state.alive;
        const std::int32_t count = state.count;
        if (!alive || count <= 0) continue;
        stored[w.db().item(DefId{state.definition}).name] += count;
    }
    for (const auto& [name, n] : sortedDesc(stored)) o << "  " << name << ": " << n << "\n";

    o << "\n-- skills reached --\n";
    auto people = w.ecs().view<const ecs::Identity, const ecs::Person>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        const auto& p = w.person(PersonId{identity.legacyIndex});
        std::string best;
        std::int32_t bestLevel = 0;
        for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i)
            if (p.skills[i].level > bestLevel) {
                bestLevel = p.skills[i].level;
                best = content::workCategoryName(static_cast<WorkCategory>(i));
            }
        o << "  " << p.name << " (" << p.ageYears << "y, "
          << content::workCategoryName(p.profession) << ")";
        if (bestLevel > 0) o << " best: " << best << " " << bestLevel;
        o << ", known methods " << p.knownMethods.size() << "\n";
    }
    return o.str();
}


std::string formatDecisionDump(const World& w) {
    std::ostringstream o;
    const auto date = w.now();
    o << "=== decisions at tick " << w.tickCount() << " (" << date.year << "y "
      << core::seasonName(date.season) << " d" << (date.dayOfSeason + 1) << " h" << date.hour << ") ===\n";

    for (const auto& st : w.settlements()) {
        if (!st.alive) continue;
        const work::Demand demand = work::computeDemand(w, st.id);

        std::vector<std::int32_t> have(w.db().items().size(), 0);
        auto stackView = w.ecs().view<const ecs::Identity, const ecs::ItemStack,
                                       const ecs::ItemStackState>();
        for (const entt::entity entity : stackView) {
            const auto& state = stackView.get<const ecs::ItemStackState>(entity);
            const bool alive = state.alive;
            const std::int32_t count = state.count;
            const StackWhere where = static_cast<StackWhere>(state.where);
            const bool available = alive && count > 0 &&
                                   (where == StackWhere::Ground || where == StackWhere::InBuilding);
            if (alive && count > 0 && (available || where == StackWhere::Equipped))
                have[state.definition] += count;
        }

        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < demand.item.size(); ++i)
            if (demand.item[i] > core::kZero) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return demand.item[a] != demand.item[b] ? demand.item[a] > demand.item[b] : a < b;
        });

        o << "-- demand (" << w.db().items()[0].name.substr(0, 0) << "value per unit / units wanted / held) --\n";
        for (std::size_t i : order) {
            o << "  " << w.db().items()[i].name << ": " << core::toString(demand.item[i], 2)
              << " / " << core::toString(demand.wanted[i], 0) << " / " << have[i] << "\n";
        }

        // Why the community is not making what it wants. "No materials" was the
        // largest single cause of idleness for a long time and there was no way
        // to see which recipe was short of what, so it went uninvestigated.
        // What the community has worked out for itself. A discovery that never
        // fires looks exactly like a feature that does not work.
        o << "-- discovered --\n";
        for (const auto& k : w.db().knowledge()) {
            if (k.discoveredFrom == content::WorkCategory::Count || k.discoveryChanceDenominator <= 0)
                continue;
            std::int32_t knowers = 0;
            for (PersonId id : st.members)
                if (personAlive(w, w.person(id)) && w.person(id).knows(k.id)) ++knowers;
            o << "  " << k.name << ": " << knowers << " of " << st.members.size() << "\n";
        }

        o << "-- wanted but not made --\n";
        for (const auto& r : w.db().recipes()) {
            Fixed value = core::kZero;
            for (const auto& out : r.outputs) value += demand.forItem(out.item) * std::int64_t(out.count);
            if (value <= core::kZero) continue;

            std::vector<TilePos> sites;
            if (r.workplaceDefs.empty()) {
                sites.push_back(st.hearth);
            } else {
                auto workplaces = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                                const ecs::ConstructionProgress, ecs::Building>();
                for (const entt::entity entity : workplaces) {
                    if (!workplaces.get<const ecs::Alive>(entity).value ||
                        !workplaces.get<const ecs::ConstructionProgress>(entity).complete) continue;
                    const auto& b = w.building(BuildingId{
                        workplaces.get<const ecs::Identity>(entity).legacyIndex});
                    if (b.settlement == st.id &&
                        std::find(r.workplaceDefs.begin(), r.workplaceDefs.end(), b.def) !=
                            r.workplaceDefs.end()) sites.push_back(b.origin);
                }
                if (sites.empty()) {
                    o << "  " << r.name << ": no workshop\n";
                    continue;
                }
            }

            std::string worst;
            std::int32_t worstNear = 0;
            std::int32_t worstTotal = 0;
            std::int32_t worstNeed = 0;
            std::int32_t worstDistance = -1;
            for (const auto& in : r.inputs) {
                std::int32_t total = 0;
                std::int32_t near = 0;
                std::int32_t nearest = 9999;
                auto stackView = w.ecs().view<const ecs::ItemStackState>();
                for (const entt::entity entity : stackView) {
                    const auto& state = stackView.get<const ecs::ItemStackState>(entity);
                    if (!state.alive || state.count <= 0 ||
                        (state.where != static_cast<std::uint8_t>(StackWhere::Ground) &&
                         state.where != static_cast<std::uint8_t>(StackWhere::InBuilding)) ||
                        DefId{state.definition} != in.item) continue;
                    const std::int32_t count = state.count;
                    total += count;
                    const StackWhere where = static_cast<StackWhere>(state.where);
                    const TilePos at = where == StackWhere::InBuilding
                                               ? w.building(BuildingId{state.building}).origin
                                               : TilePos{state.tileX, state.tileY};
                    for (TilePos site : sites) {
                        nearest = std::min(nearest, core::chebyshev(at, site));
                        if (core::chebyshev(at, site) <= 10) { near += count; break; }
                    }
                }
                if (near >= in.count) continue;
                if (!worst.empty() && near >= worstNear) continue;
                worst = w.db().item(in.item).name;
                worstNear = near;
                worstTotal = total;
                worstNeed = in.count;
                worstDistance = nearest;
            }
            if (worst.empty()) continue;
            o << "  " << r.name << ": needs " << worstNeed << " " << worst << ", " << worstNear
              << " within reach of " << sites.size() << " site(s), " << worstTotal
              << " in the settlement, nearest " << worstDistance << " away; site 0 at "
              << sites[0].x << "," << sites[0].y << ", hearth at " << st.hearth.x << ","
              << st.hearth.y << "\n";
        }

        o << "-- people --\n";
        for (PersonId id : st.members) {
            const Person& p = w.person(id);
            if (!personAlive(w, p)) continue;
            const ReportNeeds needs = needsForReport(w, p);
            o << "  " << p.name << " " << p.ageYears << "y "
              << (p.stage == LifeStage::Child ? "child" : p.stage == LifeStage::Elder ? "elder" : "adult")
              << " sat " << core::toString(needs.satiety, 2)
              << " hyd " << core::toString(needs.hydration, 2)
              << " rest " << core::toString(needs.rest, 2)
              << " hp " << core::toString(needs.health, 2) << " -> ";
            const entt::entity entity = w.ecsEntity(ecs::Kind::Person, p.id.value);
            const bool hasJob = entity != entt::null && w.ecs().valid(entity) &&
                                w.ecs().all_of<ecs::JobState, ecs::JobCategory,
                                               ecs::JobProgress, ecs::JobLinks>(entity) &&
                                w.ecs().get<const ecs::JobState>(entity).active;
            if (hasJob) {
                const auto& state = w.ecs().get<const ecs::JobState>(entity);
                const auto& category = w.ecs().get<const ecs::JobCategory>(entity);
                const auto& progress = w.ecs().get<const ecs::JobProgress>(entity);
                const auto& links = w.ecs().get<const ecs::JobLinks>(entity);
                o << jobKindName(static_cast<JobKind>(state.kind)) << " ("
                  << content::workCategoryName(static_cast<WorkCategory>(category.value)) << ") "
                  << core::toString(progress.done, 0) << "/" << core::toString(progress.required, 0)
                  << " at " << links.target.x << "," << links.target.y;
            } else {
                o << "idle: " << idleReasonName(p.idleReason);
            }
            o << "\n";
        }
    }
    return o.str();
}

} // namespace sim
