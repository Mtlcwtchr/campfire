#pragma once
// A scripted camera and a frame-time report: `asr_client --explore --bench`.
//
// The question it answers is the one the frame counter in the title cannot:
// how bad is the WORST view, and where are the hitches. So it flies a fixed
// route through the kinds of view that each stress something different, and
// reports every segment separately - an average over the whole route hides a
// forest that halves the frame rate for two seconds.
//
//   load        from the first frame until the streaming settles (the hitch
//               a player sees when the world opens)
//   map-far     straight down on a large area: every object is far
//   orbit       the default three-quarter view, one full turn
//   forest      two metres above the ground, looking at the horizon: the
//               nearest trees and the farthest plan in one frame
//   flight      low and fast: streaming under pressure
//   zoom-out    from close up to the whole region
//
// Nothing here opens a window by itself; with --headless the frames go to an
// offscreen target and the timings are the card's, not the display's.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/client/camera.hpp"

namespace engine { class Runner; }

namespace client {

class ExploreBench {
public:
    using GroundAt = std::function<double(double x, double y)>;
    ExploreBench(int framesPerSegment, std::string jsonPath, int loadFrames = 900)
        : frames_(framesPerSegment), loadFrames_(loadFrames), jsonPath_(std::move(jsonPath)) {}
    ~ExploreBench();

    // Before the frame: moves the camera for this frame of the route.
    void aim(Camera& camera, const GroundAt& ground, bool settled);
    // After the frame: records it. True when the route is over and the report
    // has been printed.
    bool record(const engine::Runner& runner, bool settled);

private:
    struct Sample {
        double millis = 0, cpu = 0, pipelines = 0;
        double acquire = 0, setup = 0, submit = 0;
        std::uint64_t triangles = 0;
        std::uint32_t draws = 0, instances = 0;
    };
    struct Segment {
        std::string name;
        std::vector<Sample> samples;
        bool complete = false, settled = false;
    };
    void report() const;

    int frames_;
    int loadFrames_;
    std::string jsonPath_;
    std::vector<Segment> segments_;
    int segment_ = 0;       // 0 = load
    int inSegment_ = 0;
    std::uint64_t lastTicks_ = 0;
    double startX_ = 0, startY_ = 0;
    bool started_ = false;
    bool finished_ = false;
};

} // namespace client
