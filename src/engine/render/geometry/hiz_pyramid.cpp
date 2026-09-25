#include "engine/render/geometry/hiz_pyramid.hpp"

#include <algorithm>

#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {

bool HiZPyramid::ensure(Device& device, RenderPipeline& pipeline, std::uint32_t width,
                        std::uint32_t height) {
    if (!width || !height) return false;
    if (width == width_ && height == height_ && pyramid_[0] && pyramid_[1]) return true;

    SDL_WaitForGPUIdle(device.handle());
    pyramid_[0].reset();
    pyramid_[1].reset();
    scratchLevels_[0].clear();
    scratchLevels_[1].clear();
    width_ = width;
    height_ = height;
    levels_ = 1;
    // SDL/Metal use the conventional floor(log2(max-dimension))+1 mip
    // count. The physical mip dimensions are floor-halved as well.
    for (std::uint32_t side = std::max(width, height); side > 1; side /= 2)
        ++levels_;

    std::size_t values = 0;
    for (std::uint32_t mip = 0; mip < levels_; ++mip)
        values += std::size_t(std::max(1u, width >> mip)) * std::max(1u, height >> mip);
    const auto bytes = values * sizeof(float);
    pyramid_[0] = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                     SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, bytes);
    pyramid_[1] = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                     SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE, bytes);
    for (int set = 0; set < 2; ++set) {
        scratchLevels_[set].reserve(levels_);
        for (std::uint32_t mip = 0; mip < levels_; ++mip) {
            const std::size_t levelBytes =
                    std::size_t(std::max(1u, width >> mip)) *
                    std::size_t(std::max(1u, height >> mip)) * sizeof(float);
            scratchLevels_[set].push_back(device.makeBuffer(
                    SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                            SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                    levelBytes));
        }
    }
    if (!pyramid_[0] || !pyramid_[1] || scratchLevels_[0].size() != levels_ ||
        scratchLevels_[1].size() != levels_) {
        pyramid_[0].reset();
        pyramid_[1].reset();
        readIndex_ = -1;
        pendingIndex_ = -1;
        return false;
    }

    auto first = device.makeCompute({"hiz_build.hlsl", "BuildMip0CS"});
    auto reduce = device.makeCompute({"hiz_build.hlsl", "BuildMipCS"});
    auto copy = device.makeCompute({"hiz_build.hlsl", "CopyMipCS"});
    if (!first || !reduce || !copy) {
        pyramid_[0].reset();
        pyramid_[1].reset();
        readIndex_ = -1;
        pendingIndex_ = -1;
        return false;
    }
    first_ = pipeline.take(std::move(first));
    reduce_ = pipeline.take(std::move(reduce));
    copy_ = pipeline.take(std::move(copy));
    readIndex_ = -1;
    pendingIndex_ = -1;
    return true;
}

void HiZPyramid::recordBuild(const Frame& frame, RenderPipeline& pipeline) {
    if (!frame.device || !frame.depth || !pyramid_[0] || !pyramid_[1] || !levels_) return;
    // The build recorded by the preceding frame is now the only pyramid the
    // culler may consume. Publish it before scheduling this frame's build.
    if (pendingIndex_ >= 0) readIndex_ = pendingIndex_;
    const std::int32_t writeIndex = readIndex_ < 0 ? 0 : 1 - readIndex_;
    SDL_GPUBuffer* output = pyramid_[writeIndex].get();
    auto& scratch = scratchLevels_[writeIndex];

    ComputeDispatch first;
    first.pipeline = first_;
    first.groupsX = (width_ + 7) / 8;
    first.groupsY = (height_ + 7) / 8;
    first.storageReads = {frame.depth};
    first.writes = {scratch[0].get()};
    first.own[0] = float(width_);
    first.own[1] = float(height_);
    first.own[2] = 0;
    first.own[3] = 0;
    first.own[4] = 0;
    first.own[5] = 0;
    pipeline.postDispatch(std::move(first));

    std::uint32_t sourceWidth = width_, sourceHeight = height_;
    for (std::uint32_t mip = 1; mip < levels_; ++mip) {
        const std::uint32_t targetWidth = std::max(1u, sourceWidth / 2);
        const std::uint32_t targetHeight = std::max(1u, sourceHeight / 2);
        ComputeDispatch reduce;
        reduce.pipeline = reduce_;
        reduce.groupsX = (targetWidth + 7) / 8;
        reduce.groupsY = (targetHeight + 7) / 8;
        reduce.reads = {scratch[mip - 1].get()};
        reduce.writes = {scratch[mip].get()};
        reduce.own[0] = float(targetWidth);
        reduce.own[1] = float(targetHeight);
        reduce.own[2] = float(sourceWidth);
        reduce.own[3] = float(sourceHeight);
        // Each scratch level is its own allocation. The packed offsets only
        // exist in the copy pass below, so reductions always address from 0.
        reduce.own[4] = 0;
        reduce.own[5] = 0;
        pipeline.postDispatch(std::move(reduce));
        sourceWidth = targetWidth;
        sourceHeight = targetHeight;
    }
    for (std::uint32_t mip = 0; mip < levels_; ++mip) {
        ComputeDispatch copy;
        copy.pipeline = copy_;
        copy.groupsX = (std::max(1u, width_ >> mip) + 7) / 8;
        copy.groupsY = (std::max(1u, height_ >> mip) + 7) / 8;
        copy.reads = {scratch[mip].get()};
        copy.writes = {output};
        copy.own[0] = float(std::max(1u, width_ >> mip));
        copy.own[1] = float(std::max(1u, height_ >> mip));
        std::size_t targetBase = 0;
        for (std::uint32_t level = 0; level < mip; ++level)
            targetBase += std::size_t(std::max(1u, width_ >> level)) *
                          std::max(1u, height_ >> level);
        copy.own[2] = 0;
        copy.own[3] = 0;
        copy.own[4] = 0;
        copy.own[5] = float(targetBase);
        pipeline.postDispatch(std::move(copy));
    }
    pendingIndex_ = writeIndex;
}

} // namespace engine
