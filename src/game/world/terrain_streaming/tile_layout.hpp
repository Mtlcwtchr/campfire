#pragma once

#include <cstdint>

#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"
#include "game/world/terrain_streaming/base_tile.hpp"

namespace world::streaming {

// One storage page is large enough for contiguous IO and small enough to keep
// worker jobs bounded. At four metre sampling it contains 128x128 interior
// samples before padding.
inline constexpr std::int32_t kPageMetres = 512;
inline constexpr std::uint16_t kDefaultPaddingSamples = 2;

// How wide a page is at a sample spacing. Five hundred and twelve metres down
// to sixty-four metres a sample; past that a page is eight samples wide
// whatever its spacing - two kilometres at 256 m (level 6), eight at 1024 m
// (level 8) - so the coarse end of the pyramid is a few thousand pages for a
// whole continent, not the millions a 512 m page at every level would be.
// Every such page has H64's shape (nine samples a side), and is held beside
// it in the same atlas.
constexpr std::int32_t pageMetresForSpacing(std::int32_t sampleMetres) {
    return sampleMetres > 64 ? sampleMetres * 8 : kPageMetres;
}
constexpr std::int32_t pageMetresAtLevel(std::uint8_t level) {
    return pageMetresForSpacing(4 << level);   // world::kSampleMetres a step at level 0
}

constexpr TileKey tileAt(core::WorldPos position, std::uint8_t level = 0) {
    const std::int32_t metres = pageMetresAtLevel(level);
    return {static_cast<std::int32_t>(floorDiv(position.x.toInt(), metres)),
            static_cast<std::int32_t>(floorDiv(position.y.toInt(), metres)), level};
}

constexpr core::WorldPos tileOrigin(TileKey key) {
    const std::int64_t metres = pageMetresAtLevel(key.level);
    return {core::Fixed::fromInt(static_cast<std::int64_t>(key.x) * metres),
            core::Fixed::fromInt(static_cast<std::int64_t>(key.y) * metres)};
}

constexpr core::WorldRect tileBounds(TileKey key, std::int32_t haloMetres = 0) {
    const auto origin = tileOrigin(key);
    const auto halo = core::Fixed::fromInt(haloMetres);
    const auto side = core::Fixed::fromInt(pageMetresAtLevel(key.level));
    return {{origin.x - halo, origin.y - halo},
            {origin.x + side + halo, origin.y + side + halo}};
}

constexpr std::uint16_t interiorSamples(std::int32_t sampleMetres) {
    const std::int32_t page = pageMetresForSpacing(sampleMetres);
    return sampleMetres > 0 && page % sampleMetres == 0
                   ? static_cast<std::uint16_t>(page / sampleMetres + 1)
                   : 0;
}

constexpr std::uint16_t storedSamples(std::int32_t sampleMetres,
                                      std::uint16_t padding = kDefaultPaddingSamples) {
    const auto interior = interiorSamples(sampleMetres);
    return interior == 0 ? 0 : static_cast<std::uint16_t>(interior + padding * 2);
}

} // namespace world::streaming
