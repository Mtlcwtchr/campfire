#include "game/world/terrain_streaming/graph_carve.hpp"

#include <algorithm>
#include <array>

#include "game/generation/world_map_gen.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::streaming {
namespace {

using core::Fixed;
using core::WorldPos;

// Smoothstep. A valley eased rather than ramped has a floor and shoulders
// instead of a V, and a kink here would be a crease running the length of
// every river in the world.
Fixed ease(Fixed t) {
    t = core::saturate(t);
    return t * t * (Fixed::fromInt(3) - Fixed::fromInt(2) * t);
}

} // namespace

GraphCarver::GraphCarver(const HydrologyGraph& graph, core::WorldRect area,
                         std::int32_t haloMetres) {
    const Fixed halo = Fixed::fromInt(std::max(0, haloMetres));
    const core::WorldRect grown{{area.min.x - halo, area.min.y - halo},
                                {area.max.x + halo, area.max.y + halo}};

    // Which reaches to gather, from the graph's own page index rather than by
    // walking the world. A reach's stored bounds already include its valley,
    // so a page query finds the courses that can shape it even when their
    // water runs outside it.
    std::vector<RiverId> wanted;
    std::vector<WaterBodyId> wantedBodies;
    const auto first = tileAt(grown.min);
    const auto last = tileAt(grown.max);
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) {
            const auto* page = findHydrologySpatialPage(graph, {x, y, 0});
            if (page == nullptr) continue;
            wanted.insert(wanted.end(), page->segments.begin(), page->segments.end());
            wantedBodies.insert(wantedBodies.end(), page->waterBodies.begin(), page->waterBodies.end());
        }
    std::sort(wanted.begin(), wanted.end());
    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
    std::sort(wantedBodies.begin(), wantedBodies.end());
    wantedBodies.erase(std::unique(wantedBodies.begin(), wantedBodies.end()), wantedBodies.end());

    reaches_.reserve(wanted.size());
    for (const RiverId id : wanted) {
        const auto* segment = riverSegmentOf(graph, id);
        if (segment == nullptr || segment->course.size() < 2) continue;
        Reach reach;
        reach.id = id;
        reach.points.reserve(segment->course.size());
        Fixed furthest = core::kZero;
        for (const ReachPoint& source : segment->course) {
            reach.points.push_back({source.position, source.surface, source.halfWidth,
                                    source.depth, source.valleyReach});
            furthest = core::max(furthest, core::max(source.valleyReach, source.halfWidth));
        }
        reach.lowX = reach.highX = reach.points.front().position.x;
        reach.lowY = reach.highY = reach.points.front().position.y;
        for (const Point& point : reach.points) {
            reach.lowX = core::min(reach.lowX, point.position.x);
            reach.lowY = core::min(reach.lowY, point.position.y);
            reach.highX = core::max(reach.highX, point.position.x);
            reach.highY = core::max(reach.highY, point.position.y);
        }
        reach.lowX -= furthest; reach.lowY -= furthest;
        reach.highX += furthest; reach.highY += furthest;
        if (reach.highX < grown.min.x || reach.lowX > grown.max.x || reach.highY < grown.min.y ||
            reach.lowY > grown.max.y)
            continue;
        reaches_.push_back(std::move(reach));
    }

    // Standing water over its source lattice: connected Slopes nodes when
    // available, otherwise legacy macro-cell centres. The page index includes
    // interpolation support, so only locally relevant bodies need copying.
    // One cell of margin either side serves the four-corner lookup.
    cellMetres_ = Fixed::fromInt(graph.macroCellMetres > 0 ? graph.macroCellMetres : 1);
    for (const auto id : wantedBodies) {
        const auto* body = waterBodyOf(graph, id);
        if (body == nullptr || body->kind != WaterBodyKind::Lake || body->basinStep <= 0) continue;
        cellMetres_ = Fixed::fromInt(body->basinStep);
        naturalBasins_ = true;
        break;
    }
    const std::int64_t metres = cellMetres_.toInt();
    firstCellX_ = floorDiv(grown.min.x.toInt(), metres) - 1;
    firstCellY_ = floorDiv(grown.min.y.toInt(), metres) - 1;
    cellsWide_ = floorDiv(grown.max.x.toInt(), metres) + 2 - firstCellX_;
    cellsHigh_ = floorDiv(grown.max.y.toInt(), metres) + 2 - firstCellY_;
    if (cellsWide_ > 0 && cellsHigh_ > 0 &&
        cellsWide_ * cellsHigh_ < 1024 * 1024) {
        basins_.assign(static_cast<std::size_t>(cellsWide_ * cellsHigh_), Basin{});
        for (const auto id : wantedBodies) {
            const auto* source = waterBodyOf(graph, id);
            if (source == nullptr) continue;
            const WaterBody& body = *source;
            if (body.kind != WaterBodyKind::Lake) continue;
            const auto& samples = naturalBasins_ ? body.basinSamples : body.macroCells;
            const auto begin = std::lower_bound(samples.begin(), samples.end(), firstCellY_,
                    [](core::TilePos at, std::int64_t row) { return at.y < row; });
            for (auto it = begin; it != samples.end() && it->y < firstCellY_ + cellsHigh_; ++it) {
                const core::TilePos at = *it;
                const std::int64_t x = at.x - firstCellX_, y = at.y - firstCellY_;
                if (x < 0 || y < 0 || x >= cellsWide_ || y >= cellsHigh_) continue;
                Basin& basin = basins_[static_cast<std::size_t>(y * cellsWide_ + x)];
                basin.id = body.id;
                basin.level = body.level;
                basin.floor = naturalBasins_ ? core::kZero :
                        body.macroCellFloor[static_cast<std::size_t>(it - samples.begin())];
            }
        }
    } else {
        cellsWide_ = cellsHigh_ = 0;
    }

    // File every leg under the ground it can shape, so a query reads one cell
    // instead of walking the window. Counted first and then filled, so the
    // whole index is two flat arrays and not a map of vectors.
    legFirstX_ = floorDiv(grown.min.x.toInt(), kLegCellMetres) - 1;
    legFirstY_ = floorDiv(grown.min.y.toInt(), kLegCellMetres) - 1;
    legWide_ = floorDiv(grown.max.x.toInt(), kLegCellMetres) + 2 - legFirstX_;
    legHigh_ = floorDiv(grown.max.y.toInt(), kLegCellMetres) + 2 - legFirstY_;
    if (legWide_ > 0 && legHigh_ > 0 && legWide_ * legHigh_ <= 64 * 1024) {
        const auto cells = static_cast<std::size_t>(legWide_ * legHigh_);
        std::vector<std::uint32_t> counts(cells + 1, 0);
        const auto visit = [&](auto&& each) {
            for (std::uint32_t r = 0; r < reaches_.size(); ++r) {
                const Reach& reach = reaches_[r];
                for (std::uint32_t leg = 0; leg + 1 < reach.points.size(); ++leg) {
                    const Point& a = reach.points[leg];
                    const Point& b = reach.points[leg + 1];
                    const Fixed span = core::max(a.valleyReach, b.valleyReach);
                    const auto lowX = floorDiv(
                            (core::min(a.position.x, b.position.x) - span).toInt(), kLegCellMetres);
                    const auto highX = floorDiv(
                            (core::max(a.position.x, b.position.x) + span).toInt(), kLegCellMetres);
                    const auto lowY = floorDiv(
                            (core::min(a.position.y, b.position.y) - span).toInt(), kLegCellMetres);
                    const auto highY = floorDiv(
                            (core::max(a.position.y, b.position.y) + span).toInt(), kLegCellMetres);
                    for (auto cy = lowY; cy <= highY; ++cy)
                        for (auto cx = lowX; cx <= highX; ++cx) {
                            const auto x = cx - legFirstX_, y = cy - legFirstY_;
                            if (x < 0 || y < 0 || x >= legWide_ || y >= legHigh_) continue;
                            each(static_cast<std::size_t>(y * legWide_ + x), LegRef{r, leg});
                        }
                }
            }
        };
        visit([&](std::size_t cell, LegRef) { ++counts[cell + 1]; });
        for (std::size_t i = 0; i < cells; ++i) counts[i + 1] += counts[i];
        legCellStart_ = counts;
        legIndex_.resize(counts[cells]);
        auto cursor = counts;
        visit([&](std::size_t cell, LegRef ref) { legIndex_[cursor[cell]++] = ref; });
    } else {
        legWide_ = legHigh_ = 0;
    }
}

