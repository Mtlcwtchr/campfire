#pragma once
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {
class WeatherPass : public engine::DrawPass {
public:
    engine::PassPlace setup(engine::Device&,engine::RenderPipeline&) override;
    bool anything(const engine::Frame&) const override;
    void collect(const engine::Frame&,engine::DrawQueue&) override;
private:
    engine::PipelineSlot pipeline_=0;
};
}
