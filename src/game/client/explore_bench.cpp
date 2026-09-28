#include "game/client/explore_bench.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include "engine/pipeline/runner.hpp"

namespace client {
namespace {

const char* const kSegments[]{"load", "map-far", "orbit", "forest", "flight", "zoom-out"};
constexpr int kSegmentCount = int(sizeof(kSegments) / sizeof(kSegments[0]));

double percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::size_t(std::clamp(p, 0.0, 1.0) * double(values.size() - 1))];
}

} // namespace

ExploreBench::~ExploreBench() {
    if (started_ && !finished_) report();
}

void ExploreBench::aim(Camera& camera, const GroundAt& ground, bool settled) {
    (void)settled;
    if (!started_) {
        started_ = true;
        startX_ = camera.centreX;
        startY_ = camera.centreY;
        segments_.push_back({kSegments[0], {}});
        std::fprintf(stderr, "bench: load begins (limit %d frames), then %d frames per view\n", loadFrames_, frames_);
    }
    const double t = frames_ > 0 ? double(inSegment_) / frames_ : 0;
    const double pi = 3.141592653589793;
    const std::string& name = segments_.back().name;
    if (name == "load") return;   // whatever the command line asked for
    if (name == "map-far") {
        camera.setMode(Camera::Mode::Map);
        camera.pixelsPerTile = 0.6;
        camera.centreX = startX_ + t * 2000.0;
        camera.centreY = startY_;
    } else if (name == "orbit") {
        camera.setMode(Camera::Mode::Orbit);
        camera.centreX = startX_;
        camera.centreY = startY_;
        camera.pixelsPerTile = 6.0;
        camera.pitch = 0.45;
        camera.yaw = 0.785 + t * 2 * pi;
    } else if (name == "forest" || name == "flight") {
        const bool forest = name == "forest";
        camera.setMode(Camera::Mode::Free);
        // Along a line from the start, turning slowly, so the view sweeps
        // through near trees and out to the horizon.
        const double speed = forest ? 12.0 : 160.0;   // metres a frame at 60 Hz -> per second / 60
        const double travelled = double(inSegment_) * speed / 60.0;
        const double heading = 0.785 + t * (forest ? pi : 0.5);
        camera.yaw = heading;
        camera.pitch = forest ? 0.04 : 0.22;
        // Free-flight forward is the negative view direction in the yaw.
        camera.centreX = startX_ - std::cos(heading) * travelled;
        camera.centreY = startY_ - std::sin(heading) * travelled;
        camera.focusHeight = ground(camera.centreX, camera.centreY) + (forest ? 2.0 : 70.0);
    } else if (name == "zoom-out") {
        camera.setMode(Camera::Mode::Orbit);
        camera.centreX = startX_;
        camera.centreY = startY_;
        camera.pitch = 0.5;
        camera.yaw = 0.785;
        camera.pixelsPerTile = 40.0 * std::pow(0.6 / 40.0, t);
    }
}

bool ExploreBench::record(const engine::Runner& runner, bool settled) {
    const std::uint64_t now = SDL_GetTicksNS();
    const auto& timing = runner.frameTiming();
    const double millis = lastTicks_ ? double(now - lastTicks_) / 1e6 : timing.cpu() + timing.acquire;
    lastTicks_ = now;
    auto& segment = segments_.back();
    if (millis > 0) {
        const auto& work = runner.work();
        segment.samples.push_back({millis, timing.cpu(), timing.pipelines, timing.acquire, timing.setup,
                                   timing.submit, work.triangles, work.draws, work.instances});
    }
    ++inSegment_;
    segment.settled = settled;
    if (inSegment_ <= 3 || inSegment_ % 60 == 0)
        std::fprintf(stderr, "bench: %s frame=%d wall=%.2f cpu=%.2f wait=%.2f pipelines=%.2f submit=%.2f settled=%d\n",
                     segment.name.c_str(), inSegment_, millis, timing.cpu(), timing.acquire,
                     timing.pipelines, timing.submit, int(settled));
    const bool done = segment.name == "load" ? (settled && inSegment_ > 30) || inSegment_ >= loadFrames_
                                             : inSegment_ >= frames_;
    if (!done) return false;
    segment.complete = true;
    report(); // Checkpoint completed views even if the next one stalls.
    if (++segment_ >= kSegmentCount) {
        finished_ = true;
        return true;
    }
    segments_.push_back({kSegments[segment_], {}});
    inSegment_ = 0;
    std::fprintf(stderr, "bench: %s begins\n", kSegments[segment_]);
    return false;
}

void ExploreBench::report() const {
    std::printf("\nbench: frame milliseconds per segment (headless timings are the card's own pace)\n");
    std::printf("  %-9s %6s %7s %7s %7s %7s %7s %6s %6s %8s %8s %6s\n", "segment", "frames", "mean",
                "p50", "p95", "p99", "max", ">16.7", ">33", "cpu", "tris(k)", "draws");
    std::ofstream json;
    if (!jsonPath_.empty()) json.open(jsonPath_);
    if (json) json << "[\n";
    bool first = true;
    for (const auto& segment : segments_) {
        std::vector<double> ms, cpu;
        double tris = 0, draws = 0, acquire = 0, pipelines = 0, setup = 0, submit = 0;
        int over16 = 0, over33 = 0;
        for (const auto& s : segment.samples) {
            ms.push_back(s.millis);
            cpu.push_back(s.cpu);
            tris += double(s.triangles);
            draws += s.draws;
            acquire += s.acquire;
            pipelines += s.pipelines;
            setup += s.setup;
            submit += s.submit;
            over16 += s.millis > 16.7;
            over33 += s.millis > 33.3;
        }
        const double n = std::max<double>(1, double(segment.samples.size()));
        double mean = 0;
        for (const double v : ms) mean += v;
        mean /= n;
        double cpuMean = 0;
        for (const double v : cpu) cpuMean += v;
        cpuMean /= n;
        std::printf("  %-9s %6zu %7.2f %7.2f %7.2f %7.2f %7.2f %6d %6d %8.2f %8.0f %6.0f\n", segment.name.c_str(),
                    segment.samples.size(), mean, percentile(ms, 0.5), percentile(ms, 0.95),
                    percentile(ms, 0.99), percentile(ms, 1.0), over16, over33, cpuMean, tris / n / 1000,
                    draws / n);
        if (json) {
            json << (first ? "" : ",\n") << "  {\"segment\":\"" << segment.name << "\",\"frames\":"
                 << segment.samples.size() << ",\"complete\":" << (segment.complete ? "true" : "false")
                 << ",\"settled\":" << (segment.settled ? "true" : "false")
                 << ",\"mean\":" << mean << ",\"p50\":" << percentile(ms, 0.5)
                 << ",\"p95\":" << percentile(ms, 0.95) << ",\"p99\":" << percentile(ms, 0.99)
                 << ",\"max\":" << percentile(ms, 1.0) << ",\"over16\":" << over16 << ",\"over33\":" << over33
                 << ",\"cpu\":" << cpuMean << ",\"acquire\":" << acquire / n
                 << ",\"pipelines\":" << pipelines / n << ",\"setup\":" << setup / n << ",\"submit\":" << submit / n
                 << ",\"triangles\":" << tris / n << ",\"draws\":" << draws / n
                 << "}";
            first = false;
        }
    }
    if (json) json << "\n]\n";
    std::fflush(stdout);
}

} // namespace client
