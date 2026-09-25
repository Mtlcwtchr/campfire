#include "game/world/macro.hpp"

#include <iterator>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "engine/core/rng.hpp"
#include "game/generation/world_map_gen.hpp"

namespace world {
namespace {

using core::Fixed;
using core::WorldPos;

// How wide the water is, how deep it runs and how far out it draws the ground
// down, by how much water comes down it.
//
// Sized by the flow itself, not by how far the flow is above the threshold at
// which a blue line gets drawn. That threshold scales with the size of the map,
// so counting doublings above it put nearly every river in the world within one
// doubling of every other: measured across a continent, four river cells in
// five came out between thirty and forty metres wide, whatever they drained.
//
// Width goes as the square root of the flow, which is roughly how real channels
// widen. The flow is already kept as its own logarithm, so that is a halving of
// the exponent - a table, because a fractional power in fixed point is not
// worth the arithmetic.
// The narrowest channel this world will draw water in.
//
// Four doublings is five and a half metres across and about seventy centimetres
// deep. Three is four metres and half a metre, and that is past what a height
// lattice sampled every four metres can carry: the water level is worked out at
// the point asked about and the ground is interpolated from the lattice around
// it, and where the channel is shallower than the difference between those two
// ways of measuring, whether any given point is under water is a coin toss.
//
// Swept, over 564 streams on seed 11 and 579 on seed 3:
//
//     floor 3   591 channels wet (55%)   96.9% of the course wet, dry runs to 200 m
//     floor 4   382 channels wet (36%)   99.7% of the course wet, dry runs to  36 m
//     floor 5   158 channels wet (15%)   99.7%                     dry runs to  60 m
//
// Three puts water in half the network and draws a good deal of it as a dashed
// line, and a dashed river is an artefact where a dry gully is only a dry gully.
// Four is the most water this lattice can carry as water.
constexpr std::uint8_t kNarrowestWetFlow = 4;

// And what share of the carved network holds it: one valley in five.
//
// A share rather than a number, for exactly the reason the coarse map's own
// river threshold is a share (riverFlowThresholdFrom): flow is the drainage area
// above a cell, so it grows with the whole map, and a fixed number tuned at one
// size means nothing at another. This was a fixed 9, and on a world 52 km across
// the largest catchment on the map is 8 doublings - so the rule never fired
// once. Measured on seed 11: 1070 channels carved, 26 of them wet, 97.6% of the
// watercourses in the world dry gullies, and the only water anywhere was the 1%
// of cells the coarse map had flagged by hand.
constexpr std::int64_t kWetShareNumerator = 1, kWetShareDenominator = 3;

// The narrowest and widest water this world draws, as half-widths in metres.
//
// A brook you step over, and a trunk river you do not cross without a boat.
// Four metres is as narrow as the 4m height lattice can carry and still be
// water at every sample rather than at every other one; seventy is a river a
// hundred and forty across, which is what the largest catchment on a continent
// earns.
constexpr std::int64_t kBrookHalfWidth = 4, kTrunkHalfWidth = 70;

// The widest a valley may be drawn, in metres. Set by how far the drainage is
// gathered around a sample rather than by anything about rivers: carve() reads
// the channels of the cells within kCellsAround, so a valley reaching further
// than those would be cut where its own channel happens to be gathered and left
// uncut where it is not, which is a step across the ground.
constexpr std::int64_t kWidestValley = 800;

// Two waters within this many metres of each other in height are the same
// water for the purpose of dividing them: two reaches of one river, or a
// tributary at its confluence, whose flood plains are meant to join.
// And however close a different water runs, a valley keeps this much of itself.
// Below it the bank is a wall again, which is the thing all of this is for.

Fixed carveStrength(std::uint8_t flowLog) {
    if (flowLog <= 2) return core::kZero;
    if (flowLog >= 6) return core::kOne;
    return Fixed::ratio(flowLog - 2, 4);
}

Fixed depthFor(std::uint8_t flowLog) {
    // A metre and a half of gully, up to about eleven for a trunk river's bed.
    return (Fixed::ratio(3, 2) + Fixed::fromInt(std::min<std::uint8_t>(flowLog, 15)) *
                                         Fixed::ratio(3, 5)) *
           carveStrength(flowLog);
}

// How far a channel wanders off the straight line between the two cell centres
// it joins, at a fraction along it.
//
// Zero at both ends, so the network stays joined: a channel meanders between
// its junctions and arrives exactly where the next one starts. Without this a
// river is a ruled line half a kilometre long with a corner at each end, and a
// country of them looks like a circuit board - which is what the first cut
// through the ground actually produced.
Fixed meander(std::uint64_t seed, Fixed t) {
    // One arch, its height taken from the channel's own seed, its sign too.
    // sin is not available here and is not wanted: this is 4t(1-t), which is an
    // arch of the same shape and is exact in fixed point.
    const Fixed arch = t * (core::kOne - t) * Fixed::fromInt(4);
    const std::uint64_t h = core::splitmix64(seed);
    const Fixed size = Fixed::ratio(static_cast<std::int64_t>(h % 1000), 1000);
    const Fixed sign = (h & (1ULL << 40)) ? core::kOne : -core::kOne;
    return arch * arch * size * sign; // zero tangent at shared junctions
}

// A point on the water's course, at a fraction along this reach.
//
// Catmull-Rom through the four cell centres, so the course carries its
// direction through the joins instead of turning a corner at each one, plus the
// wander across it. The arch of the wander is nought at both ends, which is
// what keeps the reaches joined where they meet.
WorldPos onCurve(const Channel& c, Fixed t) {
    const auto spline = [&](Fixed p0, Fixed p1, Fixed p2, Fixed p3) {
        const Fixed t2 = t * t, t3 = t2 * t;
        const Fixed a = p1 * Fixed::fromInt(2);
        const Fixed b = (p2 - p0) * t;
        const Fixed d = (p0 * Fixed::fromInt(2) - p1 * Fixed::fromInt(5) + p2 * Fixed::fromInt(4) -
                         p3) *
                        t2;
        const Fixed e = (p1 * Fixed::fromInt(3) - p0 - p2 * Fixed::fromInt(3) + p3) * t3;
        return (a + b + d + e) * Fixed::ratio(1, 2);
    };
    const Fixed x = spline(c.before.x, c.from.x, c.to.x, c.after.x);
    const Fixed y = spline(c.before.y, c.from.y, c.to.y, c.after.y);

    // Across the course, by however much this reach wanders.
    const Fixed dx = c.to.x - c.from.x, dy = c.to.y - c.from.y;
    const Fixed span = (dx.raw != 0 && dy.raw != 0)
                               ? Fixed::fromInt(generation::kMetresPerCell) *
                                         Fixed::ratio(14142, 10000)
                               : Fixed::fromInt(generation::kMetresPerCell);
    const Fixed swing = meander(c.wander, t) * c.wanderReach;
    return {x + (-dy / span) * swing, y + (dx / span) * swing};
}

// The closest point of a segment to p, as a fraction along it.
Fixed alongTowards(WorldPos a, WorldPos b, WorldPos p) {
    const Fixed dx = b.x - a.x, dy = b.y - a.y;
    const Fixed lengthSquared = dx * dx + dy * dy;
    if (lengthSquared.raw <= 0) return core::kZero;
    Fixed t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
    if (t.raw < 0) t = core::kZero;
    if (t.raw > core::kOne.raw) t = core::kOne;
    return t;
}

} // namespace

// How wide the water is, by how much of this map's own drainage comes down it.
//
// Measured against the map, not against a table of absolute doublings. That was
// the bug this replaces, and it is worth writing down because the old code was
// not obviously wrong: width went as the square root of the flow, tabulated for
// flows up to fifteen doublings, which is correct hydraulic geometry and is
// what a river does.
//
// The trouble is that no world here has fifteen doublings in it. Flow is the
// catchment above a cell, so its range is set by the size of the map, and on a
// 64-cell world the largest catchment is seven doublings while the wet
// threshold is four. So the table was only ever read at slots four to seven,
// and the floor under it collapsed the bottom two into one value. Measured on
// seed 42: 88 of 130 wet channels came out at exactly 12.0 m, the median was
// 12.0, and the single widest river on the whole map was 27.1. Four widths, and
// in practice one - which is why every river looked the same.
//
// So the flow is normalised against this map's own range first, and the width
// is laid out geometrically across that. A doubling still widens the channel by
// a constant factor, which is the shape of the real law; what the map decides
// is how much of the brook-to-trunk range those doublings have to cover. The
// biggest river on any world is a big river on it, which is the property that
// actually matters and the one an absolute table cannot have.
Fixed MacroWorld::halfWidthFor(std::uint8_t flowLog) const {
    const std::int64_t bottom = kNarrowestWetFlow;
    const std::int64_t top = std::max<std::int64_t>(largestFlow_, bottom + 1);
    const std::int64_t here = std::clamp<std::int64_t>(flowLog, bottom, top);
    // Geometric between the two ends, in eighths - a fractional power in fixed
    // point is not worth the arithmetic, and eight steps is finer than the
    // lattice can draw the difference anyway.
    const std::int64_t steps = 8;
    const std::int64_t up = ((here - bottom) * steps) / (top - bottom);
    // kBrookHalfWidth * (kTrunkHalfWidth/kBrookHalfWidth)^(up/8), tabulated:
    // 4, 5.9, 8.7, 12.8, 18.7, 27.5, 40.3, 59.1, 70 - a ratio of 17.5 over the
    // map's whole range of flows.
    static const Fixed kStep[9] = {
            Fixed::ratio(40, 10),  Fixed::ratio(59, 10),  Fixed::ratio(87, 10),
            Fixed::ratio(128, 10), Fixed::ratio(187, 10), Fixed::ratio(275, 10),
            Fixed::ratio(403, 10), Fixed::ratio(591, 10), Fixed::ratio(700, 10)};
    return std::clamp(kStep[std::clamp<std::int64_t>(up, 0, steps)],
                      Fixed::fromInt(kBrookHalfWidth), Fixed::fromInt(kTrunkHalfWidth));
}

// How much of a valley a cell with this much water through it cuts at all.
//
// Every land cell drains somewhere, so every land cell has a channel - and with
// a floor of a metre and a half of gully and fifty metres of valley, every one
// of them cut a visible ravine. Measured on this world: fifty-four per cent of
// the land drains two doublings or less, which is a trickle after rain, and the
// country came out as a waffle of gullies five hundred and forty metres apart
// with no ridges between them.
//
// A trickle cuts nothing. What cuts a valley is a stream, and the ground between
// the streams is meant to be ground.

// The valley: how far out from the water the ground is drawn down to it.
//
// Sized against the map's own range for the same reason the width is, and for
// a second reason that matters more. This is not decoration - it is the only
// thing deciding how steep the sides of every watercourse in the world are,
// because the ground has to fall from whatever the country stands at to the
// water within it. A short reach is not a small valley; it is a wall.
//
// Two hundred metres for a brook, up to a kilometre and a half for the trunk.
Fixed MacroWorld::reachFor(std::uint8_t flowLog) const {
    const std::int64_t bottom = kNarrowestWetFlow;
    const std::int64_t top = std::max<std::int64_t>(largestFlow_, bottom + 1);
    const std::int64_t here = std::clamp<std::int64_t>(flowLog, 0, top);
    const std::int64_t over = std::max<std::int64_t>(here - bottom, 0);
    const Fixed grown = Fixed::fromInt(200) +
                        Fixed::fromInt(1300) * Fixed::ratio(over, top - bottom);
    // Below the wet threshold there is still a valley, and it still has to be a
    // valley rather than a slot: a dry gully carries the same argument, scaled
    // down by how little ever ran in it.
    const Fixed dry = Fixed::fromInt(60) + Fixed::fromInt(35) * Fixed::fromInt(here);
    return (flowLog >= bottom ? grown : dry) * carveStrength(flowLog);
}

WorldPos MacroWorld::pointOn(const Channel& channel, Fixed along) {
    // Queries, carving and water rendering must follow the SAME polyline.
    // Evaluating the cubic here but projecting onto eight chords in carve()
    // put a nominal centreline outside its own narrow river.
    along = std::clamp(along, core::kZero, core::kOne);
    const Fixed u = along * Fixed::fromInt(kCurveSteps);
    const int i = std::min(static_cast<int>(u.toInt()), kCurveSteps - 1);
    const auto a = onCurve(channel, Fixed::ratio(i, kCurveSteps));
    const auto b = onCurve(channel, Fixed::ratio(i + 1, kCurveSteps));
    const Fixed t = u - Fixed::fromInt(i);
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

namespace {
// A headwater has nothing at its own end and everything by the next: the
// spring, drawn as the channel growing out of the hillside rather than as a
// hole appearing in it. Eased, so there is no corner where it starts.
Fixed grown(const Channel& c, Fixed along) {
    if (!c.head) return core::kOne;
    if (along.raw <= 0) return core::kZero;
    if (along.raw >= core::kOne.raw) return core::kOne;
    return along * along * (Fixed::fromInt(3) - Fixed::fromInt(2) * along);
}
} // namespace

Fixed MacroWorld::halfWidthAt(const Channel& c, Fixed along) {
    return (c.halfWidth + (c.halfWidthEnd - c.halfWidth) * along) * grown(c, along);
}
// Pools and riffles: a channel is not one depth along its length.
//
// A stream shallows and deepens every few widths of itself, and the shallow
// places are where things cross. That matters for more than the look of it: at
// fordableDepth a cart gets over and out of its depth it does not, so a channel
// of one depth is a fence, and a network of them cuts the country up - measured
// on the wettest square mile of seed 11, a cart could reach a third of the
// walkable ground and no more.
//
// The water level is untouched; only the bed comes up. So a riffle is a bar of
// gravel with the same water running shallow over it, which is what a ford is,
// and it needs no representation of its own.
static Fixed riffle(const Channel& c, Fixed along) {
    // Four to a reach, and at a phase of the channel's own so that they do not
    // all land on the coarse map's five-hundred-and-forty-metre lattice.
    const std::uint64_t h = core::splitmix64(c.wander ^ 0x9e3779b97f4a7c15ull);
    const Fixed phase = Fixed::ratio(static_cast<std::int64_t>(h % 1000), 1000);
    const Fixed u = along * Fixed::fromInt(2) + phase;
    const Fixed part = u - Fixed::fromInt(u.toInt());
    // 4t(1-t): an arch, deep in the middle of a pool and shallow at either end
    // of it. sin is not available here and is not wanted - this is exact.
    const Fixed arch = part * (core::kOne - part) * Fixed::fromInt(4);
    // Long pools and short bars, rather than an even wave between the two.
    //
    // The arch on its own spends as much of the channel shallow as deep, and at
    // four to a reach that is a bar every hundred and thirty metres. The water
    // colour goes by depth, so what it drew was not a river but a string of
    // beads - a row of oval pools each with a pale rim, which is what the reach
    // looks like from the RTS camera. Two to a reach, and 1-(1-a)^2 over the
    // arch, keeps the same seventh of depth at the bar and the same full depth
    // in the pool while spending far less of the length crossing between them.
    // The ford is still there and still shallow; it is a bar now instead of
    // half the river.
    const Fixed sustained = arch * (Fixed::fromInt(2) - arch);
    // A seventh of the depth over the bar, all of it in the pool. A seventh
    // because the deepest channel this world draws is a little over three metres
    // and a ford has to come out under fordableDepth's sixty centimetres.
    return Fixed::ratio(1, 7) + sustained * Fixed::ratio(6, 7);
}

Fixed MacroWorld::depthAt(const Channel& c, Fixed along) {
    // Pool/riffle residual vanishes at BOTH nodes. Independent phases used to
    // give the outgoing and incoming reach different beds at the same point.
    const Fixed arch = along * (core::kOne - along) * Fixed::fromInt(4);
    const Fixed bedform = core::kOne - (core::kOne - riffle(c, along)) * arch * arch;
    return (c.depth + (c.depthEnd - c.depth) * along) * grown(c, along) * bedform;
}
Fixed MacroWorld::reachAt(const Channel& c, Fixed along) {
    return (c.valleyReach + (c.valleyReachEnd - c.valleyReach) * along) * grown(c, along);
}

void MacroWorld::attach(const generation::WorldMapData* coarse) {
    coarse_ = coarse;
    block_.valid = false;   // its channels were built against the old threshold
    streamFlow_ = 255;
    largestFlow_ = 0;
    surface_.clear();
    wet_.clear();
    if (coarse == nullptr) return;

    // Which channels hold water, decided by looking at the whole map once.
    //
    // Only the carved ones are counted. Every land cell sheds somewhere, so
    // every land cell has a channel, but carveStrength cuts nothing below three
    // doublings - over half the land - and counting those in the share would
    // drag the answer down to a threshold that makes streams of the trickles.
    // The network the question is about is the network that shows.
    std::int64_t carved = 0;
    std::int64_t above[16] = {};
    for (const generation::WorldCell& cell : coarse->cells) {
        if (cell.sea) continue;
        const std::uint8_t size = std::min<std::uint8_t>(cell.drainSize, 15);
        if (carveStrength(size).raw <= 0) continue;
        ++carved;
        ++above[size];
        // The trunk of the whole drainage. Every other channel is sized as a
        // share of the distance between the wet threshold and this, so a map
        // without a big river on it still has small ones that read as small.
        if (size > largestFlow_) largestFlow_ = size;
    }
    // Down from the top, until one valley in five is wet.
    std::int64_t running = 0;
    std::uint8_t chosen = 15;
    for (int size = 15; size >= 0; --size) {
        running += above[size];
        chosen = static_cast<std::uint8_t>(size);
        if (running * kWetShareDenominator >= carved * kWetShareNumerator) break;
    }
    streamFlow_ = std::max(chosen, kNarrowestWetFlow);

    const auto count = coarse->cells.size();
    surface_.resize(count);
    wet_.resize(count);
    std::vector<std::int32_t> next(count, -1), order;
    std::vector<std::uint8_t> state(count, 0);
    order.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& c = coarse->cells[i];
        const bool lake = i < coarse->lakeDepthField.size() && coarse->lakeDepthField[i] > 0 &&
                          i < coarse->lakeLevelField.size();
        const int head = c.sea ? 0 : lake ? coarse->lakeLevelField[i] : c.elevation;
        surface_[i] = Fixed::fromInt(std::max(0, head) * generation::kMetresPerElevationStep);
        wet_[i] = !c.sea && (c.river || c.drainSize >= streamFlowIn(c));
        const int dir = c.river && c.riverOut >= 0 ? c.riverOut : c.drainOut;
        if (c.sea || dir < 0 || dir >= core::kNeighbourCount) continue;
        const auto down = core::neighbour({static_cast<int>(i % coarse->width),
                                           static_cast<int>(i / coarse->width)}, dir);
        if (coarse->inBounds(down)) next[i] = down.y * coarse->width + down.x;
    }
    // Iterative topological resolution: no recursive stack on continental rivers.
    std::vector<std::int32_t> path;
    for (std::size_t i = 0; i < count; ++i) {
        if (state[i] == 2) continue;
        path.clear();
        auto at = static_cast<std::int32_t>(i);
        while (at >= 0 && state[at] == 0) {
            state[at] = 1;
            path.push_back(at);
            at = next[at];
        }
        if (at >= 0 && state[at] == 1)
            throw std::invalid_argument("cyclic terrain drainage");
        for (auto it = path.rbegin(); it != path.rend(); ++it) {
            const auto n = *it;
            if (next[n] >= 0) surface_[n] = std::max(surface_[n], surface_[next[n]]);
            state[n] = 2;
            order.push_back(n);
        }
    }
    // Rainfall thresholds can start a stream, but cannot delete upstream water
    // on the next dry cell. Losses need an explicit water balance, not a flag.
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (next[*it] >= 0 && wet_[*it]) wet_[next[*it]] = 1;
}

bool MacroWorld::wetAt(core::TilePos cell) const {
    if (coarse_ == nullptr || !coarse_->inBounds(cell)) return false;
    const auto index = static_cast<std::size_t>(cell.y) * coarse_->width + cell.x;
    return index < wet_.size() && wet_[index] != 0;
}

core::Fixed MacroWorld::surfaceAt(core::TilePos cell) const {
    if (coarse_ == nullptr || !coarse_->inBounds(cell)) return core::kZero;
    const auto index = static_cast<std::size_t>(cell.y) * coarse_->width + cell.x;
    return index < surface_.size() ? surface_[index] : core::kZero;
}

std::uint8_t MacroWorld::streamFlowIn(const generation::WorldCell& cell) const {
    // Wet country turns its gullies into streams and dry country leaves them
    // dry, and the share rule above cannot see that: it is one number for the
    // whole map, so it drew the same fifth of the network everywhere and left
    // four fifths of every valley in the wettest country on the map as a dry
    // ditch. Which is what it looked like.
    //
    // Two doublings either way. Full-dry needs four times the catchment that the
    // map's average does before its channel holds water the year round; full-wet
    // needs a quarter of it, which on this world reaches the bottom of the
    // network and makes a stream of every gully - and that is right, because a
    // gully in a rainforest is a stream.
    const std::int32_t shift = 2 - (static_cast<std::int32_t>(cell.moisture) * 4) / 255;
    const std::int32_t want = static_cast<std::int32_t>(streamFlow_) + shift;
    return static_cast<std::uint8_t>(
            std::clamp<std::int32_t>(want, kNarrowestWetFlow, 15));
}

std::optional<Channel> MacroWorld::channelOf(core::TilePos at) const {
    if (coarse_ == nullptr || !coarse_->inBounds(at)) return std::nullopt;
    const generation::WorldCell& cell = coarse_->at(at);
    if (cell.sea) return std::nullopt;
    // Every line of drainage, not only the ones with a river in them: the
    // gullies are what put a col between two peaks, and without them a range is
    // a wall (D116). Which of them carry water is a separate question.
    const std::int8_t out = cell.river && cell.riverOut >= 0 ? cell.riverOut : cell.drainOut;
    if (out < 0 || out >= core::kNeighbourCount) return std::nullopt;
    const core::TilePos down = core::neighbour(at, out);
    if (!coarse_->inBounds(down)) return std::nullopt;

    const Fixed perCell = Fixed::fromInt(generation::kMetresPerCell);
    const Fixed halfCell = perCell / Fixed::fromInt(2);
    const auto centreOf = [&](core::TilePos c) {
        return WorldPos{Fixed::fromInt(c.x) * perCell + halfCell,
                        Fixed::fromInt(c.y) * perCell + halfCell};
    };
    const auto surfaceOf = [&](core::TilePos p) {
        const auto& c = coarse_->at(p);
        if (c.sea) return core::kZero;
        const auto index = static_cast<std::size_t>(p.y) * coarse_->width + p.x;
        if (index < surface_.size()) return surface_[index];
        // The lake retains its pre-erosion head. Starting its outlet at the
        // eroded cell elevation puts the outgoing river underneath the lake,
        // introducing an artificial step before the actual spill slope.
        if (index < coarse_->lakeDepthField.size() && index < coarse_->lakeLevelField.size() &&
            coarse_->lakeDepthField[index] > 0 && coarse_->lakeLevelField[index] > 0)
            return Fixed::fromInt(static_cast<std::int64_t>(coarse_->lakeLevelField[index]) *
                                  generation::kMetresPerElevationStep);
        return Fixed::fromInt(static_cast<std::int64_t>(c.elevation) *
                              generation::kMetresPerElevationStep);
    };

    // How far back the valley has to start for the ground to reach the water at
    // a grade a hillside can hold. The country around a reach is read off the
    // cells that touch it - one lookup each, once per channel, and the channel
    // is cached for the whole neighbourhood - and the highest of them is the
    // fall this valley is responsible for.
    const auto roomFor = [&](core::TilePos cell, Fixed surface) {
        // Over the same square the drainage itself is gathered from. One ring
        // is not enough: a reach with a mountain two cells away has that
        // mountain's ground to bring down as surely as if it touched, and read
        // at one ring the valley came out sized for the low shoulder beside it
        // and walled against the high ground behind.
        Fixed above = core::kZero;
        for (std::int32_t oy = -kCellsAround; oy <= kCellsAround; ++oy)
            for (std::int32_t ox = -kCellsAround; ox <= kCellsAround; ++ox) {
                const core::TilePos n{cell.x + ox, cell.y + oy};
                if (!coarse_->inBounds(n)) continue;
                const generation::WorldCell& other = coarse_->at(n);
                if (other.sea) continue;
                const Fixed top = Fixed::fromInt(static_cast<std::int64_t>(other.elevation) *
                                                 generation::kMetresPerElevationStep);
                // Weighted by how far off it is: a wall two cells away is two
                // cells of ground away from the water as well, and the valley
                // only owes the part of the fall that happens inside it.
                const std::int32_t rings = std::max(std::abs(ox), std::abs(oy));
                const Fixed mine = (top - surface) /
                                   Fixed::fromInt(1 + static_cast<std::int64_t>(rings));
                if (mine.raw > above.raw) above = mine;
            }
        if (above.raw <= 0) return core::kZero;
        const Fixed needed = above * Fixed::fromInt(10) / Fixed::fromInt(3);
        // Bounded by how far the drainage is gathered around a point: a valley
        // whose channel is not in the neighbourhood cannot be cut by it, and a
        // valley that is cut in some places and not others is worse than a
        // steep one. kCellsAround cells reach this far and no further.
        return std::min(needed, Fixed::fromInt(kWidestValley));
    };

    // Nothing to cut, so nothing to look at. This is fifty-four per cent of the
    // land, and skipping it is also fifty-four per cent of what carve used to
    // walk through on every sample of the world.
    if (carveStrength(cell.drainSize).raw <= 0 && !cell.river) return std::nullopt;

    Channel channel;
    channel.from = centreOf(at);
    channel.to = centreOf(down);
    channel.surfaceFrom = surfaceOf(at);
    channel.surfaceTo = surfaceOf(down);
    // Water does not run uphill. Where the coarse map says it does - which
    // happens where a cell was filled to break a hollow - the level is held
    // rather than climbed, so the reach becomes still water instead of a slope
    // the wrong way.
    // attach() resolved both endpoints together, not a per-reach clamp which
    // disagrees with the next reach at a confluence.
    channel.size = cell.drainSize;
    // Water stands in it if enough drains through, not only if the coarse map
    // drew a blue line there. The line is drawn above a threshold that scales
    // with the size of the map, so on a continent it marks only the trunks -
    // every river in the world came out between ninety and two hundred and
    // fifty metres wide, and there were no streams at all.
    channel.wet = wet_[static_cast<std::size_t>(at.y) * coarse_->width + at.x] != 0;
    const auto widthOf = [&](core::TilePos p) {
        const auto& c = coarse_->at(p);
        Fixed width = halfWidthFor(c.drainSize);
        const auto i = static_cast<std::size_t>(p.y) * coarse_->width + p.x;
        // Recover sub-octave discharge where available instead of quantising
        // every river width to the integer drainSize.
        if (i < coarse_->riverDischargeField.size() && c.drainSize < 30) {
            const auto q = std::max(1, coarse_->riverDischargeField[i]);
            width *= core::sqrt(Fixed::ratio(q, std::int64_t(1) << c.drainSize));
        }
        const int dir = c.river && c.riverOut >= 0 ? c.riverOut : c.drainOut;
        if (dir >= 0 && dir < core::kNeighbourCount) {
            const auto down = core::neighbour(p, dir);
            if (coarse_->inBounds(down)) {
                const Fixed slope = std::max(core::kZero, surfaceOf(p) - surfaceOf(down)) / perCell;
                width *= std::max(Fixed::ratio(45, 100), core::kOne / (core::kOne + slope * Fixed::fromInt(4)));
            }
        }
        // Slope may contract a trunk, but cannot shrink a brook below the
        // canonical 4 m lattice's supported width and turn riffles into dams.
        return std::max(Fixed::fromInt(kBrookHalfWidth), width);
    };
    // Everything is sized by what drains through, which is the same number for
    // the valley and for the water in it. A dry gully has the valley and no
    // channel; a river has both.
    channel.halfWidth = channel.wet ? widthOf(at) : core::kZero;
    channel.depth = depthFor(cell.drainSize);
    channel.valleyReach = reachFor(cell.drainSize);
    // A bed no deeper than the water is wide can carry.
    //
    // Depth came off the drainage alone, and width off the drainage alone, and
    // the two only agreed at the sizes anybody had looked at. A trunk river two
    // hundred and fifty metres across and ten deep is a river; the same rule at
    // the other end gives one thirty across and seven deep, which is a slot with
    // water in it, and its banks are walls. It never showed while the only
    // rivers on the map were trunks; on a world fifty kilometres across, where
    // every river is a small one, two shores in five ended over three metres of
    // water.
    //
    // A real channel is far wider than it is deep - a quarter is already steep -
    // so the width is the bound. Only where there is water: a dry gully's depth
    // is about the ground it has cut, not about a flow that is not there.
    if (channel.wet)
        channel.depth = std::min(channel.depth, channel.halfWidth / Fixed::fromInt(4));

    // And what it becomes by the far end. A reach that holds its own cell's
    // numbers to the boundary and then jumps to the next cell's puts a crease
    // across the valley every five hundred and forty metres - the coarse map's
    // lattice, drawn in the shape of the ground.
    const generation::WorldCell& into = coarse_->at(down);
    // The sea is not where the water gives out. It is where it becomes more
    // water, and a river arriving at it is at its widest.
    //
    // A sea cell carries no drainage - the accumulation loop skips it, so its
    // drainSize is nought and its river flag is false - and this read those
    // zeros as "dry below" and tapered the channel into nothing over the last
    // reach. Measured on seed 11: every one of the six river mouths on the map
    // had a dry gap, and the last tenth of a mouth reach was 29 of its 54
    // metres dry. Every river on the world stopped short of the coast and left
    // its bed showing.
    //
    // So a reach running into the sea keeps its own numbers to the end.
    const bool intoSea = into.sea;
    const bool wetBelow = intoSea || wet_[static_cast<std::size_t>(down.y) * coarse_->width + down.x];
    const std::uint8_t below = intoSea ? cell.drainSize : into.drainSize;
    channel.halfWidthEnd = wetBelow ? (intoSea ? channel.halfWidth : widthOf(down)) : core::kZero;
    channel.depthEnd = depthFor(below);
    if (wetBelow)
        channel.depthEnd = std::min(channel.depthEnd, channel.halfWidthEnd / Fixed::fromInt(4));
    channel.valleyReachEnd = reachFor(below);
    // Where the water gives out downstream, the channel narrows into it - but
    // not to nothing while it still carries water itself.
    //
    // Whether a channel is wet depends on the country's moisture now, and
    // moisture varies from cell to cell, so a wet reach running into a drier one
    // is common rather than rare. Tapered to nothing, the water left the course
    // well before the boundary and the last stretch of a perfectly good stream
    // was dry bed: measured over 564 streams on seed 11, dry runs of up to 136
    // metres, and they were not the narrowest channels - eight-metre ones had
    // them too. Narrowed to a brook's width instead, the water reaches the
    // boundary and the dry reach below takes over there, which is what a stream
    // sinking into dry country does.
    if (!wetBelow)
        channel.halfWidthEnd = channel.wet ? halfWidthFor(kNarrowestWetFlow) : core::kZero;

    if (channel.wet) {
        // A river arriving at the sea is at its widest, and then some: the last
        // reach is not river any more but tidal water, and it opens out into
        // the valley it drowned rather than holding the width its own catchment
        // earns. Two and a half times, which on this map turns a mouth from a
        // ribbon indistinguishable from the reach above it into something that
        // reads as joined to the sea it runs into.
        if (intoSea) {
            channel.halfWidthEnd *= Fixed::ratio(5,2);
            // Apply the aspect bound AFTER the estuary widens. Keeping the
            // brook's shallow depth here left an interpolation dam where steep
            // incoming reaches meet the flat ocean on the 4 m lattice.
            channel.depthEnd = std::min(depthFor(below), channel.halfWidthEnd / Fixed::fromInt(4));
        }
    } else {
        channel.depth*=Fixed::ratio(1,3);
        channel.depthEnd*=Fixed::ratio(1,3);
        channel.valleyReach*=Fixed::ratio(1,2);
        channel.valleyReachEnd*=Fixed::ratio(1,2);
    }

    // And then the constraint that decides what a riverbank looks like: however
    // far the water's own size says its valley reaches, the ground still has to
    // get down to it from whatever the country around stands at.
    //
    // This was missing, and it is the whole of why the watercourses read as
    // slots cut into the country rather than as valleys. The reach came off the
    // flow alone and was then clamped against the channel's own width, which on
    // this map meant 64 to 76 metres for every river in the world - so a reach
    // whose neighbouring cells stand two hundred metres higher had to bring the
    // ground down two hundred metres inside seventy. Measured across the 151
    // river cross-sections on seed 11: median bank 0.71 m/m, nine in ten up to
    // 1.46, worst 4.68 - and one cross-section at cell 51,4 fell 334 m to the
    // water and rose 343 m again over a hundred and eighty metres of ground,
    // with eight metres of river at the bottom of it. That is a gorge, and
    // nothing in the world asked for a gorge; it is only what is left when a
    // valley is told to be narrower than its own relief.
    //
    // So the valley is at least as wide as its fall needs. Three in ten is a
    // hillside a person walks up; past that the ground is a wall, and a wall is
    // what the old numbers were making.
    //
    // Bounded at both ends by the same gathering radius: a valley wider than the
    // drainage is read around a point is cut where its channel happens to be in
    // the neighbourhood and left uncut a sample later where it is not, and that
    // seam is a worse artefact than the steep bank it was widening away from.
    const auto valley = [](Fixed wanted, Fixed needed) {
        return std::min(std::max(wanted, needed), Fixed::fromInt(kWidestValley));
    };
    channel.valleyReach = valley(channel.valleyReach, roomFor(at, channel.surfaceFrom));
    channel.valleyReachEnd = valley(channel.valleyReachEnd, roomFor(down, channel.surfaceTo));

    // Where the water came from and where it goes on to, so that this reach is
    // a piece of a curve rather than a segment with a corner at each end. The
    // one upstream is whichever neighbour sends the most water this way; if
    // none does, the reach begins here and its own direction will do.
    core::TilePos from = at;
    std::uint8_t most = 0;
    for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
        const core::TilePos n = core::neighbour(at, dir);
        if (!coarse_->inBounds(n)) continue;
        const generation::WorldCell& other = coarse_->at(n);
        if (other.sea) continue;
        const std::int8_t theirs =
                other.river && other.riverOut >= 0 ? other.riverOut : other.drainOut;
        if (theirs < 0 || !(core::neighbour(n, theirs) == at)) continue;
        if (other.drainSize >= most) {
            most = other.drainSize;
            from = n;
        }
    }
    channel.head = from == at;
    channel.before = channel.head ? WorldPos{channel.from.x - (channel.to.x - channel.from.x),
                                             channel.from.y - (channel.to.y - channel.from.y)}
                                  : centreOf(from);

