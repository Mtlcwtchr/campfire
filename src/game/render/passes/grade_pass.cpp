#include "game/render/passes/grade_pass.hpp"

#include <algorithm>
#include <vector>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassPlace GradePass::setup(engine::Device& device, engine::RenderPipeline& into) {
    renderer_ = &into;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "grade.hlsl";
    wanted.vertexEntry = "GradeVS";
    wanted.fragmentEntry = "GradePS";
    // It replaces every pixel with its graded self, so nothing is blended and
    // nothing is tested: the depth of the world is still there for whatever
    // comes after, and the grade does not touch it.
    wanted.blend = false;
    wanted.depthTest = false;
    wanted.depthWrite = false;
    auto graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_ = into.take(std::move(graphics));

    // Linear within and between levels, clamped at the edge of the screen,
    // and every level reachable: the glow is read from the coarse end of the
    // copy's chain.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.min_lod = 0.0f;
    sampler.max_lod = 16.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};
    // The picture is the frame's own copy and changes every frame; the slot
    // is filled in collect().
    bindings_ = into.take(std::vector<SDL_GPUTextureSamplerBinding>{{nullptr, sampler_.get()}});
    return {engine::passOf(Pass::Grade), engine::stageOf(Stage::Post),
            static_cast<engine::PassOrder>(Order::Opaque)};
}

bool GradePass::anything(const engine::Frame& frame) const {
    return frame.grab != nullptr && (frame.scene.quality[2] > 0.0f || frame.scene.quality[3] > 0.0f);
}

void GradePass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    renderer_->replace(bindings_, {{frame.grab, sampler_.get()}});
    engine::DrawItem item;
    item.author = 6;
    item.pipeline = pipeline_;
    item.bindings = bindings_;
    item.vertexCount = 3;
    item.hasOwnData = true;
    const float strength = std::clamp(frame.scene.quality[2], 0.0f, 1.0f);
    // The look itself: strength, bloom, vignette, grain. Then how many levels
    // the copy has, so the shader never asks for one beyond its end.
    item.own[0] = strength;
    item.own[1] = 0.50f;   // bloom
    item.own[2] = 0.45f;   // vignette
    item.own[3] = 0.012f;  // grain
    float levels = 1.0f;
    for (Uint32 side = std::max(frame.width, frame.height); side > 1; side /= 2) levels += 1.0f;
    item.own[4] = levels;
    item.own[5] = frame.scene.quality[3] > 0.0f ? 1.0f : 0.0f;   // FXAA
    queue.push(item);
}

} // namespace game

