#include "game/world/terrain_streaming/hydrology_selection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace world::streaming {
namespace {

// Everything draining into a node: the segments that end at it.
std::vector<std::vector<RiverId>> inflowsByNode(const HydrologyGraph& graph) {
    std::vector<std::vector<RiverId>> into(graph.nodes.size() + 1);
    for (const RiverSegment& s : graph.segments)
        if (s.to != kInvalidRiverNodeId && s.to < into.size()) into[s.to].push_back(s.id);
    return into;
}

void grow(core::WorldRect& bounds, bool& any, const core::WorldRect& r) {
    if (!r.valid()) return;
    if (!any) { bounds = r; any = true; return; }
    bounds.min.x = std::min(bounds.min.x, r.min.x);
    bounds.min.y = std::min(bounds.min.y, r.min.y);
    bounds.max.x = std::max(bounds.max.x, r.max.x);
    bounds.max.y = std::max(bounds.max.y, r.max.y);
}

// Up the network from what is already chosen: every segment ending at the
// upstream node of a chosen one, and every lake a chosen segment leaves,
// with the segments ending in that lake - until nothing new is found.
WaterSelection closeUpstream(const HydrologyGraph& graph, std::vector<RiverId> segments,
                             std::vector<WaterBodyId> bodies) {
    const auto into = inflowsByNode(graph);
    std::vector<std::uint8_t> segmentSeen(graph.segments.size() + 1, 0), bodySeen(graph.waterBodies.size() + 1, 0);
    std::vector<RiverId> segmentQueue;
    std::vector<WaterBodyId> bodyQueue;
    for (const RiverId s : segments)
        if (s < segmentSeen.size() && !segmentSeen[s]) { segmentSeen[s] = 1; segmentQueue.push_back(s); }
    for (const WaterBodyId b : bodies)
        if (b < bodySeen.size() && b != kOceanWaterBodyId && !bodySeen[b]) { bodySeen[b] = 1; bodyQueue.push_back(b); }
    const auto addSegment = [&](RiverId s) {
        if (s == kInvalidRiverId || s >= segmentSeen.size() || segmentSeen[s]) return;
        segmentSeen[s] = 1;
        segmentQueue.push_back(s);
    };
    std::size_t si = 0, bi = 0;
    while (si < segmentQueue.size() || bi < bodyQueue.size()) {
        while (si < segmentQueue.size()) {
            const RiverSegment* s = riverSegmentOf(graph, segmentQueue[si++]);
            if (!s) continue;
            if (s->from < into.size())
                for (const RiverId up : into[s->from]) addSegment(up);
            const WaterBodyId lake = s->sourceWaterBody;
            if (lake != kInvalidWaterBodyId && lake != kOceanWaterBodyId && lake < bodySeen.size() && !bodySeen[lake]) {
                bodySeen[lake] = 1;
                bodyQueue.push_back(lake);
            }
        }
        while (bi < bodyQueue.size()) {
            const WaterBody* b = waterBodyOf(graph, bodyQueue[bi++]);
            if (!b) continue;
            for (const RiverId inlet : b->inlets) addSegment(inlet);
        }
    }
    WaterSelection out;
    bool any = false;
    for (RiverId s = 1; s < segmentSeen.size(); ++s) {
        if (!segmentSeen[s]) continue;
        const RiverSegment* segment = riverSegmentOf(graph, s);
        if (!segment) continue;
        out.segments.push_back(s);
        core::WorldRect valley = segment->bounds;
        valley.min.x -= segment->valleyReach; valley.min.y -= segment->valleyReach;
        valley.max.x += segment->valleyReach; valley.max.y += segment->valleyReach;
        grow(out.bounds, any, valley);
        out.macroCells.insert(out.macroCells.end(), segment->macroCells.begin(), segment->macroCells.end());
    }
    const auto cell = core::Fixed::fromInt(std::max(1, graph.macroCellMetres));
    for (WaterBodyId b = 1; b < bodySeen.size(); ++b) {
        if (!bodySeen[b]) continue;
        const WaterBody* body = waterBodyOf(graph, b);
        if (!body) continue;
        out.waterBodies.push_back(b);
        for (const core::TilePos p : body->macroCells) {
            out.macroCells.push_back(p.y * graph.macroWidth + p.x);
            const core::WorldRect r{{core::Fixed::fromInt(p.x) * cell, core::Fixed::fromInt(p.y) * cell},
                                    {core::Fixed::fromInt(p.x + 1) * cell, core::Fixed::fromInt(p.y + 1) * cell}};
            grow(out.bounds, any, r);
        }
    }
    std::sort(out.macroCells.begin(), out.macroCells.end());
    out.macroCells.erase(std::unique(out.macroCells.begin(), out.macroCells.end()), out.macroCells.end());
    return out;
}

double distanceToCourse(const RiverSegment& s, double x, double y) {
    double best = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i + 1 < s.course.size(); ++i) {
        const double ax = s.course[i].position.x.toDouble(), ay = s.course[i].position.y.toDouble();
        const double bx = s.course[i + 1].position.x.toDouble(), by = s.course[i + 1].position.y.toDouble();
        const double dx = bx - ax, dy = by - ay, length = dx * dx + dy * dy;
        const double t = length > 0 ? std::clamp(((x - ax) * dx + (y - ay) * dy) / length, 0.0, 1.0) : 0.0;
        best = std::min(best, std::hypot(x - (ax + dx * t), y - (ay + dy * t)));
    }
    if (s.course.size() == 1) best = std::hypot(x - s.course[0].position.x.toDouble(), y - s.course[0].position.y.toDouble());
    return best;
}

} // namespace

