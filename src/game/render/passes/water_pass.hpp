#pragma once
// The water, over the ground it stands on.
//
// The same triangles as the ground, out of the same buffers, with a different
// shader and blending on: how deep the water is at a vertex is already in the
// vertex, so there is no second mesh and nothing to keep in step. It declares a
// later order than everything opaque, which is the only way it says "after" -
// the batcher does the rest.
//
// It has textures of its own, though, and only three: the surface is a shape
// rather than a colour, so what it carries is two normal maps and a foam mask
// (tools/bake_water.py). One array of layers for the same reason the ground has
// one - a shader can index a layer and cannot index a texture.

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "game/render/climate_textures.hpp"

namespace game {
class GpuTerrain;

class WaterPass : public engine::DrawPass {
public:
    // The climate textures are the terrain's as well; whichever pass sets up
    // first fills them. Water reads three of the eleven channels - air
    // temperature, moisture and drainage - to decide whether a reach is
    // frozen, and used to read them off the terrain vertex.
    WaterPass(const engine::MeshCache& cache, ClimateTextures& climate,
              const world::ClimateField& field, GpuTerrain* pages = nullptr)
        : pages_(pages), cache_(cache), climate_(climate), climateField_(field) {}

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    GpuTerrain* pages_ = nullptr;
    engine::RenderPipeline* renderer_ = nullptr;
    engine::PipelineSlot pagePipeline_ = 0;
    engine::BindingSet pageBindings_ = engine::kNoBindings;
    engine::BindingSet pageFragmentBindings_ = engine::kNoBindings;
    const engine::MeshCache& cache_;
    ClimateTextures& climate_;
    const world::ClimateField& climateField_;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
    engine::BindingSet vertexBindings_ = engine::kNoBindings;
    engine::Texture surface_;
    engine::Sampler sampler_;
    engine::Buffer gridIndices_;
    std::uint32_t gridIndexCount_ = 0;
};

} // namespace game
