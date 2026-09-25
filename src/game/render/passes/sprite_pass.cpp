#include "game/render/passes/sprite_pass.hpp"

#include <cstddef>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instanced.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/calc/sprite_queue.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/sprite_data.hpp"
#include "game/render/world_materials.hpp"

namespace game {

SpritePass::SpritePass(const SpriteQueue& queue, std::vector<std::string> pages)
    : queue_(queue), pageNames_(std::move(pages)) {}

engine::PassPlace SpritePass::setup(engine::Device& device, engine::RenderPipeline& into) {
    engine::VertexLayout wanted;
    // Two streams, which is what every instanced draw in this engine looks
    // like: the shape, once, and one entry per copy. The shape here is the unit
    // quad; the day a cart is a real mesh, this list gains that mesh's vertex
    // layout and nothing else about the pass changes.
    wanted.buffers = {
            {0, sizeof(engine::QuadVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
            {1, sizeof(SpriteInstanceGpu), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
    };
    wanted.attributes = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(engine::QuadVertex, corner)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(engine::QuadVertex, uv)},
            {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(SpriteInstanceGpu, position)},
            {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(SpriteInstanceGpu, size)},
            {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(SpriteInstanceGpu, rotation)},
            {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(SpriteInstanceGpu, tint)},
            {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(SpriteInstanceGpu, uv)},
            {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(SpriteInstanceGpu, page)},
            {8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(SpriteInstanceGpu, mode)},
    };
    // Cut out: no blending, and it writes depth, so a thousand cards need no
    // sorting against each other.
    if (!cutout_.material.setup(device, into, materials::sprite(false), wanted) ||
        !soft_.material.setup(device, into, materials::sprite(true), wanted)) return {};

    // Clamped: a card is a picture with edges, and one that wrapped would show
    // its own other side down the seam.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    std::vector<std::filesystem::path> layers;
    layers.reserve(pageNames_.size());
    for (const std::string& page : pageNames_) layers.push_back(device.assets() / page);
    pages_ = device.loadArray(layers);
    if (!pages_) return {};

    // The mesh. A quad, because that is what a sprite is - and it comes from the
    // engine rather than from here, so that a pass drawing a real mesh asks the
    // same question of the same place.
    auto shape = std::make_shared<const engine::Mesh>(engine::unitQuad(device));
    if (!*shape) return {};
    cutout_.mesh = soft_.mesh = std::move(shape);
    cutout_.material.textures({{pages_.get(), sampler_.get()}});
    soft_.material.textures({{pages_.get(), sampler_.get()}});
    return {engine::passOf(Pass::Sprite), engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Blended)};
}

bool SpritePass::anything(const engine::Frame& frame) const {
    (void)frame;
    return !queue_.batches().empty();
}

void SpritePass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    cards_ = 0;
    calls_ = 0;
    const std::vector<SpriteInstanceGpu>& instances = queue_.instances();
    if (instances.empty() || frame.instances == nullptr) return;

    // Every card in the frame into the frame's arena, in one go. The pass does
    // not own a buffer and does not upload anything: the arena is the engine's,
    // shared with every other instanced draw in the frame, and the pipeline
    // uploads the lot once before it issues a single call.
    const std::uint32_t at = frame.instances->add(instances.data(), instances.size(),
                                                  sizeof(SpriteInstanceGpu));

    for (const SpriteQueue::Batch& batch : queue_.batches()) {
        if (batch.count == 0) continue;
        const auto& renderer = batch.blend == SpriteBlend::Cutout ? cutout_ : soft_;
        renderer.submit(queue, engine::MeshInstances{batch.count,
            at + batch.first * static_cast<std::uint32_t>(sizeof(SpriteInstanceGpu)), true});
        cards_ += batch.count;
        ++calls_;
    }
}

} // namespace game
