#pragma once
// Building the cluster DAG that virtual geometry selects from.
//
// The runtime half already exists: engine::ClusterCuller draws a cluster when
// its own projected error fits the allowance and its parent's does not, which
// picks exactly one level along every path. What has been missing is anything
// that produces such clusters from a mesh. This is that.
//
// The build is the one Nanite describes, and every step of it is there for a
// reason worth stating:
//
//   1. Cut the triangles into clusters of about 128.
//   2. Gather neighbouring clusters into groups of about four.
//   3. Simplify each GROUP as one mesh, with the group's outer boundary LOCKED.
//   4. Cut the simplified group back into clusters of about 128.
//   5. Repeat until no group can simplify or the storage limit is reached.
//
// Step 3 is the whole thing. Because a group's outer boundary never moves, the
// coarse clusters that come out of a group meet their neighbours along exactly
// the geometry the fine ones did - so a cut that takes some clusters from one
// level and some from another is watertight BY CONSTRUCTION, not by a tolerance
// or a skirt. Locking the boundary of each cluster instead would freeze most of
// the mesh after two levels and stop the chain dead; locking nothing would open
// a crack at every cluster seam. Groups are what buys movement in the middle
// while the edges stay put.
//
// Errors never decrease towards the root, which is what makes the cut rule pick
// one level: if a cluster is drawn, every cluster below it has a parent error no
// larger, so none of them is drawn as well.
//
// The full-source path includes alpha-cut foliage. Disconnected cards whose
// boundaries cannot simplify remain exact roots, regardless of triangle cost.
// Legacy solid-only/shell assets are separate clients of this builder.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/smart_mesh.hpp"

namespace engine::geometry {

// One cluster: a run of the built index buffer, its bound, and the two errors
// the cut rule compares. Convert to engine::GeometryCluster to hand it to the
// card; this header knows nothing about a GPU.
struct MeshCluster {
    IndexRange indices;
    float centre[3]{0, 0, 0};
    float radius = 0;
    // How far this cluster's surface may sit from the original, in model units.
    // Zero at the finest level, which is the original.
    float error = 0;
    // The error of the group that replaces this cluster. Infinity when nothing
    // does, which is what makes a root draw whenever it is reached at all.
    float parentError = 0;
    std::uint32_t level = 0;
    // Which simplification produced this cluster, and which one replaces it.
    // Carried for tests and tools: the seam guarantee is a statement about
    // groups, and a failure is far easier to read with the group named.
    std::uint32_t bornOf = kNoGroup;
    std::uint32_t replacedBy = kNoGroup;

    static constexpr std::uint32_t kNoGroup = 0xffffffffu;
};

struct ClusterDagOptions {
    // Triangles per cluster. 128 is the number the whole approach is tuned
    // around: enough that a draw is worth making, few enough that a cluster is
    // a local decision.
    std::size_t clusterTriangles = 128;
    // Clusters per group. Fewer freezes too much boundary; more makes the
    // locked boundary a smaller fraction but couples distant geometry.
    std::size_t groupClusters = 4;
    // What fraction of a group's triangles survive one simplification.
    double survival = 0.5;
    // A storage limit, not a target. The last stored clusters stay roots even
    // if further simplification would be possible.
    std::size_t maxLevels = 24;
    // Keep importer's splits (UVs, materials, normals). Such seams are not
    // welded onto an arbitrary source vertex; the simplifier may still cross
    // a UV/normal split when it is geometrically safe.
    bool preserveSourceVertices = false;
    // Optional exact attribute identity for position welding. Vertices at the
    // same position may be welded only when this key also matches; this lets
    // regular assets keep smooth/shared vertices while preserving UV, normal,
    // colour and material seams.
    std::span<const std::uint64_t> attributeKeys{};
    // Optional hard material identity, one key per input vertex. A geometric
    // edge with multiple attribute copies is locked only when these material
    // keys disagree; UV and normal splits remain collapsible.
    std::span<const std::uint64_t> hardBoundaryKeys{};
    // Use conservative displacement plus a bidirectional sampled surface
    // distance, not just distance to face planes. Plane quadrics alone report
    // zero for destructive coplanar collapses and do not bound silhouette loss.
    bool conservativeError = false;
};

struct ClusterDag {
    // Welded vertices: the build needs shared positions to have shared indices,
    // or a seam in the index buffer becomes a crack in the geometry.
    std::vector<float> positions;             // three per vertex
    // For each welded vertex, one index into the caller's original vertex array,
    // so normals, UVs and colours can be rebuilt.
    std::vector<std::uint32_t> sourceVertex;
    // Every cluster of every level, one after another; a cluster's range points
    // in here. Indices address `positions` / `sourceVertex`.
    std::vector<std::uint32_t> indices;
    std::vector<MeshCluster> clusters;
    std::size_t levels = 0;
    // True when no remaining group could simplify; false at the level limit.
    // A mesh that stalls is not a bug: preserving source boundaries can leave
    // many exact roots, particularly with disconnected leaf cards.
    bool converged = false;
    // Triangles the input named twice, or that collapsed to a line once welded.
    std::size_t degenerate = 0;
    // Clusters whose error equals the error of what replaces them.
    //
    // Not a fault: it says the coarser representation is no further from the
    // original surface than this one and costs fewer triangles, so this one is
    // dominated and the cut rule correctly never draws it. Counted because a
    // build with many of them is carrying geometry nobody will ever see, which
    // is a thing to fix in the source mesh rather than in the rule.
    std::size_t dominated = 0;

    [[nodiscard]] std::size_t trianglesAt(std::size_t level) const;
};

// Which vertices are the same point: original index to a shared number.
//
// Exported because anything that reasons about the TOPOLOGY of a mesh - what is
// connected to what, which pieces are separate - has to weld the same way the
// build does, or the two disagree about where a mesh ends.
[[nodiscard]] std::vector<std::uint32_t> weldPositions(std::span<const float> positions);

[[nodiscard]] std::vector<std::uint32_t> weldPositions(
        std::span<const float> positions, std::span<const std::uint64_t> attributeKeys);

[[nodiscard]] ClusterDag buildClusterDag(std::span<const float> positions,
                                         std::span<const std::uint32_t> indices,
                                         const ClusterDagOptions& options = {});

// The largest distance from any sampled point of `samples` to the nearest point
// of the `target` surface, both given as triangle index lists over `positions`.
// Each triangle contributes its three corners and its centroid.
//
// Exported because the measured error a level publishes rests entirely on this
// number, and it is reached through a spatial grid rather than by comparing
// every pair. An acceleration that returned merely a near triangle instead of
// the nearest one would shrink a published error without anything failing, and
// a level would then be selected in place of geometry it does not resemble.
// A test can only pin that against a plain scan if it can call this directly.
[[nodiscard]] double surfaceDeviation(std::span<const float> positions,
                                      std::span<const std::uint32_t> samples,
                                      std::span<const std::uint32_t> target);

// The clusters a given allowance selects, by the same rule the shader states:
// own error fits, the replacement's does not. Here so that a test, a tool and
// the card cannot each have their own idea of what a cut is.
[[nodiscard]] std::vector<std::uint32_t> cutAt(const ClusterDag& dag, double allowance);

// Select a complete cut from serialized cluster family metadata. Returns true
// when the metadata is present and internally usable; false means the caller
// should use a legacy interval cut for SCC3/SCC4 assets that predate explicit
// parent links.
[[nodiscard]] bool cutAtHierarchy(std::span<const MeshCluster> clusters, double allowance,
                                  std::vector<std::uint32_t>& chosen);

} // namespace engine::geometry
