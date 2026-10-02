#include "game/render/biome_textures.hpp"

#include "engine/biomes/shader_code.hpp"

namespace game {

bool BiomeTextures::ensure(engine::Device& device, const world::ClimateField& climate) {
    if (ready()) return true;
    if (!climate.ready()) return false;
    // Nearest and clamped: ids are never filtered, and the table is read at
    // its texel centres.
    SDL_GPUSamplerCreateInfo nearest{};
    nearest.min_filter = nearest.mag_filter = SDL_GPU_FILTER_NEAREST;
    nearest.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    nearest.address_mode_u = nearest.address_mode_v = nearest.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(nearest);
    if (!sampler_) return false;

    SDL_GPUTextureCreateInfo made{};
    made.type = SDL_GPU_TEXTURETYPE_2D;
    made.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    made.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    made.width = std::uint32_t(climate.wide());
    made.height = std::uint32_t(climate.high());
    made.layer_count_or_depth = 1;
    made.num_levels = 1;
    plane_ = device.makeTexture(made);
    made.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    made.width = std::uint32_t(engine::biomes::kTableColumns);
    made.height = std::uint32_t(engine::biomes::kTableRows);
    table_ = device.makeTexture(made);
    if (!plane_ || !table_) return false;
    {
        engine::Device::Uploader uploader(device);
        const std::vector<std::uint8_t> ids = climate.categoryPlane();
        uploader.refill(plane_.get(), ids.data(), std::uint32_t(climate.wide()), std::uint32_t(climate.high()), 4);
        uploader.finish();
    }
    return upload(device);
}

bool BiomeTextures::upload(engine::Device& device) {
    auto registry = engine::biomes::active();
    if (!registry) registry = engine::biomes::Registry::builtIn();
    const std::vector<float> table = engine::biomes::shaderTable(*registry);
    engine::Device::Uploader uploader(device);
    uploader.refill(table_.get(), table.data(), std::uint32_t(engine::biomes::kTableColumns),
                    std::uint32_t(engine::biomes::kTableRows), 16);
    uploader.finish();
    uploaded_ = engine::biomes::active();
    everUploaded_ = true;
    return true;
}

void BiomeTextures::refresh(engine::Device& device) {
    if (!ready()) return;
    if (everUploaded_ && engine::biomes::active() == uploaded_) return;
    upload(device);
}

std::vector<SDL_GPUTextureSamplerBinding> BiomeTextures::bindings() const {
    if (!ready()) return {};
    return {{plane_.get(), sampler_.get()}, {table_.get(), sampler_.get()}};
}

} // namespace game
