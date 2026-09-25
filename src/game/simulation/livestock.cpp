#include "game/simulation/livestock.hpp"
#include "game/ecs/life/components.hpp"
#include "game/ecs/spatial/components.hpp"

#include <algorithm>
#include <thread>
#include <unordered_map>

#include "game/simulation/zones.hpp"

namespace sim {

bool Animal::adult(const content::ContentDb& db) const {
    return ageDays >= db.animal(def).adultAgeDays;
}

JobKind nextAnimalAction(const World& w, const Animal& a) {
    const entt::entity entity = w.ecsEntity(ecs::Kind::Animal, a.id.value);
    if (entity == entt::null || !w.ecs().valid(entity)) return JobKind::None;
    if (w.ecs().all_of<ecs::Alive>(entity) && !w.ecs().get<const ecs::Alive>(entity).value)
        return JobKind::None;
    const auto& def = w.db().animal(a.def);
    if (!w.ecs().all_of<ecs::Age, ecs::AnimalTimers>(entity)) return JobKind::None;
    const std::int32_t ageDays = w.ecs().get<const ecs::Age>(entity).days;
    if (ageDays < def.adultAgeDays) return JobKind::None;

    const auto& timers = w.ecs().get<const ecs::AnimalTimers>(entity);
    const std::int64_t nextShear = timers.nextShearTick;
    const std::int64_t nextMilk = timers.nextMilkTick;
    if (def.shearIntervalDays > 0 && !def.shearYields.empty() && w.tickCount() >= nextShear)
        return JobKind::Shear;
    if (def.milkIntervalDays > 0 && !def.milkYields.empty() && a.sex == Sex::Female &&
        w.tickCount() >= nextMilk)
        return JobKind::Milk;
    return JobKind::None;
}

void tickLivestock(World& w) {
    const auto& time = w.db().time();

    // --- movement, every tick ------------------------------------------
    // Animals drift around their pasture. Cheap, but it is what makes a flock
    // read as a flock rather than a row of markers.
    const Fixed step = Fixed::ratio(1, 40);
    auto& rng = w.rng(core::stream::kWildlife);
    const ecs::Snapshot snapshot = w.ecs().snapshot();
    ecs::CommandBuffer movementCommands;
    auto processMovement = [&](const ecs::Batch& batch, ecs::CommandBuffer& commands) {
        auto movingAnimals = snapshot.view<const ecs::Identity, const ecs::Animal,
                                           const ecs::Transform, const ecs::SleepState,
                                           const ecs::GrazingTarget, const ecs::Alive>();
        for (const entt::entity entity : batch.entities) {
            if (!movingAnimals.contains(entity)) continue;
        const auto& transform = movingAnimals.get<const ecs::Transform>(entity);
        const auto& sleep = movingAnimals.get<const ecs::SleepState>(entity);
        const auto& target = movingAnimals.get<const ecs::GrazingTarget>(entity);
        const auto& alive = movingAnimals.get<const ecs::Alive>(entity);
        if (!alive.value || sleep.asleep) continue;
        const WorldPos goal = core::tileCentre(target.tile);
        const Fixed dx = goal.x - transform.value.x;
        const Fixed dy = goal.y - transform.value.y;
        const Fixed dist = core::hypot(dx, dy);
        WorldPos next = transform.value;
        TilePos nextTile = core::toTile(transform.value);
        if (dist <= step) {
            next = goal;
            nextTile = target.tile;
        } else {
            next.x += dx * step / dist;
            next.y += dy * step / dist;
            nextTile = core::toTile(next);
        }
            commands.push(ecs::Command{ecs::SetTransform{entity, next, nextTile}});
        }
    };
    const ecs::CellId focus = ecs::cellForTile(w.localCell(), ecs::kSpatialCellExtent);
    const std::int32_t radius = std::max<std::int32_t>(1,
        std::max(w.map().width(), w.map().height()) / ecs::kSpatialCellExtent + 1);
    // Keep worker fan-out bounded: desktop machines may report hundreds of
    // logical CPUs, but spawning that many futures every tick costs more than
    // the animal batch itself.
    const std::size_t workers = std::min<std::size_t>(8,
        std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    const auto buffers = w.ecs().forEachLocalCommands<ecs::Animal>(
        focus, radius, workers, processMovement);
    for (const auto& buffer : buffers)
        for (const auto& command : buffer.commands()) movementCommands.push(command);
    w.ecs().forEachBoundary<ecs::Animal>(focus, radius, [&](const ecs::Batch& batch) {
        processMovement(batch, movementCommands);
    });
    for (const ecs::Command& command : movementCommands.commands()) {
        const auto* move = std::get_if<ecs::SetTransform>(&command);
        if (!move || !w.ecs().valid(move->entity)) continue;
        w.ecs().get<ecs::Transform>(move->entity).value = move->value;
        const auto cell = ecs::cellForTile(move->tile, ecs::kSpatialCellExtent);
        w.ecs().emplace_or_replace<ecs::SpatialCell>(move->entity, cell.x, cell.y);
        const auto& identity = w.ecs().get<ecs::Identity>(move->entity);
        Animal& animal = w.animal(AnimalId{identity.legacyIndex});
        animal.pos = move->value;
        animal.tile = move->tile;
    }

    if (w.tickCount() % time.ticksPerDay() != 0) return;

    // Needs are shared ECS components for every animal species. Their daily
    // transition is independent per entity, so calculate it from the snapshot
    // in spatial batches and commit the component writes in stable order.
    const ecs::CellId dailyFocus = ecs::cellForTile(w.localCell(), ecs::kSpatialCellExtent);
    const std::int32_t dailyRadius = std::max<std::int32_t>(1,
        std::max(w.map().width(), w.map().height()) / ecs::kSpatialCellExtent + 1);
    const std::size_t dailyWorkers = std::min<std::size_t>(8,
        std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    const auto updateAnimalNeeds = [&](const ecs::Batch& batch, ecs::CommandBuffer& commands) {
        auto view = snapshot.view<const ecs::Animal, const ecs::Hunger, const ecs::Thirst,
                                   const ecs::Fatigue, const ecs::SleepState>();
        for (const entt::entity entity : batch.entities) {
            if (!view.contains(entity)) continue;
            const auto& currentHunger = view.get<const ecs::Hunger>(entity);
            const auto& currentThirst = view.get<const ecs::Thirst>(entity);
            const auto& currentFatigue = view.get<const ecs::Fatigue>(entity);
            const auto& currentSleep = view.get<const ecs::SleepState>(entity);
            Fixed thirst = core::max(core::kZero, currentThirst.value - Fixed::ratio(1, 20));
            Fixed hunger = currentHunger.value;
            Fixed fatigue = currentFatigue.value;
            if (currentSleep.asleep) {
                hunger = core::saturate(hunger + Fixed::ratio(1, 12));
                fatigue = core::saturate(fatigue + Fixed::ratio(1, 8));
            } else {
                hunger = core::max(core::kZero, hunger - Fixed::ratio(1, 16));
                fatigue = core::max(core::kZero, fatigue - Fixed::ratio(1, 20));
            }
            commands.push(ecs::Command{ecs::SetAnimalNeedsComponents{
                    entity, hunger, thirst, fatigue, currentSleep.asleep}});
        }
    };
    const auto animalBuffers = w.ecs().forEachLocalCommands<ecs::Animal>(
            dailyFocus, dailyRadius, dailyWorkers, updateAnimalNeeds);
    ecs::CommandBuffer animalNeedCommands;
    for (const auto& buffer : animalBuffers)
        for (const auto& command : buffer.commands()) animalNeedCommands.push(command);
    w.ecs().forEachBoundary<ecs::Animal>(dailyFocus, dailyRadius,
        [&](const ecs::Batch& batch) { updateAnimalNeeds(batch, animalNeedCommands); });
    for (const ecs::Command& command : animalNeedCommands.commands()) {
        const auto* update = std::get_if<ecs::SetAnimalNeedsComponents>(&command);
        if (!update || !w.ecs().valid(update->entity)) continue;
        if (!w.ecs().all_of<ecs::Hunger, ecs::Thirst, ecs::Fatigue, ecs::SleepState>(update->entity)) continue;
        w.ecs().get<ecs::Hunger>(update->entity).value = update->hunger;
        w.ecs().get<ecs::Thirst>(update->entity).value = update->thirst;
        w.ecs().get<ecs::Fatigue>(update->entity).value = update->fatigue;
        w.ecs().get<ecs::SleepState>(update->entity).asleep = update->asleep;
    }

    // The legacy condition/penned fields remain derived metadata for now; all
    // hot needs below are read back from the committed ECS components.
    auto animalNeeds = w.ecs().view<const ecs::Identity, ecs::Animal, ecs::Health,
                                     ecs::Hunger, ecs::Thirst, ecs::Fatigue,
                                     ecs::SleepState, ecs::Age>();
    for (const entt::entity entity : animalNeeds) {
        const auto& identity = animalNeeds.get<const ecs::Identity>(entity);
        Animal& animal = w.animal(AnimalId{identity.legacyIndex});
        auto& sleep = animalNeeds.get<ecs::SleepState>(entity);
        const auto& age = animalNeeds.get<const ecs::Age>(entity);
        animal.ageDays = age.days;
        if (sleep.asleep) {
            animal.penned = true;
        } else {
            animal.penned = false;
        }
        // Health is authoritative in ECS; keep the old aggregate condition only
        // as a compatibility mirror for breeding/reporting code not migrated yet.
        animal.condition = animalNeeds.get<ecs::Health>(entity).current;
    }

    // --- once a day: grazing, ageing, breeding, death ---------------------
    // How many of each wild kind the country is carrying. Deer breed up to what
    // the land holds and no further; without this a map ends up shoulder to
    // shoulder with deer and the hunters never have to think about it.
    std::vector<std::int32_t> wildHead(w.db().animals().size(), 0);
    auto wildAnimals = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                     const ecs::SettlementMember>();
    for (const entt::entity entity : wildAnimals) {
        const Animal& a = w.animal(AnimalId{wildAnimals.get<const ecs::Identity>(entity).legacyIndex});
        if (wildAnimals.get<const ecs::SettlementMember>(entity).settlement ==
            core::Handle<core::SettlementTag>::kInvalid)
            wildHead[a.def.value] += 1;
    }

    // Out again at daybreak. Penning is for the night; a flock kept in all day
    // eats nothing and starves in its own byre.
    const bool morning = w.now().isDaylight && w.now().hour < kPenningHour;
    if (morning) {
        auto penned = w.ecs().view<const ecs::Identity, ecs::Animal, ecs::SleepState>();
        for (const entt::entity entity : penned) {
            const auto& id = penned.get<const ecs::Identity>(entity);
            Animal& a = w.animal(AnimalId{id.legacyIndex});
            if (!w.ecs().get<const ecs::Alive>(entity).value) continue;
            penned.get<ecs::SleepState>(entity).asleep = false;
            a.penned = false;
        }
    }

    struct AnimalObservation {
        AnimalId id;
        DefId def;
        SettlementId owner;
        TilePos tile;
        Sex sex;
        std::int32_t ageDays;
        Fixed condition;
        bool penned;
        bool alive;
    };
    std::vector<AnimalObservation> perception;
    auto perceptionView = w.ecs().view<const ecs::Identity, const ecs::Animal,
                                      const ecs::Alive, const ecs::SleepState,
                                      const ecs::Age, const ecs::Health,
                                      const ecs::Transform, const ecs::SettlementMember>();
    perception.reserve(perceptionView.size_hint());
    for (const entt::entity entity : perceptionView) {
        const Animal& a = w.animal(AnimalId{perceptionView.get<const ecs::Identity>(entity).legacyIndex});
        const bool alive = perceptionView.get<const ecs::Alive>(entity).value;
        const bool penned = perceptionView.get<const ecs::SleepState>(entity).asleep;
        const auto& age = perceptionView.get<const ecs::Age>(entity);
        const auto& health = perceptionView.get<const ecs::Health>(entity);
        const auto& transform = perceptionView.get<const ecs::Transform>(entity);
        const auto& settlement = perceptionView.get<const ecs::SettlementMember>(entity);
        perception.push_back({a.id, a.def, SettlementId{settlement.settlement},
                              core::toTile(transform.value), a.sex,
                              age.days, health.current, penned, alive});
    }

    struct AnimalBatchResult {
        ecs::CommandBuffer animalStateCommands;
        ecs::CommandBuffer grassCommands;
        std::vector<ecs::AnimalBirthIntent> births;
        std::vector<ecs::AnimalDeathIntent> deaths;
        std::int32_t strayed = 0;
        std::int32_t lost = 0;
    };
    const auto grassKey = [](TilePos tile) {
        return (std::uint64_t(static_cast<std::uint32_t>(tile.x)) << 32) |
               static_cast<std::uint32_t>(tile.y);
    };
    auto processAnimalBatch = [&](const ecs::Batch& batch, AnimalBatchResult& result) {
        std::unordered_map<std::uint64_t, std::int32_t> localGrass;
        for (const entt::entity entity : batch.entities) {
            const auto& identity = snapshot.get<const ecs::Identity>(entity);
            if (identity.kind != ecs::Kind::Animal) continue;
            Animal a = static_cast<const World&>(w).animal(AnimalId{identity.legacyIndex});
            const auto& alive = snapshot.get<const ecs::Alive>(entity);
            if (!alive.value) continue;
            a.alive = alive.value;
            a.tile = core::toTile(snapshot.get<const ecs::Transform>(entity).value);
            a.ageDays = snapshot.get<const ecs::Age>(entity).days;
            Fixed condition = snapshot.get<const ecs::Health>(entity).current;
            a.condition = condition;
            core::Rng animalRng(core::splitmix64(w.config().seed ^
                                                 static_cast<std::uint64_t>(w.tickCount()) ^
                                                 static_cast<std::uint64_t>(a.id.value)),
                                core::stream::kWildlife);
        const auto& def = w.db().animal(a.def);
        const auto& age = snapshot.get<const ecs::Age>(entity);
        const auto& sleep = snapshot.get<const ecs::SleepState>(entity);
        const auto& timers = snapshot.get<const ecs::AnimalTimers>(entity);
        auto nextTimers = timers;
        const std::int32_t nextAge = age.days + 1;
        if (sleep.asleep) {
            // Fed from the byre's own fodder, near enough: it holds condition
            // rather than gaining, and nothing eats it.
            result.animalStateCommands.push(ecs::Command{ecs::SetAgeComponent{entity, nextAge}});
            result.animalStateCommands.push(ecs::Command{ecs::SetAnimalTimersComponent{entity,
                    nextTimers.nextShearTick, nextTimers.nextMilkTick, nextTimers.nextBreedTick}});
            continue;
        }

        // Pick somewhere new to stand, inside the pasture if there is one.
        // Drift to a neighbouring tile of the pasture rather than striking out
        // across it. An animal that picks a target twelve tiles away spends days
        // walking over bare ground and starves on the way.
        const Zone* pasture = nearestZoneOfKind(w, a.owner, ZoneKind::Pasture, a.tile);
        TilePos wanted = a.tile;
        const auto& offsets = core::neighbourOffsets(a.tile);
        // Where the grazing is. An animal takes the best of the tiles it can
        // reach in a step, which is enough to make a flock drift off ground it
        // has eaten out without anybody driving it.
        std::int32_t bestGrass = w.map().inBounds(a.tile) ? w.map().at(a.tile).grass : 0;
        for (const auto& d : offsets) {
            const TilePos candidate{a.tile.x + d.x, a.tile.y + d.y};
            if (!w.map().inBounds(candidate) || w.map().blocked(candidate)) continue;
            if (w.map().at(candidate).building.valid()) continue;
            const std::int32_t grass = w.map().at(candidate).grass;
            if (grass > bestGrass) { bestGrass = grass; wanted = candidate; }
        }
        if (wanted == a.tile) {
            const TilePos step = offsets[animalRng.below(core::kNeighbourCount)];
            const TilePos next{a.tile.x + step.x, a.tile.y + step.y};
            if (w.map().inBounds(next) && !w.map().blocked(next)) wanted = next;
        }

        // An unwatched flock drifts. With a dog or a shepherd near it the sheep
        // keep together and stay on the grazing; with nobody near, they wander,
        // and a flock that wanders is a flock the wolves find.
        bool watched = sleep.asleep || !a.owner.valid();
        if (!watched) {
            for (const auto& dog : perception)
                if (dog.alive && dog.owner == a.owner && w.db().animal(dog.def).guardsFlock &&
                    core::tileDistance(dog.tile, a.tile) <= kDogGuardReach)
                    watched = true;
            if (!watched) {
                auto people = snapshot.view<const ecs::Identity, const ecs::Transform,
                                             const ecs::SettlementMember, const ecs::Profession>();
                for (const entt::entity entity : people) {
                    const auto& settlement = people.get<const ecs::SettlementMember>(entity);
                    const auto& profession = people.get<const ecs::Profession>(entity);
                    const TilePos shepherdTile = core::toTile(
                            people.get<const ecs::Transform>(entity).value);
                    if (settlement.settlement == a.owner.value &&
                        profession.value == static_cast<std::uint8_t>(content::WorkCategory::Herding) &&
                        core::tileDistance(shepherdTile, a.tile) <= kDogGuardReach)
                        watched = true;
                }
            }
        }

        if (pasture && !pasture->tiles.empty() && watched) {
            const bool onPasture = insideZoneOfKind(w, a.owner, ZoneKind::Pasture, a.tile);
            const bool nextOnPasture = insideZoneOfKind(w, a.owner, ZoneKind::Pasture, wanted);
            if (onPasture && !nextOnPasture) {
                wanted = a.tile;                 // do not wander off good grass
            } else if (!onPasture) {
                // Head back: one tile at a time, toward the middle of the
                // pasture. Looking for the nearest pasture tile meant scanning
                // every tile of it for every animal every day - a hundred and
                // fifty head against eight hundred tiles - and the middle is
                // just as good a thing to walk toward.
                const TilePos best = pasture->centre();
                std::int32_t closest = core::tileDistance(a.tile, best);
                for (const auto& d : core::neighbourOffsets(a.tile)) {
                    const TilePos candidate{a.tile.x + d.x, a.tile.y + d.y};
                    if (!w.map().inBounds(candidate) || w.map().blocked(candidate)) continue;
                    const std::int32_t distance = core::tileDistance(candidate, best);
                    if (distance < closest) { closest = distance; wanted = candidate; }
                }
            }
        }
        // Nobody watching: it may simply walk off, and it will not be brought
        // back until somebody notices.
        if (!watched && animalRng.chance(1, kStrayOneDayIn)) {
            const TilePos step = offsets[animalRng.below(core::kNeighbourCount)];
            const TilePos away{a.tile.x + step.x * 2, a.tile.y + step.y * 2};
            if (w.map().inBounds(away) && !w.map().blocked(away)) wanted = away;
            result.strayed++;
        }
        result.animalStateCommands.push(
                ecs::Command{ecs::SetGrazingTargetComponent{entity, wanted}});

        // Condition follows what is actually underfoot. Winter grass and bare rock
        // both starve a flock; the player sees it before the animals die.
        // What is actually underfoot, and eating it. Grazing is what moves a
        // flock: the ground it stands on goes bare, so tomorrow the grass is
        // somewhere else and so is the flock.
        bool onGrass = false;
        if (w.map().inBounds(a.tile)) {
            const Tile& here = w.map().at(a.tile);
            const std::uint64_t key = grassKey(a.tile);
            auto [it, inserted] = localGrass.emplace(key, here.grass);
            (void)inserted;
            onGrass = it->second >= kGrassBare;
            if (it->second > 0) {
                const std::int32_t consumed = std::min<std::int32_t>(it->second, kGrassEatenPerDay);
                it->second -= consumed;
                result.grassCommands.push(ecs::Command{ecs::AdjustGrassIntent{a.tile, -consumed}});
            }
        }
        const bool winter = w.now().season == core::Season::Winter;
        if (onGrass && !winter) condition = core::saturate(condition + Fixed::ratio(1, 8));
        else if (onGrass) condition = core::saturate(condition - Fixed::ratio(1, 60));
        else condition = core::max(core::kZero, condition - Fixed::ratio(1, 25));
        const entt::entity animalEntity = w.ecsEntity(ecs::Kind::Animal, a.id.value);
        if (animalEntity != entt::null)
            result.animalStateCommands.push(ecs::Command{ecs::SetHealthComponent{animalEntity, condition}});

        if (condition <= core::kZero || nextAge >= def.maxAgeDays) {
            // The carcass is not modelled yet; a pawn whose job pointed at this
            // animal drops it on its next tick.
            result.animalStateCommands.push(ecs::Command{ecs::SetAgeComponent{entity, nextAge}});
            result.animalStateCommands.push(ecs::Command{ecs::SetAnimalTimersComponent{entity,
                    nextTimers.nextShearTick, nextTimers.nextMilkTick, nextTimers.nextBreedTick}});
            result.animalStateCommands.push(ecs::Command{ecs::SetAliveComponent{entity, false}});
            result.deaths.push_back({a.id});
            result.lost++;
            continue;
        }

        // Breeding needs a grown, well-fed pair of the right sexes in one place.
        if (def.breedIntervalDays > 0 && a.sex == Sex::Female &&
            nextAge >= def.adultAgeDays &&
            w.tickCount() >= nextTimers.nextBreedTick && condition > Fixed::ratio(3, 5)) {
            bool mate = false;
            for (const auto& other : perception) {
                if (!other.alive || other.sex != Sex::Male || other.def != a.def) continue;
                if (other.ageDays < w.db().animal(other.def).adultAgeDays) continue;
                if (core::tileDistance(other.tile, a.tile) > 8) continue;
                mate = true;
                break;
            }
            const bool room = def.wildCarryingCapacity <= 0 || a.owner.valid() ||
                              wildHead[a.def.value] < def.wildCarryingCapacity;
            if (mate && room && animalRng.chance(1, 3)) {
                result.births.push_back({a.def, a.tile, a.owner});
                // Twins are common enough in sheep to decide whether a flock
                // outgrows its losses or merely replaces them.
                if (def.twinOneIn > 0 && animalRng.chance(1, def.twinOneIn))
                    result.births.push_back({a.def, a.tile, a.owner});
                nextTimers.nextBreedTick = w.tickCount() + std::int64_t(def.breedIntervalDays) * time.ticksPerDay();
            }
        }
        result.animalStateCommands.push(ecs::Command{ecs::SetAgeComponent{entity, nextAge}});
        result.animalStateCommands.push(ecs::Command{ecs::SetAnimalTimersComponent{entity,
                nextTimers.nextShearTick, nextTimers.nextMilkTick, nextTimers.nextBreedTick}});
        }
    };

    const std::size_t localWorkers = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    const auto localResults = w.ecs().forEachLocalResults<AnimalBatchResult, ecs::Animal>(
        focus, radius, localWorkers, processAnimalBatch);
    AnimalBatchResult boundaryResult;
    w.ecs().forEachBoundary<ecs::Animal>(focus, radius, [&](const ecs::Batch& batch) {
        processAnimalBatch(batch, boundaryResult);
    });

    ecs::CommandBuffer animalStateCommands;
    ecs::CommandBuffer grassCommands;
    std::vector<ecs::AnimalBirthIntent> births;
    std::vector<ecs::AnimalDeathIntent> deaths;
    const auto mergeResult = [&](const AnimalBatchResult& result) {
        for (const auto& command : result.animalStateCommands.commands()) animalStateCommands.push(command);
        for (const auto& command : result.grassCommands.commands()) grassCommands.push(command);
        births.insert(births.end(), result.births.begin(), result.births.end());
        deaths.insert(deaths.end(), result.deaths.begin(), result.deaths.end());
        w.report().animalsStrayed += result.strayed;
        w.report().animalsLost += result.lost;
    };
    for (const auto& result : localResults) mergeResult(result);
    mergeResult(boundaryResult);


    // Health writes are committed after all local grazing decisions. This is
    // the same ordering used by future parallel batches and prevents a worker
    // from observing another worker's partial mutation.
    for (const ecs::Command& command : animalStateCommands.commands()) {
        if (const auto* health = std::get_if<ecs::SetHealthComponent>(&command)) {
            if (w.ecs().valid(health->entity)) {
                w.ecs().get<ecs::Health>(health->entity).current = health->current;
                const auto& identity = w.ecs().get<const ecs::Identity>(health->entity);
                w.animal(AnimalId{identity.legacyIndex}).condition = health->current;
            }
            continue;
        }
        if (const auto* age = std::get_if<ecs::SetAgeComponent>(&command)) {
            if (w.ecs().valid(age->entity)) {
                w.ecs().get<ecs::Age>(age->entity).days = age->days;
                const auto& identity = w.ecs().get<const ecs::Identity>(age->entity);
                w.animal(AnimalId{identity.legacyIndex}).ageDays = age->days;
            }
            continue;
        }
        if (const auto* alive = std::get_if<ecs::SetAliveComponent>(&command)) {
            if (w.ecs().valid(alive->entity)) {
                w.ecs().get<ecs::Alive>(alive->entity).value = alive->value;
                const auto& identity = w.ecs().get<const ecs::Identity>(alive->entity);
                w.animal(AnimalId{identity.legacyIndex}).alive = alive->value;
            }
            continue;
        }
        if (const auto* target = std::get_if<ecs::SetGrazingTargetComponent>(&command)) {
            if (w.ecs().valid(target->entity)) {
                w.ecs().get<ecs::GrazingTarget>(target->entity).tile = target->tile;
                const auto& identity = w.ecs().get<const ecs::Identity>(target->entity);
                w.animal(AnimalId{identity.legacyIndex}).grazeTarget = target->tile;
            }
            continue;
        }
        if (const auto* timers = std::get_if<ecs::SetAnimalTimersComponent>(&command)) {
            if (w.ecs().valid(timers->entity)) {
                auto& value = w.ecs().get<ecs::AnimalTimers>(timers->entity);
                value.nextShearTick = timers->nextShearTick;
                value.nextMilkTick = timers->nextMilkTick;
                value.nextBreedTick = timers->nextBreedTick;
                const auto& identity = w.ecs().get<const ecs::Identity>(timers->entity);
                Animal& animal = w.animal(AnimalId{identity.legacyIndex});
                animal.nextShearTick = value.nextShearTick;
                animal.nextMilkTick = value.nextMilkTick;
                animal.nextBreedTick = value.nextBreedTick;
            }
        }
    }

    // Aggregate grass intents in the commit phase. The local cache above keeps
    // decisions equivalent to the old sequential pass while workers never
    // write the shared map directly.
    for (const ecs::Command& command : grassCommands.commands()) {
        const auto* grass = std::get_if<ecs::AdjustGrassIntent>(&command);
        if (!grass || !w.map().inBounds(grass->tile)) continue;
        Tile& tile = w.map().at(grass->tile);
        const std::int32_t value = std::max(0, static_cast<std::int32_t>(tile.grass) + grass->delta);
        tile.grass = static_cast<std::uint8_t>(value);
    }

    for (const auto& birth : births) {
        // A calf belongs to whoever its mother belonged to, which for a wild one
        // is nobody. Handing every birth to settlement zero made the deer of the
        // whole map somebody's livestock.
        w.spawnAnimal(birth.definition, birth.at, birth.owner,
                      rng.chance(1, 2) ? Sex::Female : Sex::Male, 0);
        w.report().animalsBorn++;
    }

    // --- what the wolves take ---------------------------------------------
    // A wolf has to eat. It takes what it can catch nearby: deer and boar first,
    // and a sheep nobody is watching. A dog with the flock keeps them off.
    for (const auto& hunter : perception) {
        if (!hunter.alive || !w.db().animal(hunter.def).predator) continue;
        if (hunter.ageDays < w.db().animal(hunter.def).adultAgeDays) continue;
        if (!rng.chance(1, kWolfKillsOneDayIn)) continue;

        AnimalId prey;
        std::int32_t bestDist = kWolfReach + 1;
        for (const auto& q : perception) {
            if (!q.alive || q.id == hunter.id) continue;
            if (w.db().animal(q.def).predator) continue;
            const std::int32_t d = core::tileDistance(hunter.tile, q.tile);
            if (d > kWolfReach || d >= bestDist) continue;

            // Guarded stock is not worth the risk: a dog within reach of it, or
            // a shepherd standing there.
            if (q.penned) continue;
            if (q.owner.valid()) {
                bool guarded = false;
                for (const auto& dog : perception)
                    if (dog.alive && dog.owner == q.owner &&
                        w.db().animal(dog.def).guardsFlock &&
                        core::tileDistance(dog.tile, q.tile) <= kDogGuardReach)
                        guarded = true;
                auto people = w.ecs().view<const ecs::Identity, const ecs::Transform,
                                             const ecs::SettlementMember, const ecs::Profession>();
                for (const entt::entity entity : people) {
                    const auto& settlement = people.get<const ecs::SettlementMember>(entity);
                    const auto& profession = people.get<const ecs::Profession>(entity);
                    const TilePos shepherdTile = core::toTile(
                            people.get<const ecs::Transform>(entity).value);
                    if (settlement.settlement == q.owner.value &&
                        profession.value == static_cast<std::uint8_t>(content::WorkCategory::Herding) &&
                        core::tileDistance(shepherdTile, q.tile) <= kDogGuardReach)
                        guarded = true;
                }
                if (guarded) continue;
            }
            bestDist = d;
            prey = q.id;
        }
        if (!prey.valid()) continue;
        Animal& taken = w.animal(prey);
        w.setAnimalAlive(prey, false);
        deaths.push_back({taken.id});
        w.report().animalsLost++;
        w.report().animalsTakenByWolves++;
    }

    // Apply all death intents (grazing and predation) together after the
    // complete decision pass. This is the lifecycle boundary for parallel
    // batches; reservation releases cannot race local animal evaluation.
    for (const auto& death : deaths) {
        if (!death.animal.valid()) continue;
        w.setAnimalAlive(AnimalId{death.animal.value}, false);
        w.release(World::animalKey(AnimalId{death.animal.value}));
    }

    std::int32_t alive = 0;
    auto allAnimals = w.ecs().view<const ecs::Identity, const ecs::Animal>();
    for (const entt::entity entity : allAnimals) {
        if (w.ecs().all_of<ecs::Alive>(entity) &&
            w.ecs().get<const ecs::Alive>(entity).value) ++alive;
    }
    w.report().livestock = alive;
}

} // namespace sim
