#include "game/simulation/needs.hpp"
#include "game/ecs/construction/components.hpp"
#include "game/ecs/entity.hpp"
#include "game/ecs/life/components.hpp"

#include <algorithm>
#include <thread>

#include "game/simulation/inventory.hpp"

namespace sim {
namespace {
bool personAlive(const World& w, const Person& person) {
    return w.personAlive(person.id);
}

bool buildingComplete(const World& w, BuildingId id) {
    return w.buildingComplete(id);
}
}

NeedsSnapshot needsSnapshot(const World& w, const Person& person) {
    NeedsSnapshot result{person.satiety, person.hydration, person.rest, person.health,
                         person.asleep, person.ailment, person.ailmentSeverity};
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, person.id.value);
    if (entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::Health, ecs::Hunger, ecs::Thirst, ecs::Fatigue,
                       ecs::SleepState, ecs::AilmentState>(entity)) {
        result.health = w.ecs().get<const ecs::Health>(entity).current;
        result.satiety = w.ecs().get<const ecs::Hunger>(entity).value;
        result.hydration = w.ecs().get<const ecs::Thirst>(entity).value;
        result.rest = w.ecs().get<const ecs::Fatigue>(entity).value;
        result.asleep = w.ecs().get<const ecs::SleepState>(entity).asleep;
        const auto& ailment = w.ecs().get<const ecs::AilmentState>(entity);
        result.ailment = static_cast<Person::Ailment>(ailment.kind);
        result.ailmentSeverity = ailment.severity;
    }
    return result;
}

bool canWork(const World& w, const Person& person) {
    if (!personAlive(w, person) || person.ageYears < 5) return false;
    const NeedsSnapshot needs = needsSnapshot(w, person);
    if (needs.asleep) return false;
    return needs.ailmentSeverity < core::Fixed::ratio(1, 2);
}

Fixed dietVariety(const Person& p) {
    std::int32_t groups = 0;
    std::int32_t total = 0;
    for (auto n : p.recentFoodGroups) {
        if (n > 0) ++groups;
        total += n;
    }
    if (total == 0) return core::kOne;   // no data yet is not a deficiency
    // Three or more groups in the window counts as varied; one group is the floor.
    if (groups >= 3) return core::kOne;
    if (groups == 2) return Fixed::ratio(3, 4);
    return Fixed::ratio(2, 5);
}

Fixed workCapacity(const World& w, const Person& p) {
    const NeedsSnapshot needs = needsSnapshot(w, p);
    Fixed healthValue = needs.health;
    Fixed satiety = needs.satiety;
    Fixed hydration = needs.hydration;
    Fixed rest = needs.rest;
    bool asleep = needs.asleep;
    Fixed temperatureOffset = p.bodyTempOffset;
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, p.id.value);
    if (entity != entt::null && w.ecs().valid(entity) && w.ecs().all_of<ecs::BodyTemperature>(entity)) {
        temperatureOffset = w.ecs().get<const ecs::BodyTemperature>(entity).offsetC;
    }
    if (!personAlive(w, p) || asleep) return core::kZero;
    Fixed cap = healthValue;

    // Below the threshold, capacity falls off linearly to zero at empty.
    const auto& sim = w.db().sim();
    if (satiety < sim.hungerEatThreshold)
        cap = cap * core::max(Fixed::ratio(1, 5), satiety / sim.hungerEatThreshold);
    if (hydration < sim.thirstDrinkThreshold)
        cap = cap * core::max(Fixed::ratio(1, 5), hydration / sim.thirstDrinkThreshold);
    if (rest < sim.fatigueSleepThreshold)
        cap = cap * core::max(Fixed::ratio(1, 4), rest / sim.fatigueSleepThreshold);

    // Cold hands work slowly.
    const Fixed cold = core::abs(temperatureOffset);
    if (cold > Fixed::fromInt(5)) cap = cap * core::max(Fixed::ratio(1, 3), core::kOne - cold / Fixed::fromInt(30));

    switch (p.stage) {
        case LifeStage::Child: cap = cap * Fixed::ratio(2, 5); break;
        case LifeStage::Elder: cap = cap * Fixed::ratio(3, 5); break;
        case LifeStage::Adult: break;
    }
    return core::max(core::kZero, cap);
}

