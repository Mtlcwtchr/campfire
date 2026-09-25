#pragma once

#include <cstdint>
#include <variant>

#include <entt/entity/entity.hpp>

namespace sim::ai {

struct Observe { entt::entity entity = entt::null; std::int64_t elapsedTicks = 0; };
struct Sleep { entt::entity entity = entt::null; std::int64_t elapsedTicks = 1; };
struct Wander { entt::entity entity = entt::null; std::int64_t elapsedTicks = 1; };
using Intent = std::variant<Observe, Sleep, Wander>;

} // namespace sim::ai
