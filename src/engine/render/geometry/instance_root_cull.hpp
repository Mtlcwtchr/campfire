#pragma once
// GPU producer for instance-hierarchy replacement roots.
//
// The hierarchy builder decides which replacement records exist; this module
// owns the GPU representation boundary. One record per replacement root is
// uploaded, and the card writes the GeometryCluster consumed by the shared
// hierarchy cut. Child traversal will replace this producer later without
// changing the culler or gather contract.

#include <cstddef>
#include <cstdint>
#include <span>

#include <SDL3/SDL.h>

#include "engine/render/geometry/cluster_cull.hpp"

namespace engine {

class RenderPipeline;
struct Frame;

struct InstanceHierarchyRoot {
    float position[3]{0, 0, 0};
    float radius = 0;
    float error = 0;
    float parentError = 0;
    std::uint32_t bucket = 0;
    std::uint32_t payload = 0;
};

struct InstanceHierarchyStaticNode {
    GeometryCluster bounds;
    std::uint32_t childFirst = 0;
    std::uint32_t childCount = 0;
    std::uint32_t payload = 0;
};

struct InstanceHierarchyTraversalRoot {
    std::uint32_t nodeFirst = 0;
    std::uint32_t nodeCount = 0;
    std::uint32_t outputFirst = 0;
};

class InstanceRootCuller {
public:
    bool setup(Device& device, RenderPipeline& into, std::size_t maxRoots);
    bool dispatch(const Frame& frame, RenderPipeline& into,
                  std::span<const InstanceHierarchyRoot> roots);

    bool setupHierarchy(Device& device, RenderPipeline& into,
                        std::size_t maxNodes, std::size_t maxRoots,
                        std::size_t maxCandidates);
    bool dispatchHierarchy(const Frame& frame, RenderPipeline& into,
                           std::span<const InstanceHierarchyStaticNode> nodes,
                           std::span<const std::uint32_t> children,
                           std::span<const InstanceHierarchyTraversalRoot> roots,
                           const ClusterSelection& selection);

    [[nodiscard]] SDL_GPUBuffer* candidates() const { return candidates_.get(); }
    [[nodiscard]] SDL_GPUBuffer* hierarchyCandidates() const { return hierarchyCandidates_.get(); }
    [[nodiscard]] std::size_t candidateCount() const { return candidateCount_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    ComputeSlot expand_ = 0;
    Buffer roots_, candidates_;
    std::size_t maxRoots_ = 0, candidateCount_ = 0;
    bool ready_ = false;
    ComputeSlot hierarchyExpand_ = 0;
    Buffer hierarchyNodes_, hierarchyChildren_, hierarchyRoots_, hierarchyCandidates_;
    std::size_t maxHierarchyNodes_ = 0, maxHierarchyRoots_ = 0, maxHierarchyCandidates_ = 0;
    bool hierarchyReady_ = false;
};

} // namespace engine
