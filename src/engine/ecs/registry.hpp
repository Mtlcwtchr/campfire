#pragma once

#include <utility>
#include <vector>

#include "engine/ecs/commands.hpp"
#include "engine/ecs/scheduler.hpp"
#include "engine/ecs/snapshot.hpp"

namespace engine::ecs {

using Registry = entt::registry;

// The world a game's systems read and write, with the command vocabulary left
// to the game. Everything here - views, spatial batching, deterministic
// command merging - is mechanism; none of it knows what a command means.
template <class Command>
class EcsWorld {
public:
    Registry& writeRegistry() { return registry_; }
    const Registry& readRegistry() const { return registry_; }
    Snapshot snapshot() const { return Snapshot{registry_}; }

    template <typename... Components>
    decltype(auto) view() { return registry_.view<Components...>(); }
    template <typename... Components>
    decltype(auto) view() const {
        return const_cast<Registry&>(registry_).view<Components...>();
    }
    template <typename... Components>
    decltype(auto) get(entt::entity entity) { return registry_.get<Components...>(entity); }
    template <typename... Components>
    decltype(auto) get(entt::entity entity) const {
        return const_cast<Registry&>(registry_).get<Components...>(entity);
    }
    template <typename... Components>
    bool all_of(entt::entity entity) const { return registry_.all_of<Components...>(entity); }
    bool valid(entt::entity entity) const { return registry_.valid(entity); }
    entt::entity create() { return registry_.create(); }
    void destroy(entt::entity entity) { registry_.destroy(entity); }
    template <typename Component, typename... Args>
    decltype(auto) emplace(entt::entity entity, Args&&... args) {
        return registry_.emplace<Component>(entity, std::forward<Args>(args)...);
    }
    template <typename Component, typename... Args>
    decltype(auto) emplace_or_replace(entt::entity entity, Args&&... args) {
        return registry_.emplace_or_replace<Component>(entity, std::forward<Args>(args)...);
    }
    template <typename... Components>
    void remove(entt::entity entity) { registry_.remove<Components...>(entity); }

    using Commands = CommandBuffer<Command>;

    BatchSchedule schedule(CellId focus, std::int32_t radius) const {
        return BatchScheduler::build(registry_, focus, radius);
    }

    template <typename... Required>
    BatchSchedule scheduleFor(CellId focus, std::int32_t radius) const {
        return BatchScheduler::buildFor<Required...>(registry_, focus, radius);
    }

    template <typename... Required, typename Fn>
    std::vector<Commands> forEachLocalCommands(CellId focus, std::int32_t radius,
                                                     std::size_t workers, Fn&& fn) const {
        return BatchScheduler::template runLocalCommands<Command>(
                scheduleFor<Required...>(focus, radius), workers, std::forward<Fn>(fn));
    }

    template <typename... Required, typename Fn>
    void forEachLocal(CellId focus, std::int32_t radius, std::size_t workers, Fn&& fn) const {
        BatchScheduler::runLocal(scheduleFor<Required...>(focus, radius), workers,
                                 std::forward<Fn>(fn));
    }

    template <typename... Required, typename Fn>
    void forEachBoundary(CellId focus, std::int32_t radius, Fn&& fn) const {
        BatchScheduler::runBoundary(scheduleFor<Required...>(focus, radius),
                                    std::forward<Fn>(fn));
    }

    template <typename Result, typename... Required, typename Fn>
    std::vector<Result> forEachLocalResults(CellId focus, std::int32_t radius,
                                            std::size_t workers, Fn&& fn) const {
        return BatchScheduler::runLocalResults<Result>(
                scheduleFor<Required...>(focus, radius), workers, std::forward<Fn>(fn));
    }

    // Applying commands is intentionally a single-threaded operation. A
    // future scheduler can sort/merge buffers before calling this boundary.
    template <typename Apply>
    void apply(const Commands& buffer, Apply&& applyCommand) {
        for (const Command& command : buffer.commands()) applyCommand(registry_, command);
    }

    template <typename Apply>
    void apply(const std::vector<Commands>& buffers, Apply&& applyCommand) {
        // Buffers are already indexed by worker. Applying them in that stable
        // order is the deterministic commit boundary; futures never commit
        // directly into the registry.
        for (const Commands& buffer : buffers)
            apply(buffer, applyCommand);
    }

private:
    Registry registry_;
};

} // namespace engine::ecs
