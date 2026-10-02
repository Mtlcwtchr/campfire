#pragma once
// A sampling profiler: what every thread of the process is doing, as it runs,
// with nothing written into the code it profiles.
//
// A thread of its own wakes a thousand times a second, stops every other
// thread of the process for a few microseconds, reads where it is - the
// program counter and the chain of frame pointers, which arm64 macOS always
// keeps - and lets it go again. That is the method of Instruments' Time
// Profiler, of Unity's and Unreal's sampling: no zones, no macros, any
// function in any library, worker threads included. The addresses are turned
// into function names afterwards, off the sampled threads, with dladdr.
//
// The game marks only where a frame ends (frame()), so the samples can be
// cut into frames; a frame also carries the allocations made in it, when the
// allocation hooks are linked into the executable (profiler_alloc.cpp:
// operator new and delete, counted), and the process's resident memory.
// The last frames are kept for a viewer (engine/ui/profiler_window.hpp) and
// for a Chrome trace (writeChromeTrace: chrome://tracing, ui.perfetto.dev).
//
// Off, it costs nothing: no thread, one relaxed atomic load in the hooks.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace engine::profile {

// Nanoseconds since the process started.
std::int64_t now();

// One look at one thread: when, which, whether it was running or waiting,
// and its stack - frames[first .. first + depth), the leaf first.
struct Sample {
    std::int64_t time = 0;
    std::uint16_t thread = 0;
    bool waiting = false;
    std::uint32_t first = 0, depth = 0;
};

struct FrameRecord {
    std::uint64_t index = 0;
    std::int64_t begin = 0, end = 0;
    std::int64_t interval = 0;            // nanoseconds a sample stands for
    std::vector<Sample> samples;          // in time order, every thread
    std::vector<std::uintptr_t> frames;   // the samples' return addresses
    std::uint64_t allocations = 0, allocatedBytes = 0, frees = 0;
    std::int64_t liveBytes = 0;           // heap held, by the hooks' count
    std::uint64_t residentBytes = 0;      // the process's, as the system counts it
    [[nodiscard]] double millis() const { return double(end - begin) * 1e-6; }
};

// Starts (and stops) the sampling thread.
[[nodiscard]] bool enabled();
void setEnabled(bool on);
// How often each thread is looked at; a thousand times a second at first.
void setInterval(std::chrono::microseconds interval);
[[nodiscard]] std::chrono::microseconds interval();

// The main loop's frame boundary: closes the frame that began at the last call.
// The thread that calls it is the one shown as "main".
void frame();

struct ThreadInfo {
    std::string name;   // the thread's own (pthread), else made up from its index
    bool main = false;
};
[[nodiscard]] std::vector<ThreadInfo> threads();

// The last `count` frames, oldest first (shared, never changed after made).
[[nodiscard]] std::vector<std::shared_ptr<const FrameRecord>> frames(std::size_t count = 100000);
inline constexpr std::size_t kKeptFrames = 1200;
void clear();

// What a return address is: the function holding it (demangled, without its
// arguments), where that function begins - the key samples are counted by -
// and whether it is in the game's own executable rather than a system library.
struct Symbol {
    const char* name = "?";
    std::uintptr_t start = 0;
    bool own = false;
};
// Cached. Safe from any thread but the sampler's (which never asks).
Symbol symbolOf(std::uintptr_t address);

// The allocation hooks' entry points (profiler_alloc.cpp).
void noteAllocation(std::size_t bytes);
void noteFree(std::size_t bytes);
[[nodiscard]] bool allocationsHooked();

// A thread's samples in a frame as bars of a flame chart: consecutive samples
// whose stacks agree down to a depth make one bar at that depth. Depth 0 is
// the outermost function of the game's own (the system's thread starts are
// left off).
struct Bar {
    std::int64_t begin = 0, end = 0;
    std::uint16_t depth = 0;
    const char* name = "?";
    std::uintptr_t function = 0;
    bool waiting = false;
};
std::vector<Bar> flame(const FrameRecord& frame, std::uint16_t thread, std::size_t maxDepth = 64);

// The frames as a Chrome trace (JSON): chrome://tracing or ui.perfetto.dev.
bool writeChromeTrace(const std::filesystem::path& file,
                      const std::vector<std::shared_ptr<const FrameRecord>>& frames, std::string* why = nullptr);

} // namespace engine::profile
