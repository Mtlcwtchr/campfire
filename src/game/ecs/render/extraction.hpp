#pragma once

#include <cstdint>
#include <vector>

#include <entt/entity/registry.hpp>

#include "game/ecs/render/components.hpp"
#include "game/ecs/spatial/components.hpp"
#include "game/render/presentation.hpp"

#include "game/ecs/registry.hpp"

namespace sim::ecs::render {

using RenderEntityId = game::render::RenderEntityId;
using SpriteBatchItem = game::render::SpriteBatchItem;
using MeshBatchItem = game::render::MeshBatchItem;
using Frame = game::render::Frame;

// ECS-side presentation adapter. It owns extraction and sorting; renderer
// receives Frame and never includes EnTT or accesses the simulation registry.
Frame extract(const entt::registry& registry);
Frame extract(const ::sim::ecs::EcsWorld& world);

} // namespace sim::ecs::render