const GraphCarver::Basin* GraphCarver::basinAt(std::int64_t cellX, std::int64_t cellY) const {
    const std::int64_t x = cellX - firstCellX_, y = cellY - firstCellY_;
    if (x < 0 || y < 0 || x >= cellsWide_ || y >= cellsHigh_) return nullptr;
    return &basins_[static_cast<std::size_t>(y * cellsWide_ + x)];
}

Fixed GraphCarver::coverAt(WorldPos at, WaterBodyId& body, Fixed& level, Fixed& floor) const {
    body = kInvalidWaterBodyId;
    level = core::kZero;
    floor = core::kZero;
    if (cellsWide_ == 0) return core::kZero;
    // Bilinear between the four cell centres around the point, with dry land
    // contributing nothing: the fill then tapers to zero over half a cell
    // instead of dropping off an edge.
    const Fixed half = naturalBasins_ ? core::kZero : cellMetres_ / Fixed::fromInt(2);
    const Fixed gx = (at.x - half) / cellMetres_;
    const Fixed gy = (at.y - half) / cellMetres_;
    const std::int64_t cx = gx.toInt(), cy = gy.toInt();
    const Fixed tx = gx - Fixed::fromInt(cx), ty = gy - Fixed::fromInt(cy);
    Fixed cover = core::kZero;
    Fixed strongest = core::kZero;
    for (int dy = 0; dy <= 1; ++dy)
        for (int dx = 0; dx <= 1; ++dx) {
            const Basin* basin = basinAt(cx + dx, cy + dy);
            if (basin == nullptr || basin->id == kInvalidWaterBodyId) continue;
            const Fixed weight = (dx == 0 ? core::kOne - tx : tx) *
                                 (dy == 0 ? core::kOne - ty : ty);
            cover += weight;
            floor += basin->floor * weight;
            // The body is the strongest of the four, never a blend: a lake
            // has one head, and averaging two of them is exactly how one body
            // came to stand at two levels with a step where they met.
            if (weight.raw > strongest.raw) {
                strongest = weight;
                body = basin->id;
                level = basin->level;
            }
        }
    // Normalised by the cover, so a point that only partly reads lake gets
    // the mean floor of the cells that are lake rather than a value dragged
    // towards nothing by the ones that are not.
    if (cover.raw > 0) floor = floor / cover;
    return cover;
}

