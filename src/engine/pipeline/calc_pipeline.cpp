#include "engine/pipeline/calc_pipeline.hpp"

#include <chrono>

#include "engine/render/frame.hpp"

namespace engine {

bool CalcPipeline::build(Device& device) {
    for (std::unique_ptr<CalcPass>& pass : passes_)
        if (!pass->setup(device)) return false;
    return true;
}

bool CalcPipeline::run(Frame& frame) {
#if ASR_ENABLE_PROFILING
    passMillis_.clear();
#endif
    for (std::unique_ptr<CalcPass>& pass : passes_) {
        if (!pass->enabled) continue;
#if ASR_ENABLE_PROFILING
        const auto at = std::chrono::steady_clock::now();
#endif
        pass->run(frame);
#if ASR_ENABLE_PROFILING
        passMillis_.emplace_back(
                pass->id(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at)
                        .count());
#endif
    }
    return true;
}

} // namespace engine
