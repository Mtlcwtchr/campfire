#include "engine/core/progress.hpp"
#include <cstdio>
#include <chrono>
#include "game/world/terrain_streaming/hydrology_builder.hpp"

#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_cache.hpp"
#include <string>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <algorithm>
#include <map>
#include <unordered_map>
#include <queue>
#include <limits>
#include <utility>
#include <vector>

#include <stdexcept>

#include "engine/core/rng.hpp"
#include "engine/core/lane_hash.hpp"
#include <type_traits>
#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/world/coords.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

// Turning the macro map's drainage flags into a network that can be stored.
//
// The coarse generator already did the global hydrology: it flooded the
// depressions, routed every cell downhill, accumulated the rain and decided
// which of those courses carries enough of it to be a river. What it left
// behind is per-cell flags, and a flag is not a topology - nothing in it says
// where one river ends and the next begins, which lake a course runs into, or
// that two streams meeting are one confluence rather than two coincidences.
//
// So every consumer worked it out again. MacroWorld::attach re-interprets the
// whole map on every worker; MacroWorld::channelOf rebuilds a reach from the
// cell under the query; HeightField::lakesAt decides lake membership at the
// sample being drawn. That is why a lake could have two levels and a river
// could change its route with the camera: topology was being derived at query
// time by code that could only see a neighbourhood.
//
// This derives it once, from data that has no camera in it, and hands back
// something a cache can store: nodes where the network actually branches,
// segments between them, water bodies with one ID and one level each, and a
// page index so a terrain tile can ask what water reaches it without walking
// the world.
//
// Two things are deliberately not here. Sizing still comes from MacroWorld,
// which owns the tuned width/depth model - copying it would give the migration
// two answers to compare and no way to tell which one moved. And the graph is
// the *river* network: minor dry drainage stays in the macro map, because a
// gully that a camera happens to reach is not a water body.

namespace world::streaming {
namespace {

using core::Fixed;
using core::TilePos;
using core::WorldPos;

// Deterministic 64-bit mixing over everything the extraction reads. A cache
// entry built from a different macro map has to be a miss, and comparing two
// worlds cell by cell to find that out is not something a load path can do.
constexpr std::uint64_t mixIn(std::uint64_t hash, std::uint64_t value) {
    return core::splitmix64(hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2)));
}

WorldPos centreOf(std::int32_t x, std::int32_t y) {
    // The same centre MacroWorld builds its reaches between, so a graph
    // centreline and a legacy channel describe one course rather than two.
    const Fixed perCell = Fixed::fromInt(generation::kMetresPerCell);
    const Fixed half = perCell / Fixed::fromInt(2);
    return {Fixed::fromInt(x) * perCell + half, Fixed::fromInt(y) * perCell + half};
}

std::uint64_t fingerprintOf(const generation::WorldMapData& world) {
    std::uint64_t hash = mixIn(0x243f6a8885a308d3ull, kHydrologyGraphVersion);
    hash = mixIn(hash, world.seed);
    hash = mixIn(hash, static_cast<std::uint64_t>(world.width));
    hash = mixIn(hash, static_cast<std::uint64_t>(world.height));
    hash = mixIn(hash, static_cast<std::uint64_t>(generation::kMetresPerCell));
    hash = mixIn(hash, static_cast<std::uint64_t>(generation::kMetresPerElevationStep));
    if (world.hybridTerrain) hash = mixIn(hash, world.hybridTerrain->fingerprint());
    // The cache key, not the canonical FNV identity: the same ground gives the
    // same key, and it costs tens of milliseconds rather than hundreds.
    if (world.terrainFoundation) hash = mixIn(hash, world.terrainFoundation->contentKey());
    // Every value the extraction reads, four lanes at a time (core::LaneHash):
    // a splitmix per cell was a quarter of a second on the reference world.
    core::LaneHash cells(hash);
    cells.addAll(world.cells, [](const generation::WorldCell& cell) {
        // Moisture is in here because it decides what counts as a stream, and
        // so reaches the width of every channel on the map.
        return (std::uint64_t(cell.elevation) << 40) |
               (std::uint64_t(cell.drainSize) << 32) |
               (std::uint64_t(std::uint8_t(cell.drainOut)) << 24) |
               (std::uint64_t(std::uint8_t(cell.riverOut)) << 16) |
               (std::uint64_t(cell.moisture) << 8) |
               (cell.sea ? 2u : 0u) | (cell.river ? 1u : 0u);
    });
    // The sparse fields chunk by chunk: a chunk the field does not hold reads
    // as its fill and costs one word, so the sea costs next to nothing.
    const auto field = [&cells](const auto& values) {
        using T = typename std::remove_cvref_t<decltype(values)>::value_type;
        const auto key = [](T v) { return std::uint64_t(std::make_unsigned_t<T>(v)); };
        cells.add(values.size());
        cells.add(key(values.fill()));
        values.visitChunks([&](std::size_t, const auto& chunk) { cells.addAll(chunk, key); },
                           [&](std::size_t) { cells.add(0xab5e47c4u); });
    };
    field(world.lakeRegionField);
    field(world.lakeLevelField);
    field(world.riverDischargeField);
    field(world.flowDirectionField);
    // What the water layer forces or forbids, and the rivers dial.
    field(world.waterPaintField);
    cells.add(std::uint64_t(std::lround(double(world.riverShare) * 1000.0)));
    if (world.graphRiverFlow) cells.add(0x9100ull + world.graphRiverFlow);
    return cells.value();
}

// ---------------------------------------------------------------------------
// The shape of a course.
//
// A reach that runs straight from one cell centre to the next draws the macro
// map's own lattice across the country: half-kilometre segments turning in
// eight directions, which reads as a circuit board rather than as a river.
// A reach of one depth is a fence - at a ford a cart gets over and out of its
// depth it does not, so a network of even channels cuts the country into
// pieces. And a headwater that starts at its cell's full width and depth
// draws every spring in the world as a round pit on an open hillside.
//
// All three are resolved here, once, and stored. The old code rebuilt the
// curve, the meander and the bedform from the coarse cell under every sample
// that asked - which is most of what a page used to cost.
// ---------------------------------------------------------------------------

// How far a reach wanders off the straight line between two cell centres, at
// a fraction along it. Zero at both ends, so the network stays joined: a
// course meanders between its junctions and arrives exactly where the next
// one starts.
Fixed meander(std::uint64_t seed, Fixed t) {
    // One arch, its height and sign from the reach's own seed. This is
    // 4t(1-t), which is the shape of a sine arch and is exact in fixed point.
    const Fixed arch = t * (core::kOne - t) * Fixed::fromInt(4);
    const std::uint64_t h = core::splitmix64(seed);
    const Fixed size = Fixed::ratio(static_cast<std::int64_t>(h % 1000), 1000);
    const Fixed sign = (h & (1ULL << 40)) ? core::kOne : -core::kOne;
    return arch * arch * size * sign;   // zero tangent at shared junctions
}

// Pools and riffles. A stream shallows and deepens every few widths of
// itself, and the shallow places are where things cross; the water level is
// untouched and only the bed comes up, which is what a gravel bar is.
// Pinches and reaches. A channel widens where it is slow and narrows where it
// is quick, and a channel of one width for its whole length is a milled slot.
// A different wavelength from the pools below, so the two do not line up into
// a string of beads.
Fixed breadth(std::uint64_t seed, Fixed along) {
    const std::uint64_t h = core::splitmix64(seed ^ 0x51ed270bd7373bfdull);
    const Fixed phase = Fixed::ratio(static_cast<std::int64_t>(h % 1000), 1000);
    const Fixed u = along * Fixed::ratio(13, 10) + phase;
    const Fixed part = u - Fixed::fromInt(u.toInt());
    const Fixed arch = part * (core::kOne - part) * Fixed::fromInt(4);
    return Fixed::ratio(72, 100) + arch * Fixed::ratio(56, 100);   // 0.72 to 1.28
}

Fixed bedform(std::uint64_t seed, Fixed along) {
    const std::uint64_t h = core::splitmix64(seed ^ 0x9e3779b97f4a7c15ull);
    const Fixed phase = Fixed::ratio(static_cast<std::int64_t>(h % 1000), 1000);
    const Fixed u = along * Fixed::fromInt(2) + phase;
    const Fixed part = u - Fixed::fromInt(u.toInt());
    const Fixed arch = part * (core::kOne - part) * Fixed::fromInt(4);
    // Long pools and short bars rather than an even wave between the two: an
    // even one spends as much of the channel shallow as deep and draws the
    // river as a string of beads.
    const Fixed sustained = arch * (Fixed::fromInt(2) - arch);
    // A seventh of the depth over the bar, all of it in the pool - a seventh
    // because the deepest channel here is a little over three metres and a
    // ford has to come out under sixty centimetres.
    const Fixed shape = Fixed::ratio(1, 7) + sustained * Fixed::ratio(6, 7);
    // Vanishing at both nodes, so the outgoing and incoming reach cannot give
    // the same point two different beds.
    const Fixed ends = along * (core::kOne - along) * Fixed::fromInt(4);
    return core::kOne - (core::kOne - shape) * ends * ends;
}

// A spring grows out of the hillside rather than a hole appearing in it.
Fixed grown(Fixed along) {
    const Fixed t = core::saturate(along);
    return t * t * (Fixed::fromInt(3) - Fixed::fromInt(2) * t);
}

// Catmull-Rom through four control points, so a course carries its direction
// through a junction instead of turning a corner at it.
Fixed spline(Fixed p0, Fixed p1, Fixed p2, Fixed p3, Fixed t) {
    const Fixed t2 = t * t, t3 = t2 * t;
    return (p1 * Fixed::fromInt(2) + (p2 - p0) * t +
            (p0 * Fixed::fromInt(2) - p1 * Fixed::fromInt(5) + p2 * Fixed::fromInt(4) - p3) * t2 +
            (p1 * Fixed::fromInt(3) - p0 - p2 * Fixed::fromInt(3) + p3) * t3) *
           Fixed::ratio(1, 2);
}

// Standing water swallows the holes in its own footprint.
//
// The flood raises a basin to its outlet and labels what it raised, so a cell
// whose ground already stood exactly at that head is raised by nothing and
// never labelled - a hole in the middle of a flat the lake covers. The
// drainage then routes across the flat and through the hole, and read
// literally that is the lake spilling out and flowing straight back in: an
// outlet and an inlet a few hundred metres apart, and a loop in a graph whose
// whole point is that water runs one way.
//
// It is also the fragmented lake this rewrite exists to remove. A course that
// leaves a body and returns to the same body never left it, so the ground it
// crossed belongs to the body: one ID and one level over the whole flat.
//
// Grown along the flow from the cells already in a body, to a fixed point.
// What that reaches does not depend on the order it is reached in, which is
// what keeps two builds of one world identical.
void absorbFootprintHoles(const generation::WorldMapData& world,
                          const std::vector<std::int32_t>& downstream,
                          std::vector<std::int32_t>& bodyOfCell) {
    const std::size_t count = bodyOfCell.size();

    // Where a cell's water first meets standing water, if it does before it
    // meets the sea or runs out of map. Resolved iteratively over shared
    // paths: a continental drainage is too deep for a recursive walk.
    //
    // Cells on the path being walked are marked, so a drainage that runs in a
    // circle is answered rather than followed for ever. MacroWorld::attach
    // rejects such a map outright, but it has not run yet at this point and it
    // reads the per-cell flags where this reads flowDirectionField, so the two
    // are not the same question.
    constexpr std::int32_t kUnresolved = -2;
    constexpr std::int32_t kOnPath = -3;
    std::vector<std::int32_t> sinkBody(count, kUnresolved);
    std::vector<std::int32_t> path;
    for (std::size_t start = 0; start < count; ++start) {
        if (sinkBody[start] != kUnresolved) continue;
        path.clear();
        auto at = static_cast<std::int32_t>(start);
        while (sinkBody[at] == kUnresolved) {
            const auto cell = static_cast<std::size_t>(at);
            if (bodyOfCell[cell] >= 0) {
                sinkBody[at] = bodyOfCell[cell];
                break;
            }
            if (world.cells[cell].sea || downstream[cell] < 0) {
                sinkBody[at] = -1;
                break;
            }
            sinkBody[at] = kOnPath;
            path.push_back(at);
            at = downstream[cell];
        }
        const std::int32_t resolved = sinkBody[at] == kOnPath ? -1 : sinkBody[at];
        for (const std::int32_t cell : path) sinkBody[cell] = resolved;
    }

    std::vector<std::int32_t> queue;
    const auto consider = [&](std::int32_t cell, std::int32_t body) {
        if (cell < 0) return;
        const auto index = static_cast<std::size_t>(cell);
        if (bodyOfCell[index] >= 0 || world.cells[index].sea) return;
        if (sinkBody[index] != body) return;   // it goes somewhere else: a real river
        bodyOfCell[index] = body;
        queue.push_back(cell);
    };
    for (std::size_t i = 0; i < count; ++i)
        if (bodyOfCell[i] >= 0) consider(downstream[i], bodyOfCell[i]);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const auto cell = static_cast<std::size_t>(queue[head]);
        consider(downstream[cell], bodyOfCell[cell]);
    }
}

// ---------------------------------------------------------------------------
// The drainage model, owned here.
//
// This used to be read off MacroWorld, on the argument that borrowing the
// tuned answer was safer than copying it. That was right while MacroWorld was
// staying. It is not staying: the whole point of the rewrite is that a river's
// width, its head and the ground it cuts stop being decided by a procedural
// query that runs beside a camera. So the model moves in here, where it is
// derived once from the finished macro map and then stored - and there is
// exactly one place left that decides how wide a river is.
//
// The numbers are the ones the river linter was swept against; what changes is
// where they live and how often they run.
// ---------------------------------------------------------------------------

// The narrowest channel this world draws water in, in doublings of flow. Four
// is five and a half metres across; three is past what a lattice sampled every
// four metres can carry as water rather than as a dashed line.
constexpr std::uint8_t kNarrowestWetFlow = 4;
// And what share of the carved network holds it: one valley in three. A share
// rather than a number, because flow is the catchment above a cell and grows
// with the whole map.
constexpr std::int64_t kWetShareNumerator = 1, kWetShareDenominator = 3;
// The narrowest and widest water this world draws, as half-widths in metres.
constexpr std::int64_t kBrookHalfWidth = 4, kTrunkHalfWidth = 70;
// The widest a valley may be drawn. A valley wider than the drainage is
// gathered around a point would be cut in some places and left uncut in
// others, and that seam is worse than the steep bank it was widening away
// from.
constexpr std::int64_t kWidestValley = 800;
// How far out the relief that a valley has to absorb is read, in macro cells.
constexpr std::int32_t kCellsAround = 3;

// How much of a valley a cell with this much water through it cuts at all. A
// trickle cuts nothing, and the ground between the streams is meant to be
// ground: over half the land drains two doublings or less.
Fixed carveStrength(std::uint8_t flowLog) {
    if (flowLog <= 2) return core::kZero;
    if (flowLog >= 6) return core::kOne;
    return Fixed::ratio(flowLog - 2, 4);
}

// A metre and a half of gully, up to about eleven for a trunk river's bed.
Fixed depthFor(std::uint8_t flowLog) {
    return (Fixed::ratio(3, 2) +
            Fixed::fromInt(std::min<std::uint8_t>(flowLog, 15)) * Fixed::ratio(3, 5)) *
           carveStrength(flowLog);
}

