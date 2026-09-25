#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include "engine/render/systems/cluster_draws.hpp"
#include "engine/render/systems/level_select.hpp"

namespace engine::render {
// Immutable interval index over [error, parentError). Linear storage; queries
// visit active intervals rather than scanning every level for every instance.
class SourceClusterIndex {
public:
    SourceClusterIndex() = default;
    explicit SourceClusterIndex(std::span<const geometry::MeshCluster> clusters);
    // +infinity selects the roots; negative and NaN allowances are invalid.
    void select(double allowance, std::vector<std::uint32_t>& result) const;
private:
    struct Node {
        std::uint32_t cluster = 0;
        int left = -1, right = -1;
        float lower = 0, upper = 0, subtreeUpper = 0;
    };
    std::vector<Node> nodes_;
    int root_ = -1;
    void visit(int node, double allowance, std::vector<std::uint32_t>& result) const;
};

struct SourceGeometry {
    std::span<const geometry::MeshCluster> clusters;
    const SourceClusterIndex* index = nullptr;
    std::uint32_t indexBase = 0;
    std::int32_t vertexBase = 0;
    MeshBounds bounds;
    // Upper bound on vertex animation displacement in MODEL metres.
    double deformation = 0;
    // Optional vertex base for cluster-local morph vertices. Zero keeps the
    // source mesh path compatible with the original shared vertex buffer.
    std::int32_t clusterVertexBase = 0;
};
struct SourceDrawPlan {
    DrawPlan plan;
    std::size_t selectedClusters = 0, culledClusters = 0;
};

// Each cluster bucket can receive every instance of its model, across ALL
// material/level batches. On false, select the complete CPU plan, never truncate.
[[nodiscard]] bool sourceFitsGpuCapacity(const GatheredInstances& instances,
                                        std::span<const SourceGeometry> assets,
                                        std::size_t candidateCapacity,
                                        std::size_t instancesPerCluster);

// One complete, crack-free cut per instance, bounding perspective division over
// its whole sphere (including off-axis depth displacement). `screen.focal` must
// bound pixels-per-metre-times-depth for BOTH projection axes; `screen.scale`
// must bound the orthographic projection's operator norm in pixels per metre.
// Cluster bounds are then independently culled; no depth occlusion is assumed.
// Instance order and attributes are never changed or duplicated. No impostor,
// shell, manual LOD chain or triangle-budget-induced quality reduction.
[[nodiscard]] SourceDrawPlan planSourceDraws(const GatheredInstances& instances,
                                            std::span<const SourceGeometry> assets,
                                            const ScreenScale& screen);
} // namespace engine::render
