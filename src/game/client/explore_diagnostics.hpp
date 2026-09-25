#pragma once
#if ASR_ENABLE_PROFILING
#include <array>
#include <chrono>
#include <vector>
#include "game/render/world_renderer.hpp"

namespace client {
class ExploreDiagnostics {
public:
    enum Mark { Begin, Focus, Inspection, Scene, Draw, End };
    void mark(Mark mark) { marks_[mark] = Clock::now(); }
    void observe(const game::WorldRenderer& view, int frame, bool measuring);
    bool advance(Camera& camera, const game::WorldRenderer& view, int frame,
                 bool measuring, bool closeUp, bool tracing);
    static void crowd(game::SpriteQueue& queue, const Camera& camera, int count);
private:
    using Clock = std::chrono::steady_clock;
    std::array<Clock::time_point, 6> marks_{};
    std::array<double, 11> sum_{}, peak_{}, slowest_{};
    std::size_t frames_ = 0;
    std::uint64_t slowestFrame_ = 0;
    std::vector<double> flight_;
    double total_ = 0, worst_ = 0, standing_ = 0, foggy_ = 0, worstFog_ = 0;
};
}
#endif