// Everything the whole macro map has to answer before any one reach can be
// sized: what counts as a stream here, what the biggest river on the map is,
// where the water stands, and which courses hold it.
struct MacroHydrology {
    const generation::WorldMapData* world = nullptr;
    std::uint8_t streamFlow = 255;
    std::uint8_t largestFlow = 0;
    std::vector<Fixed> surface;
    std::vector<std::uint8_t> wet;

    // The same catchment is a stream in wet country and a dry wadi in a
    // desert, and the map-wide share cannot see that.
    [[nodiscard]] std::uint8_t streamFlowIn(const generation::WorldCell& cell) const {
        const std::int32_t shift = 2 - (static_cast<std::int32_t>(cell.moisture) * 4) / 255;
        return static_cast<std::uint8_t>(std::clamp<std::int32_t>(
                static_cast<std::int32_t>(streamFlow) + shift,
                std::max(kNarrowestWetFlow, world ? world->graphRiverFlow : std::uint8_t(0)), 15));
    }

    // Width goes as the square root of the flow, which is what a real channel
    // does - but normalised against this map's own range rather than against a
    // table of absolute doublings. No world here has fifteen doublings in it,
    // and read absolutely every river on a small map came out the same width.
    [[nodiscard]] Fixed halfWidthFor(std::uint8_t flowLog) const {
        const std::int64_t bottom = kNarrowestWetFlow;
        const std::int64_t top = std::max<std::int64_t>(largestFlow, bottom + 1);
        const std::int64_t here = std::clamp<std::int64_t>(flowLog, bottom, top);
        const std::int64_t steps = 8;
        const std::int64_t up = ((here - bottom) * steps) / (top - bottom);
        // kBrookHalfWidth * (kTrunkHalfWidth/kBrookHalfWidth)^(up/8), tabulated.
        static const Fixed kStep[9] = {
                Fixed::ratio(40, 10),  Fixed::ratio(59, 10),  Fixed::ratio(87, 10),
                Fixed::ratio(128, 10), Fixed::ratio(187, 10), Fixed::ratio(275, 10),
                Fixed::ratio(403, 10), Fixed::ratio(591, 10), Fixed::ratio(700, 10)};
        return core::clamp(kStep[std::clamp<std::int64_t>(up, 0, steps)],
                           Fixed::fromInt(kBrookHalfWidth), Fixed::fromInt(kTrunkHalfWidth));
    }

    // Two hundred metres for a brook, up to a kilometre and a half for the
    // trunk. This is not decoration: it is the only thing deciding how steep
    // the sides of every watercourse in the world are.
    [[nodiscard]] Fixed reachFor(std::uint8_t flowLog) const {
        const std::int64_t bottom = kNarrowestWetFlow;
        const std::int64_t top = std::max<std::int64_t>(largestFlow, bottom + 1);
        const std::int64_t here = std::clamp<std::int64_t>(flowLog, 0, top);
        const std::int64_t over = std::max<std::int64_t>(here - bottom, 0);
        const Fixed grown =
                Fixed::fromInt(200) + Fixed::fromInt(1300) * Fixed::ratio(over, top - bottom);
        const Fixed dry = Fixed::fromInt(60) + Fixed::fromInt(35) * Fixed::fromInt(here);
        return (flowLog >= bottom ? grown : dry) * carveStrength(flowLog);
    }
};

// Resolved once, in drainage order, over the whole map. A node has one head
// shared by every tributary and by the reach leaving it, and wet runoff is
// inherited downstream even where the local climate is dry: a rainfall
// threshold can start a stream but cannot delete the water above it.
MacroHydrology resolveHydrology(const generation::WorldMapData& world,
                                const std::vector<std::int32_t>& downstream) {
    MacroHydrology out;
    out.world = &world;
    const auto count = world.cells.size();
    const std::int32_t width = world.width;

    std::int64_t carved = 0;
    std::int64_t above[16] = {};
    for (const generation::WorldCell& cell : world.cells) {
        if (cell.sea) continue;
        const std::uint8_t size = std::min<std::uint8_t>(cell.drainSize, 15);
        if (carveStrength(size).raw <= 0) continue;   // a trickle carves nothing
        ++carved;
        ++above[size];
        if (size > out.largestFlow) out.largestFlow = size;
    }
    std::int64_t running = 0;
    std::uint8_t chosen = 15;
    // The share is the world's own dial (WorldMapData::riverShare: more or
    // fewer rivers on land made by hand), one in three when nobody set it.
    const double share = std::clamp(double(kWetShareNumerator) / double(kWetShareDenominator) *
                                            double(world.riverShare), 0.05, 0.9);
    for (int size = 15; size >= 0; --size) {
        running += above[size];
        chosen = static_cast<std::uint8_t>(size);
        // Exactly the integer rule when the dial is untouched: a world nobody
        // set it on gets the streams it always had, to the cell.
        if (world.riverShare == 1.0f ? running * kWetShareDenominator >= carved * kWetShareNumerator
                                     : double(running) >= double(carved) * share)
            break;
    }
    out.streamFlow = std::max({chosen, kNarrowestWetFlow, world.graphRiverFlow});

    out.surface.assign(count, core::kZero);
    out.wet.assign(count, 0);

    // Where the water stands in a cell is the lowest ground in it, not the
    // height of the cell.
    //
    // A cell is five hundred and forty metres of country and its elevation is
    // one number for all of it; a river runs along the bottom of that country,
    // not across the middle of it at the average height. Taking the cell's own
    // number put the surface wherever the macro map's arithmetic left it while
    // the ground went where a spline through those same numbers and a detail
    // layer tens of metres deep put it - and the two are not the same place.
    //
    // Measured on three worlds, at forty metres beyond the bank: the water
    // stood above the land in sixty-four, seventy-six and ninety-one per cent
    // of cross-sections, by a median of eighteen, forty-three and a hundred
    // and twenty-nine metres, and by four hundred and fifty-four at the worst.
    // That is a river running along the crest of a dyke half a kilometre high,
    // and every consequence downstream of it followed: unbounded, the water
    // flooded the country because it stood above the country; bounded to its
    // banks, it hangs over the land beside it instead.
    //
    // So the head is read off the ground. Five samples of the country and its
    // detail - the middle of the cell and four points a third of the way out -
    // and the lowest of them is where water in this cell can stand. A lake
    // keeps its own head: a flood filled it to its outlet, and that level is a
    // fact about the basin rather than about the ground under it.
    //
    // The field is asked for the country and the detail only. It has no graph
    // yet - this is the graph - and it does not need one for either.
    const std::int64_t cellMetres = generation::kMetresPerCell;
    const auto groundAt = [&](const world::HeightField& terrain, Fixed x, Fixed y) {
        const auto pieces = terrain.piecesAt(x, y);
        return pieces.country + pieces.moved;
    };
    const auto centreOfCell = [&](std::size_t cell) {
        return core::WorldPos{
                Fixed::fromInt(static_cast<std::int64_t>(cell % static_cast<std::size_t>(width)) *
                                       cellMetres + cellMetres / 2),
                Fixed::fromInt(static_cast<std::int64_t>(cell / static_cast<std::size_t>(width)) *
                                       cellMetres + cellMetres / 2)};
    };
    // Along the water's own way down, not across the cell it is in.
    //
    // Five probes on a grid inside a cell find the lowest of five places in
    // five hundred and forty metres, and a river runs down a line much
    // narrower than that. Measured where it goes wrong: a reach two metres
    // across came out with its surface at two hundred and eighteen metres
    // while its own bed and every metre of ground for thirty either side stood
    // at a hundred and sixty-four - fifty-four metres of water in a brook,
    // because the five probes all landed on the valley side and none of them
    // in the valley.
    //
    // So the line from this cell's middle to the middle of the cell it drains
    // into is walked instead. That is the way the water goes, to within the
    // smoothing the course puts on it later, and the lowest ground along it is
    // the ground the water will actually find.
    const auto thalwegOf = [&](const world::HeightField& terrain, std::size_t cell) {
        const core::WorldPos here = centreOfCell(cell);
        Fixed lowest = groundAt(terrain, here.x, here.y);
        for (const auto [ox, oy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1},
                                    std::pair{0, 1}}) {
            const Fixed px = here.x + Fixed::fromInt(ox * cellMetres / 3);
            const Fixed py = here.y + Fixed::fromInt(oy * cellMetres / 3);
            lowest = core::min(lowest, groundAt(terrain, px, py));
        }
        const std::int32_t next = downstream[cell];
        if (next >= 0) {
            const core::WorldPos there = centreOfCell(static_cast<std::size_t>(next));
            constexpr int kSteps = 8;
            for (int step = 1; step < kSteps; ++step) {
                const Fixed t = Fixed::ratio(step, kSteps);
                lowest = core::min(lowest, groundAt(terrain, here.x + (there.x - here.x) * t,
                                                    here.y + (there.y - here.y) * t));
            }
        }
        return lowest;
    };

    // Every cell on its own, so rows of them go wide across the machine: the
    // ground under a thalweg is most of what resolving the map costs.
    const std::size_t rows = std::size_t(std::max(0, world.height));
    std::atomic<std::size_t> nextRow{0};
    const auto resolveRows = [&] {
        world::HeightField terrain(&world, world.seed);   // a field keeps caches: one to a thread
        for (std::size_t row = nextRow++; row < rows; row = nextRow++)
            for (std::size_t i = row * std::size_t(width), end = std::min(count, i + std::size_t(width)); i < end; ++i) {
                const generation::WorldCell& cell = world.cells[i];
                const bool lake = i < world.lakeDepthField.size() && world.lakeDepthField[i] > 0 &&
                                  i < world.lakeLevelField.size();
                const int head = cell.sea ? 0 : lake ? world.lakeLevelField[i] : cell.elevation;
                Fixed surface = Fixed::fromInt(static_cast<std::int64_t>(std::max(0, head)) *
                                               generation::kMetresPerElevationStep);
                if (!cell.sea && !lake)
                    surface = core::min(surface, thalwegOf(terrain, i));
                out.surface[i] = surface;
                // What was painted on the water layer is kept: a course holds
                // water however little gathers in it, dry ground holds none.
                const auto paint = i < world.waterPaintField.size()
                        ? static_cast<generation::WaterPaint>(world.waterPaintField[i]) : generation::WaterPaint::None;
                out.wet[i] = !cell.sea && paint != generation::WaterPaint::Dry &&
                             (cell.river || cell.drainSize >= out.streamFlowIn(cell) ||
                              paint == generation::WaterPaint::Course);
            }
    };
    {
        const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
        std::vector<std::thread> workers;
        for (unsigned t = 1; t < threads && rows > 64; ++t) workers.emplace_back(resolveRows);
        resolveRows();
        for (auto& w : workers) w.join();
    }

    // Iterative topological resolution: no recursive stack on a continent's
    // rivers, and a cycle is reported rather than followed.
    std::vector<std::uint8_t> state(count, 0);
    std::vector<std::int32_t> order, path;
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (state[i] == 2) continue;
        path.clear();
        auto at = static_cast<std::int32_t>(i);
        while (at >= 0 && state[at] == 0) {
            state[at] = 1;
            path.push_back(at);
            at = downstream[static_cast<std::size_t>(at)];
        }
        if (at >= 0 && state[at] == 1) throw std::invalid_argument("cyclic terrain drainage");
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            const auto n = *it;
            const auto next = downstream[static_cast<std::size_t>(n)];
            if (next >= 0)
                out.surface[static_cast<std::size_t>(n)] =
                        core::max(out.surface[static_cast<std::size_t>(n)],
                                  out.surface[static_cast<std::size_t>(next)]);
            state[n] = 2;
            order.push_back(n);
        }
    }
    // Water inherited downstream - except into ground painted dry, where it
    // sinks (the water layer, WaterPaint::Dry): nothing painted dry is wet.
    const auto paintedDry = [&](std::size_t i) {
        return i < world.waterPaintField.size() &&
               world.waterPaintField[i] == std::uint8_t(generation::WaterPaint::Dry);
    };
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto next = downstream[static_cast<std::size_t>(*it)];
        if (next >= 0 && out.wet[static_cast<std::size_t>(*it)] && !paintedDry(static_cast<std::size_t>(next)))
            out.wet[static_cast<std::size_t>(next)] = 1;
    }
    return out;
}

// Which storage pages a span of world metres touches, as a half-open range in
// page indices. Half-open on purpose: a macro cell that ends exactly on a page
// boundary does not reach the page beyond it, and rounding that the other way
// puts a river in the index of every page it stops at.
std::pair<std::int32_t, std::int32_t> pageSpan(std::int64_t lowMetres, std::int64_t highMetres) {
    const std::int64_t last = std::max(lowMetres, highMetres - 1);
    return {static_cast<std::int32_t>(floorDiv(lowMetres, kPageMetres)),
            static_cast<std::int32_t>(floorDiv(last, kPageMetres))};
}

// Find the connected depression, stopping at its first spill, not at an
// arbitrary wet-mask boundary. Bounded to the macro footprint plus one ring;
// a downhill slope reaches that ring at the seed's height and holds no lake.
void naturalBasin(WaterBody& body, const generation::TerrainFoundation& f, int macroMetres) {
    body.basinStep = f.step;
    body.basinSamples.clear();
    if (body.macroCells.empty() || f.columns < 2 || f.rows < 2 || f.step <= 0 ||
        f.heightDm[4].size() != std::size_t(f.columns)*f.rows) return;
    int minX=f.columns-1,minY=f.rows-1,maxX=0,maxY=0,seed=-1;
    const auto& heights=f.heightDm[4];
    for (const auto cell:body.macroCells) {
        const int x0=std::clamp(cell.x*macroMetres/f.step,0,f.columns-1);
        const int y0=std::clamp(cell.y*macroMetres/f.step,0,f.rows-1);
        const int x1=std::clamp(((cell.x+1)*macroMetres+f.step-1)/f.step,0,f.columns-1);
        const int y1=std::clamp(((cell.y+1)*macroMetres+f.step-1)/f.step,0,f.rows-1);
        minX=std::min(minX,x0);minY=std::min(minY,y0);
        maxX=std::max(maxX,x1);maxY=std::max(maxY,y1);
        for (int y=y0;y<=y1;++y) for (int x=x0;x<=x1;++x) {
            const int at=y*f.columns+x;
            if (seed<0 || heights[at]<heights[seed]) seed=at;
        }
    }
    const int halo=(macroMetres+f.step-1)/f.step+1;
    minX=std::max(0,minX-halo);minY=std::max(0,minY-halo);
    maxX=std::min(f.columns-1,maxX+halo);maxY=std::min(f.rows-1,maxY+halo);
    const int width=maxX-minX+1,height=maxY-minY+1;
    std::vector<std::uint8_t> seen(std::size_t(width)*height,0);
    using Visit=std::pair<std::int32_t,int>; // minimax spill cost, global index
    std::priority_queue<Visit,std::vector<Visit>,std::greater<Visit>> queue;
    std::vector<Visit> reached;
    const auto push=[&](int x,int y,int level) {
        const auto local=std::size_t(y-minY)*width+x-minX;
        if (seen[local]) return;
        seen[local]=1;
        const int at=y*f.columns+x;
        queue.emplace(std::max(level,heights[at]),at);
    };
    push(seed%f.columns,seed/f.columns,heights[seed]);
    int level=int((body.level*Fixed::fromInt(10)).toInt());
    while (!queue.empty()) {
        const auto [cost,at]=queue.top();queue.pop();
        if (cost>=level) break;
        const int x=at%f.columns,y=at/f.columns;
        if (x==minX || x==maxX || y==minY || y==maxY) { level=cost;break; }
        reached.emplace_back(cost,at);
        // Slopes is monotone along a lattice edge, so submerged endpoints
        // certify a submerged path. Diagonal endpoints do not: the saddle
        // between them can be dry, separating two otherwise low depressions.
        // Keep connectivity conservative at the foundation's own resolution.
        push(x-1,y,cost);
        push(x+1,y,cost);
        push(x,y-1,cost);
        push(x,y+1,cost);
    }
    body.level=core::min(body.level,Fixed::ratio(level,10));
    for (const auto [cost,at]:reached)
        if (cost<level) body.basinSamples.push_back({at%f.columns,at/f.columns});
    std::sort(body.basinSamples.begin(),body.basinSamples.end(),[](auto a,auto b) {
        return a.y<b.y || (a.y==b.y && a.x<b.x);
    });
}

} // namespace

