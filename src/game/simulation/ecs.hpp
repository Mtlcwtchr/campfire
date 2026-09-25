#pragma once

// The ECS seam for simulation and presentation data.
// The legacy structs remain the serialized/domain representation during the
// migration. Components are free of SDL and renderer types.

#include <cstdint>

#include <entt/entity/registry.hpp>

#include "engine/core/fixed.hpp"
#include "game/ecs/commands.hpp"
#include "game/ecs/construction/components.hpp"
#include "game/ecs/entity.hpp"
#include "game/ecs/inventory/components.hpp"
#include "game/ecs/jobs/components.hpp"
#include "game/ecs/needs/components.hpp"
#include "game/ecs/render/components.hpp"
#include "game/ecs/registry.hpp"
#include "game/ecs/social/components.hpp"
#include "game/ecs/spatial/components.hpp"
#include "game/ecs/spatial/index.hpp"
#include "game/ecs/scheduler.hpp"

namespace sim::ecs {

using Registry = entt::registry;

} // namespace sim::ecs