    const generation::WorldCell& downstream = coarse_->at(down);
    const std::int8_t onward =
            downstream.river && downstream.riverOut >= 0 ? downstream.riverOut : downstream.drainOut;
    const core::TilePos beyond = onward >= 0 ? core::neighbour(down, onward) : down;
    channel.after = onward >= 0 && coarse_->inBounds(beyond) && !downstream.sea
                            ? centreOf(beyond)
                            : WorldPos{channel.to.x + (channel.to.x - channel.from.x),
                                       channel.to.y + (channel.to.y - channel.from.y)};
    // What this reach does between its ends, keyed on the cell it leaves: the
    // same wander whoever asks and from wherever.
    channel.wander = core::splitmix64(0x9e3779b97f4a7c15ULL ^
                                      (static_cast<std::uint64_t>(std::uint32_t(at.x)) << 20) ^
                                      static_cast<std::uint64_t>(std::uint32_t(at.y)));
    //
    // The SAME rule the hydrology graph uses, and it has to be, because these
    // are two descriptions of one river. This one divided the wander by the
    // drained area - so a trunk barely left the straight line between two cell
    // centres - while the graph was changed to the opposite, a meander belt of
    // about fifteen channel widths. Two curves, one river: the water was drawn
    // where the graph put it and asked for where this said it was, and the
    // answer came back dry. Measured by the linter: a hundred and eighty metres
    // of gap in courses that are continuous.
    //
    // A meander belt is about fifteen to twenty channel widths across, so a
    // trunk two hundred metres wide wanders over three kilometres and a brook
    // over a hundred metres. Held under half a cell, because past that the
    // course would leave the cells its own flow was accumulated through.
    channel.wanderReach = core::clamp(channel.halfWidth * Fixed::fromInt(8),
                                      perCell * Fixed::ratio(12, 100),
                                      perCell * Fixed::ratio(45, 100));
    // A steep reach has the fall to hold its line; a flat one does not.
    const Fixed gradient =
            core::max(core::kZero, channel.surfaceFrom - channel.surfaceTo) / perCell;
    channel.wanderReach = channel.wanderReach / (core::kOne + gradient * Fixed::fromInt(12));
    return channel;
}

