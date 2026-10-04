#pragma once
// The picture's last word: the grade over the finished world.
//
// One triangle over the whole screen, reading a copy of everything the world
// stages drew (the Post stage takes it, with its mip chain) and writing it
// back graded. The interface goes on afterwards, in its own stage.
//
// The engine's half (doc/plan_procedural_environment_2026-10-03.md, part H):
// the pass, the copy and its chain, edge smoothing, and up to two colour
// lookups blended as the camera crosses grading regions. What the grade
// looks like is the game's: grade.hlsl hands the picture to stylePicture(),
// which the game writes in assets/shaders/game/style_picture.hlsli, with the
// look's dials below and the scene's gradeStyle rows.
//
// How strong it is comes in the scene (Scene::quality[2]); nought turns the
// pass off entirely, and then the Post stage has nothing in it and costs
// nothing - not even the copy.
#include <array>
#include <span>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"

namespace engine {

// The dials a game hands its stylePicture(): how much light spills, how dark
// the frame's edge, how much grain. The game's body decides what they mean.
struct GradeLook {
    float bloom = 0.50f;
    float vignette = 0.28f;
    float grain = 0.012f;
};

class GradePass : public DrawPass {
public:
    GradePass(PassPlace where, GradeLook look = {});
    PassPlace setup(Device& device, RenderPipeline& into) override;
    bool anything(const Frame& frame) const override;
    void collect(const Frame& frame, DrawQueue& queue) override;

    // The colour lookups to apply after the game's grade, and how far from
    // the first to the second (0..1). Each is size^3 colours, red fastest
    // (engine/environment/style.hpp, Lut3d). Empty: no lookup. Uploaded on
    // the next frame, only when they change.
    void luts(int size, std::span<const std::array<float, 3>> a, std::span<const std::array<float, 3>> b, float t);

private:
    PassPlace where_;
    GradeLook look_;
    RenderPipeline* renderer_ = nullptr;
    PipelineSlot pipeline_ = 0;
    BindingSet bindings_ = kNoBindings;
    Sampler sampler_;
    Sampler lutSampler_;
    Texture lut_;
    int lutSize_ = 0;          // of the texture as made
    int wantedSize_ = 0;
    std::vector<std::uint8_t> wanted_;   // both lookups as strips, RGBA8, layer after layer
    bool dirty_ = false;
    float lutBlend_ = 0;
};

} // namespace engine
