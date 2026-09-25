#pragma once

#include <cstdint>

#include "engine/core/geometry.hpp"

namespace sim::ecs {

// Calendar age is per-entity state. Keeping it separate from species definition
// and health lets daily ageing run in the same disjoint animal batches as needs.
struct Age {
    std::int32_t days = 0;
};

struct Alive {
    bool value = true;
};

struct GrazingTarget {
    core::TilePos tile{};
};

struct AnimalTimers {
    std::int64_t nextShearTick = 0;
    std::int64_t nextMilkTick = 0;
    std::int64_t nextBreedTick = 0;
};

} // namespace sim::ecs
