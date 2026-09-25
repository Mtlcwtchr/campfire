#include "engine/render/targets.hpp"

#include <SDL3_image/SDL_image.h>

#include <cstring>

namespace engine {

bool Targets::resize(Device& device, Uint32 width, Uint32 height) {
    if (width == width_ && height == height_ && colour_) return true;
    // The old ones may still be in a frame the card has not finished, and
    // releasing a texture out from under a command buffer is a crash a long way
    // from here.
    SDL_WaitForGPUIdle(device.handle());
    colour_.reset();
    depth_.reset();
    resolve_.reset();

    // Four samples a pixel where the card will take them.
    //
    // Everything in this world is an edge: a hillside against the sky behind it,
    // a cliff line, the far side of a river. At one sample a pixel every one of
    // them is a staircase, and it is the staircase that reads as "not finished"
    // rather than anything about the ground itself.
    const SDL_GPUSampleCount samples = device.samples();

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = samples;
    if (samples == SDL_GPU_SAMPLECOUNT_1) info.usage |= SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colour_ = device.makeTexture(info);

    info.format = Device::kDepthFormat;
    // The completed depth target is consumed by the next frame's Hi-Z
    // occlusion pass. It remains a depth attachment for raster, and is also
    // exposed as a read-only compute texture after the render pass ends.
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
                 SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ;
    depth_ = device.makeTexture(info);

    if (samples != SDL_GPU_SAMPLECOUNT_1) {
        // Where the four samples are folded into one, and the only one anything
        // outside the render pass may read: a multisampled texture cannot be
        // blitted or copied.
        info.format = Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        resolve_ = device.makeTexture(info);
        if (!resolve_) {
            width_ = height_ = 0;
            return false;
        }
    }

    if (!colour_ || !depth_) {
        width_ = height_ = 0;
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

bool saveTargetAsPng(Device& device, const Targets& targets, const std::string& path) {
    if (!targets.colour()) return false;
    const Uint32 width = targets.width(), height = targets.height();
    const std::size_t bytes = static_cast<std::size_t>(width) * height * 4;

    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device.handle(), &info);
    if (!transfer) return false;

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.handle());
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureRegion source{targets.resolved(), 0, 0, 0, 0, 0, width, height, 1};
    SDL_GPUTextureTransferInfo destination{transfer, 0, width, height};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    SDL_WaitForGPUFences(device.handle(), true, &fence, 1);
    SDL_ReleaseGPUFence(device.handle(), fence);

    void* mapped = SDL_MapGPUTransferBuffer(device.handle(), transfer, false);
    SDL_Surface* surface = SDL_CreateSurface(static_cast<int>(width), static_cast<int>(height),
                                             SDL_PIXELFORMAT_ABGR8888);
    bool ok = false;
    if (surface && mapped) {
        std::memcpy(surface->pixels, mapped, bytes);
        ok = IMG_SavePNG(surface, path.c_str());
    }
    if (surface) SDL_DestroySurface(surface);
    SDL_UnmapGPUTransferBuffer(device.handle(), transfer);
    SDL_ReleaseGPUTransferBuffer(device.handle(), transfer);
    return ok;
}


} // namespace engine
