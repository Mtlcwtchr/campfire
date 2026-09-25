#include "game/ai/planner.hpp"

#include <algorithm>
#include <cstdlib>
#include <iterator>

#include "game/ecs/needs/components.hpp"
#include "game/ecs/scheduler.hpp"

namespace sim::ai {

std::int64_t Planner::interval(Lod lod) {
    switch (lod) {
        case Lod::Focus: return 1;
        case Lod::Near: return 2;
        case Lod::Mid: return 8;
        case Lod::Far: return 32;
        case Lod::Dormant: return 256;
    }
    return 1;
}

void Planner::assignLod(entt::registry& registry, ecs::CellId focus,
                        std::int32_t nearRadius, std::int32_t midRadius,
                        std::int32_t farRadius, std::int64_t tick) {
    auto agents = registry.view<Controlled, const ecs::SpatialCell>();
    for (const entt::entity entity : agents) {
        auto& controlled = agents.get<Controlled>(entity);
        const auto& cell = agents.get<const ecs::SpatialCell>(entity);
        const std::int32_t distance = std::max(std::abs(cell.x - focus.x),
                                               std::abs(cell.y - focus.y));
        const Lod next = distance <= 0 ? Lod::Focus :
                         distance <= nearRadius ? Lod::Near :
                         distance <= midRadius ? Lod::Mid :
                         distance <= farRadius ? Lod::Far : Lod::Dormant;
        if (next != controlled.lod) {
            controlled.lod = next;
            controlled.nextThinkTick = std::min(controlled.nextThinkTick, tick);
        }
    }
}

void Planner::plan(const ecs::Snapshot& snapshot, PlanContext context,
                   std::vector<Intent>& output) const {
    auto agents = snapshot.view<const Controlled>();
    for (const entt::entity entity : agents) {
        const Controlled& controlled = agents.get<const Controlled>(entity);
        if (context.tick < controlled.nextThinkTick) continue;
        const std::int64_t sinceDecision = snapshot.all_of<const LastDecision>(entity)
                ? context.tick - snapshot.get<const LastDecision>(entity).tick
                : context.tick;
        const std::int64_t elapsed = std::max<std::int64_t>(
            context.elapsedTicks, std::max(interval(controlled.lod),
                                           sinceDecision));

        // Planning emits data-only intents. Needs are optional because the same
        // AI layer also controls buildings and other non-bodily entities.
        if (snapshot.registry().all_of<const ecs::Fatigue>(entity)) {
            const auto& fatigue = snapshot.registry().get<const ecs::Fatigue>(entity);
            if (fatigue.value < core::Fixed::ratio(1, 4)) {
                output.push_back(Sleep{entity, elapsed});
                continue;
            }
        }
        output.push_back(Wander{entity, elapsed});
    }
}

void Planner::planParallel(const ecs::Snapshot& snapshot, PlanContext context,
                           ecs::CellId focus, std::int32_t radius, std::size_t workers,
                           std::vector<Intent>& output) const {
    const auto schedule = ecs::BatchScheduler::buildFor<Controlled>(
            snapshot.registry(), focus, radius);
    const auto planBatch = [&](const ecs::Batch& batch, std::vector<Intent>& intents) {
        for (const entt::entity entity : batch.entities) {
            if (!snapshot.all_of<const Controlled>(entity)) continue;
            const Controlled& controlled = snapshot.get<const Controlled>(entity);
            if (context.tick < controlled.nextThinkTick) continue;
            const std::int64_t sinceDecision = snapshot.all_of<const LastDecision>(entity)
                    ? context.tick - snapshot.get<const LastDecision>(entity).tick
                    : context.tick;
            const std::int64_t elapsed = std::max<std::int64_t>(
                    context.elapsedTicks, std::max(interval(controlled.lod),
                                                   sinceDecision));
            if (snapshot.all_of<const ecs::Fatigue>(entity) &&
                snapshot.get<const ecs::Fatigue>(entity).value < core::Fixed::ratio(1, 4))
                intents.push_back(Sleep{entity, elapsed});
            else
                intents.push_back(Wander{entity, elapsed});
        }
    };

    auto local = ecs::BatchScheduler::runLocalResults<std::vector<Intent>>(
            schedule, workers, planBatch);
    for (auto& intents : local)
        output.insert(output.end(), std::make_move_iterator(intents.begin()),
                      std::make_move_iterator(intents.end()));
    ecs::BatchScheduler::runBoundary(schedule,
            [&](const ecs::Batch& batch) { planBatch(batch, output); });
    std::sort(output.begin(), output.end(), [](const Intent& a, const Intent& b) {
        const auto entityOf = [](const Intent& intent) {
            return std::visit([](const auto& value) { return value.entity; }, intent);
        };
        return entt::to_entity(entityOf(a)) < entt::to_entity(entityOf(b));
    });
}

void Planner::execute(entt::registry& registry, const std::vector<Intent>& intents,
                      std::int64_t tick) const {
    for (const Intent& intent : intents) {
        const auto* observe = std::get_if<Observe>(&intent);
        const auto* sleep = std::get_if<Sleep>(&intent);
        const auto* wander = std::get_if<Wander>(&intent);
        const entt::entity entity = observe ? observe->entity : sleep ? sleep->entity :
                                                                        wander ? wander->entity : entt::null;
        if (entity == entt::null || !registry.valid(entity) ||
            !registry.all_of<Controlled>(entity))
            continue;
        auto& controlled = registry.get<Controlled>(entity);
        controlled.nextThinkTick = tick + interval(controlled.lod);
        const Decision decision = sleep ? Decision::Sleep : wander ? Decision::Wander : Decision::None;
        const std::int64_t elapsed = observe ? observe->elapsedTicks :
            sleep ? sleep->elapsedTicks : wander ? wander->elapsedTicks : 1;
        registry.emplace_or_replace<LastDecision>(entity, decision, tick, elapsed);
        if (sleep && registry.all_of<ecs::SleepState>(entity))
            registry.get<ecs::SleepState>(entity).asleep = true;
        if (wander && registry.all_of<ecs::SleepState>(entity))
            registry.get<ecs::SleepState>(entity).asleep = false;
    }
}

} // namespace sim::ai