namespace {
// The page index: which reaches and bodies can touch which storage page.
//
// Keyed by (y, x) so the map's own order is the order findHydrologySpatialPage
// binary-searches. Storage pages are square and world aligned; that a ring
// scheduler will ask for them in a different order is not this index's
// business.
void indexGraph(HydrologyGraph& graph, const generation::WorldMapData& world) {
    std::map<std::pair<std::int32_t, std::int32_t>, HydrologySpatialPage> pages;
    const auto pageAt = [&](std::int32_t x, std::int32_t y) -> HydrologySpatialPage& {
        HydrologySpatialPage& page = pages[{y, x}];
        page.key = {x, y, 0};
        return page;
    };
    for (const RiverSegment& segment : graph.segments) {
        const auto columns = pageSpan(segment.bounds.min.x.toInt(), segment.bounds.max.x.toInt() + 1);
        const auto rows = pageSpan(segment.bounds.min.y.toInt(), segment.bounds.max.y.toInt() + 1);
        for (std::int32_t y = rows.first; y <= rows.second; ++y)
            for (std::int32_t x = columns.first; x <= columns.second; ++x)
                pageAt(x, y).segments.push_back(segment.id);
    }
    const auto indexBodySpan = [&](WaterBodyId id, std::int64_t minX, std::int64_t minY,
                                    std::int64_t maxX, std::int64_t maxY) {
        const auto columns = pageSpan(minX, maxX);
        const auto rows = pageSpan(minY, maxY);
        for (std::int32_t y = rows.first; y <= rows.second; ++y)
            for (std::int32_t x = columns.first; x <= columns.second; ++x)
                pageAt(x, y).waterBodies.push_back(id);
    };
    const auto indexBodyCell = [&](WaterBodyId id, TilePos cell, std::int64_t margin) {
        const std::int64_t metres = generation::kMetresPerCell;
        indexBodySpan(id, cell.x * metres - margin, cell.y * metres - margin,
                       (cell.x + 1) * metres + margin, (cell.y + 1) * metres + margin);
    };
    for (std::size_t slot = 1; slot < graph.waterBodies.size(); ++slot) {
        const auto& body = graph.waterBodies[slot];
        if (body.basinStep == 0) {
            // Legacy samples live at macro-cell centres. Include their entire
            // interpolation support, not just the cell that holds the centre.
            for (const TilePos cell : body.macroCells)
                indexBodyCell(body.id, cell, generation::kMetresPerCell / 2);
            continue;
        }
        // Natural basins can extend beyond their macro provenance. Index the
        // actual samples, including one step of shoreline support. Coalesce
        // row runs so a large lake does not append one page entry per sample.
        const std::int64_t step = body.basinStep;
        for (std::size_t first = 0; first < body.basinSamples.size();) {
            std::size_t last = first;
            while (last + 1 < body.basinSamples.size() &&
                   body.basinSamples[last + 1].y == body.basinSamples[first].y &&
                   body.basinSamples[last + 1].x == body.basinSamples[last].x + 1)
                ++last;
            const auto begin = body.basinSamples[first], end = body.basinSamples[last];
            indexBodySpan(body.id, (std::int64_t(begin.x) - 1) * step,
                           (std::int64_t(begin.y) - 1) * step,
                           (std::int64_t(end.x) + 1) * step,
                           (std::int64_t(end.y) + 1) * step);
            first = last + 1;
        }
    }
    // The ocean is not indexed. It keeps no footprint of its own, and nobody
    // asks the index for it: the carve decides it from the ground (below the
    // sea's level and no other water there, graph_carve.cpp), and a page
    // wants the index for its reaches and its lakes. Indexed off the sea mask
    // it was one entry for every sea cell of the world - sixteen million of
    // them, gigabytes and seconds, for a world of nothing but sea.
    graph.spatialPages.clear();
    graph.spatialPages.reserve(pages.size());
    for (auto& entry : pages) {
        HydrologySpatialPage& page = entry.second;
        std::sort(page.segments.begin(), page.segments.end());
        page.segments.erase(std::unique(page.segments.begin(), page.segments.end()),
                            page.segments.end());
        std::sort(page.waterBodies.begin(), page.waterBodies.end());
        page.waterBodies.erase(std::unique(page.waterBodies.begin(), page.waterBodies.end()),
                               page.waterBodies.end());
        graph.spatialPages.push_back(std::move(page));
    }
}


// ---------------------------------------------------------------------------
// The water fitted where the country changed, and nowhere else.
//
// Fitting the lakes and the courses to the carved ground (below) is most of
// what a graph costs - three seconds of the reference world's three and a
// half - and all of it went again after any edit anywhere: an island redrawn
// off the coast of a continent refitted every river of the continent.
//
// Fitting is local. A course's banks are read through carvers that gather
// every reach and lake indexed within two and a half kilometres of the bank,
// and a lake's flood reads the ground half a kilometre past its basin through
// the same carvers; nothing further away is read at all. So the courses and
// lakes fall into groups - everything within four kilometres of something
// else in the group, and joined through every node and every lake a course
// touches - and no group reads anything of another. Fitting one group on its
// own gives it exactly what fitting the world gives it, and so:
//
// - a group whose every input is what it was when it was last fitted takes
//   that fit back (the key below is every value of its items before fitting,
//   and every value of the ground under it and around it), and
// - the groups that are fitted go wide across the machine, one to a thread.
//
// The graph comes out bit for bit the same either way; the cache only decides
// how much of it is worked out again.
// ---------------------------------------------------------------------------

constexpr std::uint32_t kNoGroup = ~std::uint32_t{0};
// How far apart two items may be and still read each other, doubled for
// margin: a carver's window (a kilometre, aligned) plus its halo (900 m) plus
// a storage page, which is how far a carver gathers past its window.
constexpr std::int64_t kFitReach = 2048;   // each side, so 4 km between two boxes

struct FitGroups {
    std::vector<std::uint32_t> ofSegment;   // by segment index
    std::vector<std::uint32_t> ofBody;      // by body index; kNoGroup for the ocean
    std::vector<std::vector<RiverId>> segments;    // per group, ascending
    std::vector<std::vector<WaterBodyId>> bodies;  // per group, ascending
    std::vector<std::vector<RiverNodeId>> nodes;   // per group, ascending
    std::vector<core::WorldRect> bounds;           // per group, what its items cover
    std::vector<std::uint64_t> keys;
    std::vector<std::uint8_t> active;              // fitted in this build
    // Whether a fit may be taken from, or kept for, another build: only over a
    // foundation, where the ground a fit reads is what fitKey hashes. Without
    // one the height field reads the whole of the macro map's cells.
    bool cacheable = false;
    bool segmentActive(RiverId id) const { return active[ofSegment[id - 1]] != 0; }
    bool bodyActive(std::size_t index) const {
        return ofBody[index] != kNoGroup && active[ofBody[index]] != 0;
    }
};

core::WorldRect bodyBounds(const WaterBody& body) {
    const std::int64_t cell = generation::kMetresPerCell;
    std::int64_t lowX = std::numeric_limits<std::int64_t>::max(), lowY = lowX;
    std::int64_t highX = std::numeric_limits<std::int64_t>::min(), highY = highX;
    for (const TilePos c : body.macroCells) {
        lowX = std::min(lowX, c.x * cell); highX = std::max(highX, (c.x + 1) * cell);
        lowY = std::min(lowY, c.y * cell); highY = std::max(highY, (c.y + 1) * cell);
    }
    for (const TilePos s : body.basinSamples) {
        const std::int64_t step = std::max(1, body.basinStep);
        lowX = std::min(lowX, s.x * step); highX = std::max(highX, (s.x + 1) * step);
        lowY = std::min(lowY, s.y * step); highY = std::max(highY, (s.y + 1) * step);
    }
    if (lowX > highX) return {};
    // The flood looks half a kilometre past its basin.
    lowX -= cell; lowY -= cell; highX += cell; highY += cell;
    return {{Fixed::fromInt(lowX), Fixed::fromInt(lowY)}, {Fixed::fromInt(highX), Fixed::fromInt(highY)}};
}

FitGroups fitGroups(const HydrologyGraph& graph) {
    const std::size_t segmentCount = graph.segments.size(), bodyCount = graph.waterBodies.size();
    // Items: segments first, then bodies.
    std::vector<std::uint32_t> parent(segmentCount + bodyCount);
    for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = std::uint32_t(i);
    const auto find = [&](std::uint32_t a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    const auto join = [&](std::uint32_t a, std::uint32_t b) {
        a = find(a); b = find(b);
        if (a != b) parent[std::max(a, b)] = std::min(a, b);
    };
    const auto isLake = [&](WaterBodyId id) {
        const WaterBody* body = waterBodyOf(graph, id);
        return body != nullptr && body->kind == WaterBodyKind::Lake;
    };
    const auto bodyItem = [&](WaterBodyId id) { return std::uint32_t(segmentCount + id - 1); };
    // Through the network: the courses meeting at a node, a course and the
    // lake it leaves or ends in.
    std::vector<std::uint32_t> atNode(graph.nodes.size() + 1, kNoGroup);
    for (std::size_t s = 0; s < segmentCount; ++s) {
        const RiverSegment& segment = graph.segments[s];
        for (const RiverNodeId node : {segment.from, segment.to}) {
            if (node == kInvalidRiverNodeId) continue;
            if (atNode[node] == kNoGroup) atNode[node] = std::uint32_t(s);
            else join(atNode[node], std::uint32_t(s));
            const WaterBodyId body = graph.nodes[node - 1].waterBody;
            if (isLake(body)) join(std::uint32_t(s), bodyItem(body));
        }
        if (isLake(segment.sourceWaterBody)) join(std::uint32_t(s), bodyItem(segment.sourceWaterBody));
        if (isLake(segment.destinationWaterBody)) join(std::uint32_t(s), bodyItem(segment.destinationWaterBody));
    }
    // And through the country: anything whose boxes, each grown by the reach,
    // share a square of the grid.
    constexpr std::int64_t kGrid = 4096;
    std::unordered_map<std::uint64_t, std::uint32_t> firstIn;
    std::vector<core::WorldRect> itemBounds(parent.size());
    std::vector<std::uint8_t> present(parent.size(), 0);
    const auto place = [&](std::uint32_t item, core::WorldRect box) {
        itemBounds[item] = box;
        present[item] = 1;
        const std::int64_t x0 = floorDiv(box.min.x.toInt() - kFitReach, kGrid), x1 = floorDiv(box.max.x.toInt() + kFitReach, kGrid);
        const std::int64_t y0 = floorDiv(box.min.y.toInt() - kFitReach, kGrid), y1 = floorDiv(box.max.y.toInt() + kFitReach, kGrid);
        for (std::int64_t y = y0; y <= y1; ++y)
            for (std::int64_t x = x0; x <= x1; ++x) {
                const std::uint64_t square = (std::uint64_t(std::uint32_t(std::int32_t(y))) << 32) | std::uint32_t(std::int32_t(x));
                const auto [it, fresh] = firstIn.emplace(square, item);
                if (!fresh) join(it->second, item);
            }
    };
    for (std::size_t s = 0; s < segmentCount; ++s) place(std::uint32_t(s), graph.segments[s].bounds);
    for (std::size_t b = 0; b < bodyCount; ++b)
        if (graph.waterBodies[b].kind == WaterBodyKind::Lake)
            place(std::uint32_t(segmentCount + b), bodyBounds(graph.waterBodies[b]));

    // Numbered by their first item, which is the order of the IDs.
    FitGroups groups;
    groups.ofSegment.assign(segmentCount, kNoGroup);
    groups.ofBody.assign(bodyCount, kNoGroup);
    std::vector<std::uint32_t> numberOf(parent.size(), kNoGroup);
    const auto groupOf = [&](std::uint32_t item) {
        const std::uint32_t root = find(item);
        if (numberOf[root] == kNoGroup) {
            numberOf[root] = std::uint32_t(groups.segments.size());
            groups.segments.emplace_back();
            groups.bodies.emplace_back();
            groups.nodes.emplace_back();
            groups.bounds.push_back(itemBounds[item]);
        }
        const std::uint32_t g = numberOf[root];
        core::WorldRect& box = groups.bounds[g];
        const core::WorldRect& mine = itemBounds[item];
        box = {{core::min(box.min.x, mine.min.x), core::min(box.min.y, mine.min.y)},
               {core::max(box.max.x, mine.max.x), core::max(box.max.y, mine.max.y)}};
        return g;
    };
    for (std::size_t s = 0; s < segmentCount; ++s) {
        const std::uint32_t g = groupOf(std::uint32_t(s));
        groups.ofSegment[s] = g;
        groups.segments[g].push_back(graph.segments[s].id);
        for (const RiverNodeId node : {graph.segments[s].from, graph.segments[s].to})
            if (node != kInvalidRiverNodeId) groups.nodes[g].push_back(node);
    }
    for (std::size_t b = 0; b < bodyCount; ++b) {
        if (!present[segmentCount + b]) continue;
        const std::uint32_t g = groupOf(std::uint32_t(segmentCount + b));
        groups.ofBody[b] = g;
        groups.bodies[g].push_back(graph.waterBodies[b].id);
    }
    for (auto& nodes : groups.nodes) {
        std::sort(nodes.begin(), nodes.end());
        nodes.erase(std::unique(nodes.begin(), nodes.end()), nodes.end());
    }
    groups.active.assign(groups.segments.size(), 1);
    groups.keys.assign(groups.segments.size(), 0);
    return groups;
}

// Every value a group's fit reads: its items as they stand before it, and the
// ground under and around them. IDs are left out - they are where the item
// falls in the world's order, and an edit elsewhere moves them - and what an
// ID points at goes in instead.
std::uint64_t fitKey(const HydrologyGraph& graph, const generation::WorldMapData& world,
                     const FitGroups& groups, std::uint32_t g, const std::vector<std::size_t>& sharedFrom) {
    core::LaneHash hash(mixIn(0x7f4a7c159e3779b9ull, kHydrologyGraphVersion));
    hash.add(world.seed);
    hash.add(std::uint64_t(world.width));
    hash.add(std::uint64_t(world.height));
    hash.add(std::uint64_t(world.terrainStage));
    // The hybrid terrain is one piece for the whole world, and its identity
    // moves with any of it. Over a foundation the ground a fit reads is the
    // foundation's (HeightField::piecesAt) and the hybrid is not read at all;
    // without one it is, and every group keys on the whole of it.
    if (world.hybridTerrain && !world.terrainFoundation) hash.add(world.hybridTerrain->fingerprint());
    const auto fixed = [](Fixed f) { return std::uint64_t(f.raw); };
    const auto position = [&](WorldPos p) { hash.add(fixed(p.x)); hash.add(fixed(p.y)); };
    const auto nodeKey = [&](RiverNodeId id) {
        if (id == kInvalidRiverNodeId) { hash.add(0x51ab); return; }
        const RiverNode& node = graph.nodes[id - 1];
        hash.add(std::uint64_t(node.macroCell));
        hash.add(std::uint64_t(node.kind));
        hash.add(fixed(node.surface));
        hash.add(node.waterBody == kOceanWaterBodyId ? 1u : node.waterBody == kInvalidWaterBodyId ? 0u : 2u);
    };
    for (const RiverId id : groups.segments[g]) {
        const RiverSegment& segment = graph.segments[id - 1];
        nodeKey(segment.from);
        nodeKey(segment.to);
        hash.add(segment.sourceWaterBody != kInvalidWaterBodyId);
        hash.add(segment.destinationWaterBody == kOceanWaterBodyId ? 1u :
                 segment.destinationWaterBody == kInvalidWaterBodyId ? 0u : 2u);
        hash.add(segment.order);
        hash.add(fixed(segment.width)); hash.add(fixed(segment.depth)); hash.add(fixed(segment.valleyReach));
        hash.add(fixed(segment.discharge));
        position(segment.bounds.min); position(segment.bounds.max);
        hash.add(id < sharedFrom.size() ? sharedFrom[id] : 0);
        hash.addAll(segment.course, [&](const ReachPoint& p) {
            return std::uint64_t(p.position.x.raw) * 0x9e3779b97f4a7c15ull ^ std::uint64_t(p.position.y.raw) ^
                   (std::uint64_t(p.surface.raw) << 1) ^ (std::uint64_t(p.halfWidth.raw) << 7) ^
                   (std::uint64_t(p.depth.raw) << 13) ^ (std::uint64_t(p.valleyReach.raw) << 19);
        });
        hash.addAll(segment.macroCells);
    }
    for (const WaterBodyId id : groups.bodies[g]) {
        const WaterBody& body = graph.waterBodies[id - 1];
        hash.add(fixed(body.level));
        hash.add(std::uint64_t(body.basinStep));
        hash.add(std::uint64_t(body.sourceRegion));
        hash.add(body.inlets.size()); hash.add(body.outlets.size());
        hash.addAll(body.macroCells, [](TilePos c) { return (std::uint64_t(std::uint32_t(c.y)) << 32) | std::uint32_t(c.x); });
        hash.addAll(body.macroCellFloor, [](Fixed f) { return std::uint64_t(f.raw); });
        hash.addAll(body.basinSamples, [](TilePos c) { return (std::uint64_t(std::uint32_t(c.y)) << 32) | std::uint32_t(c.x); });
    }
    // The ground: the box of the items, grown by how far their fit reads.
    const core::WorldRect& box = groups.bounds[g];
    const std::int64_t minX = box.min.x.toInt() - kFitReach, minY = box.min.y.toInt() - kFitReach;
    const std::int64_t maxX = box.max.x.toInt() + kFitReach, maxY = box.max.y.toInt() + kFitReach;
    hash.add(0x6c0u);
    {
        const std::int64_t cell = generation::kMetresPerCell;
        const std::int32_t x0 = std::int32_t(std::clamp<std::int64_t>(floorDiv(minX, cell) - 1, 0, world.width - 1));
        const std::int32_t x1 = std::int32_t(std::clamp<std::int64_t>(floorDiv(maxX, cell) + 1, 0, world.width - 1));
        const std::int32_t y0 = std::int32_t(std::clamp<std::int64_t>(floorDiv(minY, cell) - 1, 0, world.height - 1));
        const std::int32_t y1 = std::int32_t(std::clamp<std::int64_t>(floorDiv(maxY, cell) + 1, 0, world.height - 1));
        hash.add(std::uint64_t(x0) | (std::uint64_t(y0) << 32));
        hash.add(std::uint64_t(x1) | (std::uint64_t(y1) << 32));
        const auto count = world.cells.size();
        for (std::int32_t y = y0; y <= y1; ++y)
            for (std::int32_t x = x0; x <= x1; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(world.width) + std::size_t(x);
                const generation::WorldCell& cell = world.cells[i];
                hash.add((std::uint64_t(cell.elevation) << 40) | (std::uint64_t(cell.drainSize) << 32) |
                         (std::uint64_t(std::uint8_t(cell.drainOut)) << 24) |
                         (std::uint64_t(std::uint8_t(cell.riverOut)) << 16) | (std::uint64_t(cell.moisture) << 8) |
                         (cell.sea ? 2u : 0u) | (cell.river ? 1u : 0u));
                hash.add((std::uint64_t(cell.riverSize) << 32) | (std::uint64_t(std::uint8_t(cell.climate)) << 16) |
                         (std::uint64_t(cell.temperature) << 8) | std::uint64_t(std::uint8_t(cell.biome)));
                const auto at = [&](const auto& field) {
                    return i < field.size() ? std::uint64_t(std::uint32_t(field[i])) : 0x5eull;
                };
                hash.add(at(world.lakeRegionField) ^ (at(world.lakeLevelField) << 32));
                hash.add(at(world.lakeDepthField) ^ (at(world.riverDischargeField) << 32));
                hash.add(at(world.flowDirectionField));
                (void)count;
            }
    }
    if (const auto* f = world.terrainFoundation.get()) {
        const std::int64_t step = std::max(1, f->step);
        const int x0 = int(std::clamp<std::int64_t>(floorDiv(minX, step) - 2, 0, f->columns - 1));
        const int x1 = int(std::clamp<std::int64_t>(floorDiv(maxX, step) + 2, 0, f->columns - 1));
        const int y0 = int(std::clamp<std::int64_t>(floorDiv(minY, step) - 2, 0, f->rows - 1));
        const int y1 = int(std::clamp<std::int64_t>(floorDiv(maxY, step) + 2, 0, f->rows - 1));
        hash.add(std::uint64_t(f->step)); hash.add(std::uint64_t(f->columns)); hash.add(std::uint64_t(f->rows));
        const std::size_t lattice = std::size_t(f->columns) * std::size_t(f->rows);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(f->columns) + std::size_t(x);
                for (const auto& plane : f->heightDm) hash.add(i < plane.size() ? std::uint64_t(std::uint32_t(plane[i])) : 0x7eull);
                if (f->receiver.size() == lattice) hash.add(std::uint64_t(std::uint32_t(f->receiver[i])));
                if (f->accumulation.size() == lattice) hash.add(f->accumulation[i]);
            }
        const int px0 = std::max(0, int(floorDiv(minX, 512)) - 1), px1 = std::min(f->pageColumns - 1, int(floorDiv(maxX, 512)) + 1);
        const int py0 = std::max(0, int(floorDiv(minY, 512)) - 1), py1 = std::min(f->pageRows - 1, int(floorDiv(maxY, 512)) + 1);
        for (int y = py0; y <= py1; ++y)
            for (int x = px0; x <= px1; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(f->pageColumns) + std::size_t(x);
                hash.add(i < f->detailPages.size() ? f->detailPages[i] : 0x3du);
            }
    }
    return hash.value();
}

// What a group's fit came out as, in the order of its items.
struct GroupFit {
    std::vector<std::vector<Fixed>> surfaces;   // per segment, per course point
    std::vector<Fixed> nodeSurfaces;
    struct Lake { Fixed level; std::int32_t basinStep = 0; std::vector<TilePos> samples; };
    std::vector<Lake> lakes;
    std::size_t bytes() const {
        std::size_t n = sizeof(*this) + nodeSurfaces.size() * sizeof(Fixed);
        for (const auto& s : surfaces) n += s.size() * sizeof(Fixed) + sizeof(s);
        for (const auto& l : lakes) n += l.samples.size() * sizeof(TilePos) + sizeof(l);
        return n;
    }
};

// The fits of the last builds, by key. Held for the process: an editor
// rebuilding the world after every stroke is what they are for, and what an
// edit leaves alone is most of the world. Bounded; the oldest go first.
class FitCache {
public:
    static FitCache& instance() { static FitCache cache; return cache; }
    void clear() {
        const std::lock_guard<std::mutex> lock(guard_);
        held_.clear();
        bytes_ = 0;
    }
    std::shared_ptr<const GroupFit> find(std::uint64_t key) {
        const std::lock_guard<std::mutex> lock(guard_);
        if (limit_ == 0) return nullptr;   // ASR_HYDRO_FIT_CACHE_MB=0: every group fitted, every time
        const auto it = held_.find(key);
        if (it == held_.end()) return nullptr;
        it->second.used = ++clock_;
        return it->second.fit;
    }
    void keep(std::uint64_t key, std::shared_ptr<const GroupFit> fit) {
        const std::lock_guard<std::mutex> lock(guard_);
        if (limit_ == 0) return;
        const std::size_t size = fit->bytes();
        auto& slot = held_[key];
        if (slot.fit) bytes_ -= slot.fit->bytes();
        slot = {std::move(fit), ++clock_};
        bytes_ += size;
        while (bytes_ > limit_ && held_.size() > 1) {
            auto oldest = held_.begin();
            for (auto it = held_.begin(); it != held_.end(); ++it)
                if (it->second.used < oldest->second.used) oldest = it;
            bytes_ -= oldest->second.fit->bytes();
            held_.erase(oldest);
        }
    }
private:
    FitCache() {
        if (const char* mb = std::getenv("ASR_HYDRO_FIT_CACHE_MB")) limit_ = std::size_t(std::max(0, std::atoi(mb))) << 20;
    }
    struct Slot { std::shared_ptr<const GroupFit> fit; std::uint64_t used = 0; };
    std::mutex guard_;
    std::unordered_map<std::uint64_t, Slot> held_;
    std::size_t bytes_ = 0, limit_ = std::size_t(256) << 20;
    std::uint64_t clock_ = 0;
};

// Every group's key, and the fit of each group whose key was seen before
// laid over it: that group is not fitted again.
void reuseFits(HydrologyGraph& graph, const generation::WorldMapData& world, FitGroups& groups,
               const std::vector<std::size_t>& sharedFrom) {
    groups.cacheable = world.terrainFoundation != nullptr;
    if (!groups.cacheable) return;
    for (std::uint32_t g = 0; g < groups.segments.size(); ++g) {
        groups.keys[g] = fitKey(graph, world, groups, g, sharedFrom);
        const auto fit = FitCache::instance().find(groups.keys[g]);
        if (!fit || fit->surfaces.size() != groups.segments[g].size() ||
            fit->nodeSurfaces.size() != groups.nodes[g].size() || fit->lakes.size() != groups.bodies[g].size())
            continue;
        bool fits = true;
        for (std::size_t n = 0; n < groups.segments[g].size() && fits; ++n)
            fits = fit->surfaces[n].size() == graph.segments[groups.segments[g][n] - 1].course.size();
        if (!fits) continue;
        for (std::size_t n = 0; n < groups.segments[g].size(); ++n) {
            auto& course = graph.segments[groups.segments[g][n] - 1].course;
            for (std::size_t i = 0; i < course.size(); ++i) course[i].surface = fit->surfaces[n][i];
        }
        for (std::size_t n = 0; n < groups.nodes[g].size(); ++n)
            graph.nodes[groups.nodes[g][n] - 1].surface = fit->nodeSurfaces[n];
        for (std::size_t n = 0; n < groups.bodies[g].size(); ++n) {
            WaterBody& body = graph.waterBodies[groups.bodies[g][n] - 1];
            body.level = fit->lakes[n].level;
            body.basinStep = fit->lakes[n].basinStep;
            body.basinSamples = fit->lakes[n].samples;
        }
        groups.active[g] = 0;
    }
}

std::mutex lastFitsGuard;
HydrologyFitStats lastFits;

void keepFits(const HydrologyGraph& graph, const FitGroups& groups) {
    {
        const std::lock_guard<std::mutex> lock(lastFitsGuard);
        lastFits.groups = groups.active.size();
        lastFits.fitted = std::size_t(std::count(groups.active.begin(), groups.active.end(), std::uint8_t(1)));
    }
    if (!groups.cacheable) return;
    for (std::uint32_t g = 0; g < groups.segments.size(); ++g) {
        if (!groups.active[g]) continue;
        auto fit = std::make_shared<GroupFit>();
        for (const RiverId id : groups.segments[g]) {
            std::vector<Fixed> surfaces;
            surfaces.reserve(graph.segments[id - 1].course.size());
            for (const ReachPoint& p : graph.segments[id - 1].course) surfaces.push_back(p.surface);
            fit->surfaces.push_back(std::move(surfaces));
        }
        for (const RiverNodeId id : groups.nodes[g]) fit->nodeSurfaces.push_back(graph.nodes[id - 1].surface);
        for (const WaterBodyId id : groups.bodies[g]) {
            const WaterBody& body = graph.waterBodies[id - 1];
            fit->lakes.push_back({body.level, body.basinStep, body.basinSamples});
        }
        FitCache::instance().keep(groups.keys[g], std::move(fit));
    }
}

// A carver over a window around a point, rebuilt as the point walks out of it:
// the carve of a long course or a big lake is far more country than one
// carver indexes.
class WindowedCarver {
public:
    WindowedCarver(const HydrologyGraph& graph, bool standingWater)
        : graph_(graph), standingWater_(standingWater) {}
    const GraphCarver& at(WorldPos p) {
        if (!carver_ || p.x < window_.min.x || p.y < window_.min.y || p.x >= window_.max.x ||
            p.y >= window_.max.y) {
            constexpr std::int64_t kWindow = 1024;
            const std::int64_t x = floorDiv(p.x.toInt(), kWindow) * kWindow;
            const std::int64_t y = floorDiv(p.y.toInt(), kWindow) * kWindow;
            window_ = {{Fixed::fromInt(x), Fixed::fromInt(y)},
                       {Fixed::fromInt(x + kWindow), Fixed::fromInt(y + kWindow)}};
            carver_.emplace(graph_, window_, 900, standingWater_);
        }
        return *carver_;
    }

private:
    const HydrologyGraph& graph_;
    bool standingWater_;
    std::optional<GraphCarver> carver_;
    core::WorldRect window_{};
};

// Every lake refitted to the ground as the reaches carve it: grown into what
// that ground holds below its level, and lowered to wherever that ground lets
// it out, until its contour is closed.
//
// A natural basin is flooded over the Slopes lattice, sixty-four metres to a
// node and before any river has cut a valley. What the page is drawn from is
// neither: the detail layer's gullies and the reaches' valleys are cut into
// it afterwards, and a lake whose footprint was right on the lattice came out
// with an outlet's gorge running back into its bed, or a valley wall dropped
// away under its rim - water standing over ground a hundred metres below it,
// and ending in the air. The fit the lattice made is kept as where to look and
// how high the water may be; the flood is repeated on the carved ground, every
// sixteen metres, from every node the lattice flooded. Running water stands in
// that ground at its own level - a river channel is not a hole in the rim,
// only as low as the water in it - and so is the sea. Where the flood reaches
// the edge of the country it may look at, the water has found its way out and
// the level falls to wherever that was.
void refineLakesOnCarvedGround(HydrologyGraph& graph, const generation::WorldMapData& world,
                               const FitGroups& groups) {
    constexpr std::int64_t kFinest = kNaturalBasinStep;
    // A lake is flooded at sixteen metres when that is at most a million
    // samples - sixteen kilometres a side, which is every lake a world of
    // painted regions has - and at twice that, and twice again, when it is
    // larger: an inland sea is found out to its outlet just as well at a
    // hundred metres, and at sixteen it was tens of seconds a lake.
    constexpr std::int64_t kMostSamples = 1024 * 1024;
    const std::int64_t margin = generation::kMetresPerCell;
    const std::int64_t wide = std::int64_t(std::max(0, world.width)) * generation::kMetresPerCell;
    const std::int64_t high = std::int64_t(std::max(0, world.height)) * generation::kMetresPerCell;
    // What each lake comes out as. Lakes are refitted each on its own - the
    // graph is only read while they are - so they go wide across the machine
    // and are written back together once every one is done.
    struct Refit {
        bool done = false;
        Fixed level;
        std::int32_t step = 0;
        std::vector<TilePos> samples;
    };
    std::vector<Refit> refits(graph.waterBodies.size());
    const auto refit = [&](const WaterBody& body, HeightField& ground, WindowedCarver& carvers, Refit& out) {
        if (body.kind != WaterBodyKind::Lake || body.basinStep <= 0 || body.basinSamples.empty()) return;
        const std::int64_t step = body.basinStep;
        std::int64_t lowX = std::numeric_limits<std::int64_t>::max(), lowY = lowX;
        std::int64_t highX = std::numeric_limits<std::int64_t>::min(), highY = highX;
        for (const TilePos s : body.basinSamples) {
            lowX = std::min(lowX, s.x * step); highX = std::max(highX, s.x * step);
            lowY = std::min(lowY, s.y * step); highY = std::max(highY, s.y * step);
        }
        std::int64_t kStep = kFinest;
        while ((std::min(wide, highX + margin) - std::max<std::int64_t>(0, lowX - margin)) / kStep *
                       ((std::min(high, highY + margin) - std::max<std::int64_t>(0, lowY - margin)) / kStep) >
               kMostSamples)
            kStep *= 2;
        const std::int64_t x0 = floorDiv(std::max<std::int64_t>(0, lowX - margin), kStep);
        const std::int64_t y0 = floorDiv(std::max<std::int64_t>(0, lowY - margin), kStep);
        const std::int64_t x1 = floorDiv(std::min(wide, highX + margin), kStep);
        const std::int64_t y1 = floorDiv(std::min(high, highY + margin), kStep);
        const std::int64_t columns = x1 - x0 + 1, rows = y1 - y0 + 1;
        if (columns < 3 || rows < 3 || columns * rows > 16 * 1024 * 1024) return;
        // One carver over the lake's whole country when it is small enough
        // for one to index, which is nearly every lake: a flood wanders back
        // and forth across it, and a window that followed it would be rebuilt
        // at every step.
        std::optional<GraphCarver> whole;
        if ((x1 - x0) * kStep <= 48 * 1024 && (y1 - y0) * kStep <= 48 * 1024)
            whole.emplace(graph, core::WorldRect{{Fixed::fromInt(x0 * kStep), Fixed::fromInt(y0 * kStep)},
                                                 {Fixed::fromInt(x1 * kStep), Fixed::fromInt(y1 * kStep)}},
                          64, false);
        const auto heightAt = [&](std::int64_t x, std::int64_t y) {
            const WorldPos p{Fixed::fromInt(x * kStep), Fixed::fromInt(y * kStep)};
            const auto pieces = ground.piecesAt(p.x, p.y);
            const CarvedSample c = (whole ? *whole : carvers.at(p)).carve(p, pieces.country, pieces.moved);
            return c.wet ? core::max(c.floor, c.surface) : c.floor;
        };
        std::vector<std::uint8_t> seen(static_cast<std::size_t>(columns * rows), 0);
        using Visit = std::pair<Fixed::Raw, std::int64_t>;   // minimax spill cost, local index
        std::priority_queue<Visit, std::vector<Visit>, std::greater<Visit>> queue;
        const auto push = [&](std::int64_t x, std::int64_t y, Fixed floorCost) {
            if (x < x0 || y < y0 || x > x1 || y > y1) return;
            const auto local = static_cast<std::size_t>((y - y0) * columns + (x - x0));
            if (seen[local]) return;
            seen[local] = 1;
            queue.emplace(core::max(floorCost, heightAt(x, y)).raw, std::int64_t(local));
        };
        for (const TilePos s : body.basinSamples)
            push(floorDiv(s.x * step, kStep), floorDiv(s.y * step, kStep), Fixed::fromInt(-1000000));
        Fixed level = body.level;
        std::vector<Visit> reached;
        while (!queue.empty()) {
            const auto [cost, local] = queue.top();
            queue.pop();
            if (cost >= level.raw) break;
            const std::int64_t x = x0 + local % columns, y = y0 + local / columns;
            if (x == x0 || y == y0 || x == x1 || y == y1) {
                level = Fixed::fromRaw(cost);
                break;
            }
            reached.emplace_back(cost, local);
            const Fixed here = Fixed::fromRaw(cost);
            push(x - 1, y, here);
            push(x + 1, y, here);
            push(x, y - 1, here);
            push(x, y + 1, here);
        }
        out.done = true;
        out.step = std::int32_t(kStep);
        out.level = core::min(body.level, level);
        for (const auto [cost, local] : reached)
            if (cost < out.level.raw)
                out.samples.push_back({static_cast<std::int32_t>(x0 + local % columns),
                                       static_cast<std::int32_t>(y0 + local / columns)});
        std::sort(out.samples.begin(), out.samples.end(), [](TilePos a, TilePos b) {
            return a.y < b.y || (a.y == b.y && a.x < b.x);
        });
    };
    std::atomic<std::size_t> nextBody{0};
    const auto work = [&] {
        HeightField ground(&world, world.seed);   // a field keeps caches: one to a thread
        WindowedCarver carvers(graph, false);
        for (std::size_t i = nextBody++; i < graph.waterBodies.size(); i = nextBody++)
            if (groups.bodyActive(i)) refit(graph.waterBodies[i], ground, carvers, refits[i]);
    };
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, 8u);
    std::vector<std::thread> workers;
    for (unsigned t = 1; t < threads; ++t) workers.emplace_back(work);
    work();
    for (auto& worker : workers) worker.join();
    if (std::getenv("ASR_HYDRO_TRACE")) {
        std::size_t lakes = 0, samples = 0, largest = 0, coarse = 0;
        for (const auto& r : refits)
            if (r.done) {
                ++lakes;
                samples += r.samples.size();
                largest = std::max(largest, r.samples.size());
                coarse += r.step > kFinest;
            }
        std::fprintf(stderr, "hydrology lakes refitted %zu (%zu coarser), %zu samples under water, largest %zu\n",
                     lakes, coarse, samples, largest);
    }
    for (std::size_t i = 0; i < graph.waterBodies.size(); ++i) {
        if (!refits[i].done) continue;
        WaterBody& body = graph.waterBodies[i];
        body.level = refits[i].level;
        body.basinStep = refits[i].step;
        body.basinSamples = std::move(refits[i].samples);
    }
    // The reaches that meet a lake meet it at its level.
    for (RiverNode& node : graph.nodes) {
        if (node.kind != RiverNodeKind::LakeInlet && node.kind != RiverNodeKind::LakeOutlet) continue;
        if (node.waterBody == kInvalidWaterBodyId || !groups.bodyActive(node.waterBody - 1)) continue;
        if (const WaterBody* body = waterBodyOf(graph, node.waterBody); body && body->kind == WaterBodyKind::Lake)
            node.surface = body->level;
    }
}

