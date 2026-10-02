#include "game/render/grass_cull.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"
#include "engine/render/render_pipeline.hpp"

namespace game {
static_assert(sizeof(world::PageGrassRoot) == 32);
static_assert(offsetof(world::PageGrassRoot, run) == 28);
static_assert(GrassCuller::kCapacity <= 512 * 256); // one prefix group scans 512 block totals

bool GrassCuller::setup(engine::Device& device, engine::RenderPipeline& into) {
    auto prepare = device.makeCompute({"grass_cluster_prepare.hlsl", "PrepareCS"});
    auto gather = device.makeCompute({"grass_cluster_gather.hlsl", "GatherCS"});
    auto mark = device.makeCompute({"grass_cluster_mark.hlsl", "MarkCS"});
    auto scan = device.makeCompute({"grass_cluster_scan.hlsl", "ScanCS"});
    auto prefix = device.makeCompute({"grass_cluster_prefix.hlsl", "PrefixCS"});
    if (!prepare || !gather || !mark || !scan || !prefix ||
        !culler_.setup(device, into, kCapacity, kCapacity, 1)) return false;
    bounds_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                kCapacity * sizeof(engine::GeometryCluster));
    compacted_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                   SDL_GPU_BUFFERUSAGE_VERTEX,
                                   kCapacity * sizeof(world::PageGrassRoot));
    const engine::DrawArguments draw{6 * world::kTurfCards, 0, 0, 0, 0};
    const auto storage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    ranks_ = device.makeBuffer(storage, kCapacity * sizeof(std::uint32_t));
    blocks_ = device.makeBuffer(storage, ((kCapacity + 255) / 256) * sizeof(std::uint32_t));
    if (!bounds_ || !compacted_ || !ranks_ || !blocks_ ||
        !culler_.describe(device, std::span(&draw, 1))) return false;
    prepare_ = into.take(std::move(prepare));
    gather_ = into.take(std::move(gather));
    mark_ = into.take(std::move(mark));
    scan_ = into.take(std::move(scan));
    prefix_ = into.take(std::move(prefix));
    ready_ = true;
    return true;
}

bool GrassCuller::dispatch(const engine::Frame& frame, engine::RenderPipeline& into,
                          std::uint32_t arenaOffset, std::size_t count) {
    if (!ready_ || !frame.device || !frame.instances || count > kCapacity ||
        arenaOffset % sizeof(world::PageGrassRoot) != 0 ||
        std::size_t(arenaOffset) + count * sizeof(world::PageGrassRoot) > frame.instances->bytes()) {
        if (frame.device) frame.device->fail("grass cull input exceeds its arena range or budget");
        return false;
    }
    engine::ClusterSelection selection;
    const auto* matrix = engine::cullMatrix(frame.scene);
    std::copy_n(matrix, 4, selection.rowX);
    std::copy_n(matrix + 4, 4, selection.rowY);
    std::copy_n(matrix + 12, 4, selection.rowW);
    const bool perspective = matrix[12] != 0 || matrix[13] != 0 || matrix[14] != 0;
    selection.focal = perspective ? float(std::hypot(std::hypot(matrix[0], matrix[1]), matrix[2]) * frame.width * 0.5) : 0;
    selection.halfWidth = perspective ? float(frame.width) * 0.5f : frame.scene.camera[3];
    selection.halfHeight = float(frame.height) * 0.5f;
    if (count) {
        engine::ComputeDispatch prepare;
        prepare.pipeline = prepare_;
        prepare.groupsX = static_cast<std::uint32_t>((count + 63) / 64);
        prepare.reads = {nullptr};
        prepare.instanceReadSlot = 0;
        prepare.writes = {bounds_.get(), ranks_.get()};
        const std::uint32_t input[]{arenaOffset, static_cast<std::uint32_t>(count)};
        std::memcpy(prepare.own, input, sizeof(input));
        // |windLean|<=1, |sway|<=.16, stiffness>=.7, |shiver|<=.075,
        // |drift|<=.12*|wind.z|; height<=2.47. Covers all animation times.
        const float wind = std::abs(frame.scene.wind[2]);
        prepare.own[2] = std::hypot(frame.scene.wind[0], frame.scene.wind[1]) * 2.47f *
            (0.42f * ((wind + 0.16f) / 0.7f + 0.075f) + 0.16f * 0.12f * wind);
        into.dispatch(std::move(prepare));
    }
    culler_.dispatchGpu(frame, into, selection, bounds_.get(), count);
    if (count) {
        const std::uint32_t input[]{arenaOffset, static_cast<std::uint32_t>(count)};
        engine::ComputeDispatch mark;
        mark.pipeline = mark_;
        mark.groupsX = static_cast<std::uint32_t>((count + 63) / 64);
        mark.reads = {culler_.visible(), culler_.arguments()};
        mark.writes = {ranks_.get()};
        std::memcpy(mark.own, input, sizeof(input));
        into.dispatch(std::move(mark));
        engine::ComputeDispatch scan;
        scan.pipeline = scan_;
        scan.groupsX = static_cast<std::uint32_t>((count + 255) / 256);
        scan.writes = {ranks_.get(), blocks_.get()};
        std::memcpy(scan.own, input, sizeof(input));
        into.dispatch(std::move(scan));
        engine::ComputeDispatch prefix;
        prefix.pipeline = prefix_;
        prefix.writes = {blocks_.get()};
        std::memcpy(prefix.own, input, sizeof(input));
        into.dispatch(std::move(prefix));
        engine::ComputeDispatch gather;
        gather.pipeline = gather_;
        gather.groupsX = static_cast<std::uint32_t>((count + 63) / 64);
        gather.reads = {nullptr, ranks_.get(), blocks_.get()};
        gather.instanceReadSlot = 0;
        gather.writes = {compacted_.get()};
        std::memcpy(gather.own, input, sizeof(input));
        into.dispatch(std::move(gather));
    }
    return true;
}

#if ASR_ENABLE_DIAGNOSTICS
bool GrassCuller::readCount(engine::Device& device, std::uint32_t& count) const {
    if (!ready_) return false;
    engine::DrawArguments draw{};
    if (!device.readBuffer(culler_.arguments(), &draw, sizeof(draw)) || draw.instanceCount > kCapacity) return false;
    count = draw.instanceCount;
    return true;
}
#endif

std::size_t GrassCuller::workingBytes() const {
    return ready_ ? kCapacity * (sizeof(engine::GeometryCluster) + sizeof(world::PageGrassRoot) + 2 * sizeof(std::uint32_t)) +
                    ((kCapacity + 255) / 256) * sizeof(std::uint32_t) + sizeof(engine::DrawArguments) : 0;
}
}

