#pragma once
// A hierarchy over placed mesh instances.
//
// MeshCluster is the hierarchy inside one object. InstanceHierarchy is the
// separate hierarchy around many objects: near keeps individual object ids,
// mid can point at merged geometry, far at a canopy proxy, and the final
// level at a density/shading proxy. The geometry behind those representations
// is supplied by the renderer; this module owns the deterministic spatial DAG
// and its screen-error cut.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::geometry {

enum class InstanceRepresentation : std::uint8_t {
    Individual,
    MergedGeometry,
    CanopyProxy,
    DensityShading,
};

struct InstanceSource {
    float position[3]{0, 0, 0};
    float scale = 1;
    float radius = 1;
    std::uint32_t variant = 0;
};

struct InstanceHierarchyNode {
    // A conservative sphere around this representation in world/model space.
    float centre[3]{0, 0, 0};
    float radius = 0;
    // The source instance ids represented by this node, in the hierarchy's
    // flattened member array. A merged/canopy/density renderer uses this list
    // to retain deterministic material and variant contributions.
    std::uint32_t memberFirst = 0;
    std::uint32_t memberCount = 0;
    float error = 0;
    float parentError = 0;
    std::uint32_t level = 0;
    InstanceRepresentation representation = InstanceRepresentation::Individual;
    std::uint32_t bornOf = kNoGroup;
    std::uint32_t replacedBy = kNoGroup;
    // Direct children in the explicit hierarchy adjacency. A group node owns
    // one contiguous range here; this is the GPU traversal contract and avoids
    // rebuilding children from family ids on every cut.
    std::uint32_t childFirst = 0;
    std::uint32_t childCount = 0;

    static constexpr std::uint32_t kNoGroup = 0xffffffffu;
};

struct InstanceHierarchyOptions {
    // Spatial buckets are deliberately explicit. They are content/build
    // settings, not frame heuristics, so the same world produces the same DAG.
    double midCellMetres = 8;
    double farCellMetres = 32;
    double veryFarCellMetres = 128;
    std::size_t maxMembersPerGroup = 16;
    // Approximation budgets for the three replacement representations.
    // They are model metres and are folded into the monotonic node error.
    double mergedErrorFraction = 0.05;
    double canopyErrorFraction = 0.15;
    double densityErrorFraction = 0.5;
};

struct InstanceHierarchy {
    std::vector<InstanceSource> sources;
    std::vector<std::uint32_t> members;
    std::vector<std::uint32_t> children;
    std::vector<InstanceHierarchyNode> nodes;
    std::size_t levels = 0;

    [[nodiscard]] std::span<const std::uint32_t> membersOf(
            const InstanceHierarchyNode& node) const {
        return std::span<const std::uint32_t>(members).subspan(node.memberFirst, node.memberCount);
    }
    [[nodiscard]] std::span<const std::uint32_t> childrenOf(
            const InstanceHierarchyNode& node) const {
        return std::span<const std::uint32_t>(children).subspan(node.childFirst, node.childCount);
    }
};

[[nodiscard]] InstanceHierarchy buildInstanceHierarchy(
        std::span<const InstanceSource> sources,
        const InstanceHierarchyOptions& options = {});

// Returns a complete one-node-per-branch cut. False means the hierarchy is
// absent or malformed and the caller should use its legacy individual path.
[[nodiscard]] bool cutAtInstanceHierarchy(
        std::span<const InstanceHierarchyNode> nodes, double allowance,
        std::vector<std::uint32_t>& chosen);

// Perspective-aware variant. `allowances[i]` is the world-space error that
// node i may spend at its own depth. A branch still resolves as one complete
// cut; only the fit test changes from one global threshold to a conservative
// per-node screen-space threshold.
[[nodiscard]] bool cutAtInstanceHierarchy(
        std::span<const InstanceHierarchyNode> nodes, std::span<const double> allowances,
        std::vector<std::uint32_t>& chosen);

// Adjacency-backed variant used by runtime/GPU clients. It walks the explicit
// child ranges instead of reconstructing families from bornOf/replacedBy.
[[nodiscard]] bool cutAtInstanceHierarchy(
        const InstanceHierarchy& hierarchy, std::span<const double> allowances,
        std::vector<std::uint32_t>& chosen);

} // namespace engine::geometry
