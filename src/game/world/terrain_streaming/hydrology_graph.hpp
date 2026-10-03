#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::streaming {

// Bumped whenever the extraction changes what a stored graph means. It is
// folded into `sourceFingerprint`, so an old cache is a miss rather than a
// silently different river network.
inline constexpr std::uint32_t kHydrologyGraphVersion = 17; // fewer, smoother rivers: relaxed chains, continuous meander

// The spacing a natural basin ends up sampled at: flooded first over the
// Slopes lattice, then again over the ground as the reaches carve it, this
// finely (buildHydrologyGraph, refineLakesOnCarvedGround).
inline constexpr std::int32_t kNaturalBasinStep = 16;

using RiverId = std::uint32_t;
using RiverNodeId = std::uint32_t;
using WaterBodyId = std::uint32_t;

inline constexpr RiverId kInvalidRiverId = 0;
inline constexpr RiverNodeId kInvalidRiverNodeId = 0;
inline constexpr WaterBodyId kInvalidWaterBodyId = 0;
inline constexpr WaterBodyId kOceanWaterBodyId = 1;
// The shallow pools of the bogs (HeightField::bogPoolDepth): made when a page is
// baked, no body of the graph; anything asking is told it is a lake.
inline constexpr WaterBodyId kBogPoolWaterBodyId = 0xFFF0;

enum class RiverNodeKind : std::uint8_t {
    Source,
    Confluence,
    LakeInlet,
    LakeOutlet,
    Mouth,
    Terminal,
};

enum class WaterBodyKind : std::uint8_t { Ocean, Lake };

struct RiverNode {
    RiverNodeId id = kInvalidRiverNodeId;
    RiverNodeId downstream = kInvalidRiverNodeId;
    WaterBodyId waterBody = kInvalidWaterBodyId;
    std::int32_t macroCell = -1;
    RiverNodeKind kind = RiverNodeKind::Terminal;
    std::uint8_t order = 0;
    core::WorldPos position{};
    // Where the water stands here, in metres. Resolved in drainage order over
    // the whole map, so it never rises going downstream and every reach
    // meeting at a node shares one head. A water surface belongs to the
    // topology; deciding it per render vertex is how one lake came to have
    // two levels with a step where they met.
    core::Fixed surface{};
};

// One point of a reach's course, with everything the ground and the water
// around it are made of.
//
// The profile lives on the point rather than on the segment because a segment
// is a compressed chain: a river grows along it, and one width for the whole
// run would step at every junction and draw a brook the size of the trunk it
// joins. Interpolating between points is what keeps a valley continuous.
struct ReachPoint {
    core::WorldPos position{};
    // Where the water stands. Falls downstream and never rises: resolved over
    // the whole map, not per point.
    core::Fixed surface{};
    // Half the wet channel, how far the bed sits below the surface, and how
    // far out the ground is drawn down to the water.
    core::Fixed halfWidth{};
    core::Fixed depth{};
    core::Fixed valleyReach{};
};

// One compressed run of the drainage between two nodes. Nothing branches
// inside a segment: every cell along it has one course in and one course out,
// which is what makes the whole network addressable by segment.
struct RiverSegment {
    RiverId id = kInvalidRiverId;
    RiverNodeId from = kInvalidRiverNodeId;
    RiverNodeId to = kInvalidRiverNodeId;
    // Set when the segment leaves standing water, and when it ends in it. A
    // river between two channel nodes has neither.
    WaterBodyId sourceWaterBody = kInvalidWaterBodyId;
    WaterBodyId destinationWaterBody = kInvalidWaterBodyId;
    // Strahler order. A lake passes it through rather than restarting at one.
    std::uint8_t order = 0;
    // What the segment carries where it is largest, which is its downstream
    // end: the generator's accumulated rainfall units, not cubic metres, and
    // the least quantised measure of flow the macro map keeps.
    core::Fixed discharge{};
    // The wet channel at that same downstream end, in metres. A per-point width
    // belongs to the bank model that replaces MacroWorld's procedural carving;
    // this is the one number a spatial query and a budget need.
    core::Fixed width{};
    core::Fixed depth{};
    // How far from the middle this reach shapes the ground, and therefore how
    // far its water can stand: past it the ground belongs to some other
    // course, and a head applied beyond it floods a hillside that the water
    // never reaches. Without this bound a reach three hundred metres up a
    // slope drowns the valley below it.
    core::Fixed valleyReach{};
    // The centreline's extent grown by half the width: it covers the water and
    // nothing else. A valley query adds its own reach on top.
    core::WorldRect bounds{};
    // The course from the upstream node to the downstream one, both ends
    // included, so neighbouring segments share their junction point exactly.
    //
    // Shaped, not a chain of cell centres: the curve carries its direction
    // through a junction and the reach wanders across it, which is what keeps
    // the macro map's own lattice from showing through the rivers. Points are
    // spaced so a chord never cuts inside the channel's own bank.
    std::vector<ReachPoint> course;
    // The macro cells the reach was compressed from, upstream to downstream.
    // Provenance: the course no longer passes through their centres, so this
    // is the only record of which drainage a segment stands for.
    std::vector<std::int32_t> macroCells;
};

