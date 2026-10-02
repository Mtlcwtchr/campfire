#include "game/render/height_page_atlas.hpp"
#include "game/render/height_page_stream.hpp"

namespace game {

HeightPageAtlas::HeightPageAtlas(engine::Device& device, Layout layout, bool fields)
    : layout_(layout), residency_(layout.capacity()), entries_(layout.capacity()), withFields_(fields) {
    if (!device.handle()) {
        device.fail("height atlas requires an open GPU device");
        return;
    }
    SDL_GPUTextureCreateInfo texture{};
    texture.type = SDL_GPU_TEXTURETYPE_2D;
    texture.format = SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    texture.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    texture.width = layout_.width();
    texture.height = layout_.height();
    texture.layer_count_or_depth = 1;
    texture.num_levels = 1;
    texture.sample_count = SDL_GPU_SAMPLECOUNT_1;
    texture_ = device.makeTexture(texture);
    if (!texture_) return;
    if (withFields_) {
        texture.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
        texture.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
        texture.layer_count_or_depth = 4;
        fields_ = device.makeTexture(texture);
        if (!fields_) return;
    }

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
}

std::optional<HeightPageAtlas::Address> HeightPageAtlas::upload(
        engine::Device::Uploader& uploader, const world::streaming::BaseTile& page,
        std::uint64_t frame, bool replace) {
    if (!*this || !layout_.accepts(page)) return std::nullopt;
    const auto allocated = residency_.acquire(world::terrain::heightPageKey(page.key), frame);
    if (!allocated) return std::nullopt; // full of pinned pages; keep drawing the parent
    Entry& entry = entries_[allocated->slot];
    if (allocated->fresh || allocated->replacedKey) entry = {};
    if (entry.address && !replace) return entry.address;
    const Address address = layout_.address(allocated->slot, page);
    if (!uploader.refillRegion(texture_.get(), page.heightQuantized.data(),
                               address.x, address.y, layout_.storedSamples(),
                               layout_.storedSamples(), sizeof(std::uint16_t)))
        return std::nullopt;
    entry = {address, true};
    return address;
}

std::optional<HeightPageAtlas::Address> HeightPageAtlas::upload(
        engine::Device::Uploader& uploader, const PackedHeightPage& page, std::uint64_t frame, bool replace) {
    if (!page.source || !withFields_) return std::nullopt;
    for (const auto& plane : page.fields)
        if (plane.size() != page.source->base.heightQuantized.size() * 4) return std::nullopt;
    const auto address = upload(uploader, page.source->base, frame, replace);
    if (!address) return std::nullopt;
    auto& entry = entries_[address->slot];
    if (entry.fieldsReady) return address;
    for (std::uint32_t layer = 0; layer < page.fields.size(); ++layer)
        if (!uploader.refillRegion(fields_.get(), page.fields[layer].data(), address->x, address->y,
                                   layout_.storedSamples(), layout_.storedSamples(), 8, layer))
            return std::nullopt;
    entry.fieldsReady = true;
    entry.mayHaveWater = page.mayHaveWater;
    entry.surface = page.surface;
    return address;
}

void HeightPageAtlas::publishUploads(bool submitted) {
    for (Entry& entry : entries_) {
        if (!entry.pending) continue;
        if (!submitted) { entry.address.reset(); entry.fieldsReady = false; }
        entry.pending = false;
    }
}

std::optional<HeightPageAtlas::Address> HeightPageAtlas::find(Key key) const {
    if (!layout_.holds(key.level)) return std::nullopt;
    const auto slot = residency_.find(world::terrain::heightPageKey(key));
    if (!slot || entries_[*slot].pending || (withFields_ && !entries_[*slot].fieldsReady)) return std::nullopt;
    return entries_[*slot].address;
}

bool HeightPageAtlas::mayHaveWater(Key key) const {
    const auto address = find(key);
    return !address || !entries_[address->slot].fieldsReady || entries_[address->slot].mayHaveWater;
}

std::shared_ptr<const world::terrain::SurfacePage> HeightPageAtlas::surface(Key key) const {
    const auto address = find(key);
    return address ? entries_[address->slot].surface : nullptr;
}

bool HeightPageAtlas::pin(Key key, std::uint64_t frame) {
    if (!layout_.holds(key.level)) return false;
    const auto slot = residency_.find(world::terrain::heightPageKey(key));
    return slot && entries_[*slot].address &&
           residency_.pin(world::terrain::heightPageKey(key), frame);
}

bool HeightPageAtlas::hold(Key key) {
    if (!layout_.holds(key.level)) return false;
    const auto slot = residency_.find(world::terrain::heightPageKey(key));
    return slot && entries_[*slot].address && residency_.hold(world::terrain::heightPageKey(key));
}

bool HeightPageAtlas::unpin(Key key) {
    return layout_.holds(key.level) && residency_.unpin(world::terrain::heightPageKey(key));
}

void HeightPageAtlas::clear() {
    residency_ = world::terrain::PageResidency(layout_.capacity());
    for (Entry& entry : entries_) entry = {};
}

} // namespace game
