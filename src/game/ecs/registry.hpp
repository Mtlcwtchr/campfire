#pragma once
// Compatibility vocabulary: the ECS core belongs to the engine; this binds it
// to the game's own command variant.
#include "engine/ecs/registry.hpp"
#include "game/ecs/commands.hpp"
#include "game/ecs/scheduler.hpp"
#include "game/ecs/snapshot.hpp"

namespace sim::ecs {
using Registry = engine::ecs::Registry;
using EcsWorld = engine::ecs::EcsWorld<Command>;
} // namespace sim::ecs
