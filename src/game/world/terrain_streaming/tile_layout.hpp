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

constexpr TileKey tileAt(core::WorldPos position, std::uint8_t level = 0) {
    return {static_cast<std::int32_t>(floorDiv(position.x.toInt(), kPageMetres)),
            static_cast<std::int32_t>(floorDiv(position.y.toInt(), kPageMetres)), level};
}

constexpr core::WorldPos tileOrigin(TileKey key) {
    return {core::Fixed::fromInt(static_cast<std::int64_t>(key.x) * kPageMetres),
            core::Fixed::fromInt(static_cast<std::int64_t>(key.y) * kPageMetres)};
}

constexpr core::WorldRect tileBounds(TileKey key, std::int32_t haloMetres = 0) {
    const auto origin = tileOrigin(key);
    const auto halo = core::Fixed::fromInt(haloMetres);
    const auto side = core::Fixed::fromInt(kPageMetres);
    return {{origin.x - halo, origin.y - halo},
            {origin.x + side + halo, origin.y + side + halo}};
}

constexpr std::uint16_t interiorSamples(std::int32_t sampleMetres) {
    return sampleMetres > 0 && kPageMetres % sampleMetres == 0
                   ? static_cast<std::uint16_t>(kPageMetres / sampleMetres + 1)
                   : 0;
}

constexpr std::uint16_t storedSamples(std::int32_t sampleMetres,
                                      std::uint16_t padding = kDefaultPaddingSamples) {
    const auto interior = interiorSamples(sampleMetres);
    return interior == 0 ? 0 : static_cast<std::uint16_t>(interior + padding * 2);
}

} // namespace world::streaming
