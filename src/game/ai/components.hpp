#pragma once

#include <cstdint>

namespace sim::ai {

enum class Lod : std::uint8_t { Focus, Near, Mid, Far, Dormant };

struct Controlled {
    Lod lod = Lod::Near;
    std::int64_t nextThinkTick = 0;
};

enum class Decision : std::uint8_t { None, Sleep, Wander };
struct LastDecision {
    Decision kind = Decision::None;
    std::int64_t tick = 0;
    std::int64_t elapsedTicks = 0;
};

} // namespace sim::ai
