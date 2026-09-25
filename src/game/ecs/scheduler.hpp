#pragma once
// Compatibility vocabulary: batching and deterministic command merging belong
// to the engine.
#include "engine/ecs/scheduler.hpp"
#include "game/ecs/commands.hpp"
#include "game/ecs/spatial/components.hpp"
#include "game/ecs/spatial/index.hpp"

namespace sim::ecs {
using engine::ecs::Batch;
using engine::ecs::BatchSchedule;

// The engine's scheduler with this game's command vocabulary already bound, so
// a caller writes what it means rather than repeating the variant every time.
struct BatchScheduler : engine::ecs::BatchScheduler {
    template <typename Fn>
    static std::vector<CommandBuffer> runLocalCommands(const BatchSchedule& schedule,
                                                       std::size_t workers, Fn&& fn) {
        return engine::ecs::BatchScheduler::runLocalCommands<Command>(
                schedule, workers, std::forward<Fn>(fn));
    }
    static CommandBuffer mergeCommands(const std::vector<CommandBuffer>& buffers) {
        return engine::ecs::BatchScheduler::mergeCommands<Command>(buffers);
    }
};
} // namespace sim::ecs
