#pragma once
// Trees from the edge of the placed objects to the horizon (far_trees.hlsl).
//
// One draw, no instance buffer: a fixed grid of candidate cells in rings
// around the eye, decided entirely on the GPU from the climate field and the
// resident terrain pages, drawn with the same baked impostor views as the
// placed trees. Nothing here streams, bakes or places anything.

#include <cstdint>
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {
class GpuTerrain;

class FarTreesPass : public engine::DrawPass {
public:
    FarTreesPass(SDL_GPUTextureSamplerBinding shadow, GpuTerrain* pages)
        : shadow_(shadow), pages_(pages) {}
    // Where the placed objects stop and these take over, and whether to draw.
    void options(bool enabled, double startMetres) { enabled_ = enabled; start_ = startMetres; }

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

    // Candidate cells per ring side, the rings, and the innermost spacing.
    // 4 * start / spacing cells on a side is what the outer edge of each ring
    // needs; every ring then costs the same: side^2 vertex runs of six.
    static constexpr std::uint32_t kRings = 5;
    static constexpr double kBaseSpacing = 40.0;
    // The start distance kBaseSpacing is the right cell for; further out the
    // cells grow in proportion (FarTreesPass::collect).
    static constexpr double kReferenceStart = 1500.0;
    static constexpr double kBand = 160.0;

private:
    SDL_GPUTextureSamplerBinding shadow_{};
    GpuTerrain* pages_ = nullptr;
    engine::RenderPipeline* renderer_ = nullptr;
    engine::Texture atlas_;
    engine::Sampler sampler_;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
    engine::BindingSet vertexBindings_ = engine::kNoBindings;
    bool ready_ = false, enabled_ = true;
    double start_ = 1500.0;
};

} // namespace game

