#include "game/world/terrain_streaming/hydrology_graph.hpp"

#include <algorithm>

namespace world::streaming {
namespace {

bool refuse(std::string* why, const char* reason) {
    if (why != nullptr) *why = reason;
    return false;
}

template <class Id>
bool sortedUniqueWithin(const std::vector<Id>& ids, std::size_t limit) {
    for (std::size_t n = 0; n < ids.size(); ++n) {
        if (ids[n] == 0 || ids[n] > limit) return false;
        if (n > 0 && ids[n] <= ids[n - 1]) return false;
    }
    return true;
}

} // namespace

bool validateHydrologyGraph(const HydrologyGraph& graph, std::string* why) {
    const std::size_t nodes = graph.nodes.size();
    const std::size_t segments = graph.segments.size();
    const std::size_t bodies = graph.waterBodies.size();
    const auto isNode = [&](RiverNodeId id) { return id != kInvalidRiverNodeId && id <= nodes; };
    const auto isBody = [&](WaterBodyId id) { return id != kInvalidWaterBodyId && id <= bodies; };

    if (graph.macroWidth < 0 || graph.macroHeight < 0 || graph.macroCellMetres < 0)
        return refuse(why, "macro dimensions are negative");
    const auto cells = static_cast<std::int64_t>(graph.macroWidth) * graph.macroHeight;

    for (std::size_t n = 0; n < nodes; ++n) {
        const RiverNode& node = graph.nodes[n];
        if (node.id != static_cast<RiverNodeId>(n + 1)) return refuse(why, "node IDs are not dense");
        if (node.downstream != kInvalidRiverNodeId && !isNode(node.downstream))
            return refuse(why, "node points downstream at nothing");
        if (node.downstream == node.id) return refuse(why, "node is its own downstream");
        if (node.waterBody != kInvalidWaterBodyId && !isBody(node.waterBody))
            return refuse(why, "node belongs to a water body that is not there");
        if (node.macroCell < 0 || (cells > 0 && node.macroCell >= cells))
            return refuse(why, "node sits outside the macro map");
        if (static_cast<int>(node.kind) > static_cast<int>(RiverNodeKind::Terminal))
            return refuse(why, "node has an unknown kind");
    }
    // A second pass, because it reads a node the first pass may not have
    // reached yet: water does not run uphill, so a head never rises going
    // downstream. Two reaches meeting at one node share its head by
    // construction, so equality is the common case and not a failure.
    for (const RiverNode& node : graph.nodes) {
        if (node.downstream == kInvalidRiverNodeId) continue;
        if (graph.nodes[node.downstream - 1].surface.raw > node.surface.raw)
            return refuse(why, "a water head rises going downstream");
    }

    for (std::size_t s = 0; s < segments; ++s) {
        const RiverSegment& segment = graph.segments[s];
        if (segment.id != static_cast<RiverId>(s + 1)) return refuse(why, "segment IDs are not dense");
        if (!isNode(segment.from)) return refuse(why, "segment starts at no node");
        if (segment.to != kInvalidRiverNodeId && !isNode(segment.to))
            return refuse(why, "segment ends at no node");
        if (segment.sourceWaterBody != kInvalidWaterBodyId && !isBody(segment.sourceWaterBody))
            return refuse(why, "segment leaves a water body that is not there");
        if (segment.destinationWaterBody != kInvalidWaterBodyId &&
            !isBody(segment.destinationWaterBody))
            return refuse(why, "segment ends in a water body that is not there");
        if (segment.course.empty()) return refuse(why, "segment has no course");
        if (segment.macroCells.size() < 2)
            return refuse(why, "segment does not say which cells it came from");
        for (const std::int32_t member : segment.macroCells)
            if (member < 0 || (cells > 0 && member >= cells))
                return refuse(why, "segment came from a cell outside the macro map");
        for (const ReachPoint& point : segment.course) {
            if (point.halfWidth.raw < 0 || point.depth.raw < 0 || point.valleyReach.raw < 0)
                return refuse(why, "a reach point has a negative measurement");
        }
        if (!segment.bounds.valid()) return refuse(why, "segment bounds are inside out");
    }

    for (std::size_t b = 0; b < bodies; ++b) {
        const WaterBody& body = graph.waterBodies[b];
        if (body.id != static_cast<WaterBodyId>(b + 1))
            return refuse(why, "water body IDs are not dense");
        if (static_cast<int>(body.kind) > static_cast<int>(WaterBodyKind::Lake))
            return refuse(why, "water body has an unknown kind");
        if (body.macroCellFloor.size() != body.macroCells.size())
            return refuse(why, "a water body has a footprint without a floor under it");
        if (body.basinStep<0 || (body.basinStep==0 && !body.basinSamples.empty()))
            return refuse(why,"a natural basin has no sampling step");
        for (std::size_t i=0;i<body.basinSamples.size();++i) {
            const auto sample=body.basinSamples[i];
            if (sample.x<0 || sample.y<0 ||
                std::int64_t(sample.x)*body.basinStep>std::int64_t(graph.macroWidth)*graph.macroCellMetres+body.basinStep ||
                std::int64_t(sample.y)*body.basinStep>std::int64_t(graph.macroHeight)*graph.macroCellMetres+body.basinStep)
                return refuse(why,"a natural basin sample is outside the world");
            if (i>0) {
                const auto previous=body.basinSamples[i-1];
                if (sample.y<previous.y || (sample.y==previous.y && sample.x<=previous.x))
                    return refuse(why,"natural basin samples are not unique and row-major");
            }
        }
        if (!sortedUniqueWithin(body.inlets, segments))
            return refuse(why, "water body inlets are unsorted, repeated or unknown");
        if (!sortedUniqueWithin(body.outlets, segments))
            return refuse(why, "water body outlets are unsorted, repeated or unknown");
        for (std::size_t c = 0; c < body.macroCells.size(); ++c) {
            const core::TilePos cell = body.macroCells[c];
            if (cell.x < 0 || cell.y < 0 ||
                (graph.macroWidth > 0 && (cell.x >= graph.macroWidth || cell.y >= graph.macroHeight)))
                return refuse(why, "water body reaches outside the macro map");
            // Row-major and strictly ascending, which is what makes the first
            // cell of a body its smallest index on every rebuild.
            if (c > 0) {
                const core::TilePos previous = body.macroCells[c - 1];
                if (cell.y < previous.y || (cell.y == previous.y && cell.x <= previous.x))
                    return refuse(why, "water body footprint is not in row-major order");
            }
        }
    }

    for (std::size_t p = 0; p < graph.spatialPages.size(); ++p) {
        const HydrologySpatialPage& page = graph.spatialPages[p];
        if (p > 0) {
            const TileKey previous = graph.spatialPages[p - 1].key;
            const bool ascending = previous.y < page.key.y ||
                                   (previous.y == page.key.y && previous.x < page.key.x) ||
                                   (previous.y == page.key.y && previous.x == page.key.x &&
                                    previous.level < page.key.level);
            if (!ascending) return refuse(why, "spatial pages are not sorted by (y, x)");
        }
        if (!sortedUniqueWithin(page.segments, segments))
            return refuse(why, "a page lists segments unsorted, repeated or unknown");
        if (!sortedUniqueWithin(page.waterBodies, bodies))
            return refuse(why, "a page lists water bodies unsorted, repeated or unknown");
    }

    if (why != nullptr) why->clear();
    return true;
}

const HydrologySpatialPage* findHydrologySpatialPage(const HydrologyGraph& graph, TileKey key) {
    // (y, x, level) is the build order, so a page lookup is a binary search and
    // never a rebuild of a side table on the frame thread.
    const auto before = [](const HydrologySpatialPage& page, TileKey wanted) {
        if (page.key.y != wanted.y) return page.key.y < wanted.y;
        if (page.key.x != wanted.x) return page.key.x < wanted.x;
        return page.key.level < wanted.level;
    };
    const auto it =
            std::lower_bound(graph.spatialPages.begin(), graph.spatialPages.end(), key, before);
    if (it == graph.spatialPages.end() || !(it->key == key)) return nullptr;
    return &*it;
}

} // namespace world::streaming
