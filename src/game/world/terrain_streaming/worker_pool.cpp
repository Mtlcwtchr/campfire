#include "game/world/terrain_streaming/worker_pool.hpp"

#include <utility>

namespace world::streaming {

TerrainWorkerPool::TerrainWorkerPool(unsigned count) {
    try {
        workers_.reserve(std::max(1u, count));
        for (unsigned i = 0; i < std::max(1u, count); ++i)
            workers_.emplace_back([this, i] { run(i); });
    } catch (...) {
        stop();
        throw;
    }
}

TerrainWorkerPool::~TerrainWorkerPool() { stop(); }

void TerrainWorkerPool::stop() {
    { std::lock_guard lock(mutex_); closing_ = true; }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
}

TerrainWorkerPool::Handle TerrainWorkerPool::add(Work work, Task task, Exclusive exclusive) {
    auto source = std::make_shared<Source>();
    source->work = std::move(work);
    source->exclusive = std::move(exclusive);
    source->task = task;
    {
        std::lock_guard lock(mutex_);
        sources_.push_back(source);
        ++revision_;
    }
    wake_.notify_all();
    return source;
}

void TerrainWorkerPool::remove(Handle& source) {
    if (!source) return;
    std::unique_lock lock(mutex_);
    source->enabled = false;
    drained_.wait(lock, [&] { return source->active == 0; });
    std::erase(sources_, source);
    source.reset();
    ++revision_;
    lock.unlock();
    wake_.notify_all(); // removing an exclusive source releases sleeping peers
}

void TerrainWorkerPool::notify() {
    { std::lock_guard lock(mutex_); ++revision_; }
    wake_.notify_all();
}

TerrainWorkerPool::Stats TerrainWorkerPool::stats() const {
    std::lock_guard lock(mutex_);
    Stats out;
    out.workers = workers_.size();
    for (const auto& source : sources_) {
        out.busy += source->active;
        out.tasks[static_cast<std::size_t>(source->task)] += source->active;
    }
    return out;
}

void TerrainWorkerPool::run(std::size_t index) {
    std::size_t next = index;
    std::vector<Handle> sources;
    for (;;) {
        std::uint64_t observed;
        {
            std::lock_guard lock(mutex_);
            if (closing_) return;
            observed = revision_;
            sources = sources_;
        }
        bool worked = false;
        // Non-preemptive priority at page/task boundaries. Round robin only
        // within a priority: idle visible sources must not hide background work.
        std::rotate(sources.begin(), sources.begin() + (sources.empty() ? 0 : next % sources.size()), sources.end());
        std::stable_sort(sources.begin(), sources.end(), [](const auto& a, const auto& b) {
            return a->task < b->task;
        });
        for (std::size_t offset = 0; offset < sources.size(); ++offset) {
            const auto& source = sources[offset];
            {
                std::lock_guard lock(mutex_);
                if (closing_) return;
                if (!source->enabled) continue;
                // Check at dispatch, not just when taking the source snapshot:
                // a newly registered preparation phase must gate old snapshots.
                // A draining source keeps its gate until its last callback ends.
                const auto exclusive = std::find_if(sources_.begin(), sources_.end(), [](const auto& s) {
                    return s->exclusive && s->exclusive();
                });
                if (exclusive != sources_.end() && *exclusive != source) continue;
                ++source->active;
            }
            worked = source->work(index);
            {
                std::lock_guard lock(mutex_);
                --source->active;
            }
            drained_.notify_all();
            if (worked) {
                ++next;
                // Completion can unblock another task (e.g. the next H level).
                notify();
                break;
            }
        }
        sources.clear();
        if (!worked) {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return closing_ || revision_ != observed; });
            if (closing_) return;
        }
    }
}

} // namespace world::streaming
