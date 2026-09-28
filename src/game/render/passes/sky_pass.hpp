#pragma once
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {
// The sky the distance fog fades into; only where nothing opaque was drawn.
// With assets/generated/sky it is the UE panorama plus the volumetric cloud
// layer (tools/export_ue_sky.py + tools/prepare_sky.py); without them the
// analytic gradient.
class SkyPass : public engine::DrawPass {
public:
    engine::PassPlace setup(engine::Device&,engine::RenderPipeline&) override;
    bool anything(const engine::Frame&) const override;
    void collect(const engine::Frame&,engine::DrawQueue&) override;
    bool textured() const { return textured_; }
private:
    engine::PipelineSlot plain_=0,textured_pipeline_=0;
    engine::BindingSet bindings_=engine::kNoBindings;
    engine::Texture panorama_,weather_,noise_;
    engine::Sampler clamp_,wrap_;
    bool textured_=false;
};
}

