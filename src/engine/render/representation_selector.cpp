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
namespace {
// What one instance's metrics share whatever the candidate: the projection,
// the direction to it and the parallax lever. Worked out once an instance,
// not once a candidate - each was a tangent, a square root or a normalisation,
// and the selector asks for several candidates of every visible instance.
struct Shared {
    double scale = 0, radius = 0, parallax = 0;
    camera::Vec3 direction{};
};
// What a view makes of every instance it is asked about - whether it is
// valid, its projection scale, its unit forward - worked out once a view, not
// once an instance: the selector is called for every visible instance of the
// frame with the same view, and pixelsPerMetre re-validated it, normalised
// its forward and took a tangent each time. Keyed on the fields those depend
// on, so a changed view is never answered from the old one.
struct Prepared {
    camera::Vec3 position{}, forward{}, unit{};
    double fovY = 0, nearPlane = 0, orthographicScale = 0;
    int viewportWidth = 0, viewportHeight = 0;
    bool orthographic = false, ok = false;
    double projection = 0;
    bool same(const camera::ViewState& v) const {
        return position == v.position && forward == v.forward && fovY == v.fovY && nearPlane == v.nearPlane &&
               orthographicScale == v.orthographicScale && viewportWidth == v.viewportWidth &&
               viewportHeight == v.viewportHeight && orthographic == v.orthographic;
    }
};
const Prepared& prepared(const camera::ViewState& view) {
    thread_local Prepared p;
    thread_local bool filled = false;
    if (filled && p.same(view)) return p;
    p.position = view.position; p.forward = view.forward; p.fovY = view.fovY; p.nearPlane = view.nearPlane;
    p.orthographicScale = view.orthographicScale; p.viewportWidth = view.viewportWidth;
    p.viewportHeight = view.viewportHeight; p.orthographic = view.orthographic;
    p.ok = view.valid();
    p.projection = view.projectionScale();
    p.unit = camera::normalized(view.forward);
    filled = true;
    return p;
}
// ViewState::pixelsPerMetre, from the prepared view (the same arithmetic).
double pixelsPerMetre(const Prepared& p, camera::Vec3 centre, double radius) {
    if (!p.ok) return 0;
    if (p.orthographic) return p.orthographicScale;
    const double depth = camera::dot(camera::subtract(centre, p.position), p.unit);
    if (depth + radius <= 0) return 0;
    return p.projection / std::max(p.nearPlane, depth - std::max(0.0, radius));
}
Shared shared(const camera::ViewState& view, const RepresentationBounds& bounds) {
    Shared s;
    const Prepared& p = prepared(view);
    s.scale = pixelsPerMetre(p, bounds.centre, bounds.radius);
    s.radius = bounds.radius;
    s.direction = view.directionFrom(bounds.centre);
    const double along = camera::dot(view.velocity, s.direction);
    const double lateral = std::sqrt(std::max(0.0, camera::dot(view.velocity, view.velocity) - along*along));
    if (!view.orthographic) {
        const double distance = std::max(view.nearPlane,
            camera::dot(camera::subtract(bounds.centre, view.position), p.unit) - bounds.radius);
        s.parallax = lateral * std::clamp(view.quality.lookaheadSeconds, 0.0, 2.0) * p.projection /
                     (distance * distance);
    }
    return s;
}
RepresentationMetrics metricsFrom(const Shared& s, const RepresentationCandidate& candidate) {
    RepresentationMetrics m;
    m.projectedRadiusPx = s.radius * s.scale;
    // 2 sin(angle / 2) between two unit vectors is the chord between them.
    const auto baked = camera::normalized(candidate.bakedDirection);
    const auto apart = camera::subtract(s.direction, baked);
    const double chord = std::min(2.0, std::sqrt(camera::dot(apart, apart)));
    m.viewAngle = 2.0 * std::asin(chord * 0.5);
    m.errorPx = (candidate.errorMetres + candidate.angularErrorMetres * chord) * s.scale;
    m.parallaxErrorPx = candidate.residualDepth * s.parallax;
    return m;
}
} // namespace

RepresentationMetrics representationMetrics(const camera::ViewState& view,
        const RepresentationBounds& bounds, const RepresentationCandidate& candidate) {
    return metricsFrom(shared(view, bounds), candidate);
}
RepresentationDecision selectRepresentation(const camera::ViewState& view,
        const RepresentationBounds& bounds, std::span<const RepresentationCandidate> candidates,
        std::size_t previous, bool hasChildren, bool allowCull) {
    RepresentationDecision result;
    const Prepared& ready = prepared(view);
    if (!ready.ok || !std::isfinite(bounds.radius) || bounds.radius < 0) {
        result.qualityExceeded = true; return result;
    }
    for (double x : bounds.centre) if (!std::isfinite(x)) { result.qualityExceeded = true; return result; }
    result.residencySeconds = std::clamp(view.quality.residencySeconds, 0.0, 10.0);
    const double scale = pixelsPerMetre(ready, bounds.centre, bounds.radius);
    if (scale == 0 || (allowCull && bounds.radius * 2.0 * scale < 0.5)) {
        result.kind = RepresentationKind::Cull; return result;
    }
    const double h = std::clamp(view.quality.hysteresis, 0.0, 0.45);
    const Shared here = shared(view, bounds);
    double cost = std::numeric_limits<double>::infinity();
    std::size_t fallback = kNoRepresentation;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& candidate = candidates[i];
        if (!valid(candidate)) continue;
        const auto metrics = metricsFrom(here, candidate);
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
            result.metrics = metricsFrom(here, candidates[fallback]);
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

