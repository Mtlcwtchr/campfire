#pragma once

#include "engine/ecs/commands.hpp"

#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include <entt/entity/registry.hpp>
#include "engine/core/geometry.hpp"
#include "engine/core/ids.hpp"

namespace sim::ecs {

struct SetPosition { std::uint32_t entity = 0; core::WorldPos value{}; };
struct SetTransform {
    entt::entity entity = entt::null;
    core::WorldPos value{};
    core::TilePos tile{};
};
struct SetHealthComponent { entt::entity entity = entt::null; core::Fixed current{}; };
struct SetNeedsComponents {
    entt::entity entity = entt::null;
    core::Fixed hunger{};
    core::Fixed thirst{};
    core::Fixed fatigue{};
};
struct SetAnimalNeedsComponents {
    entt::entity entity = entt::null;
    core::Fixed hunger{};
    core::Fixed thirst{};
    core::Fixed fatigue{};
    bool asleep = false;
};
struct SetAgeComponent { entt::entity entity = entt::null; std::int32_t days = 0; };
struct SetAliveComponent { entt::entity entity = entt::null; bool value = true; };
struct SetGrazingTargetComponent { entt::entity entity = entt::null; core::TilePos tile{}; };
struct SetAnimalTimersComponent {
    entt::entity entity = entt::null;
    std::int64_t nextShearTick = 0;
    std::int64_t nextMilkTick = 0;
    std::int64_t nextBreedTick = 0;
};
struct AnimalBirthIntent {
    core::DefId definition{};
    core::TilePos at{};
    core::Handle<core::SettlementTag> owner;
};
struct AnimalDeathIntent { core::Handle<core::AnimalTag> animal; };
struct AdjustGrassIntent { core::TilePos tile{}; std::int32_t delta = 0; };
struct PlaceBlueprintIntent {
    core::DefId definition{};
    core::TilePos origin{};
    core::SettlementId settlement{};
    core::HouseholdId household{};
};
// Reservation intents are produced by worker/planner batches and resolved only
// at the deterministic commit boundary. The key is a shared resource/tile key;
// the smallest valid person id wins when several batches request it.
struct ReserveIntent { std::uint64_t key = 0; core::PersonId by{}; };
struct ReleaseReservationIntent { std::uint64_t key = 0; core::PersonId by{}; };
struct SetHealth { std::uint32_t entity = 0; core::Fixed current{}; };
struct DestroyEntity { std::uint32_t entity = 0; };
using Command = std::variant<SetPosition, SetTransform, SetHealthComponent, SetNeedsComponents,
                             SetAnimalNeedsComponents,
                             SetAgeComponent,
                             SetAliveComponent, SetGrazingTargetComponent, SetAnimalTimersComponent,
                             AnimalBirthIntent, AnimalDeathIntent, AdjustGrassIntent,
                             PlaceBlueprintIntent,
                             ReserveIntent, ReleaseReservationIntent,
                             SetHealth, DestroyEntity>;

// The buffer itself is the engine's; only what a command IS belongs here.
using CommandBuffer = engine::ecs::CommandBuffer<Command>;

} // namespace sim::ecs