namespace {
// Eased between nought and one: flat at both ends, so a valley has a floor and
// shoulders rather than a V, and so its edge does not crease where it meets the
// country it was cut into.
Fixed ease(Fixed t) {
    if (t.raw <= 0) return core::kZero;
    if (t.raw >= core::kOne.raw) return core::kOne;
    return t * t * (Fixed::fromInt(3) - Fixed::fromInt(2) * t);
}
} // namespace

const MacroWorld::Neighbourhood& MacroWorld::around(std::int64_t cellX, std::int64_t cellY,
                                                    std::int64_t narrowest) const {
    if (block_.valid && block_.cellX == cellX && block_.cellY == cellY &&
        block_.narrowest == narrowest)
        return block_;
    block_.valid = true;
    block_.cellX = cellX;
    block_.cellY = cellY;
    block_.narrowest = narrowest;
    for (std::int32_t oy = -kCellsAround; oy <= kCellsAround; ++oy)
        for (std::int32_t ox = -kCellsAround; ox <= kCellsAround; ++ox) {
            Course& course = block_.courses[(oy + kCellsAround) * kAround + (ox + kCellsAround)];
            const std::optional<Channel> maybe = channelOf(
                    {static_cast<std::int32_t>(cellX + ox), static_cast<std::int32_t>(cellY + oy)});
            course.exists = maybe.has_value();
            if (!course.exists) continue;
            course.channel = *maybe;
            const Channel& one = course.channel;
            course.span = (one.valleyReach.raw > one.valleyReachEnd.raw ? one.valleyReach
                                                                        : one.valleyReachEnd) *
                          Fixed::fromInt(2);
            // Narrower than the samples that will read it: thrown out here
            // rather than in the loop that asks, because what a channel costs is
            // the nine points of its course, and this is before they are walked.
            if (narrowest > 0 && course.span.raw < Fixed::fromInt(narrowest).raw) {
                course.exists = false;
                continue;
            }
            for (int step = 0; step <= kCurveSteps; ++step)
                course.points[step] = onCurve(one, Fixed::ratio(step, kCurveSteps));
            course.lowX = course.highX = course.points[0].x;
            course.lowY = course.highY = course.points[0].y;
            for (int step = 1; step <= kCurveSteps; ++step) {
                const core::WorldPos& on = course.points[step];
                if (on.x.raw < course.lowX.raw) course.lowX = on.x;
                if (on.x.raw > course.highX.raw) course.highX = on.x;
                if (on.y.raw < course.lowY.raw) course.lowY = on.y;
                if (on.y.raw > course.highY.raw) course.highY = on.y;
            }
            // No cell of slack: the box already holds every point of the curve,
            // wander included, so the only thing left to allow for is how far
            // the valley reaches out from it.
            course.pad = (one.valleyReach.raw > one.valleyReachEnd.raw ? one.valleyReach
                                                                       : one.valleyReachEnd);
            course.midX = (course.lowX + course.highX) / Fixed::fromInt(2);
            course.midY = (course.lowY + course.highY) / Fixed::fromInt(2);
        }

    // Never modify a channel using the query's neighbourhood. The same reach
    // appears in several neighbourhoods with different competitors. Limiting
    // its valley against those competitors made its shape jump at cell edges.
    return block_;
}

