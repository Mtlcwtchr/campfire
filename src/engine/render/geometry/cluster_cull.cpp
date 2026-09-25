#include "engine/render/geometry/cluster_cull.hpp"

#include <algorithm>
#include <cstring>

#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {
namespace {
constexpr std::uint32_t kGroupSize = 64;   // matches numthreads in cluster_cull.hlsl
std::uint32_t groupsFor(std::size_t items) {
    return static_cast<std::uint32_t>((items + kGroupSize - 1) / kGroupSize);
}
}

bool ClusterCuller::setup(Device& device, RenderPipeline& into, std::size_t clusterCapacity,
                          std::size_t bucketCapacity, std::size_t buckets) {
    if (!clusterCapacity || !bucketCapacity || !buckets) return false;
    clusterCapacity_ = clusterCapacity;
    bucketCapacity_ = bucketCapacity;
    buckets_ = buckets;

    auto reset = device.makeCompute({"cluster_reset.hlsl", "ResetCS"});
    auto cull = device.makeCompute({"cluster_cull.hlsl", "CullCS"});
    auto compact = device.makeCompute({"cluster_compact.hlsl", "CompactCS"});
    if (!reset || !cull || !compact) return false;

    // The cluster list is written by the CPU every frame and read by the
    // compute pass; the other two are written by compute and read by the draw,
    // and the arguments are also what the draw reads as indirect.
    // Allocate the CPU-upload buffer lazily; GPU producers never need it.
    visible_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                         SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ,
                                 bucketCapacity_ * buckets_ * sizeof(std::uint32_t));
    arguments_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                           SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                           SDL_GPU_BUFFERUSAGE_INDIRECT,
                                   buckets_ * sizeof(DrawArguments));
    hizFallback_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ, sizeof(float));
    std::vector<std::uint32_t> resident(buckets_, kPageResident);
    std::vector<std::uint32_t> feedback(buckets_, 0);
    pageTable_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                      resident.data(), resident.size() * sizeof(std::uint32_t));
    pageFeedback_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ |
                                                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE,
                                        feedback.data(), feedback.size() * sizeof(std::uint32_t));
    if (!visible_ || !arguments_ || !hizFallback_ || !pageTable_ || !pageFeedback_) return false;

    reset_ = into.take(std::move(reset));
    cull_ = into.take(std::move(cull));
    compact_ = into.take(std::move(compact));
    ready_ = true;
    return true;
}

bool ClusterCuller::describe(Device& device, std::span<const DrawArguments> draws) {
    if (!arguments_ || draws.size() != buckets_) return false;
    Device::Uploader uploader(device);
    uploader.rewrite(arguments_.get(), draws.data(), draws.size() * sizeof(DrawArguments));
    return uploader.finish();
}

bool ClusterCuller::updatePageTable(Device& device, std::span<const std::uint32_t> states) {
    if (!pageTable_ || states.size() != buckets_) return false;
    for (const auto state : states)
        if (state > kPageFallback) return false;
    Device::Uploader uploader(device);
    uploader.rewrite(pageTable_.get(), states.data(), states.size() * sizeof(std::uint32_t));
    return uploader.finish();
}

bool ClusterCuller::readPageFeedback(Device& device, std::span<std::uint32_t> into) const {
    return pageFeedback_ && into.size() == buckets_ &&
           device.readBuffer(pageFeedback_.get(), into.data(), into.size() * sizeof(std::uint32_t));
}

void ClusterCuller::clusters(std::span<const GeometryCluster> list) {
    submitted_ = std::min(list.size(), clusterCapacity_);
    dropped_ = list.size() - submitted_;
    staged_.assign(list.begin(), list.begin() + static_cast<std::ptrdiff_t>(submitted_));
}

void ClusterCuller::dispatch(const Frame& frame, RenderPipeline& into,
                            const ClusterSelection& selection, bool compactOutput) {
    if (!ready_ || !frame.device) return;
    // The list first, because the dispatch that reads it is recorded next and a
    // copy pass may not be opened inside a compute pass either.
    if (!staged_.empty()) {
        if (!clusters_) clusters_ = frame.device->makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                                            clusterCapacity_ * sizeof(GeometryCluster));
        if (!clusters_) return;
        Device::Uploader uploader(*frame.device);
        uploader.rewrite(clusters_.get(), staged_.data(), staged_.size() * sizeof(GeometryCluster));
        if (!uploader.finish()) return;
    }
    dispatchGpu(frame, into, selection, clusters_.get(), submitted_ + dropped_, compactOutput);
}

void ClusterCuller::dispatchGpu(const Frame& frame, RenderPipeline& into,
                                const ClusterSelection& selection, SDL_GPUBuffer* input,
                                std::size_t count, bool compactOutput) {
    if (!ready_ || !frame.device || (count && !input)) return;
    submitted_ = std::min(count, clusterCapacity_);
    dropped_ = count - submitted_;

    // The input list is already on the card for GPU producers. The reset and
    // cull are identical for CPU and GPU producers; only the upload differs.
    float own[32]{};
    std::memcpy(own + 0, selection.rowX, sizeof(selection.rowX));
    std::memcpy(own + 4, selection.rowY, sizeof(selection.rowY));
    std::memcpy(own + 8, selection.rowW, sizeof(selection.rowW));
    std::memcpy(own + 24, selection.rowZ, sizeof(selection.rowZ));
    const std::uint32_t counts[3]{static_cast<std::uint32_t>(submitted_),
                                  static_cast<std::uint32_t>(bucketCapacity_),
                                  static_cast<std::uint32_t>(buckets_)};
    std::memcpy(own + 12, counts, sizeof(counts));
    own[15] = selection.pixelError;
    own[16] = selection.halfWidth;
    own[17] = selection.halfHeight;
    own[18] = selection.focal;
    own[19] = 0;
    own[20] = selection.hiz && selection.hizLevels ? 1.0f : 0.0f;
    own[21] = float(selection.hiz ? selection.hizWidth : 1);
    own[22] = float(selection.hiz ? selection.hizHeight : 1);
    own[23] = float(selection.hiz ? selection.hizLevels : 1);

    ComputeDispatch reset;
    reset.pipeline = reset_;
    reset.groupsX = groupsFor(buckets_);
    reset.writes = {arguments_.get(), pageFeedback_.get()};
    std::memcpy(reset.own, own, sizeof(own));
    into.dispatch(std::move(reset));

    if (submitted_ > 0) {
        ComputeDispatch cull;
        cull.pipeline = cull_;
        cull.groupsX = groupsFor(submitted_);
        cull.reads = {input};
        cull.reads.push_back(selection.hiz && selection.hizLevels ? selection.hiz
                                                                  : hizFallback_.get());
        cull.reads.push_back(pageTable_.get());
        cull.writes = {visible_.get(), arguments_.get(), pageFeedback_.get()};
        std::memcpy(cull.own, own, sizeof(own));
        into.dispatch(std::move(cull));
    }

    if (compactOutput) {
        ComputeDispatch compact;
        compact.pipeline = compact_;
        compact.groupsX = 1;
        compact.writes = {arguments_.get()};
        std::memcpy(compact.own, own, sizeof(own));
        into.dispatch(std::move(compact));
    }
}

} // namespace engine
