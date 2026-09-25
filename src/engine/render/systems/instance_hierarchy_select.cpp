#include "engine/render/systems/instance_hierarchy_select.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::render {
namespace {
double row(const float r[4], const float p[3]) {
    return double(r[0]) * p[0] + double(r[1]) * p[1] + double(r[2]) * p[2] + double(r[3]);
}

double nodeAllowance(const geometry::InstanceHierarchyNode& node,
                     const InstanceHierarchyScreen& screen,
                     double pixelError) {
    if (!(pixelError > 0) || !std::isfinite(pixelError)) return 0;
    if (screen.focal <= 0) {
        return screen.scale > 0 && std::isfinite(screen.scale)
                ? pixelError / screen.scale : 0;
    }
    const double depth = row(screen.rowW, node.centre);
    const double rowLength = std::hypot(std::hypot(double(screen.rowW[0]),
                                                    double(screen.rowW[1])),
                                        double(screen.rowW[2]));
    // The nearest point of the node's sphere is the worst perspective scale.
    // This is conservative even for a rotated/non-unit clip row.
    const double nearest = depth - rowLength * node.radius;
    const double pixelsPerMetre = screen.focal / std::max(nearest, 1e-3);
    return pixelsPerMetre > 0 && std::isfinite(pixelsPerMetre)
            ? pixelError / pixelsPerMetre : 0;
}
} // namespace

double instanceHierarchyReplacementWeight(
        const geometry::InstanceHierarchyNode& node, double allowance) {
    if (!std::isfinite(allowance) || allowance < 0 ||
        !std::isfinite(node.error) || node.error < 0)
        return 0;
    if (!std::isfinite(node.parentError)) {
        if (!(node.error > 0)) return 1;
        const double transition = std::max(double(node.error) * 0.25, 1e-6);
        const double t = std::clamp((allowance - double(node.error)) / transition, 0.0, 1.0);
        return t * t * (3.0 - 2.0 * t);
    }
    if (!(node.parentError > node.error)) return allowance >= node.parentError ? 1 : 0;
    const double t = std::clamp((allowance - double(node.error)) /
                                (double(node.parentError) - double(node.error)), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

InstanceHierarchySelection selectInstanceHierarchy(
        const geometry::InstanceHierarchy& hierarchy, const InstanceHierarchyScreen& screen,
        double pixelError, InstanceRepresentationMask available) {
    InstanceHierarchySelection result;
    result.allowances.resize(hierarchy.nodes.size());
    for (std::size_t i = 0; i < hierarchy.nodes.size(); ++i)
        result.allowances[i] = nodeAllowance(hierarchy.nodes[i], screen, pixelError);

    std::vector<std::uint32_t> chosen;
    const geometry::InstanceHierarchy* selectable = &hierarchy;
    geometry::InstanceHierarchy filtered;
    if (available != kAllInstanceRepresentations) {
        filtered = hierarchy;
        for (auto& node : filtered.nodes)
            if (!(available & representationBit(node.representation)))
                node.error = std::numeric_limits<float>::infinity();
        selectable = &filtered;
    }
    if (!geometry::cutAtInstanceHierarchy(*selectable, result.allowances, chosen)) {
        result.fallbackToIndividual = true;
        for (std::uint32_t i = 0; i < hierarchy.nodes.size(); ++i)
            if (hierarchy.nodes[i].representation == geometry::InstanceRepresentation::Individual)
                chosen.push_back(i);
    }
    std::sort(chosen.begin(), chosen.end());
    result.nodes = chosen;
    for (const auto id : chosen) {
        switch (hierarchy.nodes[id].representation) {
        case geometry::InstanceRepresentation::Individual: result.individual.push_back(id); break;
        case geometry::InstanceRepresentation::MergedGeometry: result.merged.push_back(id); break;
        case geometry::InstanceRepresentation::CanopyProxy: result.canopy.push_back(id); break;
        case geometry::InstanceRepresentation::DensityShading: result.density.push_back(id); break;
        }
    }
    return result;
}

} // namespace engine::render
