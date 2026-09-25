#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"

namespace world::streaming {

// Storage and cache address. Pages are always square and world aligned; rings
// are a scheduling policy layered on top of these pages.
struct TileKey {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint8_t level = 0;
    friend constexpr bool operator==(TileKey a, TileKey b) {
        return a.x == b.x && a.y == b.y && a.level == b.level;
    }
};

enum class ResidualLevel : std::uint8_t { Large, Medium, Fine };

// Immutable gameplay/topology data shared by every visual LOD. The arrays use
// row-major samples with a padded border. Derived render values (normal,
// slope, material weights and wetness) are deliberately not stored here.
struct BaseTile {
    TileKey key{};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t padding = 0;
    std::int32_t sampleMetres = 4;
    core::Fixed elevationMin{};
    core::Fixed elevationMax{};
    std::vector<std::uint16_t> heightQuantized;
    std::vector<std::uint16_t> waterBodyId;
    std::vector<std::uint16_t> watershedId;
    std::vector<std::uint8_t> buildability;
    std::vector<std::uint8_t> walkability;

    [[nodiscard]] std::size_t sampleCount() const {
        return (static_cast<std::size_t>(width) + padding * 2u) *
               (static_cast<std::size_t>(height) + padding * 2u);
    }
    [[nodiscard]] bool valid() const {
        const auto count = sampleCount();
        return width > 0 && height > 0 && sampleMetres > 0 &&
               heightQuantized.size() == count &&
               (waterBodyId.empty() || waterBodyId.size() == count) &&
               (watershedId.empty() || watershedId.size() == count) &&
               (buildability.empty() || buildability.size() == count) &&
               (walkability.empty() || walkability.size() == count);
    }
};

// Visual height detail is cumulative over BaseTile. A missing residual never
// removes the base surface; it only lowers the available visual detail.
struct ResidualTile {
    TileKey key{};
    ResidualLevel level = ResidualLevel::Large;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t padding = 0;
    std::int32_t sampleMetres = 8;
    std::vector<std::int16_t> deltaQuantized;

    [[nodiscard]] std::size_t sampleCount() const {
        return (static_cast<std::size_t>(width) + padding * 2u) *
               (static_cast<std::size_t>(height) + padding * 2u);
    }
    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0 && sampleMetres > 0 &&
               deltaQuantized.size() == sampleCount();
    }
};

} // namespace world::streaming

template <> struct std::hash<world::streaming::TileKey> {
    std::size_t operator()(world::streaming::TileKey key) const noexcept {
        const auto packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.x)) << 32) ^
                            static_cast<std::uint32_t>(key.y);
        return std::hash<std::uint64_t>{}(packed ^ (std::uint64_t(key.level) << 56));
    }
};
