#pragma once
// The player's character: one skinned model (CharacterModel), a level of
// detail by its size on the screen, an eight-view impostor card past that,
// nothing past the impostor's reach.
//
// The pose is skinned on the CPU every frame it is drawn, only for the level
// drawn, and the skinned positions and normals go up through the frame's
// instance arena as the second vertex stream (per vertex, not per instance) -
// no buffer of its own to resize, double-buffer or fence. The texture
// coordinates, which never change, are the first stream, uploaded once.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/animation/animator.hpp"
#include "game/render/character_model.hpp"

namespace game {

class CharacterPass : public engine::DrawPass {
public:
    explicit CharacterPass(SDL_GPUTextureSamplerBinding shadow) : shadow_(shadow) {}

    struct State {
        bool visible = false;
        double x = 0, y = 0, z = 0;   // the feet, world metres
        double yaw = 0;               // facing, radians, 0 = +x
        CharacterModel::Gait gait;
        double fade = 1;              // 0..1, dithered
        // First person: the body without its head (the eye is inside it).
        bool firstPerson = false;
        // Where the character looks, world axes (the camera's forward); zero: ahead.
        double lookX = 0, lookY = 0, lookZ = 0;
        // The ground under a world point, for the feet (null: no foot IK).
        std::function<double(double, double)> ground;
        double seconds = 0;           // the frame's step
    };
    void set(const State& state) { state_ = state; }
    [[nodiscard]] const CharacterModel* model() const { return model_ ? model_.get() : nullptr; }
    // Which representation the last frame drew: 0..levels-1, levels for the
    // card, -1 nothing.
    [[nodiscard]] int drawnLevel() const { return drawnLevel_; }
    // The animator's base state ("locomotion", "fall"...), for the readout.
    [[nodiscard]] std::string animationState() const;

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    SDL_GPUTextureSamplerBinding shadow_{};
    std::unique_ptr<CharacterModel> model_;
    engine::Texture albedo_, normal_, surface_, cardColour_, cardNormal_;
    engine::Sampler sampler_;
    engine::Buffer uvs_, indices_, quad_, quadIndices_;
    engine::PipelineSlot mesh_ = 0, card_ = 0;
    engine::BindingSet meshBindings_ = engine::kNoBindings, cardBindings_ = engine::kNoBindings;
    State state_;
    int drawnLevel_ = -1;
    std::unique_ptr<engine::animation::Animator> animator_;
    std::vector<engine::animation::Transform> modelPose_;
    std::vector<CharacterModel::Affine> skinning_;
    bool skinnedValid_ = false;
    std::size_t skinnedLevel_ = 0;
    std::vector<CharacterSkinnedVertex> skinned_;
};

} // namespace game
