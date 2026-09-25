#pragma once

#include <cstdint>

#include "engine/core/fixed.hpp"

namespace sim::ecs {
struct Health { core::Fixed current = core::kOne; core::Fixed maximum = core::kOne; };
struct Hunger { core::Fixed value = core::kOne; core::Fixed rate{}; };
struct Thirst { core::Fixed value = core::kOne; core::Fixed rate{}; };
struct Fatigue { core::Fixed value = core::kOne; core::Fixed recoveryRate{}; };
struct SleepState { bool asleep = false; };
struct BodyTemperature { core::Fixed offsetC{}; };
struct AilmentState {
    std::uint8_t kind = 0;
    core::Fixed severity{};
    core::Fixed tended{};
    std::int64_t sinceTick = 0;
};
} // namespace sim::ecs
