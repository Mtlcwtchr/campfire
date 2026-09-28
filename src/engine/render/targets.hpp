#pragma once
// Where a frame is drawn before it reaches the window.
//
// Offscreen rather than straight into the swapchain, because the picture is
// wanted afterwards: a screenshot reads it back and anything that filters the
// whole frame reads it as a texture. A swapchain image can do neither portably.
//
// The runner owns these, not a pipeline: two pipelines drawing into the same
// frame have to be drawing into the same thing.

#include <SDL3/SDL.h>

#include <string>

#include "engine/render/device.hpp"

namespace engine {

class Targets {
public:
    bool resize(Device& device, Uint32 width, Uint32 height);
    // What the passes draw into. Multisampled when the card will do it.
    SDL_GPUTexture* colour() const { return colour_.get(); }
    SDL_GPUTexture* depth() const { return depth_.get(); }
    // Where the multisampled colour is folded down to one sample per pixel.
    // The same texture as `colour` when there is no multisampling, so anything
    // reading the finished picture reads this and does not care either way.
    SDL_GPUTexture* resolved() const { return resolve_ ? resolve_.get() : colour_.get(); }
    bool multisampled() const { return static_cast<bool>(resolve_); }
    // One-sample, sampled copy of the picture taken between stages.
    SDL_GPUTexture* grab() const { return grab_.get(); }
    Uint32 width() const { return width_; }
    Uint32 height() const { return height_; }
    void reset() {
        colour_.reset();
        depth_.reset();
        resolve_.reset();
        grab_.reset();
        width_ = height_ = 0;
    }

private:
    Texture colour_, depth_, resolve_, grab_;
    Uint32 width_ = 0, height_ = 0;
};

// Reads the colour target back and writes it out. Slow and synchronous, which
// is what a screenshot is.
bool saveTargetAsPng(Device& device, const Targets& targets, const std::string& path);

} // namespace engine
