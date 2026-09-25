#include "game/simulation/pathfinder.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <queue>
#include <vector>

#include "game/simulation/world.hpp"

namespace sim {
namespace {

// Costs are integers so path selection is bit-identical everywhere. Every tile
// neighbour is exactly one step away, which is the whole point of the tile grid:
// there is no diagonal to approximate and no corner-cutting rule to get wrong.
constexpr std::int32_t kStep = 10;

struct OpenEntry {
    std::int32_t f;
    std::int32_t g;
    std::int32_t index;
    // The tie-break is on g then on index, so the frontier order never depends on
    // how the container happened to arrange equal keys.
    bool operator>(const OpenEntry& o) const {
        if (f != o.f) return f > o.f;
        if (g != o.g) return g < o.g;
        return index > o.index;
    }
};

// The planner runs up to a hundred searches per tick, so the open/closed sets are
// flat arrays reused across calls and invalidated by a generation stamp rather
// than reallocated. Hash maps here cost more than the search itself.
struct Scratch {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::uint32_t generation = 0;
    std::vector<std::uint32_t> stamp;
    std::vector<std::int32_t> best;
    std::vector<std::int32_t> cameFrom;
    std::vector<std::uint32_t> goalStamp;
    std::vector<OpenEntry> heapStorage;

    void prepare(std::int32_t w, std::int32_t h) {
        if (width != w || height != h) {
            width = w;
            height = h;
            const std::size_t n = std::size_t(w) * h;
            stamp.assign(n, 0);
            goalStamp.assign(n, 0);
            best.assign(n, 0);
            cameFrom.assign(n, -1);
            generation = 0;
        }
        ++generation;
        if (generation == 0) {   // wrapped; clear once rather than every call
            std::fill(stamp.begin(), stamp.end(), 0);
            std::fill(goalStamp.begin(), goalStamp.end(), 0);
            generation = 1;
        }
    }
};

Scratch& scratch() {
    static thread_local Scratch s;
    return s;
}

std::int32_t heuristic(core::TilePos a, core::TilePos b) {
    return kStep * core::tileDistance(a, b);
}

// Shared core. `goals` is the set of acceptable end tiles; `heuristicTarget` is
// what the heuristic aims at (the real goal, even when it is itself blocked).
PathResult search(const TileMap& map, core::TilePos from, core::TilePos heuristicTarget,
                  const std::vector<core::TilePos>& goals, std::int32_t maxExpanded) {
    PathResult result;
    if (goals.empty() || !map.inBounds(from)) return result;

    Scratch& sc = scratch();
    sc.prepare(map.width(), map.height());
    const std::uint32_t gen = sc.generation;
    const std::int32_t width = map.width();

    auto indexOf = [width](core::TilePos p) { return p.y * width + p.x; };
    auto tileOf = [width](std::int32_t i) { return core::TilePos{i % width, i / width}; };

    // Mark the goal set with the same generation stamp: an O(1) membership test.
    const std::uint32_t goalGen = gen;
    for (core::TilePos g : goals) {
        if (map.inBounds(g)) sc.goalStamp[indexOf(g)] = goalGen;
    }

    sc.heapStorage.clear();
    std::priority_queue<OpenEntry, std::vector<OpenEntry>, std::greater<>> open(std::greater<>{},
                                                                                std::move(sc.heapStorage));

    const std::int32_t startIndex = indexOf(from);
    sc.stamp[startIndex] = gen;
    sc.best[startIndex] = 0;
    sc.cameFrom[startIndex] = -1;
    open.push({heuristic(from, heuristicTarget), 0, startIndex});

    while (!open.empty()) {
        const OpenEntry cur = open.top();
        open.pop();
        if (sc.stamp[cur.index] != gen || cur.g > sc.best[cur.index]) continue;   // stale entry

        if (sc.goalStamp[cur.index] == goalGen) {
            result.found = true;
            for (std::int32_t i = cur.index; i != startIndex && i >= 0; i = sc.cameFrom[i])
                result.tiles.push_back(tileOf(i));
            std::reverse(result.tiles.begin(), result.tiles.end());
            sc.heapStorage.clear();
            return result;
        }

        if (++result.expanded > maxExpanded) return result;

        const core::TilePos curPos = tileOf(cur.index);
        for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
            const core::TilePos d = core::kNeighbourOffsets[static_cast<std::size_t>(dir)];
            const core::TilePos next{curPos.x + d.x, curPos.y + d.y};
            if (!map.inBounds(next)) continue;

            // No cutting corners. A diagonal step needs both of the cardinal
            // tiles it passes between to be open, or a wall with a corner in it
            // would not be a wall: bodies would slip through the join.
            if (core::isDiagonal(dir)) {
                const core::TilePos sideA{curPos.x + d.x, curPos.y};
                const core::TilePos sideB{curPos.x, curPos.y + d.y};
                if (!map.inBounds(sideA) || map.blocked(sideA)) continue;
                if (!map.inBounds(sideB) || map.blocked(sideB)) continue;
            }
            const std::int32_t nextIndex = indexOf(next);
            const bool isGoal = sc.goalStamp[nextIndex] == goalGen;
            if (map.blocked(next) && !isGoal) continue;

            // Terrain cost is a whole-number multiplier in tenths so totals stay
            // in integers and therefore identical on every machine.
            const std::int32_t terrainTenths =
                    static_cast<std::int32_t>((tileMoveCost(map.at(next)) * 10).roundToInt());
            const std::int32_t g = cur.g + kStep * std::max(terrainTenths, 1) / 10;

            if (sc.stamp[nextIndex] == gen && sc.best[nextIndex] <= g) continue;
            sc.stamp[nextIndex] = gen;
            sc.best[nextIndex] = g;
            sc.cameFrom[nextIndex] = cur.index;
            open.push({g + heuristic(next, heuristicTarget), g, nextIndex});
        }
    }
    return result;
}

} // namespace

PathResult findPath(const TileMap& map, core::TilePos from, core::TilePos to, std::int32_t maxExpanded) {
    if (from == to) { PathResult r; r.found = true; return r; }
    if (!map.inBounds(to) || map.blocked(to)) return {};
    return search(map, from, to, {to}, maxExpanded);
}

PathResult findPathAdjacent(const TileMap& map, core::TilePos from, core::TilePos goal, std::int32_t maxExpanded) {
    std::vector<core::TilePos> goals;
    goals.reserve(core::kNeighbourCount);
    for (const auto& d : core::neighbourOffsets(goal)) {
        const core::TilePos p{goal.x + d.x, goal.y + d.y};
        if (p == from) { PathResult r; r.found = true; return r; }   // already standing next to it
        if (map.inBounds(p) && !map.blocked(p)) goals.push_back(p);
    }
    if (goals.empty()) return {};
    return search(map, from, goal, goals, maxExpanded);
}

} // namespace sim
