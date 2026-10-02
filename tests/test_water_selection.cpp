#include "framework.hpp"

#include <algorithm>

#include "game/world/terrain_streaming/hydrology_selection.hpp"

// Picking a piece of the water picks what drains into it, and nothing it
// drains into (hydrology_selection.hpp).
namespace {
using namespace world::streaming;
using core::Fixed;

// A hand-made network, nodes and segments numbered from one:
//
//   feeder(1) -> lake(2) -> outflow(2) --\
//                          brook(3) -----+-> trunk(4) -> mouth(5) -> ocean
//                                               ^
//                             side(6) ---------/ (joins the mouth reach)
//
// `side` runs into the mouth reach, downstream of the trunk.
HydrologyGraph network() {
    HydrologyGraph g;
    g.macroWidth = g.macroHeight = 64;
    g.macroCellMetres = 512;
    const auto at = [](int x, int y) { return core::WorldPos{Fixed::fromInt(x), Fixed::fromInt(y)}; };
    const auto node = [&](int x, int y) {
        RiverNode n;
        n.id = RiverNodeId(g.nodes.size() + 1);
        n.position = at(x, y);
        g.nodes.push_back(n);
        return n.id;
    };
    const RiverNodeId n1 = node(1000, 1000), n2 = node(3000, 1000), n3 = node(5000, 1000);
    const RiverNodeId n4 = node(3000, 3000), n5 = node(5000, 3000), n6 = node(9000, 3000);
    const RiverNodeId n7 = node(7000, 6000), n8 = node(7000, 3000), n9 = node(5000, 4000);
    WaterBody ocean; ocean.id = 1; ocean.kind = WaterBodyKind::Ocean;
    WaterBody lake; lake.id = 2; lake.kind = WaterBodyKind::Lake;
    lake.macroCells = {{5, 1}, {6, 1}};
    g.waterBodies = {ocean, lake};
    const auto segment = [&](RiverNodeId from, RiverNodeId to, WaterBodyId source, WaterBodyId into) {
        RiverSegment s;
        s.id = RiverId(g.segments.size() + 1);
        s.from = from; s.to = to;
        s.sourceWaterBody = source; s.destinationWaterBody = into;
        s.width = Fixed::fromInt(20);
        s.valleyReach = Fixed::fromInt(200);
        const auto& a = g.nodes[from - 1].position;
        const auto& b = g.nodes[to - 1].position;
        s.course = {ReachPoint{a}, ReachPoint{b}};
        s.bounds = {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}};
        s.macroCells = {std::int32_t(s.id) * 10};
        g.segments.push_back(s);
        return s.id;
    };
    const RiverId feeder = segment(n1, n2, 0, 2);   // into the lake
    const RiverId outflow = segment(n2, n5, 2, 0);  // out of the lake to the confluence
    const RiverId brook = segment(n4, n5, 0, 0);    // the other headwater
    const RiverId trunk = segment(n5, n9, 0, 0);    // below the confluence
    const RiverId mouth = segment(n9, n6, 0, 1);    // to the sea
    const RiverId side = segment(n7, n8, 0, 0);     // a stream of its own...
    g.segments[side - 1].to = n9;                    // ...joining the mouth reach
    g.waterBodies[1].inlets = {feeder};
    g.waterBodies[1].outlets = {outflow};
    (void)trunk; (void)mouth; (void)n3;
    return g;
}

bool has(const std::vector<std::uint32_t>& list, std::uint32_t id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}
} // namespace

TEST(water_selection_takes_the_tributaries_and_not_what_it_drains_into) {
    const auto g = network();
    // The trunk: both its headwaters, the lake one of them leaves, the lake's
    // own feeder - and not the mouth below it nor the stream joining there.
    const auto trunk = upstreamOfSegment(g, 4);
    CHECK_EQ(trunk.segments, (std::vector<RiverId>{1, 2, 3, 4}));
    CHECK_EQ(trunk.waterBodies, (std::vector<WaterBodyId>{2}));
    CHECK(!has(trunk.segments, 5));
    CHECK(!has(trunk.segments, 6));
    // The bounds cover every chosen reach with its valley, and the cells are
    // the reaches' and the lake's.
    CHECK(trunk.bounds.min.x <= Fixed::fromInt(1000 - 200));
    CHECK(trunk.bounds.max.x >= Fixed::fromInt(5000 + 200));
    CHECK(has(std::vector<std::uint32_t>(trunk.macroCells.begin(), trunk.macroCells.end()), 40));
    CHECK(has(std::vector<std::uint32_t>(trunk.macroCells.begin(), trunk.macroCells.end()), 1 * 64 + 5));

    // A brook picked is the brook alone: nothing drains into it.
    const auto brook = upstreamOfSegment(g, 3);
    CHECK_EQ(brook.segments, (std::vector<RiverId>{3}));
    CHECK(brook.waterBodies.empty());

    // The lake: itself and what fills it, not the river that leaves it.
    const auto lake = upstreamOfWaterBody(g, 2);
    CHECK_EQ(lake.waterBodies, (std::vector<WaterBodyId>{2}));
    CHECK_EQ(lake.segments, (std::vector<RiverId>{1}));

    // The mouth: the whole network above it.
    const auto mouth = upstreamOfSegment(g, 5);
    CHECK_EQ(mouth.segments, (std::vector<RiverId>{1, 2, 3, 4, 5, 6}));

    // The ocean is only the ocean.
    const auto sea = upstreamOfWaterBody(g, 1);
    CHECK_EQ(sea.waterBodies, (std::vector<WaterBodyId>{1}));
    CHECK(sea.segments.empty());
    CHECK(upstreamOfSegment(g, 99).empty());
}

TEST(water_pick_finds_the_reach_under_the_pointer_or_the_lake_around_it) {
    const auto g = network();
    // On the brook, a few metres off its line.
    const auto brook = pickWater(g, {Fixed::fromInt(4000), Fixed::fromInt(3020)});
    CHECK_EQ(brook.segment, RiverId(3));
    // Inside the lake's cells, away from any reach.
    const auto lake = pickWater(g, {Fixed::fromInt(5 * 512 + 100), Fixed::fromInt(512 + 400)});
    CHECK_EQ(lake.segment, kInvalidRiverId);
    CHECK_EQ(lake.body, WaterBodyId(2));
    // Dry ground.
    const auto dry = pickWater(g, {Fixed::fromInt(20000), Fixed::fromInt(20000)});
    CHECK_EQ(dry.segment, kInvalidRiverId);
    CHECK_EQ(dry.body, kInvalidWaterBodyId);
    // And the selection that goes with the brook's pick.
    CHECK_EQ(selectWater(g, {Fixed::fromInt(4000), Fixed::fromInt(3020)}).segments, (std::vector<RiverId>{3}));
}

