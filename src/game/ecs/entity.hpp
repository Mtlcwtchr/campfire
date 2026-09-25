#pragma once

#include <cstdint>

namespace sim::ecs {

enum class Kind : std::uint8_t { Person, Animal, Building, ResourceNode, ItemStack };
struct Identity { Kind kind{}; std::uint32_t legacyIndex = 0; };
struct Person {};
struct Animal {};
struct Building {};
struct ResourceNode {};
struct ItemStack {};
struct Selectable {};

} // namespace sim::ecs
