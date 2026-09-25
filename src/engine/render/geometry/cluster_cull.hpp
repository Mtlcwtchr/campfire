#pragma once
// The selection step of the virtual geometry layer, on the card.
//
// A frame hands over every cluster that could be drawn and gets back, without
// any of it returning to the CPU, the ones that survived and the arguments of
// the draws that read them. That is the whole point: the per-object work of
// culling and choosing detail belongs where the objects are, and a list that
// travels to the CPU and back is a frame waiting on itself.
//
// The criterion is a cut rather than a threshold - a cluster is drawn when its
// own projected error fits the allowance and its parent's does not - so one
// level is chosen along every path of a hierarchy, with no gaps and no overlap.
// A flat list of instances is the same rule with an infinite parent error, so
// the scatter path and the hierarchy path cannot drift apart.
//
// This owns the buffers and the pipelines; a pass owns one of these and fills
// the cluster list. See doc/virtual_geometry_architecture.md.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/pipeline/ids.hpp"
#include "engine/render/draw_arguments.hpp"
#include "engine/render/device.hpp"

namespace engine {

class RenderPipeline;
struct Frame;

// One cluster as the culler sees it. Must match `Cluster` in cluster_cull.hlsl.
struct GeometryCluster {
    float centre[3]{0, 0, 0};
    float radius = 0;
    float error = 0;
    // Of the coarser cluster this one refines. Infinity for a root, and for
    // every member of a flat list, which is what makes the same rule select
    // from both.
    float parentError = 0;
    std::uint32_t bucket = 0;
    std::uint32_t payload = 0;
};
static_assert(sizeof(GeometryCluster) == 32, "the shader reads this layout");

// How a frame wants the selection done.
struct ClusterSelection {
    // Rows 0, 1 and 3 of the view-projection, in the row-major order the rest
    // of the renderer uses.
    float rowX[4]{};
    float rowY[4]{};
    float rowW[4]{};
    float rowZ[4]{};
    // How far a cluster's shape may move on screen before a finer one is worth
    // drawing. The same allowance the offline chains are measured against.
    float pixelError = 1;
    // Half the viewport, and the focal term: zero says the frame is
    // orthographic and `halfWidth` already is the scale.
    float halfWidth = 1;
    float halfHeight = 1;
    float focal = 0;
    // Previous-frame max-depth pyramid. Null/zero levels disables occlusion;
    // the culler still binds its 1-float fallback buffer so the shader layout
    // is identical on the first frame and after a resize.
    SDL_GPUBuffer* hiz = nullptr;
    std::uint32_t hizWidth = 0, hizHeight = 0, hizLevels = 0;
};

class ClusterCuller {
public:
    static constexpr std::uint32_t kPageMissing = 0;
    static constexpr std::uint32_t kPageResident = 1;
    static constexpr std::uint32_t kPageFallback = 2;

    // `buckets` draws, each able to hold `bucketCapacity` visible clusters, and
    // room for `clusterCapacity` clusters to choose from. Fixed at setup,
    // because a budget that grows when a frame is busy is not a budget.
    bool setup(Device& device, RenderPipeline& into, std::size_t clusterCapacity,
               std::size_t bucketCapacity, std::size_t buckets);

    // What each bucket draws: index count, first index, vertex offset. Uploaded
    // once; the culler only ever writes the instance count.
    bool describe(Device& device, std::span<const DrawArguments> draws);

    // One entry per draw bucket/geometry page. Missing pages request feedback
    // and are skipped; fallback entries bypass the normal cut and draw the
    // resident parent frontier. All buckets start resident for compatibility.
    bool updatePageTable(Device& device, std::span<const std::uint32_t> states);
    bool readPageFeedback(Device& device, std::span<std::uint32_t> into) const;

    // This frame's clusters. Anything past the capacity is dropped and counted.
    void clusters(std::span<const GeometryCluster> list);

    // Queue the reset and the cull. Call from a pass's collect().
    void dispatch(const Frame& frame, RenderPipeline& into, const ClusterSelection& selection,
                  bool compactOutput = false);
    // A preceding compute job may build the list without a CPU round trip.
    // Input must have COMPUTE_STORAGE_READ usage and remain alive through run().
    void dispatchGpu(const Frame& frame, RenderPipeline& into, const ClusterSelection& selection,
                     SDL_GPUBuffer* input, std::size_t count, bool compactOutput = false);

    [[nodiscard]] SDL_GPUBuffer* visible() const { return visible_.get(); }
    [[nodiscard]] SDL_GPUBuffer* arguments() const { return arguments_.get(); }
    [[nodiscard]] SDL_GPUBuffer* pageTable() const { return pageTable_.get(); }
    [[nodiscard]] SDL_GPUBuffer* pageFeedback() const { return pageFeedback_.get(); }
    [[nodiscard]] std::size_t bucketCapacity() const { return bucketCapacity_; }
    [[nodiscard]] std::size_t buckets() const { return buckets_; }
    [[nodiscard]] std::size_t submitted() const { return submitted_; }
    [[nodiscard]] std::size_t dropped() const { return dropped_; }
    [[nodiscard]] bool ready() const { return ready_; }

private:
    ComputeSlot reset_ = 0, cull_ = 0, compact_ = 0;
    Buffer clusters_, visible_, arguments_;
    Buffer hizFallback_;
    Buffer pageTable_, pageFeedback_;
    std::size_t clusterCapacity_ = 0, bucketCapacity_ = 0, buckets_ = 0;
    std::size_t submitted_ = 0, dropped_ = 0;
    std::vector<GeometryCluster> staged_;
    bool ready_ = false;
};

} // namespace engine
