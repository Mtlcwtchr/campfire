#pragma once
#include <cstdint>
#include <vector>
#include "engine/core/fixed.hpp"
#include "engine/core/ids.hpp"
namespace sim::ecs {
struct Carrying {
    std::uint32_t stack = core::Handle<core::ItemStackTag>::kInvalid;
};

struct EquippedTool {
    std::uint32_t stack = core::Handle<core::ItemStackTag>::kInvalid;
};

// Items currently worn by a person.  The list is intentionally separate from
// carrying/equipment: clothing can contain several independent stacks and is
// consumed by needs/render systems without opening a legacy Person aggregate.
struct WornItems {
    std::vector<std::uint32_t> stacks;
};

// Physical stack state kept separate from the ItemStack marker.  The fields use
// stable scalar/engine types so this component stays independent of simulation
// structs and can be read by worker systems without including World.
struct ItemStackState {
    std::uint32_t generation = 0;
    std::uint32_t definition = core::Handle<core::DefTag>::kInvalid;
    std::int32_t count = 0;
    std::uint8_t where = 0;
    std::int32_t tileX = 0;
    std::int32_t tileY = 0;
    std::uint32_t holder = core::Handle<core::PersonTag>::kInvalid;
    std::uint32_t building = core::Handle<core::BuildingTag>::kInvalid;
    std::uint32_t owner = core::Handle<core::SettlementTag>::kInvalid;
    std::int64_t freshnessRaw = core::kOne.raw;
    std::int64_t qualityRaw = core::kOne.raw;
    std::int32_t durabilityLeft = 0;
    bool alive = false;
};

} // namespace sim::ecs
