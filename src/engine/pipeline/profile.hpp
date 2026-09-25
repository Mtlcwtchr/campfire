#pragma once
// What the last second cost.
//
// A frame time on its own is noise: one frame in sixty is the one that hit a
// page fault, and reading the instantaneous number tells you about that frame
// rather than about the program. So this keeps a second's worth and reports the
// average, the worst, and the share of frames that missed a target - which is
// the number that actually corresponds to what a person sees.
//
// It holds no strings and prints nothing. Who is asked to display this, and how,
// is not the engine's business.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine {

class Profile {
public:
    void add(double millis) {
        frames_.push_back(millis);
        held_ += millis;
        if (held_ < 1000.0) return;
        // A second's worth: settle it and start again.
        std::vector<double> sorted = frames_;
        std::sort(sorted.begin(), sorted.end());
        last_.frames = static_cast<std::uint32_t>(sorted.size());
        last_.average = held_ / sorted.size();
        last_.median = sorted[sorted.size() / 2];
        // The ninety-ninth rather than the worst: the worst is one frame and
        // usually somebody else's fault, and chasing it is how an afternoon goes.
        last_.high = sorted[static_cast<std::size_t>(sorted.size() * 0.99)];
        last_.worst = sorted.back();
        last_.fps = sorted.size() * 1000.0 / held_;
        frames_.clear();
        held_ = 0;
        ++settled_;
    }

    struct Second {
        std::uint32_t frames = 0;
        double fps = 0;
        double average = 0;
        double median = 0;
        double high = 0;     // ninety-ninth percentile
        double worst = 0;
    };
    const Second& lastSecond() const { return last_; }
    // How many whole seconds have been settled, so a caller can notice a new one
    // without keeping a clock of its own.
    std::uint64_t seconds() const { return settled_; }

private:
    std::vector<double> frames_;
    double held_ = 0;
    Second last_;
    std::uint64_t settled_ = 0;
};

} // namespace engine
