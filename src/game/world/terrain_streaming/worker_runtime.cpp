#include "game/world/terrain_streaming/worker_runtime.hpp"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace world::streaming {

struct TerrainWorkerRuntime::Job {
    TerrainStreamRequest request;
    std::uint64_t sequence = 0;
    std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
    std::vector<TerrainStreamConsumer> consumers;
};

bool TerrainCancellation::cancelled() const noexcept {
    const auto cancelled = cancelled_.lock();
    const auto stopping = stopping_.lock();
    return !cancelled || !stopping || cancelled->load(std::memory_order_relaxed) ||
           stopping->load(std::memory_order_relaxed);
}

TerrainWorkerRuntime::TerrainWorkerRuntime(Config config, BuildFunction build)
    : config_(config), build_(std::move(build)), stopping_(std::make_shared<std::atomic_bool>(false)) {
    config_.workerCount = std::max<std::size_t>(1, config_.workerCount);
    config_.maxQueued = std::max<std::size_t>(1, config_.maxQueued);
    config_.maxReady = std::max<std::size_t>(1, config_.maxReady);

    workers_.reserve(config_.workerCount);
    for (std::size_t index = 0; index < config_.workerCount; ++index) {
        workers_.emplace_back([this] { work(); });
    }
}

TerrainWorkerRuntime::~TerrainWorkerRuntime() {
    stopping_->store(true, std::memory_order_relaxed);
    {
        std::lock_guard lock(mutex_);
        closing_ = true;
        for (const auto& [_, job] : jobs_) job->cancelled->store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
}

TerrainStreamTicket TerrainWorkerRuntime::submit(TerrainStreamRequest request) {
    std::lock_guard lock(mutex_);
    if (closing_) return {};

    const TerrainStreamTicket ticket{nextTicket_++};
    if (auto ready = std::find_if(ready_.begin(), ready_.end(), [&](const auto& completion) {
            return completion.result.key == request.key;
        }); ready != ready_.end()) {
        ready->consumers.push_back({ticket, request.viewEpoch});
        return ticket;
    }
    if (auto it = jobs_.find(request.key); it != jobs_.end() &&
                                      !it->second->cancelled->load(std::memory_order_relaxed)) {
        it->second->consumers.push_back({ticket, request.viewEpoch});
        it->second->request.priority = std::max(it->second->request.priority, request.priority);
        tickets_.emplace(ticket.value, it->second);
        std::stable_sort(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) {
            if (a->request.priority != b->request.priority)
                return a->request.priority > b->request.priority;
            return a->sequence < b->sequence;
        });
        return ticket;
    }

    discardCancelledFrontLocked();
    if (queue_.size() >= config_.maxQueued) {
        ++rejected_;
        return {};
    }

    auto job = std::make_shared<Job>();
    job->request = request;
    job->sequence = nextSequence_++;
    job->consumers.push_back({ticket, request.viewEpoch});
    jobs_.emplace(request.key, job);
    tickets_.emplace(ticket.value, job);
    queue_.push_back(std::move(job));
    std::stable_sort(queue_.begin(), queue_.end(), [](const auto& a, const auto& b) {
        if (a->request.priority != b->request.priority)
            return a->request.priority > b->request.priority;
        return a->sequence < b->sequence;
    });
    wake_.notify_one();
    return ticket;
}

bool TerrainWorkerRuntime::cancel(TerrainStreamTicket ticket) {
    if (!ticket) return false;
    std::lock_guard lock(mutex_);
    const auto found = tickets_.find(ticket.value);
    if (found != tickets_.end()) {
        eraseTicketLocked(ticket, found->second);
        wake_.notify_all();
        return true;
    }

    bool removedReady = false;
    for (auto& completion : ready_) {
        const auto before = completion.consumers.size();
        completion.consumers.erase(std::remove_if(completion.consumers.begin(), completion.consumers.end(),
                                                  [ticket](const auto& consumer) {
                                                      return consumer.ticket == ticket;
                                                  }),
                                   completion.consumers.end());
        removedReady |= completion.consumers.size() != before;
    }
    if (removedReady) {
        ready_.erase(std::remove_if(ready_.begin(), ready_.end(), [](const auto& completion) {
                         return completion.consumers.empty();
                     }),
                     ready_.end());
        ++cancelled_;
    }
    wake_.notify_all();
    return removedReady;
}

