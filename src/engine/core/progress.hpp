#pragma once
// What a long job on some thread is doing now, for whoever is waiting on it
// to show: a stage, and how far into it (done of total, total 0 when it cannot
// be counted). One for the process - there is one world being built at a
// time - written by the job, read by the interface, never a cause of work.
//
//     core::progress("fitting rivers", done, total);
//     core::progressLine()   // "fitting rivers 12 / 40"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace core {

struct Progress {
    std::mutex guard;
    std::string stage;
    std::atomic<std::int64_t> done{0}, total{0};
};

inline Progress& buildProgress() {
    static Progress progress;
    return progress;
}

// A new stage, or how far into the current one.
inline void progress(const char* stage, std::int64_t done = 0, std::int64_t total = 0) {
    auto& p = buildProgress();
    {
        const std::lock_guard<std::mutex> lock(p.guard);
        if (p.stage != stage) p.stage = stage;
    }
    p.done.store(done, std::memory_order_relaxed);
    p.total.store(total, std::memory_order_relaxed);
}
inline void progressStep(std::int64_t done) { buildProgress().done.store(done, std::memory_order_relaxed); }

inline std::string progressLine() {
    auto& p = buildProgress();
    std::string line;
    {
        const std::lock_guard<std::mutex> lock(p.guard);
        line = p.stage;
    }
    const auto total = p.total.load(std::memory_order_relaxed);
    if (total > 0) line += " " + std::to_string(p.done.load(std::memory_order_relaxed)) + " / " + std::to_string(total);
    return line;
}

} // namespace core
