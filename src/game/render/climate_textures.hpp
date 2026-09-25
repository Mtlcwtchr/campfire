#pragma once
// The world's climate on the card: three pictures, uploaded once, read by
// whatever vertex stage needs them.
//
// It is here rather than inside the terrain pass because the water wants the
// same numbers. Water reads the air temperature, the moisture and the drainage
// to decide whether a river is frozen; it used to read them off the vertex,
// which is why the terrain vertex carried them through the water's pipeline
// too. One field, one upload, two passes - and the day a third wants it, it
// asks the same object.
//
// Owned by whatever owns the passes, created before the pipeline is built, and
// filled the first time a pass asks. It never changes afterwards: the climate
// is a function of the map, and the map does not move.

#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/render/device.hpp"
#include "game/world/climate_field.hpp"

namespace game {

class ClimateTextures {
public:
    // Uploads the field if it has not been uploaded. Safe to call from each
    // pass's setup; the second call does nothing.
    bool ensure(engine::Device& device, const world::ClimateField& climate);

    [[nodiscard]] bool ready() const { return sampler_ && planes_[0]; }
    // What a pass hands to RenderPipeline::takeVertex.
    [[nodiscard]] std::vector<SDL_GPUTextureSamplerBinding> bindings() const;

private:
    engine::Texture planes_[world::ClimateField::kPlanes];
    engine::Sampler sampler_;
};

} // namespace game
