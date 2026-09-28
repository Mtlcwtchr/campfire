#include "engine/render/impostor_gpu_cache.hpp"
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace engine::render {
namespace {
unsigned levelsOf(unsigned resolution) {
    unsigned levels=1;while ((resolution>>levels)>0) ++levels;return levels;
}
}
ImpostorGpuCache::ImpostorGpuCache(unsigned slots,unsigned resolution,std::size_t byteBudget,ImpostorCache::Baker baker)
    : cache(slots,byteBudget,{},std::move(baker)),slots_(slots),resolution_(resolution),mips_(levelsOf(resolution)) {
    if (resolution<4 || resolution>512 || (resolution&(resolution-1)) || slots*21>2048)
        throw std::invalid_argument("invalid impostor GPU cache shape");
}
bool ImpostorGpuCache::setup(Device& device) {
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;info.format=Device::kColourFormat;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;info.width=info.height=resolution_;
    info.layer_count_or_depth=slots_*21;info.num_levels=mips_;
    colour_=device.makeTexture(info);normal_=device.makeTexture(info);
    info.num_levels=1;depth_=device.makeTexture(info);
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter=sampler.mag_filter=SDL_GPU_FILTER_LINEAR;sampler.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u=sampler.address_mode_v=sampler.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.max_lod=float(mips_-1);linear_=device.makeSampler(sampler);
    sampler.min_filter=sampler.mag_filter=SDL_GPU_FILTER_NEAREST;sampler.max_lod=0;nearest_=device.makeSampler(sampler);
    return colour_ && normal_ && depth_ && linear_ && nearest_;
}
bool ImpostorGpuCache::upload(Device& device,unsigned maxViews) {
    // Collected first and published after finish(): a slot's fence serial is
    // the submission that carries its bytes, so nothing is marked early.
    struct Part {std::size_t slot;unsigned views;};
    std::vector<Part> parts;bool ok=true;unsigned budget=std::max(1u,maxViews);
    const auto& slots=cache.slots();
    std::vector<std::size_t> order;
    for (std::size_t i=0;i<slots.size();++i) {
        const auto& slot=slots[i];
        if ((slot.state==ImpostorCache::State::Ready || slot.state==ImpostorCache::State::Uploading) &&
            slot.lastSubmission<=cache.completed()) order.push_back(i);
    }
    // Finish what is half uploaded before starting another: a slot is only
    // drawable once all 21 views are there.
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return slots[a].uploadedViews>slots[b].uploadedViews;});
    if (order.empty()) return true;
    Device::Uploader upload(device);
    for (const auto candidate:order) {
        if (!budget) break;
        const auto& slot=slots[candidate];const auto& atlas=*slot.atlas;
        if (atlas.resolution!=resolution_ || atlas.colourMips.size()!=mips_-1 || atlas.normalMips.size()!=mips_-1) {
            cache.uploaded(candidate,1,false);ok=false;continue;
        }
        const unsigned count=std::min(budget,21-slot.uploadedViews);
        for (unsigned view=slot.uploadedViews;view<slot.uploadedViews+count;++view) {
            const unsigned layer=unsigned(candidate)*21+view;
            for (unsigned mip=0,side=resolution_;mip<mips_;++mip,side/=2) {
                const auto offset=std::size_t(view)*side*side;
                const auto& colour=mip?atlas.colourMips[mip-1]:atlas.colour;
                const auto& normal=mip?atlas.normalMips[mip-1]:atlas.normal;
                ok=upload.refillRegion(colour_.get(),colour.data()+offset,0,0,side,side,4,layer,mip,false) && ok;
                ok=upload.refillRegion(normal_.get(),normal.data()+offset,0,0,side,side,4,layer,mip,false) && ok;
            }
            ok=upload.refillRegion(depth_.get(),atlas.depth.data()+std::size_t(view)*resolution_*resolution_,
                0,0,resolution_,resolution_,4,layer,0,false) && ok;
        }
        parts.push_back({candidate,count});budget-=count;
    }
    const bool finished=upload.finish();ok=finished && ok;
    for (const auto& part:parts) {
        cache.uploaded(part.slot,part.views,finished,device.nextSubmission());
        if (finished) uploadedViews_+=part.views;
    }
    return ok;
}
std::vector<SDL_GPUTextureSamplerBinding> ImpostorGpuCache::bindings(SDL_GPUTextureSamplerBinding shadow) const {
    return {{colour_.get(),linear_.get()},{normal_.get(),linear_.get()},shadow,{depth_.get(),nearest_.get()}};
}
std::shared_ptr<const ImpostorAtlas> ImpostorGpuCache::loadLeaf(
        std::span<const std::array<std::filesystem::path,3>> paths,camera::Vec3 centre,double side) {
    if (paths.size()!=21 || !(side>0) || !std::isfinite(side)) return {};
    auto atlas=std::make_shared<ImpostorAtlas>();atlas->resolution=128;atlas->side=side;atlas->centre=centre;
    atlas->error=2*side/atlas->resolution+0.054*side;atlas->members=1;
    const auto pixels=std::size_t(128)*128;
    atlas->colour.resize(pixels*21);atlas->normal.resize(pixels*21);atlas->depth.resize(pixels*21);
    for (unsigned view=0;view<21;++view) for (unsigned channel=0;channel<3;++channel) {
        std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)> image(Device::loadDataPng(paths[view][channel]),SDL_DestroySurface);
        if (!image || image->w!=image->h || image->w<128) return {};
        std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)> rgba(SDL_ConvertSurface(image.get(),SDL_PIXELFORMAT_RGBA32),SDL_DestroySurface);
        if (!rgba) return {};
        auto& target=channel==0?atlas->colour:channel==1?atlas->normal:atlas->depth;
        for (unsigned y=0;y<128;++y) for (unsigned x=0;x<128;++x) {
            const unsigned sx=(2*x+1)*unsigned(rgba->w)/256,sy=(2*y+1)*unsigned(rgba->h)/256;
            std::memcpy(target[view*pixels+y*128+x].data(),static_cast<const char*>(rgba->pixels)+sy*rgba->pitch+sx*4,4);
        }
    }
    for (std::size_t i=0;i<atlas->depth.size();++i) {
        if (atlas->depth[i][3]!=atlas->colour[i][3] || (atlas->depth[i][3]>=51 && atlas->depth[i][2]!=i/pixels)) {
            SDL_SetError("impostor source mismatch at %zu: view=%u expected=%zu alpha=%u/%u",i,
                unsigned(atlas->depth[i][2]),i/pixels,unsigned(atlas->depth[i][3]),unsigned(atlas->colour[i][3]));
            return {};
        }
    }
    return atlas;
}
} // namespace engine::render
