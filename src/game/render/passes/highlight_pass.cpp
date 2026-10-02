#include "game/render/passes/highlight_pass.hpp"

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassPlace HighlightPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    device_ = &device;
    uploaded_ = ~std::uint64_t{0};
    vertices_ = {};
    vertexCount_ = 0;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "highlight.hlsl";
    wanted.vertexEntry = "HighlightVS";
    wanted.fragmentEntry = "HighlightPS";
    wanted.primitive = SDL_GPU_PRIMITIVETYPE_LINELIST;
    wanted.buffers = {{0, sizeof(float) * 3, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0}};
    wanted.blend = false;
    wanted.depthTest = false;   // through the ground: it says what is chosen, wherever it is
    wanted.depthWrite = false;
    wanted.depthClip = true;
    auto graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_ = into.take(std::move(graphics));
    return {engine::passOf(Pass::Highlight), engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(int(Order::Cover) + 1)};
}

bool HighlightPass::anything(const engine::Frame&) const {
    return source_ && source_->lines && source_->lines->size() >= 6;
}

void HighlightPass::collect(const engine::Frame&, engine::DrawQueue& queue) {
    const auto& lines = *source_->lines;
    if (uploaded_ != source_->revision) {
        vertices_ = device_->uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, lines.data(), lines.size() * sizeof(float));
        vertexCount_ = vertices_ ? std::uint32_t(lines.size() / 6 * 2) : 0;
        uploaded_ = source_->revision;
    }
    if (vertexCount_ == 0) return;
    engine::DrawItem item;
    item.author = 6;
    item.pipeline = pipeline_;
    item.vertex[0] = vertices_.get();
    item.vertexStreams = 1;
    item.vertexCount = vertexCount_;
    queue.push(item);
}

} // namespace game

