#include "game/render/climate_textures.hpp"

namespace game {

bool ClimateTextures::ensure(engine::Device& device, const world::ClimateField& climate) {
    if (ready()) return true;
    if (!climate.ready()) return false;

    // Clamped rather than repeated, because the world ends: a vertex of the
    // sea grid stands well outside the map and should read the coast it is
    // beside, not the far shore wrapped round. Linear, because the samples are
    // sixty-four metres apart and a nearest read of them would put a visible
    // step across the ground every sixty-four metres. No mip chain: this is
    // never minified below one texel to a vertex.
    SDL_GPUSamplerCreateInfo flat{};
    flat.min_filter = flat.mag_filter = SDL_GPU_FILTER_LINEAR;
    flat.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    flat.address_mode_u = flat.address_mode_v = flat.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(flat);
    if (!sampler_) return false;

    const auto wide = static_cast<std::uint32_t>(climate.wide());
    const auto high = static_cast<std::uint32_t>(climate.high());
    engine::Device::Uploader uploader(device);
    for (int plane = 0; plane < world::ClimateField::kPlanes; ++plane) {
        SDL_GPUTextureCreateInfo made{};
        made.type = SDL_GPU_TEXTURETYPE_2D;
        made.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        made.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        made.width = wide;
        made.height = high;
        made.layer_count_or_depth = 1;
        made.num_levels = 1;
        planes_[plane] = device.makeTexture(made);
        if (!planes_[plane]) return false;
        const std::vector<std::uint8_t> pixels = climate.plane(plane);
        uploader.refill(planes_[plane].get(), pixels.data(), wide, high, 4);
    }
    uploader.finish();
    return true;
}

std::vector<SDL_GPUTextureSamplerBinding> ClimateTextures::bindings() const {
    std::vector<SDL_GPUTextureSamplerBinding> out;
    if (!ready()) return out;
    out.reserve(world::ClimateField::kPlanes);
    for (const engine::Texture& plane : planes_) out.push_back({plane.get(), sampler_.get()});
    return out;
}

} // namespace game
