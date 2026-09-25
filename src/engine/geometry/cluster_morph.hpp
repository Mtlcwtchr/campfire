#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"

namespace engine::geometry {

// A cluster-local vertex expansion for a geometric transition. One source
// vertex may need different targets in different replacement families, so the
// result intentionally duplicates vertices at cluster boundaries.
struct ClusterMorphAsset {
    // One source vertex id for every expanded vertex.
    std::vector<std::uint32_t> sourceVertices;
    // One replacement-surface position (three floats) per expanded vertex.
    std::vector<float> targetPositions;
    // Geometric normal of the replacement surface, parallel to targetPositions.
    // A zero triplet means the source normal is retained (root/no replacement).
    std::vector<float> targetNormals;
    // Indices are local to sourceVertices and preserve every MeshCluster range.
    std::vector<std::uint32_t> indices;

    [[nodiscard]] bool empty() const {
        return sourceVertices.empty() || targetPositions.size() != sourceVertices.size() * 3 ||
               indices.empty();
    }
};

// Expands a DAG's index ranges and computes the closest point on the concrete
// replacement-cluster surface for every child vertex. Root clusters target
// themselves and therefore have a zero morph interval.
[[nodiscard]] ClusterMorphAsset buildClusterMorph(
        std::span<const float> sourcePositions,
        std::span<const std::uint32_t> clusterIndices,
        std::span<const MeshCluster> clusters,
        std::span<const float> clusterPositions = {});

} // namespace engine::geometry
