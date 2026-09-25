#pragma once

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace world::streaming {

// One budget for terrain streaming, permanent preparation and mesh/inspection
// work together, not half the machine per queue. Zero/one reported CPU needs
// one worker to make progress; otherwise leave at least half for gameplay.
constexpr unsigned terrainWorkerBudget(unsigned hardwareThreads) {
    return std::max(1u, hardwareThreads / 2);
}

class TerrainWorkerPool {
    struct Source;
public:
    using Handle = std::shared_ptr<Source>;
    enum class Task : std::size_t { Visible, Preload, Preparation, Inspection, Count };
    struct Stats {
        std::size_t workers = 0, busy = 0;
        std::array<std::size_t, static_cast<std::size_t>(Task::Count)> tasks{};
    };
    // Try one task and return whether work was done. Never wait for more work
    // or for another task in this pool. The stable index owns per-worker caches.
    using Work = std::function<bool(std::size_t)>;
    // Nonblocking/noexcept predicate (typically an atomic load). While true,
    // only this source can start tasks, even when its work() temporarily has
    // nothing to claim. Notify after releasing the phase outside a callback.
    using Exclusive = std::function<bool()>;

    explicit TerrainWorkerPool(unsigned count = terrainWorkerBudget(std::thread::hardware_concurrency()));
    ~TerrainWorkerPool();
    TerrainWorkerPool(const TerrainWorkerPool&) = delete;
    TerrainWorkerPool& operator=(const TerrainWorkerPool&) = delete;

    Handle add(Work work, Task task = Task::Inspection, Exclusive exclusive = {});
    // Owner-thread teardown only: remove a source, then wait for its active
    // callbacks before destroying anything they borrow. Other sources continue.
    void remove(Handle& source);
    void notify();
    std::size_t size() const { return workers_.size(); }
    Stats stats() const;

private:
    struct Source {
        Work work;
        Exclusive exclusive;
        Task task = Task::Inspection;
        std::size_t active = 0;
        bool enabled = true;
    };
    void run(std::size_t index);
    void stop();

    mutable std::mutex mutex_;
    std::condition_variable wake_, drained_;
    std::vector<Handle> sources_;
    std::vector<std::thread> workers_;
    std::uint64_t revision_ = 0;
    bool closing_ = false;
};

} // namespace world::streaming
