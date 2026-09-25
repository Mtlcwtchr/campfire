#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/fixed.hpp"

namespace sim::ecs {

struct ConstructionProgress {
    core::Fixed done{};
    core::Fixed required{};
    bool complete = false;
    std::uint8_t phase = 0;
};

struct DeliveredMaterials {
    std::vector<std::int32_t> values;
};

} // namespace sim::ecs
