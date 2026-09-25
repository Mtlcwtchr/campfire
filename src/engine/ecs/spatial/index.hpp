#pragma once

#include <cstdint>
#include <functional>

#include "engine/core/geometry.hpp"

namespace engine::ecs {

struct CellId {
    std::int32_t x = 0;
    std::int32_t y = 0;
    friend constexpr bool operator==(CellId a, CellId b) { return a.x == b.x && a.y == b.y; }
};

// Spatial ownership is independent of the camera's ring mesh. Negative
// coordinates use mathematical floor so cells remain stable across the origin.
inline CellId cellForTile(core::TilePos tile, std::int32_t extent) {
    const auto floorDiv = [extent](std::int32_t value) {
        const std::int32_t q = value / extent;
        const std::int32_t r = value % extent;
        return r < 0 ? q - 1 : q;
    };
    return {floorDiv(tile.x), floorDiv(tile.y)};
}

} // namespace engine::ecs

template <> struct std::hash<engine::ecs::CellId> {
    std::size_t operator()(engine::ecs::CellId id) const noexcept {
        const auto x = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id.x));
        const auto y = static_cast<std::uint64_t>(static_cast<std::uint32_t>(id.y));
        return static_cast<std::size_t>((x << 32) ^ y);
    }
};
