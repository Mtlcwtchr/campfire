#pragma once
// GPU cut for the hierarchy around placed objects.
//
// The node bounds use the same 32-byte record as GeometryCluster. This is
// intentional: a hierarchy node is selected by the same conservative screen
// error rule as a mesh cluster, but its payload is an instance-arena offset and
// its bucket is a merged/canopy representation rather than a mesh cluster.

#include <cstddef>
#include <cstdint>
#include <span>

#include <SDL3/SDL.h>

#include "engine/render/geometry/cluster_cull.hpp"

namespace engine {

class RenderPipeline;
struct Frame;

class InstanceHierarchyCuller {
public:
    bool setup(Device& device, RenderPipeline& into, std::size_t nodeCapacity,
               std::size_t bucketCapacity, std::size_t buckets);
    bool describe(Device& device, std::span<const DrawArguments> draws);
    void nodes(std::span<const GeometryCluster> list);
    void dispatch(const Frame& frame, RenderPipeline& into, const ClusterSelection& selection,
                  bool compactOutput = false);
    void dispatchGpu(const Frame& frame, RenderPipeline& into, const ClusterSelection& selection,
                     SDL_GPUBuffer* input, std::size_t count, bool compactOutput = false);

    [[nodiscard]] SDL_GPUBuffer* visible() const { return culler_.visible(); }
    [[nodiscard]] SDL_GPUBuffer* arguments() const { return culler_.arguments(); }
    [[nodiscard]] std::size_t bucketCapacity() const { return culler_.bucketCapacity(); }
    [[nodiscard]] std::size_t buckets() const { return culler_.buckets(); }
    [[nodiscard]] std::size_t submitted() const { return culler_.submitted(); }
    [[nodiscard]] std::size_t dropped() const { return culler_.dropped(); }
    [[nodiscard]] bool ready() const { return culler_.ready(); }

private:
    ClusterCuller culler_;
};

} // namespace engine