// Every course's head fitted to the ground its banks END UP with - after the
// valleys of every other reach have been cut, which is ground no fitting done
// while the graph was still being built could see.
//
// Where a tributary comes down to a trunk river, the trunk's valley is cut
// under the tributary's last stretch, tens of metres below it; the tributary
// kept its own head over that valley and its water stood above the trunk's
// floor, ending at its bank in a curtain thirty metres high. Measured on
// three worlds at eight metres to the sample, one river edge in thirty was
// such a curtain, and almost all of them were in the last cells before a
// confluence, a mouth or a lake.
//
// The rule is the one the first fitting uses, applied to the ground as carve
// makes it: at every point the head may stand no higher than the bank on
// either side - or than another water already standing there - and no higher
// than it stood upstream. So a tributary arriving above its trunk's valley
// falls down the valley wall to the trunk: a waterfall, with the trunk to
// fall into. Upstream first, twice over: a trunk fitted after the tributary
// that meets it can still lower the ground under that tributary's end.
//
// A group at a time (FitGroups): nothing one group reads is written by
// another, so the groups go wide across the machine, and each is fitted in
// the order the whole world's fit would have come to its courses in.
void fitCoursesToCarvedGround(HydrologyGraph& graph, const generation::WorldMapData& world,
                              const std::vector<std::size_t>& sharedFrom, const FitGroups& groups) {
    if (graph.segments.empty()) return;
    auto& segments = graph.segments;
    auto& nodes = graph.nodes;
    std::vector<RiverId> leaving(nodes.size() + 1, kInvalidRiverId);
    for (const RiverSegment& segment : segments) leaving[segment.from] = segment.id;
    // Shared, and written by each group only at its own nodes.
    std::vector<std::size_t> arriving(nodes.size() + 1, 0);
    std::vector<Fixed> arrived(nodes.size() + 1, Fixed::fromInt(1 << 20));
    std::atomic<std::size_t> fittedCourses{0};
    const auto lakeLevel = [&](WaterBodyId id, Fixed& level) {
        const WaterBody* body = waterBodyOf(graph, id);
        if (body == nullptr || body->kind != WaterBodyKind::Lake) return false;
        level = body->level;
        return true;
    };
    // One pass a call: the caller alternates this with refitting the lakes,
    // and the second round is what catches a trunk fitted after a tributary.
    const auto fitGroup = [&](const std::vector<RiverId>& members, HeightField& ground) {
        for (const RiverId id : members)
            if (segments[id - 1].to != kInvalidRiverNodeId) ++arriving[segments[id - 1].to];
        std::vector<RiverId> order;
        for (const RiverId id : members)
            if (arriving[segments[id - 1].from] == 0) order.push_back(id);
        for (std::size_t next = 0; next < order.size(); ++next) {
            RiverSegment& segment = segments[order[next] - 1];
            auto& course = segment.course;
            const RiverNode& from = nodes[segment.from - 1];
            Fixed floorLevel = core::kZero;
            Fixed level;
            if (lakeLevel(segment.destinationWaterBody, level)) floorLevel = level;
            Fixed running = course.front().surface;
            const bool leavesLake = from.kind == RiverNodeKind::LakeOutlet && lakeLevel(from.waterBody, level);
            if (leavesLake)
                running = level;
            else
                running = core::min(running, arrived[segment.from]);
            // Carve reads the course as it stands, this one included: its own
            // valley can only rise to meet a lowered head, never fall below it.
            // A window of its own around each stretch, as HeightField keeps:
            // a trunk's bounds are far more country than one carver indexes.
            std::optional<GraphCarver> carver;
            core::WorldRect window{};
            const auto carverFor = [&](WorldPos at) -> const GraphCarver& {
                if (!carver || at.x < window.min.x || at.y < window.min.y || at.x >= window.max.x ||
                    at.y >= window.max.y) {
                    constexpr std::int64_t kWindow = 1024;
                    const std::int64_t x = floorDiv(at.x.toInt(), kWindow) * kWindow;
                    const std::int64_t y = floorDiv(at.y.toInt(), kWindow) * kWindow;
                    window = {{Fixed::fromInt(x), Fixed::fromInt(y)},
                              {Fixed::fromInt(x + kWindow), Fixed::fromInt(y + kWindow)}};
                    carver.emplace(graph, window, 900);
                }
                return *carver;
            };
            for (std::size_t i = 0; i < course.size(); ++i) {
                ReachPoint& point = course[i];
                Fixed lowest = point.surface;
                // A course leaving a lake leaves it AT the lake's level: its
                // first point is the lake's own spill, whatever its banks.
                if (point.halfWidth.raw > 0 && !(leavesLake && i == 0)) {
                    const WorldPos a = course[i > 0 ? i - 1 : i].position;
                    const WorldPos b = course[i + 1 < course.size() ? i + 1 : i].position;
                    const Fixed dx = b.x - a.x, dy = b.y - a.y;
                    const Fixed length = core::hypot(dx, dy);
                    if (length.raw > 0) {
                        // Just past where this reach's own water reaches, so
                        // what is read is the bank and not the river.
                        const Fixed out = point.halfWidth +
                                          core::max(point.depth, Fixed::fromInt(kSampleMetres / 2)) +
                                          Fixed::fromInt(kSampleMetres / 2);
                        for (const int side : {1, -1}) {
                            const WorldPos bank{point.position.x - dy / length * out * Fixed::fromInt(side),
                                                point.position.y + dx / length * out * Fixed::fromInt(side)};
                            const auto pieces = ground.piecesAt(bank.x, bank.y);
                            const CarvedSample there = carverFor(bank).carve(bank, pieces.country, pieces.moved);
                            // Standing water holds its own level; the river
                            // meets it there rather than going under it.
                            if (there.body != kInvalidWaterBodyId) continue;
                            lowest = core::min(lowest, there.wet ? there.surface : there.floor);
                        }
                    }
                }
                running = core::min(running, lowest);
                point.surface = core::max(running, floorLevel);
            }
            core::progressStep(std::int64_t(fittedCourses.fetch_add(1, std::memory_order_relaxed) + 1));
            if (segment.to != kInvalidRiverNodeId) {
                arrived[segment.to] = core::min(arrived[segment.to], course.back().surface);
                const RiverId onward = leaving[segment.to];
                if (onward != kInvalidRiverId && --arriving[segment.to] == 0) order.push_back(onward);
            }
        }
        // The nodes follow the courses, and every course arriving at a node
        // ends at that node's head: a step there is the fall into the river
        // below, drawn over the last chord.
        for (const RiverId id : members) {
            const RiverSegment& segment = segments[id - 1];
            RiverNode& from = nodes[segment.from - 1];
            if (from.kind != RiverNodeKind::LakeOutlet) from.surface = segment.course.front().surface;
        }
        for (const RiverId id : members) {
            RiverSegment& segment = segments[id - 1];
            if (segment.to == kInvalidRiverNodeId) continue;
            RiverNode& to = nodes[segment.to - 1];
            if (leaving[to.id] == kInvalidRiverId && to.kind != RiverNodeKind::LakeInlet)
                to.surface = core::min(to.surface, arrived[to.id]);
            ReachPoint& tail = segment.course.back();
            tail.surface = core::min(tail.surface, core::max(to.surface, core::kZero));
            // Courses meeting at a confluence share its head over the stretch
            // where their water overlaps (the sharing in buildHydrologyGraph):
            // held there, so the fall into a lower trunk comes before the two
            // waters meet and not in the middle of them.
            if (to.kind == RiverNodeKind::Confluence && segment.id < sharedFrom.size() &&
                sharedFrom[segment.id] > 0)
                for (std::size_t i = sharedFrom[segment.id]; i < segment.course.size(); ++i)
                    segment.course[i].surface = to.surface;
        }
    };
    // The largest first, so the last thread is not left with the continent.
    std::vector<std::uint32_t> work;
    for (std::uint32_t g = 0; g < groups.segments.size(); ++g)
        if (groups.active[g] && !groups.segments[g].empty()) work.push_back(g);
    std::stable_sort(work.begin(), work.end(), [&](std::uint32_t a, std::uint32_t b) {
        return groups.segments[a].size() > groups.segments[b].size();
    });
    std::atomic<std::size_t> next{0};
    std::size_t courses = 0;
    for (const std::uint32_t g : work) courses += groups.segments[g].size();
    core::progress("rivers: fitting courses to the ground", 0, std::int64_t(courses));
    const auto worker = [&] {
        HeightField ground(&world, world.seed);   // a field keeps caches: one to a thread
        for (std::size_t i = next++; i < work.size(); i = next++) fitGroup(groups.segments[work[i]], ground);
    };
    const unsigned threads = std::clamp<unsigned>(std::min<std::size_t>(work.size(), std::thread::hardware_concurrency()), 1u, 8u);
    std::vector<std::thread> workers;
    for (unsigned t = 1; t < threads; ++t) workers.emplace_back(worker);
    worker();
    for (auto& w : workers) w.join();
}
} // namespace