WaterSelection upstreamOfSegment(const HydrologyGraph& graph, RiverId segment) {
    if (!riverSegmentOf(graph, segment)) return {};
    return closeUpstream(graph, {segment}, {});
}

WaterSelection upstreamOfWaterBody(const HydrologyGraph& graph, WaterBodyId body) {
    const WaterBody* b = waterBodyOf(graph, body);
    if (!b) return {};
    if (b->kind == WaterBodyKind::Ocean) {
        WaterSelection ocean;
        ocean.waterBodies.push_back(body);
        return ocean;
    }
    return closeUpstream(graph, {}, {body});
}

WaterPick pickWater(const HydrologyGraph& graph, core::WorldPos at, core::Fixed slack) {
    WaterPick pick;
    const double x = at.x.toDouble(), y = at.y.toDouble();
    double nearest = std::numeric_limits<double>::max();
    for (const RiverSegment& s : graph.segments) {
        const double reach = std::max(s.width.toDouble() * 0.5, 1.0) + slack.toDouble();
        if (x < s.bounds.min.x.toDouble() - reach || x > s.bounds.max.x.toDouble() + reach ||
            y < s.bounds.min.y.toDouble() - reach || y > s.bounds.max.y.toDouble() + reach)
            continue;
        const double d = distanceToCourse(s, x, y);
        if (d <= reach && d < nearest) { nearest = d; pick.segment = s.id; }
    }
    if (pick.segment != kInvalidRiverId) return pick;
    if (graph.macroCellMetres <= 0) return pick;
    const core::TilePos cell{std::int32_t(std::floor(x / graph.macroCellMetres)),
                             std::int32_t(std::floor(y / graph.macroCellMetres))};
    for (const WaterBody& b : graph.waterBodies) {
        if (b.kind != WaterBodyKind::Lake) continue;
        if (std::find(b.macroCells.begin(), b.macroCells.end(), cell) != b.macroCells.end()) {
            pick.body = b.id;
            break;
        }
    }
    return pick;
}

WaterSelection selectWater(const HydrologyGraph& graph, core::WorldPos at) {
    const WaterPick pick = pickWater(graph, at);
    if (pick.segment != kInvalidRiverId) return upstreamOfSegment(graph, pick.segment);
    if (pick.body != kInvalidWaterBodyId) return upstreamOfWaterBody(graph, pick.body);
    return {};
}

} // namespace world::streaming

