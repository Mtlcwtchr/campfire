#pragma once
#include <array>
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/draw_queue.hpp"

namespace game {
class WeatherPass : public engine::DrawPass {
public:
    engine::PassPlace setup(engine::Device&,engine::RenderPipeline&) override;
    bool anything(const engine::Frame&) const override;
    void collect(const engine::Frame&,engine::DrawQueue&) override;
private:
    engine::PipelineSlot pipeline_=0;
    engine::RenderPipeline* renderer_=nullptr;
    engine::Sampler sampler_;
    engine::BindingSet bindings_=engine::kNoBindings;
    std::array<std::array<double,2>,3> drift_{};
    std::array<std::array<double,2>,3> rainWind_{},snowWind_{};
    std::array<double,3> previousEye_{},previousForward_{};
    double previousTime_=-1;
    double previousClock_=-1;
};
}
