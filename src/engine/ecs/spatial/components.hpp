#pragma once

#include <cstdint>
#include "engine/core/geometry.hpp"

namespace engine::ecs {
inline constexpr std::int32_t kSpatialCellExtent = 32;
struct Position { core::WorldPos value{}; };
struct SpatialCell { std::int32_t x = 0; std::int32_t y = 0; };
struct Velocity { core::Fixed dx{}; core::Fixed dy{}; };
using Transform = Position;
} // namespace engine::ecs
