#pragma once
// The terrain categories on the card (engine/biomes, terrain_biomes.hlsli):
// the category plane - four raw ids a climate sample, read nearest - and the
// biome table, every number of the libraries a row each.
//
// The plane is the climate's fourth picture and never changes after the map
// is raised. The table is refilled whenever the active registry is swapped
// (a live edit of content/config/terrain): numbers reach the shader without
// a recompile.
#include <memory>
#include <vector>

#include <SDL3/SDL.h>

#include "engine/biomes/registry.hpp"
#include "engine/render/device.hpp"
#include "game/world/climate_field.hpp"

namespace game {

class BiomeTextures {
public:
    // Uploads the plane and the table if they are not there. Safe to call
    // from every pass's setup; the second call does nothing.
    bool ensure(engine::Device& device, const world::ClimateField& climate);
    // Refills the table when the active registry is not the one uploaded.
    void refresh(engine::Device& device);

    [[nodiscard]] bool ready() const { return plane_ && table_ && sampler_; }
    // The category plane, then the table: what a fragment stage binds after
    // its own textures (BIOME_PLANE_SLOT, BIOME_TABLE_SLOT).
    [[nodiscard]] std::vector<SDL_GPUTextureSamplerBinding> bindings() const;

private:
    bool upload(engine::Device& device);
    engine::Texture plane_, table_;
    engine::Sampler sampler_;
    std::shared_ptr<const engine::biomes::Registry> uploaded_;
    bool everUploaded_ = false;
};

} // namespace game