CarvedSample GraphCarver::carve(WorldPos at, Fixed country, Fixed detail) const {
    CarvedSample out;
    out.bankDistance = Fixed::fromInt(1 << 20);

    // Give the basin back.
    //
    // The depression fill raises a basin to its outlet and the macro map then
    // stores the filled ground, on the argument that a basin silted up to its
    // outlet is flat ground. Some are; a great many are lakes, and the
    // generator records how far it raised each cell precisely so the floor
    // can be had back. Nothing did: the old carving raised the ground to the
    // waterline instead, and drew every lake on the map as a lawn with a
    // millimetre of water on it. Measured on seed 11 at macro cell 21,31 -
    // thirty-six metres of recorded fill, and a lake bed drawn one metre
    // under its own surface.
    //
    // Dug where the ground is mostly lake, left alone where it is not. The
    // interpolated fill reaches half a macro cell past the last flooded cell,
    // and applied out there it digs a basin out of the hillside beside the
    // lake: that band is the rim's business, below, not the basin's.
    // The floor is interpolated, not the fill. Subtracting a fill from the
    // country digs the basin twice over: the country is a curve through the
    // cell heights and already dips below a lake cell's own value towards its
    // lower neighbours, so taking the recorded fill off that as well put the
    // bed sixty metres under a lake the map says is thirty-six deep.
    WaterBodyId basinBody = kInvalidWaterBodyId;
    Fixed basinLevel = core::kZero, basinFloor = core::kZero;
    const Fixed cover = coverAt(at, basinBody, basinLevel, basinFloor);
    const Fixed ground =
            !naturalBasins_ && cover.raw > 0
                    ? core::lerp(country, basinFloor,
                                 ease((cover - Fixed::ratio(1, 2)) * Fixed::fromInt(2)))
                    : country;
    out.floor = ground + detail;

    // Every reach that shapes this ground, gathered before any of it is
    // settled. The detail layer has to be damped by whichever channel asks
    // for the most of it, and that is not known until they have all been
    // looked at - which is why this cannot be one pass.
    struct Hit {
        Fixed distance, surface, halfWidth, depth, valleyReach;
        RiverId id = kInvalidRiverId;
        Fixed flowX, flowY;
    };
    constexpr std::size_t kMaxHits = 32;
    std::array<Hit, kMaxHits> hits{};
    std::size_t hitCount = 0;

    // Only the legs filed under this patch of ground, and only the closest one
    // of each reach - which is what the walk over every reach in the window was
    // working out the long way.
    std::array<Fixed, kMaxHits> closest{};
    std::array<std::uint32_t, kMaxHits> owner{};
    if (legWide_ > 0) {
        const auto cellX = floorDiv(at.x.toInt(), kLegCellMetres) - legFirstX_;
        const auto cellY = floorDiv(at.y.toInt(), kLegCellMetres) - legFirstY_;
        if (cellX >= 0 && cellY >= 0 && cellX < legWide_ && cellY < legHigh_) {
            const auto cell = static_cast<std::size_t>(cellY * legWide_ + cellX);
            for (auto i = legCellStart_[cell]; i < legCellStart_[cell + 1]; ++i) {
                const LegRef ref = legIndex_[i];
                const Reach& reach = reaches_[ref.reach];
                const Point& a = reach.points[ref.leg];
                const Point& b = reach.points[ref.leg + 1];
                // A course is finely stepped on purpose, so most of its legs
                // are nowhere near any one sample even inside its own cell.
                // Four comparisons against the box this leg can reach cost a
                // fraction of the projection and the root that would otherwise
                // be spent finding that out.
                const Fixed span = core::max(a.valleyReach, b.valleyReach);
                if (at.x < core::min(a.position.x, b.position.x) - span ||
                    at.x > core::max(a.position.x, b.position.x) + span ||
                    at.y < core::min(a.position.y, b.position.y) - span ||
                    at.y > core::max(a.position.y, b.position.y) + span)
                    continue;
                const Fixed dx = b.position.x - a.position.x, dy = b.position.y - a.position.y;
                const Fixed lengthSquared = dx * dx + dy * dy;
                Fixed t = core::kZero;
                if (lengthSquared.raw > 0) {
                    t = ((at.x - a.position.x) * dx + (at.y - a.position.y) * dy) / lengthSquared;
                    t = core::clamp(t, core::kZero, core::kOne);
                }
                const Fixed cx = a.position.x + dx * t, cy = a.position.y + dy * t;
                const Fixed distance = core::hypot(at.x - cx, at.y - cy);

                // Which hit this reach already owns, if any.
                std::size_t slot = hitCount;
                for (std::size_t h = 0; h < hitCount; ++h)
                    if (owner[h] == ref.reach) { slot = h; break; }
                if (slot == hitCount) {
                    if (hitCount == kMaxHits) continue;
                    ++hitCount;
                    closest[slot] = Fixed::fromInt(1 << 20);
                    owner[slot] = ref.reach;
                    hits[slot] = Hit{};
                }
                if (distance.raw >= closest[slot].raw) continue;
                closest[slot] = distance;
                const Point profile{{cx, cy},
                                    core::lerp(a.surface, b.surface, t),
                                    core::lerp(a.halfWidth, b.halfWidth, t),
                                    core::lerp(a.depth, b.depth, t),
                                    core::lerp(a.valleyReach, b.valleyReach, t)};
                Hit& hit = hits[slot];
                hit.distance = distance;
                hit.surface = profile.surface;
                hit.halfWidth = profile.halfWidth;
                hit.depth = profile.depth;
                hit.valleyReach = core::max(profile.valleyReach, profile.halfWidth + core::kOne);
                hit.id = reach.id;
                const Fixed length = core::hypot(dx, dy);
                hit.flowX = length.raw > 0 ? dx / length : core::kZero;
                hit.flowY = length.raw > 0 ? dy / length : core::kZero;
            }
        }
    }
    // Reaches whose closest leg turned out to be too far to shape this ground
    // are dropped, which the walk did before it ever made a hit of them.
    for (std::size_t h = hitCount; h-- > 0;)
        if (hits[h].distance.raw > hits[h].valleyReach.raw) hits[h] = hits[--hitCount];

    // How much of the detail layer survives here.
    //
    // The detail is a landscape tens of metres deep and a river's surface is
    // a level: where the two meet unarbitrated the water fills whatever
    // hollow the noise put beside the channel, and the bed comes out tens of
    // metres below where it belongs. So it fades out towards a channel, which
    // is also what a flood plain is - none of it at the bank, all of it half
    // as far again beyond, or forty metres, whichever is more.
    //
    // Never none of it, though. What is left when the detail is gone is one
    // number per macro cell, and where two neighbours hold the same number
    // that is dead flat ground over hundreds of metres.
    Fixed keep = core::kOne;
    for (std::size_t n = 0; n < hitCount; ++n) {
        const Hit& hit = hits[n];
        const Fixed plain = hit.halfWidth * Fixed::ratio(3, 5) + Fixed::fromInt(40);
        const Fixed keepLeast = hit.halfWidth.raw > 0 ? Fixed::ratio(1, 4) : Fixed::ratio(3, 4);
        const Fixed outside =
                hit.distance.raw <= hit.halfWidth.raw
                        ? core::kZero
                        : ease((hit.distance - hit.halfWidth) / core::max(plain, core::kOne));
        keep = core::min(keep, keepLeast + (core::kOne - keepLeast) * outside);
    }
    const Fixed damped = ground + detail * keep;
    out.floor = damped;

    Fixed nearest = Fixed::fromInt(1 << 20);
    Fixed highest = core::kZero;
    bool anyWater = false;
    for (std::size_t n = 0; n < hitCount; ++n) {
        const Hit& hit = hits[n];
        // The bed is below the surface; the ground rises from the bank out to
        // the reach of the valley, where it meets the country again. Every
        // channel within reach has its say and the lowest floor wins, which is
        // a minimum of continuous functions and so continuous: valleys that
        // meet simply join, instead of tiling the country into flat-bottomed
        // cells with walls between them.
        // A channel is a trough, not a slot.
        //
        // The bed used to sit at one depth right across the wet width and then
        // climb out of the valley from there, which gives a river vertical
        // walls as high as it is deep. Step off the water and you are not on a
        // bank, you are at the bottom of the channel: measured on the shore
        // tests, the ground at the exit stood twenty-seven, fifty-five and a
        // hundred and fifty metres below the surface it had just left.
        //
        // Deepest in the middle and rising to the waterline at the bank, as
        // the square of the distance across - which is the shape a channel in
        // alluvium takes. The water then ends where the bed reaches the
        // surface, which is a contour and needs no rule to say where it is.
        // Past the bank the ground goes on rising, out to the reach of the
        // valley, where it meets the country again.
        Fixed shaped;
        if (hit.distance.raw <= hit.halfWidth.raw && hit.halfWidth.raw > 0) {
            const Fixed across = hit.distance / hit.halfWidth;
            shaped = hit.surface - hit.depth * (core::kOne - across * across);
        } else {
            const Fixed t = core::saturate((hit.distance - hit.halfWidth) /
                core::max(hit.valleyReach - hit.halfWidth, core::kOne));
            // In high relief, keep a narrow gorge with steep rock shoulders.
            // Lowland floodplains retain the old broad profile; the wet channel,
            // bank height and stored river head are deliberately unchanged.
            const Fixed highland = ease((ground - Fixed::fromInt(280)) / Fixed::fromInt(620));
            const Fixed incision = ease((damped - hit.surface - Fixed::fromInt(80)) / Fixed::fromInt(280));
            const Fixed outward = core::lerp(ease(t), ease(t*Fixed::fromInt(3)), highland*incision);
            shaped = core::lerp(hit.surface, damped, outward);
        }
        out.floor = core::min(out.floor, shaped);

        if (hit.distance.raw < nearest.raw) {
            nearest = hit.distance;
            out.reach = hit.id;
            out.bankDistance = hit.distance - hit.halfWidth;
            out.flowX = hit.flowX;
            out.flowY = hit.flowY;
            out.surface = hit.surface; // dry bank must not interpolate to sea level
        }
        // Water finds its own level, and the level is the highest one that
        // reaches: taking the nearest channel alone let two crossing courses
        // each keep their own, with a step where they met.
        //
        // Reaches, not shapes. A hit is anything whose valley touches this
        // point, and a valley is how far a reach *moves the ground* - four
        // hundred metres for a brook eight metres across. Offering the surface
        // over all of it made every watercourse in the world a lake as wide as
        // its valley: measured over three hundred cross-sections on two worlds,
        // a channel of eight metres came out wet across nine hundred and
        // twenty-five, seventy-seven times its own width, and the ninetieth
        // percentile ran past the twelve hundred metres the measurement could
        // see. That is what a river looked like from the air, and it is why
        // the country read as flooded.
        //
        // The water reaches its bank and a little past it - the depth again,
        // which is a bank of about one in one - and no further. Past that the
        // ground is the valley's business and the valley is dry land.
        const Fixed reachOfWater =
                hit.halfWidth + core::max(hit.depth, Fixed::fromInt(kSampleMetres / 2));
        if (hit.halfWidth.raw > 0 && hit.distance.raw <= reachOfWater.raw &&
            (!anyWater || hit.surface.raw > highest.raw)) {
            highest = hit.surface;
            anyWater = true;
        }
    }

    // A lake is contained by its rim, and the rim is the only ground a body
    // moves. Where the cover is partial the ground is brought up to just over
    // the waterline, so a body cannot leak across the edge of its own
    // footprint; where the cover is whole the bed is left exactly as the
    // country and the reaches made it.
    if (!naturalBasins_ && cover.raw > 0) {
        // The rim belongs at the very OUTSIDE of the footprint, and used to peak
        // in the middle of it.
        //
        // Peaked there, it raises a bank half a cell inside the lake's own edge
        // and the water ends against that bank - so the shoreline is the bank's
        // shape, and the bank's shape is the half-contour of a bilinear mask on
        // a five-hundred-and-forty-metre lattice. That is a lake with straight
        // sides and mitred corners, whatever the ground under it looks like.
        //
        // Kept to the outer fifth it still does its job, which is to stop a body
        // running away down some valley that happens to lie below its level,
        // while leaving the shore itself to the ground.
        const Fixed rim = cover < Fixed::ratio(1, 5)
                                  ? ease((Fixed::ratio(1, 5) - cover) * Fixed::fromInt(5))
                                  : core::kZero;
        // An inlet is an opening in the rim, not a bank to raise: rimmed over,
        // a lake's own inflow is dammed short of the water it feeds.
        const Fixed protect =
                out.reach != kInvalidRiverId
                        ? ease(out.bankDistance / Fixed::fromInt(2 * kSampleMetres))
                        : core::kOne;
        // And the lip is CAPPED, which is the difference between a bank and a
        // dam.
        //
        // It is here to stop a body running away across a fringe that is very
        // nearly level with it - a couple of metres of bank does that. Left
        // unbounded it raises the ground to the waterline however far below the
        // waterline that ground is, and in the mountains the ground at the edge
        // of a small basin is forty or fifty metres below it: the lake then
        // arrives sitting on a wall of its own making, which is exactly what it
        // looked like. Where the ground really does fall away that far the
        // answer is that the water goes down with it, not that a rampart
        // appears around the water.
        const Fixed lip = core::clamp(basinLevel + Fixed::fromInt(2) - out.floor, core::kZero,
                                      Fixed::fromInt(3));
        out.floor += lip * rim * protect;
    }
    // And the water itself, where the point is inside the body rather than on
    // its rim.
    //
    // Inside means the ground is under the water, not that a mask says so. A
    // lake is a level surface and its edge is the contour where the country
    // rises through it - which is as crooked as the country is. Testing the
    // mask instead drew the edge on the coarse lattice and nowhere else.
    if (basinBody != kInvalidWaterBodyId) out.surface = basinLevel;
    if (basinBody != kInvalidWaterBodyId && out.floor.raw < basinLevel.raw) {
        if (!anyWater || basinLevel.raw > highest.raw) {
            highest = basinLevel;
            anyWater = true;
        }
        if (out.floor.raw < basinLevel.raw) out.body = basinBody;
    }

    // Sea level is zero everywhere in the world, which is what makes it a sea
    // level, so the coast is wherever the ground crosses it.
    if (out.floor.raw < 0 && (!anyWater || highest.raw < 0)) {
        highest = core::kZero;
        anyWater = true;
    }
    if (out.floor.raw < 0 && out.body == kInvalidWaterBodyId && highest.raw <= 0)
        out.body = kOceanWaterBodyId;

    out.wet = anyWater && out.floor.raw < highest.raw;
    if (out.wet) out.surface = highest;
    return out;
}

} // namespace world::streaming
