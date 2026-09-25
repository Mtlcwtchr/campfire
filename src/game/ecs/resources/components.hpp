#pragma once

#include <cstdint>

#include "engine/core/fixed.hpp"

namespace sim::ecs {

struct ResourceState {
    core::Fixed workDone{};
    bool depleted = false;
    std::int64_t regrowAtTick = 0;
};

} // namespace sim::ecs
