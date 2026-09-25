#include "engine/geometry/instance_hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <functional>

namespace engine::geometry {
namespace {

struct Cell {
    std::int64_t x = 0, y = 0, z = 0;
    auto operator<=>(const Cell&) const = default;
};

float upperFloat(double value) {
    const float rounded = float(value);
    return double(rounded) < value
        ? std::nextafter(rounded, std::numeric_limits<float>::infinity()) : rounded;
}

Cell cellOf(const float* position, double width) {
    return {std::int64_t(std::floor(double(position[0]) / width)),
            std::int64_t(std::floor(double(position[1]) / width)),
            std::int64_t(std::floor(double(position[2]) / width))};
}

double distance(const float* a, const float* b) {
    const double x = double(a[0]) - b[0], y = double(a[1]) - b[1], z = double(a[2]) - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

void boundOf(const InstanceHierarchy& hierarchy, std::span<const std::uint32_t> ids,
             InstanceHierarchyNode& node) {
    if (ids.empty()) return;
    double centre[3]{0, 0, 0};
    for (const auto id : ids) {
        const auto& child = hierarchy.nodes[id];
        for (int axis = 0; axis < 3; ++axis) centre[axis] += child.centre[axis];
    }
    for (int axis = 0; axis < 3; ++axis) node.centre[axis] = float(centre[axis] / ids.size());
    double radius = 0;
    for (const auto id : ids)
        radius = std::max(radius, distance(node.centre, hierarchy.nodes[id].centre) +
                                   hierarchy.nodes[id].radius);
    node.radius = upperFloat(radius);
}

InstanceRepresentation representationFor(std::size_t stage) {
    if (stage == 0) return InstanceRepresentation::MergedGeometry;
    if (stage == 1) return InstanceRepresentation::CanopyProxy;
    return InstanceRepresentation::DensityShading;
}

double errorFraction(const InstanceHierarchyOptions& options, std::size_t stage) {
    if (stage == 0) return options.mergedErrorFraction;
    if (stage == 1) return options.canopyErrorFraction;
    return options.densityErrorFraction;
}

} // namespace

InstanceHierarchy buildInstanceHierarchy(std::span<const InstanceSource> sources,
                                          const InstanceHierarchyOptions& options) {
    InstanceHierarchy hierarchy;
    if (sources.empty() || !(options.midCellMetres > 0) || !(options.farCellMetres > 0) ||
        !(options.veryFarCellMetres > 0) || options.maxMembersPerGroup < 2 ||
        !(options.mergedErrorFraction >= 0 && options.canopyErrorFraction >= 0 &&
          options.densityErrorFraction >= 0) ||
        !std::isfinite(options.midCellMetres + options.farCellMetres +
                       options.veryFarCellMetres + options.mergedErrorFraction +
                       options.canopyErrorFraction + options.densityErrorFraction))
        return hierarchy;

    hierarchy.sources.assign(sources.begin(), sources.end());
    hierarchy.nodes.reserve(sources.size() * 2);
    hierarchy.members.reserve(sources.size() * 2);
    hierarchy.children.reserve(sources.size() * 2);
    std::vector<std::uint32_t> active;
    active.reserve(sources.size());
    for (std::uint32_t id = 0; id < sources.size(); ++id) {
        const auto& source = sources[id];
        if (!std::isfinite(source.position[0]) || !std::isfinite(source.position[1]) ||
            !std::isfinite(source.position[2]) || !(source.scale > 0) ||
            !std::isfinite(source.scale) || !(source.radius > 0) ||
            !std::isfinite(source.radius))
            return {};
        InstanceHierarchyNode node;
        for (int axis = 0; axis < 3; ++axis) node.centre[axis] = source.position[axis];
        node.radius = upperFloat(double(source.radius) * source.scale);
        node.memberFirst = std::uint32_t(hierarchy.members.size());
        node.memberCount = 1;
        hierarchy.members.push_back(id);
        node.error = 0;
        node.parentError = std::numeric_limits<float>::infinity();
        node.level = 0;
        node.representation = InstanceRepresentation::Individual;
        active.push_back(std::uint32_t(hierarchy.nodes.size()));
        hierarchy.nodes.push_back(node);
    }

    const double cells[]{options.midCellMetres, options.farCellMetres,
                         options.veryFarCellMetres};
    for (std::size_t stage = 0; stage < 3 && !active.empty(); ++stage) {
        std::map<Cell, std::vector<std::uint32_t>> buckets;
        for (const auto id : active) buckets[cellOf(hierarchy.nodes[id].centre, cells[stage])].push_back(id);
        std::vector<std::uint32_t> next;
        next.reserve(active.size());
        bool madeParent = false;
        for (auto& [cell, bucket] : buckets) {
            (void)cell;
            std::sort(bucket.begin(), bucket.end());
            for (std::size_t first = 0; first < bucket.size();) {
                const std::size_t count = std::min(options.maxMembersPerGroup, bucket.size() - first);
                // A singleton can become density shading at the final stage,
                // but it must not become a canopy shell: the shell asset is a
                // merged group representation and drawing a nine-tree shell
                // for one source creates a large false silhouette. A bucket
                // with one child is still valid when that child already
                // represents multiple sources.
                std::size_t represented = 0;
                for (std::size_t i = 0; i < count; ++i)
                    represented += hierarchy.nodes[bucket[first + i]].memberCount;
                const bool promoteSingleton = stage == 2 && represented > 0;
                if (count < 2 && !promoteSingleton) {
                    next.push_back(bucket[first]);
                    ++first;
                    continue;
                }
                const auto group = std::uint32_t(hierarchy.nodes.size());
                const auto family = group;
                InstanceHierarchyNode parent;
                parent.level = std::uint32_t(stage + 1);
                parent.representation = representationFor(stage);
                parent.bornOf = family;
                parent.childFirst = std::uint32_t(hierarchy.children.size());
                parent.childCount = std::uint32_t(count);
                hierarchy.children.insert(hierarchy.children.end(), bucket.begin() +
                    static_cast<std::ptrdiff_t>(first), bucket.begin() +
                    static_cast<std::ptrdiff_t>(first + count));
                parent.memberFirst = std::uint32_t(hierarchy.members.size());
                for (std::size_t i = 0; i < count; ++i) {
                    const auto childId = bucket[first + i];
                    auto& child = hierarchy.nodes[childId];
                    child.replacedBy = family;
                    const auto childMembers = hierarchy.membersOf(child);
                    hierarchy.members.insert(hierarchy.members.end(), childMembers.begin(), childMembers.end());
                }
                parent.memberCount = std::uint32_t(hierarchy.members.size()) - parent.memberFirst;
                boundOf(hierarchy, std::span<const std::uint32_t>(bucket).subspan(first, count), parent);
                double childError = 0;
                for (std::size_t i = 0; i < count; ++i)
                    childError = std::max(childError, double(hierarchy.nodes[bucket[first + i]].error));
                parent.error = upperFloat(childError + parent.radius * errorFraction(options, stage));
                parent.parentError = std::numeric_limits<float>::infinity();
                for (std::size_t i = 0; i < count; ++i) {
                    auto& child = hierarchy.nodes[bucket[first + i]];
                    child.parentError = parent.error;
                    child.replacedBy = family;
                }
                hierarchy.nodes.push_back(parent);
                next.push_back(group);
                madeParent = true;
                first += count;
            }
        }
        active = std::move(next);
        if (!madeParent && stage + 1 >= 3) break;
        // A sparse source may have no valid merged group at the mid stage, but
        // it still needs to reach the final density representation. Advancing
        // the spatial stage with the unchanged active leaves preserves that
        // path without inventing a canopy shell for a singleton.
        hierarchy.levels = std::max(hierarchy.levels, stage + 2);
    }
    return hierarchy;
}

namespace {
template <class Allowance>
bool cutAtInstanceHierarchyImpl(std::span<const InstanceHierarchyNode> nodes,
                                Allowance&& allowanceFor,
                                std::vector<std::uint32_t>& chosen) {
    chosen.clear();
    if (nodes.empty()) return false;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> children;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;
    for (std::uint32_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].replacedBy != InstanceHierarchyNode::kNoGroup)
            children[nodes[i].replacedBy].push_back(i);
        if (nodes[i].bornOf != InstanceHierarchyNode::kNoGroup)
            parents[nodes[i].bornOf].push_back(i);
    }
    if (children.empty()) return false;
    for (const auto& [family, ids] : children)
        if (ids.empty() || !parents.count(family) || parents[family].empty()) return false;
    for (const auto& [family, ids] : parents)
        if (ids.empty() || !children.count(family) || children[family].empty()) return false;

