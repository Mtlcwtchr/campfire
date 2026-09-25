#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <future>
#include <thread>
#include <utility>
#include <vector>

#include <entt/entity/registry.hpp>

#include "engine/ecs/commands.hpp"
#include "engine/ecs/spatial/components.hpp"
#include "engine/ecs/spatial/index.hpp"

namespace engine::ecs {

struct Batch {
    CellId cell{};
    std::vector<entt::entity> entities;
    bool boundary = false;
};

struct BatchSchedule {
    std::vector<Batch> local;
    std::vector<Batch> boundary;
};

// Builds ownership batches from a read-only registry. The registry must not be
// mutated until all callbacks return. Ordering is stable for deterministic
// command merging, while local callbacks are free to run concurrently.
class BatchScheduler {
public:
    static BatchSchedule build(const entt::registry& registry, CellId focus, std::int32_t radius) {
        return buildFromView(registry.view<const SpatialCell>(), focus, radius);
    }

    // Restrict ownership to a component family when a system only operates on
    // one kind of entity. This keeps accidental cross-domain legacy-index
    // lookups out of callbacks and makes the scheduler express the system's
    // read set directly.
    template <typename... Required>
    static BatchSchedule buildFor(const entt::registry& registry, CellId focus,
                                  std::int32_t radius) {
        return buildFromView(registry.view<const SpatialCell, const Required...>(), focus, radius);
    }

private:
    template <typename View>
    static BatchSchedule buildFromView(View view, CellId focus, std::int32_t radius) {
        BatchSchedule result;
        for (const entt::entity entity : view) {
            const CellId cell{view.template get<const SpatialCell>(entity).x,
                              view.template get<const SpatialCell>(entity).y};
            if (std::abs(cell.x - focus.x) > radius || std::abs(cell.y - focus.y) > radius)
                continue;
            auto* batch = find(result, cell);
            if (!batch) {
                result.local.push_back(Batch{cell, {}, false});
                batch = &result.local.back();
            }
            batch->entities.push_back(entity);
        }
        for (Batch& batch : result.local) {
            std::sort(batch.entities.begin(), batch.entities.end(),
                      [](entt::entity a, entt::entity b) { return entt::to_entity(a) < entt::to_entity(b); });
            batch.boundary = cellDistance(batch.cell, focus) == radius;
        }
        // Process spatial batches in concentric rings around the focus. This
        // gives callers a natural progressive update order while retaining a
        // deterministic tie-break for cells on the same ring.
        std::stable_sort(result.local.begin(), result.local.end(), [&](const Batch& a, const Batch& b) {
            const std::int32_t ad = cellDistance(a.cell, focus);
            const std::int32_t bd = cellDistance(b.cell, focus);
            if (ad != bd) return ad < bd;
            return byCell(a, b);
        });
        for (auto it = result.local.begin(); it != result.local.end();) {
            if (!it->boundary) { ++it; continue; }
            result.boundary.push_back(std::move(*it));
            it = result.local.erase(it);
        }
        return result;
    }

public:

    template <typename Fn>
    static void runLocal(const BatchSchedule& schedule, std::size_t workers, Fn&& fn) {
        if (schedule.local.empty()) return;
        workers = std::max<std::size_t>(1, std::min(workers, schedule.local.size()));
        if (workers == 1 || schedule.local.size() < workers * 2) {
            for (const Batch& batch : schedule.local) fn(batch);
            return;
        }
        std::vector<std::future<void>> tasks;
        tasks.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            tasks.push_back(std::async(std::launch::async, [&, worker] {
                for (std::size_t i = worker; i < schedule.local.size(); i += workers)
                    fn(schedule.local[i]);
            }));
        }
        for (auto& task : tasks) task.get();
    }

    template <class Command, typename Fn>
    static std::vector<CommandBuffer<Command>> runLocalCommands(const BatchSchedule& schedule,
                                                       std::size_t workers, Fn&& fn) {
        workers = std::max<std::size_t>(1, std::min(workers, std::max<std::size_t>(1, schedule.local.size())));
        std::vector<CommandBuffer<Command>> buffers(workers);
        if (schedule.local.empty()) return buffers;
        if (workers == 1 || schedule.local.size() < workers * 2) {
            for (const Batch& batch : schedule.local) fn(batch, buffers[0]);
            return buffers;
        }
        std::vector<std::future<void>> tasks;
        tasks.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            tasks.push_back(std::async(std::launch::async, [&, worker] {
                for (std::size_t i = worker; i < schedule.local.size(); i += workers)
                    fn(schedule.local[i], buffers[worker]);
            }));
        }
        for (auto& task : tasks) task.get();
        return buffers;
    }

    template <typename Result, typename Fn>
    static std::vector<Result> runLocalResults(const BatchSchedule& schedule,
                                               std::size_t workers, Fn&& fn) {
        workers = std::max<std::size_t>(1, std::min(workers, std::max<std::size_t>(1, schedule.local.size())));
        std::vector<Result> results(workers);
        if (schedule.local.empty()) return results;
        if (workers == 1 || schedule.local.size() < workers * 2) {
            for (const Batch& batch : schedule.local) fn(batch, results[0]);
            return results;
        }
        std::vector<std::future<void>> tasks;
        tasks.reserve(workers);
        for (std::size_t worker = 0; worker < workers; ++worker) {
            tasks.push_back(std::async(std::launch::async, [&, worker] {
                for (std::size_t i = worker; i < schedule.local.size(); i += workers)
                    fn(schedule.local[i], results[worker]);
            }));
        }
        for (auto& task : tasks) task.get();
        return results;
    }

    template <class Command>
    static CommandBuffer<Command> mergeCommands(const std::vector<CommandBuffer<Command>>& buffers) {
        CommandBuffer<Command> merged;
        for (const CommandBuffer<Command>& buffer : buffers)
            for (const Command& command : buffer.commands()) merged.push(command);
        return merged;
    }

    template <typename Fn>
    static void runBoundary(const BatchSchedule& schedule, Fn&& fn) {
        // Boundary work is deliberately serialized and ordered. It is where a
        // later implementation resolves cross-cell intents and reservations.
        for (const Batch& batch : schedule.boundary) fn(batch);
    }

private:
    static std::int32_t cellDistance(CellId a, CellId b) {
        return std::max(std::abs(a.x - b.x), std::abs(a.y - b.y));
    }
    static bool byCell(const Batch& a, const Batch& b) {
        if (a.cell.x != b.cell.x) return a.cell.x < b.cell.x;
        return a.cell.y < b.cell.y;
    }
    static Batch* find(BatchSchedule& schedule, CellId cell) {
        for (Batch& batch : schedule.local)
            if (batch.cell == cell) return &batch;
        return nullptr;
    }
};

} // namespace engine::ecs
