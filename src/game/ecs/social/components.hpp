#pragma once
#include <cstdint>
#include "engine/core/ids.hpp"
namespace sim::ecs {
struct SettlementMember {
    std::uint32_t settlement = core::Handle<core::SettlementTag>::kInvalid;
};

struct HouseholdMember {
    std::uint32_t household = core::Handle<core::HouseholdTag>::kInvalid;
};

struct Parentage {
    std::uint32_t mother = core::Handle<core::PersonTag>::kInvalid;
    std::uint32_t father = core::Handle<core::PersonTag>::kInvalid;
};

struct Spouse {
    std::uint32_t person = core::Handle<core::PersonTag>::kInvalid;
};

struct Profession {
    std::uint8_t value = 0;
};

} // namespace sim::ecs
