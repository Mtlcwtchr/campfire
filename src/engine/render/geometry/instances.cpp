#include "engine/render/geometry/instances.hpp"

#include <cstring>

namespace engine {

void InstanceArena::begin() {
    staging_.clear();
    instances_ = 0;
}

std::uint32_t InstanceArena::add(const void* data, std::size_t count, std::size_t stride) {
    if (data == nullptr || count == 0 || stride == 0) {
        return static_cast<std::uint32_t>(staging_.size());
    }
    // Aligned to the stride, because a vertex stream is read at whole strides
    // from wherever it is bound: an offset a byte out of step does not draw the
    // instance next door, it draws its bytes read as the wrong fields.
    const std::size_t at = (staging_.size() + stride - 1) / stride * stride;
    staging_.resize(at + count * stride);
    std::memcpy(staging_.data() + at, data, count * stride);
    instances_ += count;
    return static_cast<std::uint32_t>(at);
}

bool InstanceArena::upload(Device& device, Device::Uploader& uploader) {
    if (staging_.empty()) return true;
    if (staging_.size() > capacity_) {
        // Half again over what was asked for. A crowd that grows by one every
        // frame would otherwise mean a new buffer every frame, which is the one
        // thing this class exists to avoid.
        const std::size_t wanted = staging_.size() + staging_.size() / 2;
        SDL_GPUBufferCreateInfo info{};
        info.usage = usage_;
        info.size = static_cast<Uint32>(wanted);
        SDL_GPUBuffer* fresh = SDL_CreateGPUBuffer(device.handle(), &info);
        if (fresh == nullptr) return false;
        held_ = Buffer(device.handle(), fresh);
        capacity_ = wanted;
    }
    uploader.rewrite(held_.get(), staging_.data(), staging_.size());
    return true;
}

} // namespace engine
