#pragma once
// How many whole steps of the world fit in the time that has passed.
//
// Pulled out of the runner so that it can be tested without a graphics device,
// because everything interesting about it is arithmetic and every one of the
// interesting cases is a bug somebody has shipped:
//
//  - dropping the remainder every frame, so the world quietly runs slow;
//  - no cap, so a frame that took a second asks for ten steps, which take longer
//    than a second, which asks for eleven - the spiral, and it looks like a hang;
//  - keeping the remainder after the cap, so the world spends the next minute
//    trying to catch up with a hitch it will never catch up with.

#include <cstdint>

namespace engine {

class FixedClock {
public:
    explicit FixedClock(double step = 0.1, int catchUp = 5) : step_(step), catchUp_(catchUp) {}

    void step(double seconds) { step_ = seconds; }
    double step() const { return step_; }
    void catchUpLimit(int steps) { catchUp_ = steps; }

    // Takes the time since the last frame and returns how many whole steps to
    // run now. What is left over is kept for next time, unless the cap was hit -
    // then it is thrown away, because the world is behind and pretending it can
    // catch up is what makes the spiral.
    std::uint32_t advance(double seconds) {
        held_ += seconds;
        std::uint32_t steps = 0;
        while (held_ >= step_ && static_cast<int>(steps) < catchUp_) {
            held_ -= step_;
            ++steps;
        }
        // Hit the cap: throw the rest away rather than carry it. Carrying it
        // means the next frame starts already owing a step, and the frame after
        // that owes two - the world spends the next minute chasing a hitch it
        // cannot catch, and every one of those minutes looks like a stutter.
        if (held_ >= step_) held_ = 0;
        return steps;
    }

    // How far the world is between the step it has finished and the one it has
    // not started: nought just after a step, approaching one just before the
    // next. What a moving body is interpolated by.
    float alpha() const { return static_cast<float>(held_ / step_); }
    double held() const { return held_; }

private:
    double step_;
    int catchUp_;
    double held_ = 0;
};

} // namespace engine
