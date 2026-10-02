#pragma once
// What the editor has picked out of the world, drawn over it as lines: the
// water chosen on the Water tab - a river with the brooks that feed it, a lake
// with what fills it - so the person sees exactly what an edit to it covers.
// Drawn through everything (a marker, not an object), in one colour.
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {

// Line segments as pairs of points, x y z in world metres, replaced whole.
struct HighlightSource {
    std::shared_ptr<const std::vector<float>> lines;
    std::uint64_t revision = 0;
};

class HighlightPass : public engine::DrawPass {
public:
    explicit HighlightPass(const HighlightSource* source) : source_(source) {}
    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    const HighlightSource* source_ = nullptr;
    engine::Device* device_ = nullptr;
    engine::PipelineSlot pipeline_ = 0;
    engine::Buffer vertices_;
    std::uint32_t vertexCount_ = 0;
    std::uint64_t uploaded_ = ~std::uint64_t{0};
};

} // namespace game

