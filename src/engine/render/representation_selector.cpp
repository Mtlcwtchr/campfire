#include "engine/render/representation_selector.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine::render {
namespace {
bool valid(const RepresentationCandidate& c) {
    return (c.kind == RepresentationKind::Geometry || c.kind == RepresentationKind::Impostor ||
            c.kind == RepresentationKind::Aggregate) &&
        std::isfinite(c.estimatedCost) && c.estimatedCost >= 0 &&
        std::isfinite(c.errorMetres) && c.errorMetres >= 0 &&
        std::isfinite(c.residualDepth) && c.residualDepth >= 0 &&
        std::isfinite(c.angularErrorMetres) && c.angularErrorMetres >= 0;
}
bool accepts(const camera::ViewState& v, const RepresentationCandidate& c,
             const RepresentationMetrics& m, double factor) {
    const double error = c.kind == RepresentationKind::Geometry ? v.quality.geometryErrorPx : v.quality.impostorErrorPx;
    return m.errorPx <= error * factor && m.parallaxErrorPx <= v.quality.parallaxErrorPx * factor;
}
}
RepresentationMetrics representationMetrics(const camera::ViewState& view,
        const RepresentationBounds& bounds, const RepresentationCandidate& candidate) {
    RepresentationMetrics m;
    const double scale = view.pixelsPerMetre(bounds.centre, bounds.radius);
    m.projectedRadiusPx = bounds.radius * scale;
    m.viewAngle = std::acos(std::clamp(camera::dot(view.directionFrom(bounds.centre),
                            camera::normalized(candidate.bakedDirection)), -1.0, 1.0));
    m.errorPx = (candidate.errorMetres + candidate.angularErrorMetres *
                2.0 * std::sin(m.viewAngle * 0.5)) * scale;
    const auto direction = view.directionFrom(bounds.centre);
    const double along = camera::dot(view.velocity, direction);
    const double lateral = std::sqrt(std::max(0.0, camera::dot(view.velocity, view.velocity) - along*along));
    if (!view.orthographic) {
        const double distance = std::max(view.nearPlane,
            camera::dot(camera::subtract(bounds.centre, view.position), camera::normalized(view.forward)) - bounds.radius);
        m.parallaxErrorPx = candidate.residualDepth * lateral *
            std::clamp(view.quality.lookaheadSeconds, 0.0, 2.0) * view.projectionScale() / (distance * distance);
    }
    return m;
}
RepresentationDecision selectRepresentation(const camera::ViewState& view,
        const RepresentationBounds& bounds, std::span<const RepresentationCandidate> candidates,
        std::size_t previous, bool hasChildren, bool allowCull) {
    RepresentationDecision result;
    if (!view.valid() || !std::isfinite(bounds.radius) || bounds.radius < 0) {
        result.qualityExceeded = true; return result;
    }
    for (double x : bounds.centre) if (!std::isfinite(x)) { result.qualityExceeded = true; return result; }
    result.residencySeconds = std::clamp(view.quality.residencySeconds, 0.0, 10.0);
    const double scale = view.pixelsPerMetre(bounds.centre, bounds.radius);
    if (scale == 0 || (allowCull && bounds.radius * 2.0 * scale < 0.5)) {
        result.kind = RepresentationKind::Cull; return result;
    }
    const double h = std::clamp(view.quality.hysteresis, 0.0, 0.45);
    double cost = std::numeric_limits<double>::infinity();
    std::size_t fallback = kNoRepresentation;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (!valid(candidate)) continue;
        const auto metrics = representationMetrics(view, bounds, candidate);
        if (!candidate.resident) {
            if (accepts(view, candidate, metrics, 1.0)) result.prefetchChildren = true;
            continue;
        }
        if (candidate.kind == RepresentationKind::Geometry &&
            (fallback == kNoRepresentation || candidate.errorMetres < candidates[fallback].errorMetres)) fallback = i;
        const double factor = i == previous ? 1.0 + h : (previous == kNoRepresentation ? 1.0 : 1.0 - h);
        if (accepts(view, candidate, metrics, factor) && candidate.estimatedCost < cost) {
            result.candidate = i; result.kind = candidate.kind; result.metrics = metrics; cost = candidate.estimatedCost;
        }
    }
    if (result.candidate == kNoRepresentation) {
        if (!hasChildren && fallback != kNoRepresentation) {
            result.candidate = fallback; result.kind = RepresentationKind::Geometry;
            result.metrics = representationMetrics(view, bounds, candidates[fallback]);
            result.qualityExceeded = true;
        } else result.prefetchChildren = true;
    } else {
        auto predicted = view; predicted.position = view.predictedPosition();
        const auto& chosen = candidates[result.candidate];
        if (!accepts(predicted, chosen, representationMetrics(predicted, bounds, chosen), 1.0))
            result.prefetchChildren = true;
    }
    return result;
}
RepresentationCut selectRepresentations(const camera::ViewState& view,
        std::span<const RepresentationNode> nodes, std::span<const std::uint32_t> roots) {
    RepresentationCut result;
    std::vector<std::uint32_t> pending(roots.rbegin(), roots.rend());
    std::vector<bool> visited(nodes.size());
    while (!pending.empty()) {
        const auto id = pending.back(); pending.pop_back();
        if (id >= nodes.size() || visited[id]) throw std::invalid_argument("representation hierarchy is not a tree");
        visited[id] = true;
        const auto& node = nodes[id];
        const auto decision = selectRepresentation(view, node.bounds, node.representations,
            kNoRepresentation, !node.children.empty());
        if (decision.prefetchChildren) result.prefetch.insert(result.prefetch.end(), node.children.begin(), node.children.end());
        if (decision.kind == RepresentationKind::Refine) {
            if (node.children.empty()) result.complete = false;
            pending.insert(pending.end(), node.children.rbegin(), node.children.rend());
        } else if (decision.kind != RepresentationKind::Cull) result.draws.push_back({id, decision});
    }
    return result;
}
} // namespace engine::render

