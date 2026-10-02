#pragma once
// The sketch of a coast (generation/world_sketch.hpp), extruded: a slab out of
// the sea wherever land was painted on a region that is still a sketch, its
// top clipped at the painted shore and its walls along it. Nothing under it
// is generated - it is the person's mark, drawn the moment it is made, until
// the sketch is pinned and the ground is worked out in its place.
#include <cstdint>
#include <memory>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "game/generation/world_sketch.hpp"

namespace game {

// What the pass draws, set by whoever owns the editor. Replaced whole; the
// revision says the buffers on the card are stale.
struct SketchSource {
    std::shared_ptr<const generation::SketchMesh> mesh;
    std::uint64_t revision = 0;
};

class SketchPass : public engine::DrawPass {
public:
    explicit SketchPass(const SketchSource* source) : source_(source) {}
    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    const SketchSource* source_ = nullptr;
    engine::Device* device_ = nullptr;
    engine::PipelineSlot pipeline_ = 0;
    engine::Buffer vertices_, indices_;
    std::uint32_t indexCount_ = 0;
    std::uint64_t uploaded_ = ~std::uint64_t{0};
};

} // namespace game