HydrologyGraph buildHydrologyGraph(const generation::WorldMapData& world) {
    static const bool traced = std::getenv("ASR_HYDRO_TRACE") != nullptr;
    auto lapAt = std::chrono::steady_clock::now();
    const auto lapTrace = [&](const char* what) {
        if (!traced) return;
        const auto now = std::chrono::steady_clock::now();
        std::fprintf(stderr, "hydrology %s %.1f ms\n", what, std::chrono::duration<double, std::milli>(now - lapAt).count());
        lapAt = now;
    };
    HydrologyGraph graph;
    graph.worldSeed = world.seed;
    graph.macroCellMetres = generation::kMetresPerCell;
    core::progress("rivers: drainage");

    const std::int32_t width = world.width;
    const std::int32_t height = world.height;
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || world.cells.size() != count) return graph;
    graph.macroWidth = width;
    graph.macroHeight = height;
    graph.sourceFingerprint = fingerprintOf(world);

    // Nothing but sea: the ocean is the whole graph, and every array below
    // would be a cell's worth of nothing for every cell of the world.
    if (std::none_of(world.cells.begin(), world.cells.end(), [](const generation::WorldCell& c) { return !c.sea; })) {
        WaterBody ocean;
        ocean.id = kOceanWaterBodyId;
        ocean.kind = WaterBodyKind::Ocean;
        ocean.sourceRegion = -1;
        ocean.level = core::kZero;
        graph.waterBodies.push_back(ocean);
        return graph;
    }
    const auto columnOf = [width](std::size_t cell) {
        return static_cast<std::int32_t>(cell % static_cast<std::size_t>(width));
    };
    const auto rowOf = [width](std::size_t cell) {
        return static_cast<std::int32_t>(cell / static_cast<std::size_t>(width));
    };
    const auto positionOf = [&](std::size_t cell) {
        return TilePos{columnOf(cell), rowOf(cell)};
    };

    // --- where each cell sends its water ------------------------------------
    //
    // flowDirectionField is the canonical downhill edge and carries -1 for a
    // sink, so a present field is believed exactly as it stands. The per-cell
    // flags are the fallback for a map that predates it or was hand-built for
    // a test. The sea is where drainage ends, never a cell that passes it on.
    std::vector<std::int32_t> downstream(count, -1);
    const bool routed = world.flowDirectionField.size() == count;
    for (std::size_t i = 0; i < count; ++i) {
        const generation::WorldCell& cell = world.cells[i];
        if (cell.sea) continue;
        const std::int32_t dir =
                routed ? world.flowDirectionField[i]
                       : (cell.river && cell.riverOut >= 0 ? cell.riverOut : cell.drainOut);
        if (dir < 0 || dir >= core::kNeighbourCount) continue;
        const TilePos down = core::neighbour(positionOf(i), dir);
        if (!world.inBounds(down)) continue;   // off the map: a terminal, not a course
        downstream[i] = down.y * width + down.x;
    }

    // Land that drains nowhere and holds no lake: ground whose water has not
    // been asked for yet (an import, a pinned sketch - the editor's Primary
    // and Relief stages). The ocean is all the water there is, and walking a
    // continent's worth of cells to find no course in it is what made an
    // import of heights alone wait minutes on "rivers".
    {
        const bool drains = std::any_of(downstream.begin(), downstream.end(), [](std::int32_t d) { return d >= 0; });
        bool lakes = false;
        if (world.lakeRegionField.size() == count)
            for (std::size_t i = 0; i < count && !lakes; ++i)
                lakes = world.lakeRegionField[i] >= 0 && !world.cells[i].sea;
        if (!drains && !lakes) {
            WaterBody ocean;
            ocean.id = kOceanWaterBodyId;
            ocean.kind = WaterBodyKind::Ocean;
            ocean.sourceRegion = -1;
            ocean.level = core::kZero;
            graph.waterBodies.push_back(ocean);
            indexGraph(graph, world);
            return graph;
        }
    }

    // --- water bodies -------------------------------------------------------
    //
    // The ocean is one body by definition. Every lake is a connected run of
    // filled cells standing at one head, which the generator already labelled
    // with its smallest member index - sorting those labels turns them into
    // IDs that do not depend on the order anything was visited in.
    std::vector<WaterBody>& bodies = graph.waterBodies;
    WaterBody ocean;
    ocean.id = kOceanWaterBodyId;
    ocean.kind = WaterBodyKind::Ocean;
    ocean.sourceRegion = -1;
    ocean.level = core::kZero;   // a sea cell stands at elevation zero
    bodies.push_back(ocean);

    std::vector<std::int32_t> bodyOfCell(count, -1);   // index into bodies, -1 off water
    if (world.lakeRegionField.size() == count) {
        std::vector<std::int32_t> labels;
        for (std::size_t i = 0; i < count; ++i)
            if (world.lakeRegionField[i] >= 0 && !world.cells[i].sea)
                labels.push_back(world.lakeRegionField[i]);
        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
        for (std::size_t n = 0; n < labels.size(); ++n) {
            WaterBody lake;
            lake.id = static_cast<WaterBodyId>(bodies.size() + 1);
            lake.kind = WaterBodyKind::Lake;
            lake.sourceRegion = labels[n];
            bodies.push_back(lake);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (world.lakeRegionField[i] < 0 || world.cells[i].sea) continue;
            const auto at = std::lower_bound(labels.begin(), labels.end(), world.lakeRegionField[i]);
            const auto slot = static_cast<std::size_t>(at - labels.begin()) + 1;   // 0 is the ocean
            bodyOfCell[i] = static_cast<std::int32_t>(slot);
            // One head to a body: every member was labelled by equal
            // lakeLevelField, so the first of them - the smallest cell index -
            // answers for all of them.
            if (bodies[slot].level == core::kZero) {
                const std::int32_t steps =
                        world.lakeLevelField.size() == count ? world.lakeLevelField[i] : 0;
                bodies[slot].level =
                        Fixed::fromInt(static_cast<std::int64_t>(std::max(0, steps)) *
                                       generation::kMetresPerElevationStep);
            }
        }
        // Footprints are taken after the holes are closed, and in row-major
        // order, so the first cell of a body is also its smallest index.
        absorbFootprintHoles(world, downstream, bodyOfCell);
        for (std::size_t i = 0; i < count; ++i) {
            if (bodyOfCell[i] < 0) continue;
            WaterBody& body = bodies[static_cast<std::size_t>(bodyOfCell[i])];
            body.macroCells.push_back(positionOf(i));
            // What the flood covered: the brim the map now stores, less how
            // far it was raised. A cell absorbed as a hole in the footprint
            // was raised by nothing and its floor is the brim.
            const std::int64_t fill =
                    world.lakeDepthField.size() == count ? std::max(0, world.lakeDepthField[i]) : 0;
            body.macroCellFloor.push_back(Fixed::fromInt(
                    (static_cast<std::int64_t>(world.cells[i].elevation) - fill) *
                    generation::kMetresPerElevationStep));
        }
    }

    // --- what carries water --------------------------------------------------
    //
    // Not `WorldCell::river`. That flag is drawn above a threshold that scales
    // with the size of the map, so on a continent it marks the trunks and
    // nothing else: on seed 11 at 64 cells it is twelve cells of the whole
    // world, against the eight hundred-odd courses the linter measures water
    // in. A graph built from it would leave every stream on the map to be
    // rediscovered per query, which is the thing this replaces.
    //
    // resolveHydrology answers it over the whole map - the blue lines, plus
    // every catchment that clears its own cell's threshold, plus everything
    // downstream of those - with no camera and no LOD in it. Dry drainage
    // stays out: a gully is a valley, not a body.
    MacroHydrology hydro = resolveHydrology(world, downstream);
    {
        const std::lock_guard<std::mutex> lock(lastFitsGuard);
        lastFits.streamFlow = hydro.streamFlow;
        lastFits.largestFlow = hydro.largestFlow;
    }
    lapTrace("resolve");
    core::progress("rivers: basins");
    if (world.terrainFoundation) {
        for (auto& body:bodies)
            if (body.kind==WaterBodyKind::Lake)
                naturalBasin(body,*world.terrainFoundation,graph.macroCellMetres);
        // Lowering a spill must lower its outlet as well. A body is one head,
        // including all inlet/outlet nodes; never repair this by raising land.
        std::queue<std::size_t> changed;
        for (std::size_t i=0;i<count;++i) if (bodyOfCell[i]>=0) {
            const auto level=bodies[std::size_t(bodyOfCell[i])].level;
            if (level<hydro.surface[i]) { hydro.surface[i]=level;changed.push(i); }
        }
        while (!changed.empty()) {
            const auto i=changed.front();changed.pop();
            const auto next=downstream[i];
            if (next<0 || hydro.surface[next]<=hydro.surface[i]) continue;
            const int body=bodyOfCell[next];
            if (body>=0) {
                auto& lake=bodies[std::size_t(body)];
                lake.level=core::min(lake.level,hydro.surface[i]);
                for (const auto cell:lake.macroCells) {
                    const auto at=std::size_t(cell.y)*width+cell.x;
                    if (hydro.surface[at]>lake.level) { hydro.surface[at]=lake.level;changed.push(at); }
                }
            } else { hydro.surface[next]=hydro.surface[i];changed.push(std::size_t(next)); }
        }
        // Downstream constraints may have lowered another lake in this pass.
        for (auto& body:bodies)
            if (body.kind==WaterBodyKind::Lake)
                naturalBasin(body,*world.terrainFoundation,graph.macroCellMetres);
    }

    lapTrace("basins");
    core::progress("rivers: courses");
    const auto flows = [&](std::size_t cell) {
        return !world.cells[cell].sea && hydro.wet[cell] != 0;
    };
    // Whether the graph represents whatever is at this cell at all: the sea, a
    // standing body, or a course with water in it. A course that runs on into
    // dry drainage has left the network and ends where it does so.
    const auto carries = [&](std::size_t cell) {
        return world.cells[cell].sea || bodyOfCell[cell] >= 0 || flows(cell);
    };

    // --- where the network branches -----------------------------------------
    std::vector<std::uint16_t> inflow(count, 0);        // wet courses arriving
    std::vector<std::uint16_t> foreignInflow(count, 0); // ... from outside this cell's body
    for (std::size_t i = 0; i < count; ++i) {
        if (!flows(i)) continue;
        const std::int32_t next = downstream[i];
        if (next < 0) continue;
        const auto to = static_cast<std::size_t>(next);
        if (inflow[to] < 0xffff) ++inflow[to];
        if (bodyOfCell[to] != bodyOfCell[i] && foreignInflow[to] < 0xffff) ++foreignInflow[to];
    }

    // A cell hosts at most one node that a course can leave from and at most
    // one that a course can arrive at. A confluence is both; a lake shore cell
    // that a river enters and the outflow leaves from is two distinct nodes,
    // because the body between them is not a channel and has no course of its
    // own to carry the flow across.
    std::vector<RiverNodeId> startNode(count, kInvalidRiverNodeId);
    std::vector<RiverNodeId> endNode(count, kInvalidRiverNodeId);
    std::vector<RiverNode>& nodes = graph.nodes;
    const auto addNode = [&](std::size_t cell, RiverNodeKind kind, WaterBodyId body) {
        RiverNode node;
        node.id = static_cast<RiverNodeId>(nodes.size() + 1);
        node.waterBody = body;
        node.macroCell = static_cast<std::int32_t>(cell);
        node.kind = kind;
        node.position = centreOf(columnOf(cell), rowOf(cell));
        node.surface = hydro.surface[cell];
        nodes.push_back(node);
        return node.id;
    };

    // Row-major, with a fixed order of kinds within a cell: that is the whole
    // of the ID rule, and it is why two builds of one world agree.
    for (std::size_t i = 0; i < count; ++i) {
        const generation::WorldCell& cell = world.cells[i];
        const std::int32_t body = bodyOfCell[i];
        const std::int32_t next = downstream[i];

        if (cell.sea) {
            if (inflow[i] > 0) endNode[i] = addNode(i, RiverNodeKind::Mouth, kOceanWaterBodyId);
            continue;
        }
        if (body >= 0) {
            const WaterBodyId id = bodies[static_cast<std::size_t>(body)].id;
            if (foreignInflow[i] > 0) endNode[i] = addNode(i, RiverNodeKind::LakeInlet, id);
            const bool leaves = flows(i) && next >= 0 &&
                                carries(static_cast<std::size_t>(next)) &&
                                (world.cells[static_cast<std::size_t>(next)].sea ||
                                 bodyOfCell[static_cast<std::size_t>(next)] != body);
            if (leaves) startNode[i] = addNode(i, RiverNodeKind::LakeOutlet, id);
            continue;
        }
        if (!flows(i)) continue;

        const bool hasOut = next >= 0 && carries(static_cast<std::size_t>(next));
        if (inflow[i] == 0 && !hasOut) continue;   // a lone flagged cell is not a course
        if (inflow[i] >= 2) {
            const RiverNodeId id = addNode(i, RiverNodeKind::Confluence, kInvalidWaterBodyId);
            endNode[i] = id;
            if (hasOut) startNode[i] = id;
        } else if (inflow[i] == 0) {
            startNode[i] = addNode(i, RiverNodeKind::Source, kInvalidWaterBodyId);
        }
        if (!hasOut && inflow[i] < 2)
            endNode[i] = addNode(i, RiverNodeKind::Terminal, kInvalidWaterBodyId);
    }

    // --- the profile of a course, point by point -----------------------------
    //
    // Everything a reach is made of, at one of its cells. On the point rather
    // than on the segment: a segment is a compressed chain, and one width for
    // the whole of it would step at every junction.
    const Fixed perCell = Fixed::fromInt(generation::kMetresPerCell);

    // How far back a valley has to start for the ground to reach the water at
    // a grade a hillside can hold. Read over the same square the drainage is
    // gathered from, and weighted by distance: a wall two cells away is two
    // cells of ground away from the water as well, and this valley only owes
    // the part of the fall that happens inside it.
    //
    // Without it a reach whose neighbours stand two hundred metres higher has
    // to bring the ground down two hundred metres inside seventy, and every
    // watercourse in the world reads as a slot cut into the country.
    const auto roomFor = [&](std::size_t cell, Fixed surface) {
        const TilePos at = positionOf(cell);
        Fixed above = core::kZero;
        for (std::int32_t oy = -kCellsAround; oy <= kCellsAround; ++oy)
            for (std::int32_t ox = -kCellsAround; ox <= kCellsAround; ++ox) {
                const TilePos n{at.x + ox, at.y + oy};
                if (!world.inBounds(n)) continue;
                const generation::WorldCell& other = world.at(n);
                if (other.sea) continue;
                const Fixed top = Fixed::fromInt(static_cast<std::int64_t>(other.elevation) *
                                                 generation::kMetresPerElevationStep);
                const std::int32_t rings = std::max(std::abs(ox), std::abs(oy));
                const Fixed mine = (top - surface) / Fixed::fromInt(1 + rings);
                if (mine.raw > above.raw) above = mine;
            }
        if (above.raw <= 0) return core::kZero;
        // Three in ten is a hillside a person walks up; past that it is a wall.
        return core::min(above * Fixed::fromInt(10) / Fixed::fromInt(3),
                         Fixed::fromInt(kWidestValley));
    };

    const auto profileAt = [&](std::size_t cell) {
        ReachPoint point;
        point.position = centreOf(columnOf(cell), rowOf(cell));
        point.surface = hydro.surface[cell];
        const generation::WorldCell& here = world.cells[cell];
        const bool holdsWater = !here.sea && hydro.wet[cell] != 0;

        Fixed halfWidth = core::kZero;
        if (holdsWater) {
            halfWidth = hydro.halfWidthFor(here.drainSize);
            // Recover sub-octave discharge instead of quantising every river
            // width to the integer drainSize.
            if (world.riverDischargeField.size() == count && here.drainSize < 30) {
                const auto flow = std::max(1, world.riverDischargeField[cell]);
                halfWidth *= core::sqrt(Fixed::ratio(flow, std::int64_t(1) << here.drainSize));
            }
            const std::int32_t next = downstream[cell];
            if (next >= 0) {
                const Fixed fall = core::max(core::kZero,
                                             hydro.surface[cell] -
                                                     hydro.surface[static_cast<std::size_t>(next)]);
                const Fixed slope = fall / perCell;
                halfWidth *= core::max(Fixed::ratio(45, 100),
                                       core::kOne / (core::kOne + slope * Fixed::fromInt(4)));
            }
            // Slope may contract a trunk, but cannot shrink a brook below the
            // width the four-metre lattice can carry as water.
            halfWidth = core::max(Fixed::fromInt(kBrookHalfWidth), halfWidth);
        }

        Fixed depth = depthFor(here.drainSize);
        // A real channel is far wider than it is deep - a quarter is already
        // steep - so the width is the bound wherever there is water.
        if (holdsWater) depth = core::min(depth, halfWidth / Fixed::fromInt(4));
        Fixed reach = hydro.reachFor(here.drainSize);
        if (!holdsWater) {
            depth *= Fixed::ratio(1, 3);
            reach *= Fixed::ratio(1, 2);
        }
        point.halfWidth = halfWidth;
        point.depth = depth;
        point.valleyReach =
                core::min(core::max(reach, roomFor(cell, point.surface)),
                          Fixed::fromInt(kWidestValley));
        return point;
    };

    // --- segments -----------------------------------------------------------
    std::vector<RiverSegment>& segments = graph.segments;
    std::vector<RiverId> outgoingOf(nodes.size() + 1, kInvalidRiverId);
    std::vector<std::size_t> chain;
    for (std::size_t n = 0; n < nodes.size(); ++n) {
        const RiverNode& from = nodes[n];
        const auto cell = static_cast<std::size_t>(from.macroCell);
        if (startNode[cell] != from.id) continue;

        RiverSegment segment;
        segment.id = static_cast<RiverId>(segments.size() + 1);
        segment.from = from.id;
        if (from.kind == RiverNodeKind::LakeOutlet) segment.sourceWaterBody = from.waterBody;
        chain.assign(1, cell);
        std::size_t at = cell;
        for (std::size_t step = 0; step <= count; ++step) {
            const std::int32_t next = downstream[at];
            if (next < 0) break;   // a start node always has one; guarded anyway
            const auto to = static_cast<std::size_t>(next);
            chain.push_back(to);
            if (endNode[to] != kInvalidRiverNodeId) {
                segment.to = endNode[to];
                const RiverNode& target = nodes[endNode[to] - 1];
                if (target.kind == RiverNodeKind::Mouth || target.kind == RiverNodeKind::LakeInlet)
                    segment.destinationWaterBody = target.waterBody;
                break;
            }
            at = to;
        }

        // The profile at each cell the course passes through, before the
        // shape is put on it.
        std::vector<ReachPoint> knots;
        knots.reserve(chain.size());
        for (const std::size_t member : chain) knots.push_back(profileAt(member));

        // A river arriving at the sea is at its widest, and then some: the
        // last reach is tidal water opening into the valley it drowned, not a
        // ribbon holding the width its own catchment earns. A sea cell also
        // carries no drainage of its own, so read literally it would taper
        // every mouth in the world into a dry bed over its last reach.
        if (knots.size() >= 2 && world.cells[chain.back()].sea) {
            ReachPoint& mouth = knots.back();
            const ReachPoint& last = knots[knots.size() - 2];
            mouth.halfWidth = last.halfWidth * Fixed::ratio(5, 2);
            mouth.depth = core::min(depthFor(world.cells[chain[chain.size() - 2]].drainSize),
                                    mouth.halfWidth / Fixed::fromInt(4));
            mouth.valleyReach = core::max(mouth.valleyReach, last.valleyReach);
        }

        // The segment's own summary is taken where it is largest, which is the
        // last of its cells that is still land.
        for (std::size_t n = chain.size(); n-- > 0;) {
            if (world.cells[chain[n]].sea) continue;
            if (world.riverDischargeField.size() == count)
                segment.discharge =
                        Fixed::fromInt(std::max(0, world.riverDischargeField[chain[n]]));
            segment.width = knots[n].halfWidth * 2;
            segment.depth = knots[n].depth;
            segment.valleyReach = knots[n].valleyReach;
            break;
        }

        // --- and now the shape ---------------------------------------------
        //
        // The control points either side, so the course carries its direction
        // through a junction rather than turning a corner at it. Where there
        // is nothing beyond the end, the line is extended: a headwater has no
        // upstream to curve away from.
        const auto upstreamOf = [&](std::size_t member) {
            const TilePos here = positionOf(member);
            std::size_t best = member;
            std::uint8_t most = 0;
            for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
                const TilePos n = core::neighbour(here, dir);
                if (!world.inBounds(n)) continue;
                const auto index = static_cast<std::size_t>(n.y) * width + n.x;
                if (world.cells[index].sea) continue;
                if (downstream[index] != static_cast<std::int32_t>(member)) continue;
                if (world.cells[index].drainSize >= most) {
                    most = world.cells[index].drainSize;
                    best = index;
                }
            }
            return best;
        };
        const std::size_t feeder = upstreamOf(chain.front());
        const std::int32_t onward = downstream[chain.back()];
        const WorldPos before =
                feeder != chain.front()
                        ? centreOf(columnOf(feeder), rowOf(feeder))
                        : WorldPos{knots.front().position.x * 2 - knots[1].position.x,
                                   knots.front().position.y * 2 - knots[1].position.y};
        const WorldPos after =
                onward >= 0
                        ? centreOf(columnOf(static_cast<std::size_t>(onward)),
                                   rowOf(static_cast<std::size_t>(onward)))
                        : WorldPos{knots.back().position.x * 2 -
                                           knots[knots.size() - 2].position.x,
                                   knots.back().position.y * 2 -
                                           knots[knots.size() - 2].position.y};

        segment.macroCells.reserve(chain.size());
        for (const std::size_t member : chain)
            segment.macroCells.push_back(static_cast<std::int32_t>(member));

        const bool headwater = from.kind == RiverNodeKind::Source;
        segment.course.clear();
        for (std::size_t leg = 0; leg + 1 < knots.size(); ++leg) {
            const ReachPoint& a = knots[leg];
            const ReachPoint& b = knots[leg + 1];
            const WorldPos p0 = leg == 0 ? before : knots[leg - 1].position;
            const WorldPos p3 = leg + 2 < knots.size() ? knots[leg + 2].position : after;

            // How far this leg wanders, and which way. A brook wanders most:
            // a trunk river has the water to hold its line, and a steep reach
            // has the fall to.
            const generation::WorldCell& source = world.cells[chain[leg]];
            const std::uint64_t seed = core::splitmix64(
                    0x9e3779b97f4a7c15ULL ^
                    (static_cast<std::uint64_t>(std::uint32_t(positionOf(chain[leg]).x)) << 20) ^
                    static_cast<std::uint64_t>(std::uint32_t(positionOf(chain[leg]).y)));
            // How far this leg wanders. It grows with the river, and that is the
            // opposite of what it used to do.
            //
            // The old rule divided the wander by the drained area, so a trunk
            // barely left the line between two cell centres - and the line
            // between two cell centres of a grid that drains to one of eight
            // neighbours runs at nought, forty-five or ninety degrees. That is
            // the circuit-board pattern: the biggest, most visible rivers were
            // the straightest, and they were straight along the four directions
            // the flow model happens to have.
            //
            // A meander belt is about fifteen to twenty channel widths across,
            // so a trunk two hundred metres wide wanders over three kilometres
            // and a brook over a hundred metres. Held under half a cell, because
            // beyond that the course would leave the cells its own flow was
            // accumulated through.
            const Fixed gradient = core::max(core::kZero, a.surface - b.surface) / perCell;
            Fixed wander = core::clamp(a.halfWidth * Fixed::fromInt(8),
                                       perCell * Fixed::ratio(12, 100),
                                       perCell * Fixed::ratio(45, 100));
            // A steep reach has the fall to hold its line; a flat one does not.
            wander = wander / (core::kOne + gradient * Fixed::fromInt(12));
            (void)source;

            const Fixed dx = b.position.x - a.position.x, dy = b.position.y - a.position.y;
            const Fixed span = core::max(core::hypot(dx, dy), core::kOne);

            // Chords fine enough that the line never cuts inside its own
            // bank. Two things ask for resolution and the wider ask wins: the
            // deviation of a chord from an arch of amplitude A over n pieces
            // goes as A over n squared, and the curve itself has to be
            // followed at a step tied to the width of the water rather than
            // to the length of the leg. A trunk river holds its line and
            // needs few; a brook wanders furthest and is narrowest, so it
            // needs most.
            const Fixed bank = core::max(a.halfWidth, Fixed::fromInt(4));
            const Fixed forWander = core::sqrt(wander * Fixed::fromInt(4) / bank);
            const Fixed forCurve = span / (bank * Fixed::fromInt(8));
            const auto steps = static_cast<std::int64_t>(std::clamp<std::int64_t>(
                    core::max(forWander, forCurve).toInt() + 1, 6, 24));

            for (std::int64_t n = 0; n < steps; ++n) {
                const Fixed t = Fixed::ratio(n, steps);
                ReachPoint point;
                point.position = {spline(p0.x, a.position.x, b.position.x, p3.x, t) +
                                          (-dy / span) * meander(seed, t) * wander,
                                  spline(p0.y, a.position.y, b.position.y, p3.y, t) +
                                          (dx / span) * meander(seed, t) * wander};
                point.surface = core::lerp(a.surface, b.surface, t);
                // Width varies along the reach as well as depth. A channel of
                // one width for its whole length is a milled slot, and that
                // reads as machined however well the line itself wanders.
                point.halfWidth = core::lerp(a.halfWidth, b.halfWidth, t) * breadth(seed, t);
                point.depth = core::lerp(a.depth, b.depth, t) * bedform(seed, t);
                point.valleyReach = core::lerp(a.valleyReach, b.valleyReach, t);
                // A spring grows out of its hillside over its first cell.
                if (headwater && leg == 0) {
                    const Fixed swell = grown(t);
                    point.halfWidth *= swell;
                    point.depth *= swell;
                    point.valleyReach *= swell;
                }
                segment.course.push_back(point);
            }
        }
        segment.course.push_back(knots.back());

        // The envelope covers the water, and the water may stand as far out
        // as the valley: a page index built on the channel alone would not
        // list the reach that floods the page's own corner.
        // The envelope covers the water, and the water may stand as far out as
        // the valley: a page index built on the channel alone would not list
        // the reach that floods the page's own corner. Taken per point, so a
        // wide mouth does not grow the whole run.
        WorldPos low = segment.course.front().position, high = low;
        Fixed reach = core::kZero;
        for (const ReachPoint& point : segment.course) {
            low = {core::min(low.x, point.position.x), core::min(low.y, point.position.y)};
            high = {core::max(high.x, point.position.x), core::max(high.y, point.position.y)};
            reach = core::max(reach, core::max(point.halfWidth, point.valleyReach));
        }
        segment.bounds = {{low.x - reach, low.y - reach}, {high.x + reach, high.y + reach}};

        outgoingOf[from.id] = segment.id;
        segments.push_back(std::move(segment));
    }

    lapTrace("segments");
    core::progress("rivers: order");
    // --- how it all joins up -------------------------------------------------
    for (RiverNode& node : nodes) {
        const RiverId out = outgoingOf[node.id];
        if (out != kInvalidRiverId) node.downstream = segments[out - 1].to;
    }
    for (const RiverSegment& segment : segments) {
        if (segment.destinationWaterBody != kInvalidWaterBodyId)
            bodies[segment.destinationWaterBody - 1].inlets.push_back(segment.id);
        if (segment.sourceWaterBody != kInvalidWaterBodyId)
            bodies[segment.sourceWaterBody - 1].outlets.push_back(segment.id);
    }
    // Inside standing water there is no course to follow, so an inlet is linked
    // to its body's lowest-indexed outflow rather than to a route invented
    // across the lake. Nodes are emitted in cell order, so "first seen" is that
    // lowest index and nothing else.
    std::vector<RiverNodeId> primaryOutlet(bodies.size() + 1, kInvalidRiverNodeId);
    for (const RiverNode& node : nodes)
        if (node.kind == RiverNodeKind::LakeOutlet && node.waterBody != kInvalidWaterBodyId &&
            primaryOutlet[node.waterBody] == kInvalidRiverNodeId)
            primaryOutlet[node.waterBody] = node.id;
    for (RiverNode& node : nodes) {
        if (node.kind != RiverNodeKind::LakeInlet || node.waterBody == kInvalidWaterBodyId)
            continue;
        const RiverNodeId outlet = primaryOutlet[node.waterBody];
        // absorbFootprintHoles removes the way this happens, and the link is
        // still refused rather than trusted: a followable downstream that can
        // loop is worse than one that stops.
        const RiverId leaving = outlet == kInvalidRiverNodeId ? kInvalidRiverId : outgoingOf[outlet];
        if (leaving != kInvalidRiverId &&
            segments[leaving - 1].destinationWaterBody == node.waterBody)
            continue;
        node.downstream = outlet;
    }

    // --- water that stays in its channel -------------------------------------
    //
    // A reach's head was decided per macro cell - the lowest ground along the
    // cell's way down, made to fall monotonically - and then drawn along a
    // course that wanders up to half a cell off that line, over ground nobody
    // had looked at. Where that ground lies below the head the channel is
    // perched: the water reaches its banks, the banks are lower than it, and
    // it ends in mid-air a few metres out. Measured on the page the renderer
    // draws, at four metres to the sample, one river edge in five in the
    // worst window stood eight to fourteen metres over the ground beside it -
    // most of them an outlet held at its lake's level while the country fell
    // away under it.
    //
    // So each course is fitted to the ground it actually crosses, downstream
    // from its source: at every point the head may be no higher than the
    // ground either bank will stand at, and no higher than it was upstream.
    // Where the country rises across the way the river keeps its level and
    // cuts through; where it falls away the river falls with it - rapids, or
    // over a scarp a waterfall - and runs on below. Nothing flows under the
    // sea, and a reach arriving in a lake arrives at the lake's level.
    //
    // The bank is read where carve will put it: the country, and the quarter
    // of the detail layer carve keeps beside a channel. Inside a lake the
    // course is the lake's and is left alone.
    {
        world::HeightField ground(&world, world.seed);
        const auto bankAt = [&](WorldPos at) {
            const auto pieces = ground.piecesAt(at.x, at.y);
            return pieces.country + pieces.moved * Fixed::ratio(1, 4);
        };
        const auto inLake = [&](WorldPos at) {
            const std::int64_t cx = floorDiv(at.x.toInt(), generation::kMetresPerCell);
            const std::int64_t cy = floorDiv(at.y.toInt(), generation::kMetresPerCell);
            if (cx < 0 || cy < 0 || cx >= width || cy >= height) return false;
            return bodyOfCell[static_cast<std::size_t>(cy * width + cx)] > 0;
        };
        // Upstream first. A node has at most one course leaving it, so a
        // course is ready once everything arriving at its start is fitted.
        std::vector<std::size_t> arriving(nodes.size() + 1, 0);
        for (const RiverSegment& segment : segments)
            if (segment.to != kInvalidRiverNodeId) ++arriving[segment.to];
        std::vector<Fixed> arrived(nodes.size() + 1, Fixed::fromInt(1 << 20));
        std::vector<RiverId> order;
        for (const RiverSegment& segment : segments)
            if (arriving[segment.from] == 0) order.push_back(segment.id);
        for (std::size_t next = 0; next < order.size(); ++next) {
            RiverSegment& segment = segments[order[next] - 1];
            auto& course = segment.course;
            const RiverNode& from = nodes[segment.from - 1];
            Fixed floorLevel = core::kZero;
            if (segment.destinationWaterBody != kInvalidWaterBodyId &&
                bodies[segment.destinationWaterBody - 1].kind == WaterBodyKind::Lake)
                floorLevel = bodies[segment.destinationWaterBody - 1].level;
            Fixed running = course.front().surface;
            if (from.kind == RiverNodeKind::LakeOutlet && from.waterBody != kInvalidWaterBodyId)
                running = bodies[from.waterBody - 1].level;
            else
                running = core::min(running, arrived[segment.from]);
            for (std::size_t i = 0; i < course.size(); ++i) {
                ReachPoint& point = course[i];
                Fixed lowest = point.surface;
                if (point.halfWidth.raw > 0 && !inLake(point.position)) {
                    const WorldPos a = course[i > 0 ? i - 1 : i].position;
                    const WorldPos b = course[i + 1 < course.size() ? i + 1 : i].position;
                    const Fixed dx = b.x - a.x, dy = b.y - a.y;
                    const Fixed length = core::hypot(dx, dy);
                    if (length.raw > 0) {
                        const Fixed out = point.halfWidth +
                                          core::max(point.depth, Fixed::fromInt(world::kSampleMetres / 2));
                        const Fixed ax = -dy / length * out, ay = dx / length * out;
                        lowest = core::min(lowest, core::min(bankAt({point.position.x + ax, point.position.y + ay}),
                                                             bankAt({point.position.x - ax, point.position.y - ay})));
                    }
                }
                running = core::min(running, lowest);
                point.surface = core::max(running, floorLevel);
            }
            if (segment.to != kInvalidRiverNodeId) {
                arrived[segment.to] = core::min(arrived[segment.to], course.back().surface);
                const RiverId onward = outgoingOf[segment.to];
                if (onward != kInvalidRiverId && --arriving[segments[onward - 1].from] == 0)
                    order.push_back(onward);
            }
        }
        // The heads at the nodes follow: a course starts where its node stands
        // and every course arriving at a node ends there (the sharing below
        // brings each arriving tail down to it).
        for (const RiverSegment& segment : segments) {
            RiverNode& from = nodes[segment.from - 1];
            if (from.kind != RiverNodeKind::LakeOutlet) from.surface = segment.course.front().surface;
        }
        for (const RiverSegment& segment : segments) {
            if (segment.to == kInvalidRiverNodeId) continue;
            RiverNode& to = nodes[segment.to - 1];
            if (outgoingOf[to.id] == kInvalidRiverId && to.kind != RiverNodeKind::LakeInlet)
                to.surface = core::min(to.surface, arrived[to.id]);
        }
    }

    // --- Strahler order ------------------------------------------------------
    //
    // A lake is not a break in the river: its outflow continues from what ran
    // into it, or every lake on the map would start a first-order stream again.
    std::vector<std::vector<RiverId>> incoming(nodes.size() + 1);
    for (const RiverSegment& segment : segments)
        if (segment.to != kInvalidRiverNodeId) incoming[segment.to].push_back(segment.id);

    // Where each course arriving at a confluence starts sharing the junction's
    // head (below), kept so the fitting to the carved ground can hold it there.
    std::vector<std::size_t> sharedFrom(segments.size() + 1, 0);
    // Tributaries share water BEFORE their centrelines meet. Give that whole
    // overlap the junction's head; otherwise the carver's union of water heads
    // raises a flat tributary as a higher neighbour approaches (seed 11).
    // Find overlap of wet corridors, not valleys, and start the shared profile
    // at the upstream endpoint of each intersecting chord. The preceding chord
    // interpolates down to it outside the overlap, without an artificial step.
    const auto wetReach = [](const ReachPoint& p) {
        return p.halfWidth + core::max(p.depth, Fixed::fromInt(world::kSampleMetres / 2));
    };
    const auto distanceToChord = [](WorldPos p, WorldPos a, WorldPos b) {
        const auto dx = b.x - a.x, dy = b.y - a.y;
        const auto lengthSquared = dx * dx + dy * dy;
        const auto t = lengthSquared > core::kZero
                ? core::saturate(((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared)
                : core::kZero;
        return core::hypot(p.x - a.x - dx * t, p.y - a.y - dy * t);
    };
    for (const auto& node : nodes) {
        const auto& feeders = incoming[node.id];
        if (feeders.size() < 2) continue;
        std::vector<std::size_t> firstShared;
        for (const auto id : feeders) firstShared.push_back(segments[id - 1].course.size() - 1);
        for (std::size_t a = 0; a < feeders.size(); ++a)
            for (std::size_t b = a + 1; b < feeders.size(); ++b) {
                const auto& left = segments[feeders[a] - 1].course;
                const auto& right = segments[feeders[b] - 1].course;
                for (std::size_t i = 0; i + 1 < left.size(); ++i)
                    for (std::size_t j = 0; j + 1 < right.size(); ++j) {
                        const auto& p = left[i]; const auto& q = left[i + 1];
                        const auto& r = right[j]; const auto& s = right[j + 1];
                        const auto span = core::max(wetReach(p), wetReach(q)) +
                                          core::max(wetReach(r), wetReach(s));
                        if (core::min(p.position.x, q.position.x) > core::max(r.position.x, s.position.x) + span ||
                            core::max(p.position.x, q.position.x) < core::min(r.position.x, s.position.x) - span ||
                            core::min(p.position.y, q.position.y) > core::max(r.position.y, s.position.y) + span ||
                            core::max(p.position.y, q.position.y) < core::min(r.position.y, s.position.y) - span)
                            continue;
                        const auto distance = std::min({distanceToChord(p.position, r.position, s.position),
                            distanceToChord(q.position, r.position, s.position),
                            distanceToChord(r.position, p.position, q.position),
                            distanceToChord(s.position, p.position, q.position)});
                        // Chords crossing in their interiors also overlap.
                        const auto cross = [](WorldPos u, WorldPos v, WorldPos w) {
                            return (v.x-u.x)*(w.y-u.y) - (v.y-u.y)*(w.x-u.x);
                        };
                        const auto opposite = [](Fixed u, Fixed v) {
                            return (u <= core::kZero && v >= core::kZero) ||
                                   (u >= core::kZero && v <= core::kZero);
                        };
                        const bool crossing = opposite(cross(p.position, q.position, r.position), cross(p.position, q.position, s.position)) &&
                                              opposite(cross(r.position, s.position, p.position), cross(r.position, s.position, q.position));
                        if (distance > span && !crossing) continue;
                        firstShared[a] = std::min(firstShared[a], i);
                        firstShared[b] = std::min(firstShared[b], j);
                    }
            }
        for (std::size_t n = 0; n < feeders.size(); ++n) {
            auto& course = segments[feeders[n] - 1].course;
            // Keep the upstream node's canonical head. A single shared chord
            // still interpolates to the junction rather than altering topology.
            sharedFrom[feeders[n]] = std::max<std::size_t>(1, firstShared[n]);
            for (std::size_t i = std::max<std::size_t>(1, firstShared[n]); i < course.size(); ++i)
                course[i].surface = node.surface;
        }
    }
    const auto feedersOf = [&](const RiverSegment& segment) -> const std::vector<RiverId>& {
        const RiverNode& from = nodes[segment.from - 1];
        if (from.kind == RiverNodeKind::LakeOutlet && from.waterBody != kInvalidWaterBodyId)
            return bodies[from.waterBody - 1].inlets;
        return incoming[segment.from];
    };
    std::vector<std::size_t> waiting(segments.size() + 1, 0);
    std::vector<std::vector<RiverId>> consumers(segments.size() + 1);
    for (const RiverSegment& segment : segments) {
        const std::vector<RiverId>& feeders = feedersOf(segment);
        waiting[segment.id] = feeders.size();
        for (const RiverId feeder : feeders) consumers[feeder].push_back(segment.id);
    }
    std::vector<RiverId> ready;
    for (const RiverSegment& segment : segments)
        if (waiting[segment.id] == 0) ready.push_back(segment.id);
    for (std::size_t head = 0; head < ready.size(); ++head) {
        RiverSegment& segment = segments[ready[head] - 1];
        std::uint8_t best = 0;
        std::size_t equals = 0;
        for (const RiverId feeder : feedersOf(segment)) {
            const std::uint8_t order = segments[feeder - 1].order;
            if (order > best) {
                best = order;
                equals = 1;
            } else if (order == best) {
                ++equals;
            }
        }
        segment.order = best == 0 ? 1
                                  : static_cast<std::uint8_t>(std::min<std::int32_t>(
                                            best + (equals >= 2 ? 1 : 0), 255));
        for (const RiverId consumer : consumers[segment.id])
            if (--waiting[consumer] == 0) ready.push_back(consumer);
    }
    for (RiverNode& node : nodes) {
        std::uint8_t order = 0;
        for (const RiverId in : incoming[node.id]) order = std::max(order, segments[in - 1].order);
        const RiverId out = outgoingOf[node.id];
        if (out != kInvalidRiverId) order = std::max(order, segments[out - 1].order);
        node.order = order;
    }

    // --- page index ----------------------------------------------------------
    //
    // Built before the fittings below, which carve through it, and again after
    // standing water has been refitted to the ground it ended up on.
    lapTrace("order");
    core::progress("rivers: fit groups");
    // Which water reads which, and the fit of every group that is what it was
    // the last time it was fitted (FitGroups): only the rest are fitted below.
    FitGroups groups = fitGroups(graph);
    reuseFits(graph, world, groups, sharedFrom);
    if (traced) {
        std::size_t fitted = 0;
        for (const auto a : groups.active) fitted += a;
        std::fprintf(stderr, "hydrology %zu group(s), %zu to fit\n", groups.active.size(), fitted);
    }
    lapTrace("groups");
    core::progress("rivers: index");
    indexGraph(graph, world);
    lapTrace("index");

    // --- the water fitted to the ground it ended up in --------------------------
    //
    // Everything above decided heads from the macro map and the uncarved
    // country. What the page is drawn from is the country as the reaches carve
    // it, and the standing water and the running water are both fitted to that
    // here, the lakes first because a river leaving or entering one takes its
    // level from it. Twice, since each moves the ground the other stands on.
    for (int round = 0; round < 2; ++round) {
        if (world.terrainFoundation) {
            core::progress("rivers: fitting lakes to the ground");
            refineLakesOnCarvedGround(graph, world, groups);
            lapTrace("refine lakes");
            indexGraph(graph, world);
            lapTrace("index");
        }
        fitCoursesToCarvedGround(graph, world, sharedFrom, groups);
        lapTrace("fit courses");
    }
    keepFits(graph, groups);
    return graph;
}

} // namespace world::streaming