WaterNearby MacroWorld::carve(WorldPos p, Fixed country, Fixed detail,
                              std::int64_t narrowest) const {
    WaterNearby out;
    out.floor = country + detail;
    if (coarse_ == nullptr) return out;
    // Past the size of a map cell there is no valley left to cut, and gathering
    // the twenty-five channels around the point to find that out is most of what
    // the drainage costs at that spacing. So the question is answered before it
    // is asked.
    if (narrowest > 0 && narrowest >= generation::kMetresPerCell / 2) return out;

    // How much of the detail layer survives at this point: nought in the
    // channel, all of it once the flood plain is behind us. Taken as the least
    // over every channel that reaches here, so a confluence is flat rather than
    // half flat. And the lowest floor any channel cuts here, kept apart from it:
    // the ground is the country with the detail faded, and then that, cut.
    // The ground here cannot be settled until every channel that reaches this
    // point has been looked at, because how much of the detail layer survives
    // is the least any of them allows and every one of them is then cut into
    // that same ground. Taking each channel's own view of the ground as it went
    // undid the flattening: a gully two hundred metres away, measuring the
    // ground as it sees it, put its floor back at the undamped height and cut
    // to there, which is a twenty-metre hole under a river. So what the loop
    // does is measure; the cutting is done afterwards, once.
    struct Near {
        Fixed surface, halfWidth, depth, distance, away, mine;
        bool wet;
    };
    // Room for every channel the search can reach, so none is ever dropped:
    // a dropped one is a valley that is cut in some places and not in others.
    Near near[(2 * kCellsAround + 1) * (2 * kCellsAround + 1)];
    std::size_t nearCount = 0;
    Fixed keep = core::kOne;

    const std::int64_t perCell = generation::kMetresPerCell;
    const std::int64_t cellX = floorDiv(p.x.toInt(), perCell);
    const std::int64_t cellY = floorDiv(p.y.toInt(), perCell);
    Fixed nearest = Fixed::fromInt(1 << 20);
    std::size_t nearestOne = 0;
    Fixed nearestWater = Fixed::fromInt(1 << 20);
    std::size_t nearestWaterOne = 0;
    Fixed bed = core::kZero, bedWeight = core::kZero;

    const Neighbourhood& neighbourhood = around(cellX, cellY, narrowest);
    for (const Course& course : neighbourhood.courses) {
            if (!course.exists) continue;
            const Channel& c = course.channel;

            // Thrown out before any of the expensive arithmetic if it cannot
            // possibly reach: a square root costs fifty multiplies, a divide
            // costs five, and twenty-five channels are looked at for every
            // sample of the world. Squared distances answer "is it near" without
            // either. The reach and the middle came off the cache with the
            // channel, so a rejection is two comparisons and nothing else.
            if (p.x.raw < course.lowX.raw - course.pad.raw) continue;
            if (p.x.raw > course.highX.raw + course.pad.raw) continue;
            if (p.y.raw < course.lowY.raw - course.pad.raw) continue;
            if (p.y.raw > course.highY.raw + course.pad.raw) continue;

            // Where the water actually runs. A curve through this reach and the
            // two it joins - the same curve the coarse map's own heights are
            // interpolated with - plus a small wander across it. The nearest
            // point on it is found by walking a few steps rather than solved
            // for: a cubic's closest point is a quintic, and eight steps over
            // five hundred metres is closer than the ground is drawn.
            //
            // The nine points were walked when the cell changed. Walking them
            // per sample - two hundred and twenty-five splines a sample - was
            // most of what the terrain cost.
            constexpr int kSteps = kCurveSteps;
            const WorldPos* points = course.points;
            Fixed spread = Fixed::fromInt(1'000'000'000), t;
            Fixed distances[kSteps], fractions[kSteps];
            for (int step = 0; step < kSteps; ++step) {
                const WorldPos a = points[step], b = points[step + 1];
                const Fixed dx = b.x - a.x, dy = b.y - a.y;
                const Fixed lengthSquared = dx * dx + dy * dy;
                const Fixed along = lengthSquared > core::kZero ? core::saturate(
                    ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared) : core::kZero;
                const WorldPos foot{a.x + dx * along, a.y + dy * along};
                const Fixed offX = p.x - foot.x, offY = p.y - foot.y;
                const Fixed here = offX * offX + offY * offY;
                distances[step] = here;
                fractions[step] = (Fixed::fromInt(step) + along) / kSteps;
                if (here.raw < spread.raw) {
                    spread = here;
                    t = fractions[step];
                }
            }
            // Against the widest the valley gets anywhere along this reach:
            // the exact reach at this point is not known until the fraction is.
            const Fixed widest = c.valleyReach.raw > c.valleyReachEnd.raw ? c.valleyReach
                                                                          : c.valleyReachEnd;
            if (spread.raw > (widest * widest).raw) continue;
            const Fixed distance = core::sqrt(spread);
            // Distance to a polyline is continuous; its nearest parameter is
            // not. Across the medial axis of a bend, nearest-t can jump by
            // hundreds of metres and put a cliff through the valley. Shepard
            // weights blend segment projections away from the course. On the
            // course itself the exact segment wins (including shared nodes).
            if (spread > core::kZero) {
                Fixed weighted, weights;
                for (int step = 0; step < kSteps; ++step) {
                    const Fixed relative = spread / distances[step];
                    const Fixed weight = relative * relative;
                    weighted += fractions[step] * weight;
                    weights += weight;
                }
                t = weighted / weights;
            }

            const Fixed surface = c.surfaceFrom + (c.surfaceTo - c.surfaceFrom) * t;
            // Width, depth and reach as they are here, part way along, rather
            // than as the cell this reach starts in happens to have them.
            const Fixed halfWidth = halfWidthAt(c, t);
            const Fixed depth = depthAt(c, t);
            const Fixed valleyReach = reachAt(c, t);
            // The flood plain: no detail at all out to the bank, and all of it
            // by half as far again beyond, or forty metres, whichever is more.
            const Fixed plain = halfWidth * Fixed::ratio(3, 5) + Fixed::fromInt(40);
            // How much of the detail layer survives here. Never none.
            //
            // It used to go to nothing in the flood plain, and what is left when
            // the detail is gone is the coarse map's own height - which is one
            // number per five-hundred-and-forty-metre cell, so where two
            // neighbouring cells hold the same number the ground is dead flat
            // over hundreds of metres. Measured at a river mouth: six hundred
            // and forty metres at exactly 0.00 m, slope 0.000. Flat and at sea
            // level is drawn as bare pale sand with no shading in it at all, and
            // that is what the pale blotches with straight edges were.
            //
            // A quarter of it left is still a flood plain - the detail layer is
            // metres, not tens of them, at this scale - and it is enough that no
            // two square metres of the world are identical.
            const Fixed keepLeast = c.wet ? Fixed::ratio(1,4) : Fixed::ratio(3,4);
            const Fixed grown = distance.raw <= halfWidth.raw
                                        ? core::kZero
                                        : ease((distance - halfWidth) / plain);
            // Two different things were being read off one number. How much
            // detail survives wants a floor under it - that is what stops the
            // flood plain being a table. How far inside its own channel a point
            // is does not: the bed has to win completely in the middle of the
            // river, and giving it only three quarters put the staircase back on
            // the banks (measured: walls over three metres went from 48 shores
            // in 557 to 104).
            const Fixed mine = grown;
            const Fixed keepDetail = keepLeast + (core::kOne - keepLeast) * grown;
            const Fixed away = distance.raw > halfWidth.raw
                                       ? (distance - halfWidth) / std::max(Fixed::ratio(1, 100), valleyReach - halfWidth)
                                       : core::kZero;
            if (keepDetail.raw < keep.raw) keep = keepDetail;
            const std::size_t slot = nearCount;
            near[nearCount++] = {surface, halfWidth, depth, distance, away, mine, c.wet};
            out.found = true;
            if (distance.raw < nearest.raw) {
                nearest = distance;
                out.distance = distance;
                nearestOne = slot;
            }
            // Whether there is water here is a question about the channels that
            // carry water, and it was being answered by whichever channel was
            // closest whether it carried any or not.
            //
            // Every land cell has a channel and most of them are dry gullies, so
            // wherever a wet reach ends beside a dry one's start - which is every
            // cell centre at the top of a river, and every junction with a gully
            // - the dry one is nearer to its own start than the wet one is, wins
            // the comparison, and reports no water. Dissected on seed 11 at cell
            // 53,68: the water level came back as nought for the first two metres
            // of the reach and for the last sixty, while the wet channel ran
            // through both, so the river was drawn with its bed showing at
            // exactly the places two reaches meet.
            //
            // So the wet channels are tracked separately, and the water comes
            // from the nearest of those.
            if (c.wet && distance.raw < nearestWater.raw) {
                nearestWater = distance;
                nearestWaterOne = slot;
                const int segment=std::clamp(static_cast<int>((t*Fixed::fromInt(kSteps)).toInt()),0,kSteps-1);
                const auto dx=points[segment+1].x-points[segment].x;
                const auto dy=points[segment+1].y-points[segment].y;
                const auto length=core::hypot(dx,dy);
                if (length>core::kZero) { out.flowX=dx/length; out.flowY=dy/length; }
            }
    }

    if (nearCount > 0 && nearestWater.raw < Fixed::fromInt(1 << 20).raw) {
        const Near& water = near[nearestWaterOne];
        out.nearestWet = true;
        out.channelWidth = water.halfWidth * Fixed::fromInt(2);
        out.bankDistance = water.distance - water.halfWidth;
        out.wet = water.distance.raw <= water.halfWidth.raw;
    } else if (nearCount > 0) {
        // No water within reach: the bank of the nearest dry gully, for whoever
        // wants to know how far from a channel this is.
        out.bankDistance = near[nearestOne].distance - near[nearestOne].halfWidth;
    }

    // The ground: the country with its detail faded out over the flood plain of
    // whichever channel asks for the most of it.
    const Fixed ground = country + detail * keep;
    out.floor = ground;
    Fixed wetFloor, wetHead, wetWeight, wetBlend;
    // Normalise BEFORE taking the reciprocal. At a distant bank, 1/r^4 can
    // be below one Q32.32 unit; losing that channel changes a weighted mean
    // by tens of metres. A common scale cancels from the mean and keeps the
    // strongest contributor resolved without changing the profile kernel.
    const auto denominator = [](const Near& n) {
        const Fixed r = n.distance / std::max(n.halfWidth, Fixed::fromInt(4));
        const Fixed r2 = r * r;
        // Cardinal at the centreline. A 1/4096 regulariser let a nearby
        // upstream tributary raise the trunk's head just before confluence.
        return Fixed::fromRaw(1) + r2 * r2;
    };
    Fixed weightScale = Fixed::fromInt(2'000'000'000);
    for (std::size_t i = 0; i < nearCount; ++i)
        if (near[i].wet && near[i].away < core::kOne)
            weightScale = std::min(weightScale, denominator(near[i]));

    for (std::size_t i = 0; i < nearCount; ++i) {
        const Near& n = near[i];
        Fixed floor;
        if (n.wet) {
            // The level the water actually stands at here.
            //
            // The table says one thing and the ground says another: the surface
            // of a reach is interpolated between two cell heights, while the
            // ground under it is a curve through those same heights with a
            // detail layer on top, and in broken country the two are fifty
            // metres apart. Taken from the table alone, a trunk river came out
            // as a chain of fifty-metre pools with no bank leading down to
            // them. The ground wins: water does not stand above the country it
            // is running through.
            // Held down to the country, but no longer flush with it.
            //
            // Pinning the level to the ground at every point is what tilts the
            // water: a surface that copies the terrain is not a surface, and on
            // a slope it draws a river running down a ramp with its far bank
            // higher than its near one. The clamp exists so a reach whose table
            // level runs above the country does not stand in the air, and that
            // is still needed - but the bed of a channel is cut below the
            // country by its own depth, so the water may stand that much above
            // the untouched ground without floating. Inside that allowance the
            // level stays flat, and the lift at the end of carve raises the
            // ground beside it to meet it.
            // Stage is a property of the drainage graph, not of the terrain
            // sample on either bank. Clamping it to ground+depth made water
            // slope across its width and run uphill over noise ridges.
            Fixed level = n.surface;
            // And never below the sea, which floods whatever dips under it.
            //
            // The ground wins the line above, and at a river mouth the ground
            // goes on falling past sea level - so the channel's surface followed
            // it down and the mouth came out as a trench of water standing six
            // metres below the sea it was running into, with the sea's own
            // surface at nought a few metres to either side. Held at sea level,
            // the last of the channel simply is the sea, which is what a mouth
            // is; the bed underneath is still cut, because cutting only ever
            // lowers the ground.
            if (level.raw < 0) level = core::kZero;
            // Across the channel the bed is a dish rather than a trench -
            // deepest in the middle, and coming up to exactly the water's level
            // at the bank. It used to be flat at full depth right out to the
            // bank and then rise from there, which meant the ground beside a
            // big river stood ten metres below the water it was beside and was
            // not wet, and the shore itself was the step between them: every
            // river was drawn with a staircase of four-metre treads down each
            // side, because the whole fall happened inside one step of the
            // lattice. Coming up to the water line instead, the last few metres
            // of bed are shallows, and where the shore falls is decided by the
            // ground rather than by the width in the table.
            if (n.distance.raw <= n.halfWidth.raw && n.halfWidth.raw > 0) {
                const Fixed acrossIt = n.distance / n.halfWidth;
                floor = level - n.depth * (core::kOne - acrossIt * acrossIt);
            } else {
                // Out of the water and up to the country.
                //
                // Not on an eased curve alone. Eased is flat at both ends, which
                // is right at the top - a valley should meet the country without
                // a crease - and wrong at the bottom: the floor then leaves the
                // water so slowly that half a valley later it is still within
                // centimetres of it. Where the water stands at sea level that is
                // a plain a kilometre across at exactly nought, dead flat, with
                // a straight edge where the curve finally lifts. Measured on
                // this world: six hundred and forty metres at 0.00 m and slope
                // 0.000, then a rise - and being flat and at sea level it is
                // drawn as bare pale sand with no shading in it at all, which is
                // what the blotches with straight edges were.
                //
                // Mixed with the straight line it climbs from the first metre
                // and still flattens into the country at the top.
                const Fixed rise =
                        n.away * Fixed::ratio(2, 5) + ease(n.away) * Fixed::ratio(3, 5);
                floor = level + (ground - level) * rise;
            }
            // The highest water that reaches this point, over every channel and
            // not only the closest.
            //
            // The reach's own level, not the one held down to the ground above.
            // That one exists to stop water standing over the country it runs
            // through, and it is the right number for cutting the bed - but as
            // a water level it reads "the ground here", and since the bed is cut
            // by whichever channel cuts deepest, every hollow in the world then
            // came out filled to its own brim. The first cut of this drowned the
            // whole valley.
            //
            // A channel whose surface is above the untouched country here cannot
            // have reached here, so it is not asked. What is left is the highest
            // water that can actually stand at this point, and the ground is
            // under water wherever the cut bed lies below it.
            //
            // And only over its own flood plain, not over the whole valley it
            // shapes. A valley reaches as far as its relief needs - hundreds of
            // metres - while the water in it spreads over the flat ground
            // beside the channel and no further. Asked over the whole valley,
            // the higher of two neighbouring reaches drowned everything down to
            // the other one, because the bed between them is cut by whichever
            // cuts deepest and that is below both. `mine` is nought in the
            // channel and one past the flood plain, which is exactly the reach
            // of the water rather than of the valley.
            Fixed reaches = n.surface;
            if (reaches.raw < 0) reaches = core::kZero;
            if (n.distance <= n.halfWidth &&
                (!out.anyWet || reaches.raw > out.highest.raw)) {
                out.highest = reaches;
                out.anyWet = true;
            }
            if (i == nearestWaterOne) {
                // The level and the bed of the nearest channel that carries
                // water, whether or not this point is inside it: the shore needs
                // both to know where the water runs out. The nearest channel of
                // any kind is the wrong one to ask - see above.
                out.surface = level;
                // Continue the correction outside the wet bank; stopping it
                // exactly at halfWidth created a step on the shoreline.
                bed = floor;
                bedWeight = core::kOne - n.mine;
            }
        } else {
            // A gully is relative: a notch a few metres deep cut into whatever
            // the ground here happens to be.
            //
            // Cutting it down to the cell's own height instead was a
            // hundred-metre trench wherever the detail layer had put a ridge -
            // the coarse map's average and the ground at a point are different
            // things, and in a range they differ by more than the whole depth
            // of any valley.
            floor = ground - n.depth * (core::kOne - ease(n.away));
        }
        if (n.wet) {
            // A partition of continuous profiles, rather than nearest-channel
            // ownership. The latter creates a vertical step at its Voronoi
            // boundary, even if every individual valley is perfectly smooth.
            const Fixed support = core::kOne - ease(n.away);
            const Fixed weight = support * (weightScale / denominator(n));
            wetFloor += floor * weight;
            wetHead += std::max(core::kZero, n.surface) * weight;
            wetWeight += weight;
            wetBlend = std::max(wetBlend, support);
        } else if (floor < out.floor) out.floor = floor;
    }

    // Inside a channel the ground is that channel's bed, and not the lowest
    // thing anything else had to say about it.
    //
    // Valleys overlap. Where a reach whose water stands at three hundred and
    // twenty-eight metres runs eight hundred metres from one whose water stands
    // at two hundred and sixty, the second one's valley is still falling as it
    // passes under the first, and taking the lower of the two put the ground
    // fourteen metres beneath the water that was supposed to be running over
    // it - so the near river came out as a trench of even depth that stopped
    // dead at its bank. Its own bed wins over its own width: a dish that comes
    // up to the water line at the edge, whatever the far valley wanted. Blended
    // out over the flood plain rather than stopped at the bank, or the
    // correction itself would be the step it is there to prevent.
    if (wetWeight > core::kZero) {
        out.floor += (wetFloor / wetWeight - out.floor) * wetBlend;
        out.surface = wetHead / wetWeight;
    }
    out.floor = std::min(out.floor, std::max(country + detail, out.surface));
    // The canonical channel includes deposition as well as incision. Forcing
    // min(original, bed) here excavated holes beneath a fixed water surface.

    // The ground beside a river is not below the river.
    //
    // This is what makes water stand on a plinth. The bed is cut by whichever
    // channel cuts deepest, so a reach passing near a lower one has the ground
    // beside it dug out below its own water line - and the water cannot spread
    // into the hollow, because how much of a square is wet is measured from the
    // channel's width. What that draws is a ribbon of water with a vertical
    // side, hanging over the ground it is supposed to be lying in. Lakes get
    // the same treatment at their rim, which is the "lake standing on end".
    //
    // Lifted to the level of the *nearest* wet channel, not the highest that
    // reaches. The highest is the version that failed: two reaches at different
    // levels overlap, and lifting to the higher one walls the lower one in -
    // measured, banks p90 5.11 m/m against 1.09. The nearest one is the water
    // this ground actually belongs to, and it is by construction close to the
    // local bed, so the lift is small.
    //
    // Faded out across the flood plain rather than applied as a step: at the
    // bank it is the whole correction, a plain's width away it is nothing. And
    // never above the untouched country, so this cannot build an embankment.
    // A neighbouring upstream reach must not flood the downstream centreline
    // to its higher head. All queries use the same local graph profile.
    if (out.nearestWet) {
        out.highest = out.surface;
        // Being below a nearby reach's head is not hydraulic connectivity.
        // Flooding to an arbitrary apron radius creates a vertical water wall
        // where that radius ends. Standing floodwater belongs to a basin.
        out.anyWet = out.wet && out.floor < out.surface;
    }
    return out;
}

} // namespace world
