#pragma once
// One R16 atlas per data level. Uploads consume immutable PageStore results;
// this object never bakes or waits for a page on the frame thread.

#include <optional>
#include <vector>

#include "engine/render/device.hpp"
#include "game/world/terrain_height_pages.hpp"
#include "game/world/terrain_residency.hpp"
#include "game/world/terrain_adaptive.hpp"

namespace game {

struct PackedHeightPage;

class HeightPageAtlas {
public:
    using Address = world::terrain::HeightPageAddress;
    using Layout = world::terrain::HeightPageLayout;
    using Key = world::streaming::TileKey;

    HeightPageAtlas(engine::Device& device, Layout layout, bool fields = false);
    explicit operator bool() const { return bool(texture_) && bool(sampler_) && (!withFields_ || bool(fields_)); }
    [[nodiscard]] const Layout& layout() const { return layout_; }
    [[nodiscard]] SDL_GPUTextureSamplerBinding binding() const {
        return {texture_.get(), sampler_.get()};
    }
    [[nodiscard]] SDL_GPUTextureSamplerBinding fieldBinding() const {
        return {fields_.get(), sampler_.get()};
    }
    [[nodiscard]] std::optional<Address> upload(engine::Device::Uploader& uploader,
                                               const PackedHeightPage& page, std::uint64_t frame);

    // Pin the draw's complete working set (including morph parents/children)
    // before acquiring other pages. Queued pages can be pinned too. Never cycle
    // an atlas: cycling a partial upload would lose its other resident pages.
    [[nodiscard]] std::optional<Address> upload(engine::Device::Uploader& uploader,
                                               const world::streaming::BaseTile& page,
                                               std::uint64_t frame);
    // Call after uploader.finish(), with its result, on every atlas in that
    // batch. Only successfully submitted pages become visible through find().
    // Returned upload addresses are provisional until this publication.
    void publishUploads(bool submitted);
    [[nodiscard]] std::optional<Address> find(Key key) const;
    // Missing, provisional and height-only pages cannot prove dry land.
    [[nodiscard]] bool mayHaveWater(Key key) const;
    std::shared_ptr<const world::terrain::SurfacePage> surface(Key key) const;
    bool pin(Key key, std::uint64_t frame);
    bool unpin(Key key);
    // A different world/version must not reuse the old world's page keys.
    void clear();

private:
    struct Entry {
        std::optional<Address> address;
        bool pending = false;
        bool fieldsReady = false;
        bool mayHaveWater = true;
        std::shared_ptr<const world::terrain::SurfacePage> surface;
    };
    Layout layout_;
    world::terrain::PageResidency residency_;
    std::vector<Entry> entries_;
    engine::Texture texture_;
    engine::Texture fields_;
    engine::Sampler sampler_;
    bool withFields_ = false;
};

} // namespace game
