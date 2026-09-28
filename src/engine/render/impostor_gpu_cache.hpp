#pragma once
#include "engine/render/device.hpp"
#include "engine/render/impostor_cache.hpp"

namespace engine::render {
class ImpostorGpuCache {
public:
    static constexpr unsigned kSlots=8,kResolution=64;
    // Fixed at construction: the arrays are allocated once by setup() and a
    // slot is never resized. The byte budget bounds CPU atlases held by slots.
    explicit ImpostorGpuCache(unsigned slots=kSlots,unsigned resolution=kResolution,
                              std::size_t byteBudget=16*1024*1024,
                              ImpostorCache::Baker baker=bakeRecursiveImpostor);
    ImpostorCache cache;
    bool setup(Device& device);
    // Uploads at most maxViews complete views (all mips + depth) in one
    // submission, oldest ready slot first. Default: one view, <64 KiB at 64².
    bool upload(Device& device,unsigned maxViews=1);
    std::vector<SDL_GPUTextureSamplerBinding> bindings(SDL_GPUTextureSamplerBinding shadow) const;
    std::uint64_t uploadedViews() const { return uploadedViews_; }
    unsigned slots() const { return slots_; }
    unsigned resolution() const { return resolution_; }
    unsigned mipLevels() const { return mips_; }
    // Asset-load time only. Never called by collect()/frame upload.
    static std::shared_ptr<const ImpostorAtlas> loadLeaf(
        std::span<const std::array<std::filesystem::path,3>> paths,camera::Vec3 centre,double side);
private:
    unsigned slots_,resolution_,mips_;
    Texture colour_,normal_,depth_;
    Sampler linear_,nearest_;
    std::uint64_t uploadedViews_=0;
};
} // namespace engine::render
