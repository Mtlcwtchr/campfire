#include "game/client/explore_diagnostics.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include "game/render/calc/terrain_collect.hpp"
#include "game/render/pass_ids.hpp"

namespace client {
void ExploreDiagnostics::observe(const game::WorldRenderer& view, int frame, bool measuring) {
    mark(End);
    static const bool profile = std::getenv("ASR_FRAME_PROFILE") != nullptr;
    if (profile) {
        constexpr std::array<const char*, 11> names{"total", "input/control", "focus-height", "inspection",
            "scene/crowd", "acquire/wait", "prepare", "render-CPU", "blit/submit", "fixed/setup", "other"};
        const auto ms = [&](Mark a, Mark b) { return std::chrono::duration<double, std::milli>(marks_[b] - marks_[a]).count(); };
        const auto& timing = view.runner().frameTiming();
        double prepare = 0, render = 0;
        for (const auto& [phase, duration] : view.runner().timings()) {
            if (phase == engine::PipelineId(game::Phase::Prepare)) prepare += duration;
            if (phase == engine::PipelineId(game::Phase::Render)) render += duration;
        }
        std::array<double, 11> sample{ms(Begin, End), ms(Begin, Focus), ms(Focus, Inspection), ms(Inspection, Scene),
            ms(Scene, Draw) + ms(Draw, End) - view.frameMillis(), timing.acquire, prepare, render,
            timing.submit, timing.fixed + timing.setup, 0};
        sample.back() = sample[0];
        for (std::size_t i = 1; i + 1 < sample.size(); ++i) sample.back() -= sample[i];
        for (std::size_t i = 0; i < sample.size(); ++i) { sum_[i] += sample[i]; peak_[i] = std::max(peak_[i], sample[i]); }
        if (sample[0] > slowest_[0]) { slowest_ = sample; slowestFrame_ = frame; }
        ++frames_;
        if (sum_[0] >= 1000.0) {
            std::fprintf(stderr, "main-profile %zu frames target=%ux%u lod=%d (CPU wall ms, not GPU time; excludes reporting)\n",
                frames_, view.runner().last().width, view.runner().last().height, view.terrain().target());
            for (std::size_t i = 0; i < sample.size(); ++i)
                std::fprintf(stderr, "  %-14s avg=%7.3f max=%7.3f slowest[%llu]=%7.3f\n", names[i],
                    sum_[i] / double(frames_), peak_[i], static_cast<unsigned long long>(slowestFrame_), slowest_[i]);
            sum_ = {}; peak_ = {}; slowest_ = {}; frames_ = 0;
        }
    }
    if (!measuring) return;
    flight_.push_back(view.frameMillis());
    total_ += view.frameMillis(); worst_ = std::max(worst_, view.frameMillis());
    const auto& collect = *view.collector();
    if (collect.patchesDrawn()) {
        const double drawn = double(collect.patchesDrawn());
        standing_ += collect.coarseLevels() / drawn;
        const double fog = collect.muchCoarser() / drawn;
        foggy_ += fog; worstFog_ = std::max(worstFog_, fog);
    }
}

bool ExploreDiagnostics::advance(Camera& camera, const game::WorldRenderer& view, int frame,
                                 bool measuring, bool closeUp, bool tracing) {
    const double across = camera.viewportWidth / std::max(0.001, camera.pixelsPerTile);
    if (tracing) {
        static const bool local = std::getenv("ASR_VEGETATION_TRACE_LOCAL") != nullptr;
        static const int frames = std::getenv("ASR_VEGETATION_TRACE_SETTLE") ? 900 : 300;
        const char* phase = "settle";
        if (frame >= 90 && frame < 180) { camera.centreX += local ? 384.0 / 90.0 : across * 0.02; phase = "pan"; }
        else if (frame >= 180 && frame < 300) { camera.pixelsPerTile *= 1.015; phase = "zoom"; }
        else if (frame >= 300) phase = "detail";
        if (frame % 5 == 0 || frame < 20) {
            std::printf("%-7s %4d lod %2d drawn %4zu standing %4zu missing %4zu\n", phase, frame,
                view.terrain().target(), view.collector()->patchesDrawn(), view.terrain().coarse(), view.terrain().missing());
            std::cout << "vegetation " << frame << ' ' << view.vegetationReport() << '\n';
        }
        if (frame >= frames) return true;
    }
    if (!measuring) return false;
    if (frame == 1 && closeUp) camera.pixelsPerTile = kMaxPixelsPerTile;
    if (frame < 100) camera.centreX += across / 3;
    else if (frame < 200 && !closeUp) camera.pixelsPerTile *= 0.90;
    else if (!closeUp) camera.pixelsPerTile /= 0.90;
    else camera.centreY += across / 3;
    camera.pixelsPerTile = std::clamp(camera.pixelsPerTile, camera.minZoom, double(kMaxPixelsPerTile));
    if (frame < 300) return false;
    std::sort(flight_.begin(), flight_.end());
    const auto at = [&](double share) { return flight_[std::min(flight_.size() - 1, std::size_t(flight_.size() * share))]; };
    std::cout << "GPU flight over " << frame << " frames: " << at(0.5) << " ms median, " << at(0.99)
              << " ms 99th, " << worst_ << " ms worst (" << total_ / frame << " ms average)\n"
              << "  coarser than asked: " << standing_ / frame << " levels per patch; three or more levels off: "
              << foggy_ / frame * 100 << "% average, " << worstFog_ * 100 << "% worst\n";
    return true;
}

void ExploreDiagnostics::crowd(game::SpriteQueue& queue, const Camera& camera, int count) {
    if (count <= 0) return;
    const int side = std::max(1, int(std::sqrt(double(count))));
    const double span = camera.viewportWidth / std::max(0.001, camera.pixelsPerTile) * 1.2;
    for (int i = 0; i < count; ++i) {
        game::SpriteInstanceGpu card{};
        card.position[0] = float(camera.centreX + (double(i % side) / side - 0.5) * span);
        card.position[1] = float(camera.centreY + (double(i / side) / side - 0.5) * span);
        card.position[2] = float(camera.focusHeight);
        card.size[0] = 1.6f; card.size[1] = 2.0f;
        card.tint[0] = card.tint[1] = card.tint[2] = card.tint[3] = 1.0f;
        card.uv[2] = card.uv[3] = 1.0f;
        card.page = float(i % 2);
        queue.add(card);
    }
}
} // namespace client