    std::vector<std::uint32_t> roots;
    for (std::uint32_t i = 0; i < nodes.size(); ++i)
        if (std::isinf(nodes[i].parentError)) roots.push_back(i);
    if (roots.empty()) return false;

    std::unordered_set<std::uint32_t> expanded, visiting;
    const auto visitFamily = [&](auto&& self, std::uint32_t family) -> bool {
        if (expanded.count(family)) return true;
        if (!visiting.insert(family).second) return false;
        const auto it = children.find(family);
        if (it == children.end() || it->second.empty()) return false;
        for (const auto childId : it->second) {
            const auto& child = nodes[childId];
            if (child.error <= allowanceFor(childId)) {
                chosen.push_back(childId);
                continue;
            }
            if (child.bornOf == InstanceHierarchyNode::kNoGroup ||
                !self(self, child.bornOf)) {
                visiting.erase(family);
                return false;
            }
        }
        visiting.erase(family);
        expanded.insert(family);
        return true;
    };
    for (const auto root : roots) {
        if (nodes[root].error <= allowanceFor(root)) {
            chosen.push_back(root);
        } else if (nodes[root].bornOf == InstanceHierarchyNode::kNoGroup ||
                   !visitFamily(visitFamily, nodes[root].bornOf)) {
            chosen.clear();
            return false;
        }
    }
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    return true;
}
} // namespace

