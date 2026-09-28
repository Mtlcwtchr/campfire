#pragma once
// The one thing that knows what a frame is.
//
// It acquires the command buffer, gets an image from the window, keeps the
// targets the right size, sets the clock, pushes the scene every shader reads,
// runs each pipeline in the order it was given, blits the result to the window
// and submits. Nothing else in the engine does any of that, and no pipeline
// decides what comes before or after it.
//
// That is the point of having a runner at all. The order of a frame is a real
// decision - calculations before drawing, drawing before anything that reads the
// drawn picture - and a decision that important should be written down in one
// place where it can be read, not distributed across the things it orders.

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/pipeline/ids.hpp"
#include "engine/pipeline/clock.hpp"
#include "engine/pipeline/profile.hpp"
#include "engine/pipeline/pipeline.hpp"
#include "engine/render/device.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"
#include "engine/render/targets.hpp"

namespace engine {

class Runner {
public:
    // Pipelines run in the order they are added. Returns a borrowed pointer so
    // the caller can keep talking to its own pipeline without owning it.
    template <class T>
    T* add(std::unique_ptr<T> pipeline) {
        T* raw = pipeline.get();
        pipelines_.push_back(std::move(pipeline));
        return raw;
    }

    bool build(Device& device);
    // Multisampling to build with: 1 or 4 (clamped to what the device supports).
    // Must be set before build(); a pipeline cannot change it afterwards.
    void samples(int count) { samples_ = count; }

    // How long one step of the world is. A tenth of a second by default, which
    // is this game's tick; anything that steps faster than the frames arrive is
    // a machine spending its evening simulating and never drawing.
    void fixedStep(double seconds) { clock_.step(seconds); }
    double fixedStep() const { return clock_.step(); }
    // The most steps one frame may run to catch up. Without a cap a frame that
    // took a second asks for ten steps, which takes longer than a second, which
    // asks for eleven: the spiral of death, and it looks like a hang.
    void catchUpLimit(int steps) { clock_.catchUpLimit(steps); }

    // Stops the clock the shaders see, at a given second. Anything that moves
    // by itself - water, grass in the wind - then draws the same way every time,
    // which is what makes two screenshots of the same place comparable. A
    // negative value gives the shaders the real clock back.
    void holdTime(double seconds) { held_ = seconds; }
    std::uint32_t stepsLastFrame() const { return stepsLastFrame_; }

    // One frame. `scene` is what the game wants every shader to know; the runner
    // fills in the parts only it can know - the size of the window and the
    // clock - and pushes it once for the whole frame.
    //
    // Returns false only when the frame could not be started at all. A window
    // with no image ready this instant is not a failure: it is a frame that does
    // not happen, and that returns true.
    bool frame(Device& device, Scene scene);

    // The colour of the last frame, for a screenshot or for a pipeline that
    // wants to read what was drawn.
    const Targets& targets() const { return targets_; }
    bool screenshot(Device& device, const std::string& path) const;

    double frameMillis() const { return frameMillis_; }
    // CPU wall durations, including blocking driver calls; NOT GPU timestamps.
    struct FrameTiming {
        double fixed = 0, acquire = 0, setup = 0, pipelines = 0, submit = 0;
        double cpu() const { return fixed + setup + pipelines + submit; }
    };
    const FrameTiming& frameTiming() const { return frameTiming_; }
    // What the last whole second cost. Counted from the wall clock rather than
    // from the work this class did, because a frame waiting on the display is
    // still a frame the person waited for.
    const Profile& profile() const { return profile_; }
    struct FrameRate { double fps = 0, millis = 0; std::uint64_t revision = 0; };
    const FrameRate& frameRate() const { return frameRate_; }
    // What the last frame put through the GPU. Counted at the draw calls, so
    // the triangle figure is exactly what was submitted - not what survived the
    // rasteriser, which nothing here can see.
    const Frame::Work& work() const { return work_; }
    std::uint64_t frames() const { return frames_; }
    const std::vector<std::pair<PipelineId, double>>& timings() const { return timings_; }

    // A frame with nothing in it, for anything that has to look like a frame
    // without one having happened - the first call, mostly.
    const Frame& last() const { return last_; }

    // Every instance drawn in the frame, in one buffer. Kept here because it is
    // one buffer for the whole frame by definition: a pass that owned its own
    // would be a second arena, a second upload and a second thing to grow.
    const InstanceArena& instances() const { return instances_; }

private:
    std::vector<std::unique_ptr<Pipeline>> pipelines_;
    Targets targets_;
    InstanceArena instances_;
    InstanceArena arguments_{SDL_GPU_BUFFERUSAGE_INDIRECT};
    std::vector<std::pair<PipelineId, double>> timings_;
    Frame last_;
    std::uint64_t frames_ = 0;
    Uint64 startedAt_ = 0;
    Uint64 lastAt_ = 0;
    double frameMillis_ = 0;
    FrameTiming frameTiming_;
    FixedClock clock_;
    Profile profile_;
    FrameRate frameRate_;
    Frame::Work work_;
#if ASR_ENABLE_FPS
    double fpsElapsed_ = 0;
    std::uint64_t fpsFrames_ = 0;
#endif
    double held_ = -1;
    std::uint32_t stepsLastFrame_ = 0;
    int samples_ = 4;
};

} // namespace engine
