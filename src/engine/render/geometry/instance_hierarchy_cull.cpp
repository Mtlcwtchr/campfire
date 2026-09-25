#include "engine/render/geometry/instance_hierarchy_cull.hpp"

#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {

bool InstanceHierarchyCuller::setup(Device& device, RenderPipeline& into,
                                    std::size_t nodeCapacity, std::size_t bucketCapacity,
                                    std::size_t buckets) {
    return culler_.setup(device, into, nodeCapacity, bucketCapacity, buckets);
}

bool InstanceHierarchyCuller::describe(Device& device, std::span<const DrawArguments> draws) {
    return culler_.describe(device, draws);
}

void InstanceHierarchyCuller::nodes(std::span<const GeometryCluster> list) {
    culler_.clusters(list);
}

void InstanceHierarchyCuller::dispatch(const Frame& frame, RenderPipeline& into,
                                       const ClusterSelection& selection, bool compactOutput) {
    culler_.dispatch(frame, into, selection, compactOutput);
}

void InstanceHierarchyCuller::dispatchGpu(const Frame& frame, RenderPipeline& into,
                                           const ClusterSelection& selection,
                                           SDL_GPUBuffer* input, std::size_t count,
                                           bool compactOutput) {
    culler_.dispatchGpu(frame, into, selection, input, count, compactOutput);
}

} // namespace engine
