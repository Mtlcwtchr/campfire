#include "game/render/passes/sky_pass.hpp"
#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include <filesystem>
#include <iostream>

namespace game {
engine::PassPlace SkyPass::setup(engine::Device& device,engine::RenderPipeline& into) {
    engine::PipelineWanted wanted;
    wanted.shaderFile="sky_dome.hlsl";
    wanted.vertexEntry="SkyVS";
    wanted.fragmentEntry="SkyPlainPS";
    wanted.blend=false;
    wanted.depthTest=true;   // LESS_OR_EQUAL at the far plane: only empty pixels
    wanted.depthWrite=false;
    auto plain=device.makePipeline(wanted);
    if (!plain) return {};
    plain_=into.take(std::move(plain));
    // The GoodSky cirrus dome is optional content (tools/export_ue_sky.py +
    // tools/prepare_sky.py); without it the sky is the atmosphere and the
    // volumetric deck, which need no texture at all.
    const auto dome=device.assets().parent_path()/"generated/sky/cloud_dome.png";
    if (std::filesystem::is_regular_file(dome)) {
        panorama_=device.loadMipped({dome});
        SDL_GPUSamplerCreateInfo sampler{};
        sampler.min_filter=sampler.mag_filter=SDL_GPU_FILTER_LINEAR;
        sampler.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sampler.address_mode_u=sampler.address_mode_v=sampler.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        clamp_=device.makeSampler(sampler);
        wanted.fragmentEntry="SkyPS";
        auto textured=panorama_ && clamp_ ? device.makePipeline(wanted) : engine::GraphicsPipeline{};
        if (textured) {
            textured_pipeline_=into.take(std::move(textured));
            bindings_=into.take(std::vector<SDL_GPUTextureSamplerBinding>{{panorama_.get(),clamp_.get()}});
            textured_=true;
        } else std::cerr<<"sky: cloud dome unavailable, atmosphere only: "<<device.error()<<'\n';
    }
    // After everything opaque, before anything blended over it.
    return {engine::passOf(Pass::Sky),engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Sky)};
}
bool SkyPass::anything(const engine::Frame& frame) const {
    const auto* m=frame.scene.viewProjection;
    const bool perspective=m[12]!=0 || m[13]!=0 || m[14]!=0;
    return perspective && frame.scene.fog[2]>0.5f;
}
void SkyPass::collect(const engine::Frame& frame,engine::DrawQueue& queue) {
    engine::DrawItem item;
    item.author=6;
    // The dome needs the textured variant; clouds and atmosphere do not.
    const bool textured=textured_ && frame.scene.skyHorizon[3]>0.5f;
    item.pipeline=textured?textured_pipeline_:plain_;
    if (textured) item.bindings=bindings_;
    item.vertexCount=3;
    queue.push(item);
}
}

