#pragma once
// GPU expansion of mesh roots into world-space cluster candidates.
//
// A root is one placed mesh, not one placed mesh multiplied by every cluster
// in that mesh. The card expands the immutable local cluster table and writes
// the same GeometryCluster record consumed by ClusterCuller. Keeping this
// boundary separate makes the producer replaceable: a future DAG/page walker
// can write the same candidate buffer without changing the cut or draw path.

#include <cstddef>
#include <cstdint>
#include <span>

#include <SDL3/SDL.h>

#include "engine/render/geometry/cluster_cull.hpp"

namespace engine {

class RenderPipeline;
struct Frame;

struct MeshRootInstance {
    float position[3]{0, 0, 0};
    float scale = 1;
    float yaw = 0;
    std::uint32_t clusterFirst = 0;
    std::uint32_t clusterCount = 0;
    std::uint32_t bucketBase = 0;
    std::uint32_t outputFirst = 0;
    std::uint32_t payload = 0;
    std::uint32_t familyFirst = 0;
};

struct MeshStaticCluster {
    GeometryCluster bounds;
    std::uint32_t bornOf = 0xffffffffu;
    std::uint32_t replacedBy = 0xffffffffu;
};

struct MeshFamilyRange {
    std::uint32_t childFirst = 0;
    std::uint32_t childCount = 0;
};

class MeshRootCuller {
public:
    bool setup(Device& device, RenderPipeline& into,
               std::span<const GeometryCluster> staticClusters,
               std::size_t maxRoots, std::size_t maxCandidates);
    bool setup(Device& device, RenderPipeline& into,
               std::span<const MeshStaticCluster> staticClusters,
               std::span<const MeshFamilyRange> families,
               std::span<const std::uint32_t> familyChildren,
               std::size_t maxRoots, std::size_t maxCandidates);

    // Upload one compact root record per object and queue the expansion.
    bool dispatch(const Frame& frame, RenderPipeline& into,
                  std::span<const MeshRootInstance> roots);
    bool dispatch(const Frame& frame, RenderPipeline& into,
                  std::span<const MeshRootInstance> roots,
                  const ClusterSelection& selection);

    [[nodiscard]] SDL_GPUBuffer* candidates() const { return candidates_.get(); }
    [[nodiscard]] std::size_t candidateCount() const { return candidateCount_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    ComputeSlot expand_ = 0;
    Buffer staticClusters_, roots_, candidates_;
    Buffer families_, familyChildren_;
    std::size_t maxRoots_ = 0, maxCandidates_ = 0, candidateCount_ = 0;
    bool ready_ = false, dag_ = false;
};

} // namespace engine