bool cutAtInstanceHierarchy(std::span<const InstanceHierarchyNode> nodes, double allowance,
                            std::vector<std::uint32_t>& chosen) {
    if (!std::isfinite(allowance) || allowance < 0) {
        chosen.clear();
        return false;
    }
    return cutAtInstanceHierarchyImpl(nodes, [allowance](std::uint32_t) { return allowance; }, chosen);
}

bool cutAtInstanceHierarchy(std::span<const InstanceHierarchyNode> nodes,
                            std::span<const double> allowances,
                            std::vector<std::uint32_t>& chosen) {
    if (allowances.size() != nodes.size() ||
        !std::all_of(allowances.begin(), allowances.end(),
                     [](double value) { return std::isfinite(value) && value >= 0; })) {
        chosen.clear();
        return false;
    }
    return cutAtInstanceHierarchyImpl(nodes, [&](std::uint32_t id) { return allowances[id]; }, chosen);
}

bool cutAtInstanceHierarchy(const InstanceHierarchy& hierarchy,
                            std::span<const double> allowances,
                            std::vector<std::uint32_t>& chosen) {
    chosen.clear();
    if (allowances.size() != hierarchy.nodes.size() || hierarchy.nodes.empty() ||
        !std::all_of(allowances.begin(), allowances.end(),
                     [](double value) { return std::isfinite(value) && value >= 0; }))
        return false;
    std::vector<std::uint8_t> state(hierarchy.nodes.size(), 0);
    std::function<bool(std::uint32_t)> visit = [&](std::uint32_t id) {
        if (id >= hierarchy.nodes.size() || state[id] == 1) return false;
        if (state[id] == 2) return true;
        state[id] = 1;
        const auto& node = hierarchy.nodes[id];
        if (node.error <= allowances[id]) {
            chosen.push_back(id);
        } else {
            if (!node.childCount) return false;
            for (const auto child : hierarchy.childrenOf(node))
                if (!visit(child)) return false;
        }
        state[id] = 2;
        return true;
    };
    bool root = false;
    for (std::uint32_t id = 0; id < hierarchy.nodes.size(); ++id) {
        if (!std::isinf(hierarchy.nodes[id].parentError)) continue;
        root = true;
        if (!visit(id)) {
            chosen.clear();
            return false;
        }
    }
    if (!root) {
        chosen.clear();
        return false;
    }
    std::sort(chosen.begin(), chosen.end());
    chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());
    return !chosen.empty();
}

} // namespace engine::geometry
