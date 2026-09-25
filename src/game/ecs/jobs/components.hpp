#pragma once
#include <cstdint>

#include "engine/core/geometry.hpp"
#include "engine/core/fixed.hpp"
namespace sim::ecs {
struct Job { std::uint8_t kind = 0; std::uint8_t phase = 0; bool active = false; };
struct JobCategory { std::uint8_t value = 0; };
struct JobProgress {
    core::Fixed done{};
    core::Fixed required{};
};

// References owned by an assignment. Kept separate from kind/phase and
// progress so systems can query only the relationship they need.
struct JobLinks {
    std::uint32_t stack = core::Handle<core::ItemStackTag>::kInvalid;
    std::uint32_t building = core::Handle<core::BuildingTag>::kInvalid;
    std::uint32_t node = core::Handle<core::ResourceNodeTag>::kInvalid;
    std::uint32_t animal = core::Handle<core::AnimalTag>::kInvalid;
    std::uint32_t person = core::Handle<core::PersonTag>::kInvalid;
    core::TilePos target{};
};
using JobState = Job;
} // namespace sim::ecs
