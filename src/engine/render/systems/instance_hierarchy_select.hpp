#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/instance_hierarchy.hpp"

namespace engine::render {

// Kept independent from ECS/instance_gather so this selection layer can be
// used by GPU/render clients that do not link the world registry.
struct InstanceHierarchyScreen {
    float rowW[4]{};
    double focal = 0; // zero means orthographic, scale is pixels/metre
    double scale = 1;
};

struct InstanceHierarchySelection {
    // All selected nodes, sorted by node id for deterministic batching.
    std::vector<std::uint32_t> nodes;
    std::vector<std::uint32_t> individual;
    std::vector<std::uint32_t> merged;
    std::vector<std::uint32_t> canopy;
    // Density nodes do not submit mesh triangles. They are handed to the
    // terrain-integrated vegetation-density pass instead.
    std::vector<std::uint32_t> density;
    // Kept for diagnostics and for a later transition/dither pass: the exact
    // world-space allowance used for every hierarchy node.
    std::vector<double> allowances;
    bool fallbackToIndividual = false;
};

using InstanceRepresentationMask = std::uint32_t;
[[nodiscard]] constexpr InstanceRepresentationMask representationBit(
        geometry::InstanceRepresentation representation) {
    return InstanceRepresentationMask{1} << static_cast<std::uint32_t>(representation);
}
inline constexpr InstanceRepresentationMask kAllInstanceRepresentations =
        representationBit(geometry::InstanceRepresentation::Individual) |
        representationBit(geometry::InstanceRepresentation::MergedGeometry) |
        representationBit(geometry::InstanceRepresentation::CanopyProxy) |
        representationBit(geometry::InstanceRepresentation::DensityShading);

// Selects one complete cut from an instance hierarchy. The allowance for a
// node is derived from its own conservative sphere, so a near node cannot be
// simplified merely because a distant sibling made a global threshold fit.
// A representation absent from `available` is never selected: traversal keeps
// descending until it finds an available child, preserving complete coverage.
[[nodiscard]] InstanceHierarchySelection selectInstanceHierarchy(
        const geometry::InstanceHierarchy& hierarchy, const InstanceHierarchyScreen& screen,
        double pixelError = 1.0,
        InstanceRepresentationMask available = kAllInstanceRepresentations);

// Fraction of a selected replacement node that may be shown during the
// crossover from its finer members. The value is monotonic in the node's
// world-space allowance and is intended for complementary screen-door clips:
// the replacement uses this value, while its finer members use one minus it.
[[nodiscard]] double instanceHierarchyReplacementWeight(
        const geometry::InstanceHierarchyNode& node, double allowance);

} // namespace engine::render
