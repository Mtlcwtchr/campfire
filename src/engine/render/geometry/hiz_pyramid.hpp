#pragma once

#include <cstdint>
#include <vector>

#include "engine/pipeline/ids.hpp"
#include "engine/render/device.hpp"

namespace engine {

class RenderPipeline;
struct Frame;

// A previous-frame max-depth pyramid. Culling consumes the completed pyramid
// before the current render starts; the pyramid for the current depth target is
// recorded as post-render compute and becomes readable on the next frame.
class HiZPyramid {
public:
    bool ensure(Device& device, RenderPipeline& pipeline, std::uint32_t width,
                std::uint32_t height);
    void recordBuild(const Frame& frame, RenderPipeline& pipeline);

    [[nodiscard]] SDL_GPUBuffer* buffer() const {
        return readIndex_ >= 0 ? pyramid_[readIndex_].get() : nullptr;
    }
    [[nodiscard]] std::uint32_t width() const { return width_; }
    [[nodiscard]] std::uint32_t height() const { return height_; }
    [[nodiscard]] std::uint32_t levels() const { return levels_; }
    [[nodiscard]] bool valid() const { return buffer() != nullptr; }

private:
    // The packed buffers are what the culler reads. Build levels are separate
    // resources because Metal does not allow a structured buffer to be bound
    // as both t0 and u0 in the same compute pass. Each frame builds into one
    // scratch set, then copies its levels into the packed buffer.
    Buffer pyramid_[2];
    std::vector<Buffer> scratchLevels_[2];
    ComputeSlot first_ = 0xffff;
    ComputeSlot reduce_ = 0xffff;
    ComputeSlot copy_ = 0xffff;
    std::int32_t readIndex_ = -1;
    std::int32_t pendingIndex_ = -1;
    std::uint32_t width_ = 0, height_ = 0, levels_ = 0;
};

} // namespace engine
