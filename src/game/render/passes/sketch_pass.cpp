#include "game/render/passes/sketch_pass.hpp"

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassPlace SketchPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    device_ = &device;
    uploaded_ = ~std::uint64_t{0};
    vertices_ = {};
    indices_ = {};
    indexCount_ = 0;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "sketch.hlsl";
    wanted.vertexEntry = "SketchVS";
    wanted.fragmentEntry = "SketchPS";
    wanted.buffers = {{0, sizeof(generation::SketchMesh::Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0}};
    wanted.blend = false;
    // Over the finished world and its water, and hidden by nothing in front
    // of it that is real: it reads the depth and writes none of its own, as
    // the water does.
    wanted.depthTest = true;
    wanted.depthWrite = false;
    auto graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_ = into.take(std::move(graphics));
    return {engine::passOf(Pass::Sketch), engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(int(Order::Blended) + 10)};
}

bool SketchPass::anything(const engine::Frame&) const {
    return source_ && source_->mesh && !source_->mesh->empty();
}

void SketchPass::collect(const engine::Frame&, engine::DrawQueue& queue) {
    const auto& mesh = *source_->mesh;
    if (uploaded_ != source_->revision) {
        // A stroke rebuilt the sketch: a few thousand vertices, replaced whole.
        vertices_ = device_->uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, mesh.vertices.data(),
                                          mesh.vertices.size() * sizeof(mesh.vertices[0]));
        indices_ = device_->uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, mesh.indices.data(),
                                         mesh.indices.size() * sizeof(mesh.indices[0]));
        indexCount_ = vertices_ && indices_ ? std::uint32_t(mesh.indices.size()) : 0;
        uploaded_ = source_->revision;
    }
    if (indexCount_ == 0) return;
    engine::DrawItem item;
    item.author = 6;
    item.pipeline = pipeline_;
    item.vertex[0] = vertices_.get();
    item.vertexStreams = 1;
    item.index = indices_.get();
    item.indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
    item.indexCount = indexCount_;
    // The shore value the top is clipped at.
    item.own[0] = mesh.shore;
    queue.push(item);
}

} // namespace game

