#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/ai/components.hpp"
#include "game/ai/intents.hpp"
#include "game/ecs/snapshot.hpp"
#include "game/ecs/spatial/components.hpp"
#include "game/ecs/spatial/index.hpp"

namespace sim::ai {

struct PlanContext {
    std::int64_t tick = 0;
    std::int64_t elapsedTicks = 1;
};

class Planner {
public:
    // Updates only scheduling metadata. The simulation can call this once per
    // tick after the camera/player focus is known; planning remains a separate
    // read-only pass over the resulting LOD buckets.
    static void assignLod(entt::registry& registry, ecs::CellId focus,
                          std::int32_t nearRadius, std::int32_t midRadius,
                          std::int32_t farRadius, std::int64_t tick);

    void plan(const ecs::Snapshot& snapshot, PlanContext context,
              std::vector<Intent>& output) const;

    void planParallel(const ecs::Snapshot& snapshot, PlanContext context,
                      ecs::CellId focus, std::int32_t radius, std::size_t workers,
                      std::vector<Intent>& output) const;

    // Commit is deliberately separate from planning: only the simulation
    // thread may update AI scheduling components.
    void execute(entt::registry& registry, const std::vector<Intent>& intents,
                 std::int64_t tick) const;

    static std::int64_t interval(Lod lod);
};

} // namespace sim::ai
