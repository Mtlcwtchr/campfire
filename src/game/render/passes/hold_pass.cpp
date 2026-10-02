#include "game/render/passes/hold_pass.hpp"

#include <vector>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassPlace HoldPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    renderer_ = &into;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "hold.hlsl";
    wanted.vertexEntry = "HoldVS";
    wanted.fragmentEntry = "HoldPS";
    wanted.blend = false;
    wanted.depthTest = false;
    wanted.depthWrite = false;
    auto graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_ = into.take(std::move(graphics));
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};
    bindings_ = into.take(std::vector<SDL_GPUTextureSamplerBinding>{{nullptr, sampler_.get()}});
    // Last in the surface stage: over the new world's ground, water and cards,
    // under the grade (which reads the stage's copy) and the interface.
    return {engine::passOf(Pass::Hold), engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(int(Order::Cover) + 2)};
}

bool HoldPass::anything(const engine::Frame&) const { return held_ && *held_; }

void HoldPass::collect(const engine::Frame&, engine::DrawQueue& queue) {
    renderer_->replace(bindings_, {{held_->get(), sampler_.get()}});
    engine::DrawItem item;
    item.author = 6;
    item.pipeline = pipeline_;
    item.bindings = bindings_;
    item.vertexCount = 3;
    queue.push(item);
}

} // namespace game
