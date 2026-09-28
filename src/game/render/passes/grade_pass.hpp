#pragma once
// The picture's last word: the grade over the finished world.
//
// One triangle over the whole screen, reading a copy of everything the world
// stages drew (the Post stage takes it, with its mip chain) and writing it back
// warmer, softer at the top and bottom of its range, with light spilling out
// of what is brightest. The interface goes on afterwards, in its own stage.
//
// How strong it is comes in the scene (Scene::quality[2]); nought turns the
// pass off entirely, and then the Post stage has nothing in it and costs
// nothing - not even the copy.
#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace game {

class GradePass : public engine::DrawPass {
public:
    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    engine::RenderPipeline* renderer_ = nullptr;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
    engine::Sampler sampler_;
};

} // namespace game