struct WaterBody {
    WaterBodyId id = kInvalidWaterBodyId;
    WaterBodyKind kind = WaterBodyKind::Lake;
    std::int32_t sourceRegion = -1;
    core::Fixed level{};
    // The first implementation retains the deterministic macro footprint, in
    // row-major macro-cell order. A precise shoreline is derived in
    // worker-local water tiles, not recreated independently by visual LODs.
    //
    // The ocean is the exception and carries none: its footprint is the macro
    // sea mask, and copying tens of thousands of cells into the one body that
    // every consumer can already identify buys nothing. The spatial index still
    // lists the ocean on every page it reaches, so a page query stays total.
    std::vector<core::TilePos> macroCells;
    // The ground under the water, per cell of that footprint, in metres.
    //
    // The flood raises a basin to its outlet and the macro map then stores
    // the brim, so a lake read from the map alone is a flat plateau standing
    // at its own waterline with nothing under it. The floor is what the flood
    // covered, and it is what makes a lake a lake rather than a lawn.
    std::vector<core::Fixed> macroCellFloor;
    // Connected submerged Slopes samples, not a mask used to sculpt the bed.
    // A nonzero step with an empty list means the proposed lake holds no water.
    std::int32_t basinStep = 0;
    std::vector<core::TilePos> basinSamples; // row-major, world coordinates / basinStep
    // Segments that end in this body, and segments that leave it. Ascending,
    // and each appears once.
    std::vector<RiverId> inlets;
    std::vector<RiverId> outlets;
};

// Storage is square and page-aligned even though request priority may be rings
// or a camera frustum. Lists are sorted and deduplicated at build time, making
// lookup deterministic and cache-friendly.
struct HydrologySpatialPage {
    TileKey key{};
    std::vector<RiverId> segments;
    std::vector<WaterBodyId> waterBodies;
};

// Generated once from the global macro relief and then treated as immutable by
// runtime terrain streaming. Runtime tiles query this graph spatially; they do
// not rerun accumulation or decide river topology independently.
//
// The three lists are dense and ordered by ID: element `n` has ID `n + 1`, and
// zero is the invalid ID. That is what makes a stored graph addressable without
// a map, and it is why IDs are handed out in a fixed macro-cell order rather
// than in whatever order a build happens to finish in.
struct HydrologyGraph {
    std::uint64_t worldSeed = 0;
    std::uint64_t sourceFingerprint = 0;
    std::int32_t macroWidth = 0;
    std::int32_t macroHeight = 0;
    std::int32_t macroCellMetres = 0;
    std::vector<RiverNode> nodes;
    std::vector<RiverSegment> segments;
    std::vector<WaterBody> waterBodies;
    std::vector<HydrologySpatialPage> spatialPages;
};

// `pages` is sorted by (y, x), so this is a logarithmic lookup without a
// mutable hash table or any order dependence.
const HydrologySpatialPage* findHydrologySpatialPage(const HydrologyGraph& graph,
                                                     TileKey key);

// Whether the graph holds together well enough to be used: dense IDs, every
// link resolving, pages ordered and their lists sorted and unique.
//
// Consumers rely on all of it - a page lookup binary-searches, an ID indexes
// straight into a vector - so a graph that fails this is a graph nothing may
// read. It is the check a loaded cache has to pass before it is believed,
// and `why` says what failed so a bad file can be reported rather than
// guessed at.
bool validateHydrologyGraph(const HydrologyGraph& graph, std::string* why = nullptr);

// ID to element, or nullptr for the invalid ID and for anything a truncated or
// foreign graph does not contain. Cheap enough to call per sample.
inline const RiverNode* riverNodeOf(const HydrologyGraph& graph, RiverNodeId id) {
    return id != kInvalidRiverNodeId && id <= graph.nodes.size() ? &graph.nodes[id - 1] : nullptr;
}
inline const RiverSegment* riverSegmentOf(const HydrologyGraph& graph, RiverId id) {
    return id != kInvalidRiverId && id <= graph.segments.size() ? &graph.segments[id - 1] : nullptr;
}
inline const WaterBody* waterBodyOf(const HydrologyGraph& graph, WaterBodyId id) {
    return id != kInvalidWaterBodyId && id <= graph.waterBodies.size()
                   ? &graph.waterBodies[id - 1]
                   : nullptr;
}

} // namespace world::streaming
