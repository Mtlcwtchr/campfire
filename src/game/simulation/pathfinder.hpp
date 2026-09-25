#pragma once
// A* over the tile grid. Bodies move continuously between tile centres, but they
// navigate on the grid (DECISIONS.md D2).

#include <vector>

#include "engine/core/geometry.hpp"

namespace sim {

class TileMap;

struct PathResult {
    bool found = false;
    std::vector<core::TilePos> tiles;   // excludes the start tile, ends at the goal
    std::int32_t expanded = 0;          // nodes popped, for budgeting
};

// Straight path to a walkable goal tile.
PathResult findPath(const TileMap& map, core::TilePos from, core::TilePos to,
                    std::int32_t maxExpanded = 20000);

// Budget for speculative searches during planning: a pawn tries several candidate
// jobs per tick, and a target it cannot reach within this many expansions is not
// worth more time than the next candidate.
inline constexpr std::int32_t kPlanningExpansionBudget = 2500;

// Path to any walkable tile orthogonally or diagonally adjacent to the goal.
// Used for everything a pawn works on rather than stands on - trees, walls, a
// building under construction - which are usually blocked tiles themselves.
PathResult findPathAdjacent(const TileMap& map, core::TilePos from, core::TilePos goal,
                            std::int32_t maxExpanded = 20000);

} // namespace sim
