#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>
#include "engine/camera/view_state.hpp"

namespace engine::render {
enum class RepresentationKind { Geometry, Impostor, Aggregate, Refine, Cull };
struct RepresentationBounds { camera::Vec3 centre{}; double radius = 0; };
struct RepresentationCandidate {
    RepresentationKind kind = RepresentationKind::Geometry;
    bool resident = true;
    double errorMetres = 0;
    double estimatedCost = 1;
    camera::Vec3 bakedDirection{0, 0, 1};
    // Residual depth after reprojection. A flat card uses the full object depth.
    double residualDepth = 0;
    double angularErrorMetres = 0;
};
struct RepresentationMetrics {
    double projectedRadiusPx = 0, errorPx = 0, viewAngle = 0, parallaxErrorPx = 0;
};
inline constexpr std::size_t kNoRepresentation = std::numeric_limits<std::size_t>::max();
struct RepresentationDecision {
    RepresentationKind kind = RepresentationKind::Refine;
    std::size_t candidate = kNoRepresentation;
    RepresentationMetrics metrics;
    bool prefetchChildren = false;
    bool qualityExceeded = false;
    double residencySeconds = 0;
};
[[nodiscard]] RepresentationMetrics representationMetrics(const camera::ViewState& view,
    const RepresentationBounds& bounds, const RepresentationCandidate& candidate);
// Cost crossover applies only to resident, visually acceptable representations.
// Previous is a stable candidate index belonging to THIS node, never a global LOD.
[[nodiscard]] RepresentationDecision selectRepresentation(const camera::ViewState& view,
    const RepresentationBounds& bounds, std::span<const RepresentationCandidate> candidates,
    std::size_t previous = kNoRepresentation, bool hasChildren = false, bool allowCull = false);

struct RepresentationNode {
    RepresentationBounds bounds;
    std::vector<RepresentationCandidate> representations;
    std::vector<std::uint32_t> children;
};
struct RepresentationDraw { std::uint32_t node; RepresentationDecision decision; };
struct RepresentationCut {
    std::vector<RepresentationDraw> draws;
    std::vector<std::uint32_t> prefetch;
    bool complete = true;
};
// One traversal for geometry, individual impostors and recursively baked proxies.
// Invalid/cyclic hierarchies are rejected rather than silently dropping coverage.
[[nodiscard]] RepresentationCut selectRepresentations(const camera::ViewState& view,
    std::span<const RepresentationNode> nodes, std::span<const std::uint32_t> roots);
} // namespace engine::render

