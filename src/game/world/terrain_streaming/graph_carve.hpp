#pragma once
// Cutting the ground to the water, from the graph.
//
// This replaces MacroWorld::carve and HeightField::waterProfile. Those decide
// what the ground and the water are at a point by rebuilding a procedural
// reach from the coarse cell underneath it, every time they are asked - which
// is why the answer could depend on which channel happened to be nearest to
// one sample, why a lake could stand at two heights, and why ninety per cent
// of what a page costs to bake is spent walking the same drainage again.
//
// Here the reaches are read from the persistent graph, gathered once for an
// area, and every sample inside it is answered from that. The ground and the
// water come out of the same call, so they cannot disagree: a bed is below
// its own surface by construction rather than by two functions arriving at
// the same number.
//
// Deterministic and page-independent: a sample's answer depends on the graph
// and on its own world coordinate, never on which page asked.

#include <cstdint>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"

namespace world::streaming {

// What the water leaves at a point, given what the country would be without
// it.
struct CarvedSample {
    // The ground, cut down to the valleys around it.
    core::Fixed floor{};
    // Hydraulic head, extended onto its dry bank for filtering. `wet` decides
    // whether there is water here; zero remains the default sea level.
    core::Fixed surface{};
    bool wet = false;
    // Standing water that owns the sample. A river is a reach, not a body.
    WaterBodyId body = kInvalidWaterBodyId;
    // The nearest reach, whether or not it holds water here, and how far
    // outside its bank the sample is: negative in the channel, zero at the
    // water's edge, positive across the flood plain.
    RiverId reach = kInvalidRiverId;
    core::Fixed bankDistance{};
    // Unit tangent of that reach, downstream.
    core::Fixed flowX{}, flowY{};
    // How much of the water here is the sea's rather than that reach's: nought
    // up a river, one past its mouth, eased between over the reach's last
    // stretch to the coast. Nought for a reach that does not run out to sea.
    core::Fixed estuary{};
};

// The reaches and bodies that can shape an area, gathered once.
//
// Worker-owned and read-only afterwards. Building one walks the graph's page
// index over the area grown by the halo; carving a sample walks only what
// that found, which for a page is a handful of reaches.
class GraphCarver {
public:
    // `standingWater` false leaves the lakes out: the ground and the running
    // water only, which is what a lake is refitted to (hydrology_builder).
    GraphCarver(const HydrologyGraph& graph, core::WorldRect area, std::int32_t haloMetres,
                bool standingWater = true);

    // `country` is what the coarse map says the ground stands at here and
    // `detail` is how far the detail layer has moved it. They are handed in
    // rather than sampled because the caller has just computed them, and
    // because the detail is not welcome everywhere: it is faded out towards a
    // channel, which is also what a flood plain is.
    [[nodiscard]] CarvedSample carve(core::WorldPos at, core::Fixed country,
                                     core::Fixed detail) const;

    [[nodiscard]] std::size_t reachCount() const { return reaches_.size(); }

private:
    struct Point {
        core::WorldPos position;
        core::Fixed surface, halfWidth, depth, valleyReach;
        core::Fixed toEnd{};   // along the course to its last point, metres
    };
    struct Reach {
        RiverId id = kInvalidRiverId;
        core::Fixed lowX, lowY, highX, highY;
        std::vector<Point> points;
        // Runs out to sea, and how far short of its last point (which stands
        // in the first sea cell) the coast is.
        bool toSea = false;
        core::Fixed coast{};
    };
    // Standing water over the macro lattice, so that restoring a basin is an
    // interpolation rather than a lookup. Read per cell it would put a
    // five-hundred-and-forty-metre terrace round every lake in the world.
    struct Basin {
        core::Fixed floor;   // the ground the flood covered, in metres
        core::Fixed level;   // the body's head
        WaterBodyId id = kInvalidWaterBodyId;
        // Natural basins only: the body whose footprint this node is on or
        // beside, and its head. The footprint dilated by one node, so the
        // ground can be told where a lake's water has to end a cell before the
        // footprint itself says so.
        core::Fixed shoreLevel;
        WaterBodyId shore = kInvalidWaterBodyId;
    };

    [[nodiscard]] const Basin* basinAt(std::int64_t cellX, std::int64_t cellY) const;
    // How much of the ground around this point is lake, and whose. One is
    // the middle of a body, nought is off it, and between is the rim.
    [[nodiscard]] core::Fixed coverAt(core::WorldPos at, WaterBodyId& body, core::Fixed& level,
                                      core::Fixed& floor) const;
    // The same over the dilated footprint: one everywhere the water may
    // stand and on the line where it has to stop, nought a cell beyond it.
    [[nodiscard]] core::Fixed shoreAt(core::WorldPos at, core::Fixed& level) const;

    // Which legs can reach which patch of ground.
    //
    // A query used to walk every reach in the window and, for any whose box it
    // fell inside, every leg of it. A course is finely stepped on purpose, so a
    // trunk river crossing the window is hundreds of legs and a point anywhere
    // near it paid for all of them - measured at three and a half microseconds
    // a height query, which is ninety milliseconds for one page of ground and
    // is most of why the landscape arrives late.
    //
    // A window is built once and asked thousands of times, so it is worth an
    // index. Each leg is filed under the cells its own reach can shape, and a
    // query reads one cell. The legs found are exactly the legs the walk would
    // have found; only the ones that were never going to match are skipped.
    struct LegRef { std::uint32_t reach, leg; };
    std::vector<LegRef> legIndex_;             // every cell's legs, end to end
    std::vector<std::uint32_t> legCellStart_;  // where each cell's run begins
    std::int64_t legFirstX_ = 0, legFirstY_ = 0, legWide_ = 0, legHigh_ = 0;
    static constexpr std::int64_t kLegCellMetres = 256;

    std::vector<Reach> reaches_;
    std::vector<Basin> basins_;   // firstCell-based grid, row major
    std::int64_t firstCellX_ = 0, firstCellY_ = 0;
    std::int64_t cellsWide_ = 0, cellsHigh_ = 0;
    core::Fixed cellMetres_{};
    bool naturalBasins_ = false;
};

} // namespace world::streaming
