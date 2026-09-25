#include "game/world/terrain_streaming/hydrology_builder.hpp"

#include "game/world/height_field.hpp"

#include <memory>
#include <mutex>

#include <algorithm>
#include <map>
#include <queue>
#include <limits>
#include <utility>
#include <vector>

#include <stdexcept>

#include "engine/core/rng.hpp"
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
    if (world.terrainFoundation) hash = mixIn(hash, world.terrainFoundation->fingerprint());
    for (const generation::WorldCell& cell : world.cells) {
        // Moisture is in here because it decides what counts as a stream, and
        // so reaches the width of every channel on the map.
        hash = mixIn(hash, (std::uint64_t(cell.elevation) << 40) |
                                   (std::uint64_t(cell.drainSize) << 32) |
                                   (std::uint64_t(std::uint8_t(cell.drainOut)) << 24) |
                                   (std::uint64_t(std::uint8_t(cell.riverOut)) << 16) |
                                   (std::uint64_t(cell.moisture) << 8) |
                                   (cell.sea ? 2u : 0u) | (cell.river ? 1u : 0u));
    }
    const auto fold = [&hash](const std::vector<std::int32_t>& field) {
        hash = mixIn(hash, field.size());
        for (const std::int32_t value : field)
            hash = mixIn(hash, static_cast<std::uint64_t>(static_cast<std::uint32_t>(value)));
    };
    fold(world.lakeRegionField);
    fold(world.lakeLevelField);
    fold(world.riverDischargeField);
    hash = mixIn(hash, world.flowDirectionField.size());
    for (const std::int8_t dir : world.flowDirectionField)
        hash = mixIn(hash, static_cast<std::uint64_t>(static_cast<std::uint8_t>(dir)));
    return hash;
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
                static_cast<std::int32_t>(streamFlow) + shift, kNarrowestWetFlow, 15));
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
    for (int size = 15; size >= 0; --size) {
        running += above[size];
        chosen = static_cast<std::uint8_t>(size);
        if (running * kWetShareDenominator >= carved * kWetShareNumerator) break;
    }
    out.streamFlow = std::max(chosen, kNarrowestWetFlow);

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
    world::HeightField terrain(&world, world.seed);
    const std::int64_t cellMetres = generation::kMetresPerCell;
    const auto groundAt = [&](Fixed x, Fixed y) {
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
    const auto thalwegOf = [&](std::size_t cell) {
        const core::WorldPos here = centreOfCell(cell);
        Fixed lowest = groundAt(here.x, here.y);
        for (const auto [ox, oy] : {std::pair{-1, 0}, std::pair{1, 0}, std::pair{0, -1},
                                    std::pair{0, 1}}) {
            const Fixed px = here.x + Fixed::fromInt(ox * cellMetres / 3);
            const Fixed py = here.y + Fixed::fromInt(oy * cellMetres / 3);
            lowest = core::min(lowest, groundAt(px, py));
        }
        const std::int32_t next = downstream[cell];
        if (next >= 0) {
            const core::WorldPos there = centreOfCell(static_cast<std::size_t>(next));
            constexpr int kSteps = 8;
            for (int step = 1; step < kSteps; ++step) {
                const Fixed t = Fixed::ratio(step, kSteps);
                lowest = core::min(lowest, groundAt(here.x + (there.x - here.x) * t,
                                                    here.y + (there.y - here.y) * t));
            }
        }
        return lowest;
    };

    for (std::size_t i = 0; i < count; ++i) {
        const generation::WorldCell& cell = world.cells[i];
        const bool lake = i < world.lakeDepthField.size() && world.lakeDepthField[i] > 0 &&
                          i < world.lakeLevelField.size();
        const int head = cell.sea ? 0 : lake ? world.lakeLevelField[i] : cell.elevation;
        Fixed surface = Fixed::fromInt(static_cast<std::int64_t>(std::max(0, head)) *
                                       generation::kMetresPerElevationStep);
        if (!cell.sea && !lake)
            surface = core::min(surface, thalwegOf(i));
        out.surface[i] = surface;
        out.wet[i] = !cell.sea && (cell.river || cell.drainSize >= out.streamFlowIn(cell));
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
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const auto next = downstream[static_cast<std::size_t>(*it)];
        if (next >= 0 && out.wet[static_cast<std::size_t>(*it)])
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

HydrologyGraph buildHydrologyGraph(const generation::WorldMapData& world) {
    HydrologyGraph graph;
    graph.worldSeed = world.seed;
    graph.macroCellMetres = generation::kMetresPerCell;

    const std::int32_t width = world.width;
    const std::int32_t height = world.height;
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (width <= 0 || height <= 0 || world.cells.size() != count) return graph;
    graph.macroWidth = width;
    graph.macroHeight = height;
    graph.sourceFingerprint = fingerprintOf(world);

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

    // --- Strahler order ------------------------------------------------------
    //
    // A lake is not a break in the river: its outflow continues from what ran
    // into it, or every lake on the map would start a first-order stream again.
    std::vector<std::vector<RiverId>> incoming(nodes.size() + 1);
    for (const RiverSegment& segment : segments)
        if (segment.to != kInvalidRiverNodeId) incoming[segment.to].push_back(segment.id);

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
    // Keyed by (y, x) so the map's own order is the order findHydrologySpatialPage
    // binary-searches. Storage pages are square and world aligned; that a ring
    // scheduler will ask for them in a different order is not this index's
    // business.
    std::map<std::pair<std::int32_t, std::int32_t>, HydrologySpatialPage> pages;
    const auto pageAt = [&](std::int32_t x, std::int32_t y) -> HydrologySpatialPage& {
        HydrologySpatialPage& page = pages[{y, x}];
        page.key = {x, y, 0};
        return page;
    };
    for (const RiverSegment& segment : segments) {
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
    for (std::size_t slot = 1; slot < bodies.size(); ++slot) {
        const auto& body = bodies[slot];
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
    // The ocean keeps no footprint of its own, so it is indexed straight off
    // the sea mask. Without this a page query would be total for lakes and
    // partial for the one body most of the map is made of.
    for (std::size_t i = 0; i < count; ++i)
        if (world.cells[i].sea) indexBodyCell(kOceanWaterBodyId, positionOf(i), 0);

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
    return graph;
}

} // namespace world::streaming

namespace world::streaming {

std::shared_ptr<const HydrologyGraph> sharedHydrologyGraph(
        const generation::WorldMapData& world) {
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
    auto graph = std::make_shared<const HydrologyGraph>(buildHydrologyGraph(world));
    held.push_back({&world, world.seed, world.width, world.height, graph});
    return graph;
}

} // namespace world::streaming
