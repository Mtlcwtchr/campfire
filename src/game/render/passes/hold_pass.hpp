#pragma once
// The last picture of the world that was showing, held over the one that
// replaced it until that one has its ground.
//
// A world whose shape was edited is raised again and swapped in whole, and a
// new world's terrain starts with nothing on the card: for a second or two
// the view was open sea where the country had been. This covers it. The
// renderer copies the finished picture (before the grade and the interface)
// out of the old world's targets before letting it go, and this draws that
// copy over the new world's surface stage - graded and written over as ever,
// so the interface stays live - until the new ground in view is in. Then the
// copy goes, in one frame.
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {

class HoldPass : public engine::DrawPass {
public:
    // `held` is owned by the renderer and outlives every pipeline it builds;
    // null (or an empty texture) draws nothing.
    explicit HoldPass(const engine::Texture* held) : held_(held) {}
    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    const engine::Texture* held_ = nullptr;
    engine::RenderPipeline* renderer_ = nullptr;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
    engine::Sampler sampler_;
};

} // namespace game