Fixed moveCapacity(const World& w, const Person& p) {
    Fixed cap = workCapacity(w, p);
    // Carrying slows a pawn in proportion to the load, which is what makes
    // distance to a store a real economic cost (GDD 7, Stronghold logistics).
    const ItemStackId carried = carriedStack(w, p.id);
    if (carried.valid()) {
        if (stackAlive(w, carried)) {
            const Fixed massKg = w.db().item(stackDefinition(w, carried)).massPerUnit *
                                 std::int64_t(stackCount(w, carried));
            cap = cap * core::max(Fixed::ratio(1, 3), core::kOne - massKg * w.db().sim().carryPenaltyPerKg);
        }
    }
    return core::max(Fixed::ratio(1, 10), cap);
}

std::int32_t mealPortions(const World& w, const Person& p, ItemStackId stackId) {
    if (!stackAlive(w, stackId) || stackCount(w, stackId) <= 0) return 0;
    const auto& def = w.db().item(stackDefinition(w, stackId));
    if (def.nutrition <= core::kZero) return 0;

    const Fixed perPortion = def.nutrition * stackFreshness(w, stackId);
    std::int32_t portions = 0;
    Fixed satiety = needsSnapshot(w, p).satiety;
    while (portions < stackCount(w, stackId) && satiety < Fixed::ratio(19, 20)) {
        satiety = satiety + perPortion;
        ++portions;
        if (perPortion <= core::kZero) break;
    }
    return std::max(1, portions);
}

void eatFrom(World& w, Person& p, ItemStackId stackId) {
    if (!stackAlive(w, stackId) || stackCount(w, stackId) <= 0) return;
    const auto& def = w.db().item(stackDefinition(w, stackId));
    if (def.nutrition <= core::kZero) return;

    // A person sits down to a meal rather than swallowing one berry and walking
    // back for the next: they eat portions from the batch until close to full, or
    // until the batch runs out.
    const Fixed perPortion = def.nutrition * stackFreshness(w, stackId);
    const std::int32_t portions = mealPortions(w, p, stackId);

    // ECS owns hot needs. Keep the legacy mirror updated for callers that still
    // hold Person&, but never let a meal disappear when tickNeeds reads ECS on
    // the following tick.
    Fixed* satiety = &p.satiety;
    Fixed* health = &p.health;
    const entt::entity entity = w.ecsEntity(ecs::Kind::Person, p.id.value);
    if (entity != entt::null && w.ecs().valid(entity) &&
        w.ecs().all_of<ecs::Hunger, ecs::Health>(entity)) {
        satiety = &w.ecs().get<ecs::Hunger>(entity).value;
        health = &w.ecs().get<ecs::Health>(entity).current;
    }
    *satiety = core::saturate(*satiety + perPortion * std::int64_t(portions));
    p.satiety = *satiety;
    for (auto g : def.foodGroups) p.recentFoodGroups[static_cast<std::size_t>(g)] += 1;

    // Eating something known to be risky raw is a real health cost, not flavour.
    if (def.rawUnsafe) {
        auto& rng = w.rng(core::stream::kHealth);
        for (std::int32_t i = 0; i < portions; ++i)
            if (rng.chance(1, 3)) *health = core::max(Fixed::ratio(1, 10), *health - Fixed::ratio(1, 8));
    }
    p.health = *health;
    consume(w, stackId, portions);
}