void TerrainWorkerRuntime::cancelViewEpoch(std::uint64_t viewEpoch) {
    std::lock_guard lock(mutex_);
    std::vector<TerrainStreamTicket> cancelThese;
    cancelThese.reserve(tickets_.size());
    for (const auto& [ticket, job] : tickets_) {
        if (std::any_of(job->consumers.begin(), job->consumers.end(), [=](const auto& consumer) {
                return consumer.ticket.value == ticket && consumer.viewEpoch == viewEpoch;
            })) {
            cancelThese.push_back({ticket});
        }
    }
    for (const auto ticket : cancelThese) {
        if (const auto found = tickets_.find(ticket.value); found != tickets_.end())
            eraseTicketLocked(ticket, found->second);
    }
    for (auto& completion : ready_) {
        const auto before = completion.consumers.size();
        completion.consumers.erase(std::remove_if(completion.consumers.begin(), completion.consumers.end(),
                                                  [=](const auto& consumer) {
                                                      return consumer.viewEpoch == viewEpoch;
                                                  }),
                                   completion.consumers.end());
        cancelled_ += before - completion.consumers.size();
    }
    ready_.erase(std::remove_if(ready_.begin(), ready_.end(), [](const auto& completion) {
                     return completion.consumers.empty();
                 }),
                 ready_.end());
    wake_.notify_all();
}

std::vector<TerrainStreamCompletion> TerrainWorkerRuntime::takeReady(std::size_t limit) {
    std::vector<TerrainStreamCompletion> taken;
    if (limit == 0) return taken;
    std::lock_guard lock(mutex_);
    const auto count = std::min(limit, ready_.size());
    taken.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        taken.push_back(std::move(ready_[index]));
    }
    ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(count));
    wake_.notify_all();
    return taken;
}

bool TerrainWorkerRuntime::hasReady() const {
    std::lock_guard lock(mutex_);
    return !ready_.empty();
}

TerrainWorkerRuntime::Stats TerrainWorkerRuntime::stats() const {
    std::lock_guard lock(mutex_);
    return {workers_.size(), queue_.size(), running_, ready_.size(), rejected_, cancelled_};
}

void TerrainWorkerRuntime::eraseTicketLocked(TerrainStreamTicket ticket, const std::shared_ptr<Job>& job) {
    tickets_.erase(ticket.value);
    const auto oldSize = job->consumers.size();
    job->consumers.erase(std::remove_if(job->consumers.begin(), job->consumers.end(),
                                        [ticket](const auto& consumer) {
                                            return consumer.ticket == ticket;
                                        }),
                         job->consumers.end());
    if (job->consumers.size() != oldSize) ++cancelled_;
    if (!job->consumers.empty()) return;

    job->cancelled->store(true, std::memory_order_relaxed);
    if (const auto found = jobs_.find(job->request.key); found != jobs_.end() && found->second == job)
        jobs_.erase(found);
}

void TerrainWorkerRuntime::discardCancelledFrontLocked() {
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [](const auto& job) {
                     return job->cancelled->load(std::memory_order_relaxed);
                 }),
                 queue_.end());
}

void TerrainWorkerRuntime::work() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] {
                discardCancelledFrontLocked();
                return closing_ || (!queue_.empty() && ready_.size() < config_.maxReady);
            });
            if (closing_) return;
            job = std::move(queue_.front());
            queue_.erase(queue_.begin());
            if (job->cancelled->load(std::memory_order_relaxed)) continue;
            ++running_;
        }

        const TerrainCancellation cancellation{job->cancelled, stopping_};
        std::optional<TerrainBuildResult> result;
        try {
            if (!cancellation.cancelled() && build_) result = build_(job->request, cancellation);
        } catch (...) {
            // A failed worker product is indistinguishable from a cancelled
            // one to the frame.  It can resubmit without risking an exception
            // tearing down the worker pool.
            result.reset();
        }
        if (result) result->key = job->request.key;

        std::unique_lock lock(mutex_);
        --running_;
        if (const auto found = jobs_.find(job->request.key); found != jobs_.end() && found->second == job)
            jobs_.erase(found);

        for (const auto& consumer : job->consumers) tickets_.erase(consumer.ticket.value);
        if (!result || job->cancelled->load(std::memory_order_relaxed) || closing_) {
            wake_.notify_all();
            continue;
        }

        wake_.wait(lock, [this, &job] {
            return closing_ || job->cancelled->load(std::memory_order_relaxed) ||
                   ready_.size() < config_.maxReady;
        });
        if (!closing_ && !job->cancelled->load(std::memory_order_relaxed)) {
            ready_.push_back({std::move(*result), std::move(job->consumers)});
        }
        lock.unlock();
        wake_.notify_all();
    }
}

} // namespace world::streaming
