#include "game/render/passes/weather_pass.hpp"
#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {
engine::PassPlace WeatherPass::setup(engine::Device& device,engine::RenderPipeline& into) {
    engine::PipelineWanted wanted;
    wanted.shaderFile="precipitation.hlsl";
    wanted.vertexEntry="PrecipitationVS";
    wanted.fragmentEntry="PrecipitationPS";
    wanted.blend=true;
    wanted.depthTest=false;
    wanted.depthWrite=false;
    auto graphics=device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_=into.take(std::move(graphics));
    return {engine::passOf(Pass::Weather),engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(240)};
}
bool WeatherPass::anything(const engine::Frame& frame) const {
    return frame.scene.extra[3]<0.5f && frame.scene.parameters[0][0]>0.5f &&
           frame.scene.parameters[2][1]>0.005f;
}
void WeatherPass::collect(const engine::Frame&,engine::DrawQueue& queue) {
    engine::DrawItem item;
        item.author = 6;
    item.pipeline=pipeline_;
    item.vertexCount=3;
    queue.push(item);
}
}
