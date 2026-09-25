#pragma once
// Compatibility vocabulary: the ECS core belongs to the engine.
#include "engine/ecs/spatial/components.hpp"

namespace sim::ecs {
using engine::ecs::kSpatialCellExtent;
using engine::ecs::Position;
using engine::ecs::SpatialCell;
using engine::ecs::Velocity;
using engine::ecs::Transform;
} // namespace sim::ecs