namespace world::streaming {

std::uint64_t hydrologySourceFingerprint(const generation::WorldMapData& world) { return fingerprintOf(world); }

HydrologyFitStats lastHydrologyFits() {
    const std::lock_guard<std::mutex> lock(lastFitsGuard);
    return lastFits;
}
void forgetHydrologyFits() { FitCache::instance().clear(); }

std::shared_ptr<const HydrologyGraph> sharedHydrologyGraph(
        const generation::WorldMapData& world) {
    return sharedHydrologyGraph(world, {});
}

std::shared_ptr<const HydrologyGraph> sharedHydrologyGraph(
        const generation::WorldMapData& world, const std::filesystem::path& cacheRoot) {
    struct Held {
        const generation::WorldMapData* map;
        std::uint64_t seed;
        std::int32_t width, height;
        std::weak_ptr<const HydrologyGraph> graph;
    };
    static std::mutex guard;
    static std::vector<Held> held;

    const std::lock_guard<std::mutex> lock(guard);
    for (auto it = held.begin(); it != held.end();) {
        if (auto graph = it->graph.lock()) {
            if (it->map == &world && it->seed == world.seed && it->width == world.width &&
                it->height == world.height)
                return graph;
            ++it;
        } else {
            it = held.erase(it);   // its world has gone
        }
    }
    // From disk when this very map was opened before: the graph is a pure
    // function of the map, and building it is most of what raising a
    // snapshot costs. Held under the lock like the build is, so two fields
    // asking at once do not both read it.
    std::shared_ptr<const HydrologyGraph> graph;
    std::filesystem::path root;
    if (!cacheRoot.empty() && world.width > 0 && world.height > 0) {
        const std::uint64_t source = fingerprintOf(world);
        root = cacheRoot / "graphs" / (std::to_string(world.seed) + "-" + std::to_string(source));
        HydrologyGraph stored;
        if (readHydrologyGraphCache(root, world.seed, source, stored)) graph = std::make_shared<const HydrologyGraph>(std::move(stored));
    }
    if (!graph) {
        graph = std::make_shared<const HydrologyGraph>(buildHydrologyGraph(world));
        if (!root.empty()) (void)writeHydrologyGraphCache(root, *graph);
    }
    held.push_back({&world, world.seed, world.width, world.height, graph});
    return graph;
}

} // namespace world::streaming
