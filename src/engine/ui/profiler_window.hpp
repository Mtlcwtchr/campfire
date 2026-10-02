#pragma once
// The sampling profiler's window (engine/core/profiler.hpp), a window of its
// own beside the game's, in the manner of Unreal Insights and Unity's
// profiler - from samples, with nothing in the profiled code:
//
//   - the frame graph: a bar a frame for the last twenty seconds against
//     16.7 and 33.3 ms, spikes red; click a bar to look at that frame
//   - the frame's timeline: a lane a thread, a flame chart of what it ran,
//     outermost call on top; the wheel zooms, a drag pans
//   - the functions of the frame (or of the last second) by time: inclusive
//     (in it or anything it called) and self (in it alone), per thread
//   - frame time, allocations and bytes a frame, live heap, resident memory
//
// Space pauses (the view keeps its frames), <- -> step frames, T writes a
// Chrome trace (chrome://tracing, ui.perfetto.dev), A shows every thread's
// functions or the main one's, S sorts by self or inclusive, +/- the
// sampling rate, C clears, Escape or the close button closes.
//
// Drawn with an SDL_Renderer of its own and SDL's debug font: nothing of the
// game's renderer is touched.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/core/profiler.hpp"

namespace ui {

class ProfilerWindow {
public:
    ProfilerWindow() = default;
    ~ProfilerWindow();
    ProfilerWindow(const ProfilerWindow&) = delete;
    ProfilerWindow& operator=(const ProfilerWindow&) = delete;

    // Opens the window and starts sampling; closes it and stops.
    void open();
    void close();
    void toggle() { if (shown()) close(); else open(); }
    [[nodiscard]] bool shown() const { return window_ != nullptr; }

    // An event for this window: taken (true) or not. The game's loop asks this
    // first, so nothing done here moves the camera.
    bool handle(const SDL_Event& event);
    // Whether the pointer or the keyboard is on this window: the game then
    // ignores the mouse and the keys it reads by state.
    [[nodiscard]] bool hasMouse() const;
    [[nodiscard]] bool hasKeyboard() const;

    // Draws, at most twenty times a second. Call once a frame, after
    // engine::profile::frame().
    void draw();

private:
    void drawHeader(float y);
    float drawFrameGraph(float y, float height);
    float drawTimeline(float y, float height);
    void drawFunctions(float x, float y, float w, float h);
    void drawCounters(float x, float y, float w, float h);
    void text(float x, float y, const std::string& s, SDL_Color c, float scale = 1.0f);
    void fill(float x, float y, float w, float h, SDL_Color c);
    std::shared_ptr<const engine::profile::FrameRecord> selected() const;
    void writeTrace();
    void say(std::string what);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::uint64_t lastDraw_ = 0;
    bool paused_ = false;
    std::vector<std::shared_ptr<const engine::profile::FrameRecord>> frames_;   // what is shown
    std::int64_t selectedIndex_ = -1;   // FrameRecord::index; -1 the latest
    bool overSecond_ = false;           // the function table: the frame, or the last second
    bool allThreads_ = false;           // the function table: every thread, or the main one
    bool bySelf_ = false;
    // The timeline's window into the selected frame, in its own milliseconds.
    double viewStart_ = 0, viewSpan_ = 0;
    float laneScroll_ = 0;
    float mouseX_ = 0, mouseY_ = 0;
    bool dragging_ = false;
    float dragFromX_ = 0;
    double dragFromStart_ = 0;
    SDL_FRect graphRect_{}, timelineRect_{}, functionsRect_{};
    std::string status_;
    std::uint64_t statusUntil_ = 0;
};

} // namespace ui