void tickNeeds(World& w) {
    std::int32_t ailing = 0;
    std::int32_t unburied = 0;
    // The dead are counted here, before the pass below drops anybody who is not
    // alive on its first line. Put inside that pass the number was always zero,
    // and with it zero the planner never offered a burial to anybody (D99).
    auto bodies = w.ecs().view<const ecs::Identity, const ecs::Person,
                               const ecs::Alive>();
    for (const entt::entity entity : bodies) {
        const auto& alive = bodies.get<const ecs::Alive>(entity);
        if (alive.value) continue;
        const auto& body = w.person(PersonId{bodies.get<const ecs::Identity>(entity).legacyIndex});
        if (!body.buried) ++unburied;
    }
    w.setUnburied(unburied);
    const auto& sim = w.db().sim();
    const auto& time = w.db().time();
    const Fixed perTick = core::kOne / std::int64_t(time.ticksPerHour);

    // Hunger, thirst and fatigue are per-entity state transitions. Their input
    // is immutable for this phase, so workers calculate them from one snapshot
    // and publish only component commands. Cross-cell interactions (if added
    // later) remain in the boundary callback and never race the registry.
    const ecs::Snapshot snapshot = w.ecs().snapshot();
    const ecs::CellId focus = ecs::cellForTile(w.localCell(), ecs::kSpatialCellExtent);
    const std::int32_t radius = std::max<std::int32_t>(1,
        std::max(w.map().width(), w.map().height()) / ecs::kSpatialCellExtent + 1);
    const std::size_t workers = std::min<std::size_t>(8,
        std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    const auto decay = [&](const ecs::Batch& batch, ecs::CommandBuffer& commands) {
        auto view = snapshot.view<const ecs::Person, const ecs::Hunger, const ecs::Thirst,
                                   const ecs::Fatigue, const ecs::SleepState,
                                   const ecs::JobState, const ecs::JobLinks>();
        for (const entt::entity entity : batch.entities) {
            if (!view.contains(entity)) continue;
            const auto& hunger = view.get<const ecs::Hunger>(entity);
            const auto& thirst = view.get<const ecs::Thirst>(entity);
            const auto& fatigue = view.get<const ecs::Fatigue>(entity);
            const auto& sleep = view.get<const ecs::SleepState>(entity);
            const auto& job = view.get<const ecs::JobState>(entity);
            const auto& links = view.get<const ecs::JobLinks>(entity);
            const Fixed nextHunger = core::max(core::kZero,
                                               hunger.value - sim.hungerPerHour * perTick);
            const Fixed nextThirst = core::max(core::kZero,
                                               thirst.value - sim.thirstPerHour * perTick);
            Fixed nextFatigue = fatigue.value;
            if (sleep.asleep) {
                Fixed recovery = sim.sleepRecoveryPerHour * perTick;
                const BuildingId homeId{links.building};
                if (job.kind == static_cast<std::uint8_t>(JobKind::Sleep) && homeId.valid() &&
                    homeId.value < w.buildings().size() && buildingComplete(w, homeId)) {
                    const auto& home = w.building(homeId);
                    recovery = recovery * (core::kOne + w.db().building(home.def).comfortBonus);
                }
                nextFatigue = core::saturate(nextFatigue + recovery);
            } else {
                Fixed drain = sim.fatiguePerHour;
                if (job.active && job.phase == static_cast<std::uint8_t>(JobPhase::Working))
                    drain = drain * Fixed::ratio(5, 4);
                nextFatigue = core::max(core::kZero, nextFatigue - drain * perTick);
            }
            commands.push(ecs::Command{ecs::SetNeedsComponents{entity, nextHunger,
                                                                nextThirst, nextFatigue}});
        }
    };
    const auto localBuffers = w.ecs().forEachLocalCommands<ecs::Person>(
            focus, radius, workers, decay);
    ecs::CommandBuffer decayCommands;
    for (const auto& buffer : localBuffers)
        for (const auto& command : buffer.commands()) decayCommands.push(command);
    w.ecs().forEachBoundary<ecs::Person>(focus, radius,
                                          [&](const ecs::Batch& batch) { decay(batch, decayCommands); });
    for (const ecs::Command& command : decayCommands.commands()) {
        const auto* update = std::get_if<ecs::SetNeedsComponents>(&command);
        if (!update || !w.ecs().valid(update->entity)) continue;
        if (!w.ecs().all_of<ecs::Hunger, ecs::Thirst, ecs::Fatigue>(update->entity)) continue;
        w.ecs().get<ecs::Hunger>(update->entity).value = update->hunger;
        w.ecs().get<ecs::Thirst>(update->entity).value = update->thirst;
        w.ecs().get<ecs::Fatigue>(update->entity).value = update->fatigue;
    }

    auto& health = w.rng(core::stream::kHealth);

    auto people = w.ecs().view<const ecs::Identity, ecs::Person, ecs::Health,
                                ecs::Hunger, ecs::Thirst, ecs::Fatigue,
                                ecs::SleepState, ecs::BodyTemperature,
                                ecs::AilmentState, const ecs::Transform,
                                const ecs::JobState,
                                const ecs::WornItems>();
    for (const entt::entity entity : people) {
        const auto& identity = people.get<const ecs::Identity>(entity);
        auto& bodyHealth = people.get<ecs::Health>(entity);
        auto& bodyHunger = people.get<ecs::Hunger>(entity);
        auto& bodyThirst = people.get<ecs::Thirst>(entity);
        auto& bodyFatigue = people.get<ecs::Fatigue>(entity);
        auto& bodySleep = people.get<ecs::SleepState>(entity);
        auto& bodyTemperature = people.get<ecs::BodyTemperature>(entity);
        auto& bodyAilment = people.get<ecs::AilmentState>(entity);
        const auto& transform = people.get<const ecs::Transform>(entity);
        const auto& jobState = people.get<const ecs::JobState>(entity);
        const auto& worn = people.get<const ecs::WornItems>(entity);
        const TilePos tilePos = core::toTile(transform.value);
        Person& p = w.person(PersonId{identity.legacyIndex});
        // ECS owns the independent hot state. The legacy object is a temporary
        // compatibility facade for systems that have not migrated yet.
        Fixed& satiety = bodyHunger.value;
        Fixed& hydration = bodyThirst.value;
        Fixed& rest = bodyFatigue.value;
        Fixed& healthValue = bodyHealth.current;
        bool& asleep = bodySleep.asleep;
        p.satiety = satiety;
        p.hydration = hydration;
        p.rest = rest;
        p.health = healthValue;
        p.asleep = asleep;
        p.bodyTempOffset = bodyTemperature.offsetC;
        p.ailment = static_cast<Person::Ailment>(bodyAilment.kind);
        p.ailmentSeverity = bodyAilment.severity;
        p.ailmentTended = bodyAilment.tended;
        p.ailmentSinceTick = bodyAilment.sinceTick;

        // Basic need decay was committed above from the immutable snapshot.
        // This pass now handles only dependent effects (shelter, temperature,
        // illness and death), so no component is written twice in one phase.

        // Temperature: shelter and clothing offset the outdoor swing.
        Fixed felt = w.outdoorTemperature();
        const auto& tile = w.map().at(tilePos);
        if (tile.building.valid()) {
            const auto& b = w.building(tile.building);
            if (buildingComplete(w, tile.building)) {
                const auto& bd = w.db().building(b.def);
                if (bd.sheltered) felt = felt + Fixed::fromInt(5);
                felt = felt + bd.warmthBonus;
            }
        }
        // Nearby hearths warm whoever stands close to them.
        auto hearths = w.ecs().view<const ecs::Identity, const ecs::Alive,
                                    const ecs::ConstructionProgress, ecs::Building>();
        for (const entt::entity entity : hearths) {
            if (!hearths.get<const ecs::Alive>(entity).value ||
                !hearths.get<const ecs::ConstructionProgress>(entity).complete) continue;
            const auto& b = w.building(BuildingId{
                hearths.get<const ecs::Identity>(entity).legacyIndex});
            if (!w.db().building(b.def).providesHeat) continue;
            if (core::chebyshev(tilePos, b.origin) <= 3) { felt = felt + Fixed::fromInt(8); break; }
        }
        // Clothing is a real item that a person is wearing, not a stat.
        for (const std::uint32_t wornId : worn.stacks) {
            const ItemStackId id{wornId};
            if (!id.valid() || !stackAlive(w, id)) continue;
            felt = felt + w.db().item(stackDefinition(w, id)).insulation *
                   (ecsStackState(w, id) ? Fixed::fromRaw(ecsStackState(w, id)->qualityRaw)
                                         : w.stack(id).quality);
        }
        bodyTemperature.offsetC = felt - sim.comfortableTempC;
        p.bodyTempOffset = bodyTemperature.offsetC;

        // Consequences, not mood penalties.
        const bool starving = satiety <= core::kZero;
        const bool parched = hydration <= core::kZero;
        const bool freezing = felt < sim.hypothermiaTempC;

        if (starving || parched || freezing) {
            // Rates are per game hour: an empty stomach kills in about a week, no
            // water in about two days, exposure somewhere between.
            //
            // Cold is graded by how far below the line it is, five degrees under
            // being the full rate. A flat rate for anything under the line meant
            // one degree of chill did as much harm as a blizzard, so a winter
            // spent a degree or two below cost as much health as starving and
            // nobody could recover until spring.
            Fixed hunger = core::kZero, thirst = core::kZero, chill = core::kZero;
            if (starving) hunger = Fixed::ratio(1, 200);
            if (parched) thirst = Fixed::ratio(1, 60);
            if (freezing)
                chill = Fixed::ratio(1, 300) *
                        core::min(Fixed::fromInt(2), (sim.hypothermiaTempC - felt) / 5);
            healthValue = healthValue - (hunger + thirst + chill) * perTick;
            // Remembered so the death is reported by what killed the person, not
            // by whatever happened to be true in the last moment of it: people
            // frozen through a winter were being counted as starved, because
            // satiety dips to nothing between one meal and the next.
            p.harmFromHunger += hunger * perTick;
            p.harmFromThirst += thirst * perTick;
            p.harmFromCold += chill * perTick;
        } else if (satiety > Fixed::ratio(1, 2) && hydration > Fixed::ratio(1, 2)) {
            healthValue = core::saturate(healthValue + Fixed::ratio(1, 5000));
        }

        // Poor sanitation raises illness risk rather than subtracting happiness.
        if (tile.pollution > Fixed::ratio(1, 2) && health.chance(1, 20000))
            healthValue = core::max(Fixed::ratio(1, 20), healthValue - Fixed::ratio(1, 6));

        // --- what is wrong with them ------------------------------------
        // An ailment is its own axis (D98): health is what hunger and cold wear
        // away, an ailment is a thing that happened and then runs its course.
        // Untended it deepens and takes health with it; tended it closes.
        {
            const auto& sim = w.db().sim();
            // Catching something. Three ways in, each with its own cause, so a
            // sickness can be traced back to what the community did rather than
            // to a die roll: filth underfoot, cold and exhaustion, and injury at
            // work (which is raised where the work is done, not here).
            if (bodyAilment.kind == static_cast<std::uint8_t>(Person::Ailment::None)) {
                const Tile& here = w.map().at(tilePos);
                const bool filthy = here.pollution > Fixed::ratio(2, 5);
                const bool worn = rest < Fixed::ratio(1, 5) || satiety < Fixed::ratio(1, 5);
                const bool cold = bodyTemperature.offsetC < Fixed::fromInt(-3);
                if (filthy && health.chance(1, 26000)) {
                    bodyAilment.kind = static_cast<std::uint8_t>(Person::Ailment::Flux);
                    bodyAilment.severity = Fixed::ratio(2, 5);
                    bodyAilment.sinceTick = w.tickCount();
                    ++w.report().fellIll;
                } else if ((cold || worn) && health.chance(1, 40000)) {
                    bodyAilment.kind = static_cast<std::uint8_t>(Person::Ailment::Fever);
                    bodyAilment.severity = Fixed::ratio(7, 20);
                    bodyAilment.sinceTick = w.tickCount();
                    ++w.report().fellIll;
                }
            } else {
                // Its course. The body does most of the work itself - people got
                // better long before anybody knew why - but it is slow, and the
                // deeper it is the slower it goes. What a healer did shows up as
                // ailmentTended, and that is what turns "slowly better" into
                // "better".
                const Fixed heal = Fixed::ratio(1, 9000) + bodyAilment.tended / 700;
                const Fixed worsen = Fixed::ratio(1, 9000);
                // Being hungry, cold or worn out while ill is what kills people.
                const bool struggling = satiety < Fixed::ratio(2, 5) ||
                                        rest < Fixed::ratio(1, 4) ||
                                        bodyTemperature.offsetC < Fixed::fromInt(-2);
                bodyAilment.severity = bodyAilment.severity + (struggling ? worsen : core::kZero) - heal;
                bodyAilment.tended = core::max(core::kZero, bodyAilment.tended - Fixed::ratio(1, 2400));
                if (bodyAilment.severity <= core::kZero) {
                    bodyAilment.kind = static_cast<std::uint8_t>(Person::Ailment::None);
                    bodyAilment.severity = core::kZero;
                    bodyAilment.tended = core::kZero;
                    p.tendedBy = PersonId{};
                    ++w.report().recovered;
                } else {
                    ++ailing;
                    bodyAilment.severity = core::min(core::kOne, bodyAilment.severity);
                    // What it costs to be ill: health, in proportion to how bad
                    // it is. A scratch is nothing; a deep wound left alone kills
                    // in a fortnight.
                    const Fixed toll = bodyAilment.severity * bodyAilment.severity / 900;
                    healthValue = healthValue - toll;
                    p.harmFromIllness = p.harmFromIllness + toll;
                }
            }
            (void)sim;
        }

        if (healthValue <= core::kZero) {
            w.setPersonAlive(p.id, false);
            p.deathTick = w.tickCount();
            // Whichever cause took the most health over this life.
            if (p.harmFromIllness >= p.harmFromCold && p.harmFromIllness >= p.harmFromHunger &&
                p.harmFromIllness >= p.harmFromThirst && p.harmFromIllness > core::kZero)
                p.deathCause = bodyAilment.kind == static_cast<std::uint8_t>(Person::Ailment::Wound)
                                       ? "a wound" : "sickness";
            else if (p.harmFromCold >= p.harmFromHunger && p.harmFromCold >= p.harmFromThirst &&
                p.harmFromCold > core::kZero)
                p.deathCause = "exposure";
            else if (p.harmFromThirst >= p.harmFromHunger && p.harmFromThirst > core::kZero)
                p.deathCause = "thirst";
            else if (p.harmFromHunger > core::kZero)
                p.deathCause = "starvation";
            else
                p.deathCause = "illness";
            w.report().deaths++;
            w.report().deathCauses[p.deathCause]++;
            if (w.report().deathNotes.size() < 40) {
                RunReport::DeathNote note;
                note.cause = p.deathCause;
                note.day = static_cast<std::int32_t>(w.tickCount() / w.db().time().ticksPerDay());
                note.age = p.ageYears;
                note.satiety = satiety;
                note.hydration = hydration;
                note.feltTempC = felt;
                note.outdoorTempC = w.outdoorTemperature();
                for (const std::uint32_t wornId : worn.stacks) {
                    const ItemStackId id{wornId};
                    if (id.valid() && stackAlive(w, id)) ++note.garments;
                }
                note.underARoof = buildingComplete(w, tile.building);
                note.doing = std::string(jobKindName(static_cast<JobKind>(jobState.kind)));
                if (const ItemStackId meal = findNearestEdible(w, p); meal.valid())
                    note.foodWithin = core::chebyshev(tilePos, stackTile(w, meal));
                if (p.settlement.valid()) {
                    note.foodDaysInStore = w.settlement(p.settlement).foodDays;
                    note.larderDaysInStore = w.settlement(p.settlement).larderDays;
                }
                w.report().deathNotes.push_back(std::move(note));
            }
            w.releaseAllBy(p.id);
            p.ailment = static_cast<Person::Ailment>(bodyAilment.kind);
            p.ailmentSeverity = bodyAilment.severity;
            p.ailmentTended = bodyAilment.tended;
            p.ailmentSinceTick = bodyAilment.sinceTick;
            p.satiety = satiety;
            p.hydration = hydration;
            p.rest = rest;
            p.health = healthValue;
            p.asleep = asleep;
            continue;
        }

        auto& rep = w.report();
        if (satiety < sim.hungerEatThreshold) rep.hungryPersonTicks++;
        if (satiety < Fixed::ratio(1, 4)) rep.starvingPersonTicks++;
        if (hydration < sim.thirstDrinkThreshold) rep.thirstyPersonTicks++;

        p.ailment = static_cast<Person::Ailment>(bodyAilment.kind);
        p.ailmentSeverity = bodyAilment.severity;
        p.ailmentTended = bodyAilment.tended;
        p.ailmentSinceTick = bodyAilment.sinceTick;
        p.satiety = satiety;
        p.hydration = hydration;
        p.rest = rest;
        p.health = healthValue;
        p.asleep = asleep;
    }

    // Age out the diet window once a day so variety reflects recent eating.
    if (w.tickCount() % time.ticksPerDay() == 0) {
        auto alivePeople = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : alivePeople) {
            Person& p = w.person(PersonId{alivePeople.get<const ecs::Identity>(entity).legacyIndex});
            for (auto& n : p.recentFoodGroups) n = std::max(0, n - 1);
        }
    }
    w.setAiling(ailing);
}

} // namespace sim
