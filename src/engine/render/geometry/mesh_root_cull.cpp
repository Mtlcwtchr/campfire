#include "engine/render/geometry/mesh_root_cull.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {
namespace {
constexpr std::uint32_t kGroupSize = 64;
std::uint32_t groupsFor(std::size_t items) {
    return static_cast<std::uint32_t>((items + kGroupSize - 1) / kGroupSize);
}

// The shader uses raw uint words because a 20/32-byte mixed structured layout
// has historically been rounded differently by Metal and DXIL. The words are
// nevertheless exactly the GeometryCluster/ MeshRootInstance byte layouts.
void writeFloat(std::uint32_t& destination, float value) {
    std::memcpy(&destination, &value, sizeof(value));
}
}

bool MeshRootCuller::setup(Device& device, RenderPipeline& into,
                           std::span<const GeometryCluster> staticClusters,
                           std::size_t maxRoots, std::size_t maxCandidates) {
    std::vector<MeshStaticCluster> flat(staticClusters.size());
    for (std::size_t i = 0; i < staticClusters.size(); ++i) flat[i].bounds = staticClusters[i];
    return setup(device, into, flat, {}, {}, maxRoots, maxCandidates);
}

bool MeshRootCuller::setup(Device& device, RenderPipeline& into,
                           std::span<const MeshStaticCluster> staticClusters,
                           std::span<const MeshFamilyRange> families,
                           std::span<const std::uint32_t> familyChildren,
                           std::size_t maxRoots, std::size_t maxCandidates) {
    if (staticClusters.empty() || !maxRoots || !maxCandidates) return false;
    if (staticClusters.size() > std::numeric_limits<std::uint32_t>::max() ||
        maxRoots > std::numeric_limits<std::uint32_t>::max() ||
        maxCandidates > std::numeric_limits<std::uint32_t>::max()) return false;

    const bool dag = !families.empty() && !familyChildren.empty();
    auto expand = device.makeCompute({dag ? "scene_mesh_dag_expand.hlsl"
                                           : "scene_mesh_root_expand.hlsl", "ExpandCS"});
    if (!expand) return false;

    std::vector<std::uint32_t> clusterWords(staticClusters.size() * 12);
    for (std::size_t i = 0; i < staticClusters.size(); ++i) {
        const auto& cluster = staticClusters[i];
        auto* words = clusterWords.data() + i * 12;
        writeFloat(words[0], cluster.bounds.centre[0]);
        writeFloat(words[1], cluster.bounds.centre[1]);
        writeFloat(words[2], cluster.bounds.centre[2]);
        writeFloat(words[3], cluster.bounds.radius);
        writeFloat(words[4], cluster.bounds.error);
        writeFloat(words[5], cluster.bounds.parentError);
        words[6] = cluster.bounds.bucket;
        words[7] = cluster.bounds.payload;
        words[8] = cluster.bornOf;
        words[9] = cluster.replacedBy;
        words[10] = words[11] = 0;
    }
    staticClusters_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                           clusterWords.data(),
                                           clusterWords.size() * sizeof(std::uint32_t));
    std::vector<std::uint32_t> familyWords(families.size() * 2);
    for (std::size_t i = 0; i < families.size(); ++i) {
        familyWords[i * 2 + 0] = families[i].childFirst;
        familyWords[i * 2 + 1] = families[i].childCount;
    }
    if (dag) {
        families_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                        familyWords.data(), familyWords.size() * sizeof(std::uint32_t));
        familyChildren_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                              familyChildren.data(),
                                              familyChildren.size() * sizeof(std::uint32_t));
    }
    roots_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                               maxRoots * 12 * sizeof(std::uint32_t));
    candidates_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                         SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                     maxCandidates * sizeof(GeometryCluster));
    if (!staticClusters_ || !roots_ || !candidates_ ||
        (dag && (!families_ || !familyChildren_))) return false;

    expand_ = into.take(std::move(expand));
    maxRoots_ = maxRoots;
    maxCandidates_ = maxCandidates;
    dag_ = dag;
    ready_ = true;
    return true;
}

bool MeshRootCuller::dispatch(const Frame& frame, RenderPipeline& into,
                              std::span<const MeshRootInstance> roots) {
    return dispatch(frame, into, roots, ClusterSelection{});
}

bool MeshRootCuller::dispatch(const Frame& frame, RenderPipeline& into,
                              std::span<const MeshRootInstance> roots,
                              const ClusterSelection& selection) {
    if (!ready_ || !frame.device || roots.empty() || roots.size() > maxRoots_) return false;

    std::size_t candidateCount = 0;
    for (const auto& root : roots) {
        if (root.clusterCount > maxCandidates_ ||
            root.clusterFirst > std::numeric_limits<std::uint32_t>::max() - root.clusterCount ||
            root.outputFirst > std::numeric_limits<std::uint32_t>::max() - root.clusterCount) {
            return false;
        }
        const std::size_t end = std::size_t(root.outputFirst) + root.clusterCount;
        if (end > maxCandidates_) return false;
        candidateCount = std::max(candidateCount, end);
    }
    if (!candidateCount) return false;

    std::vector<std::uint32_t> rootWords(roots.size() * 12);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        const auto& root = roots[i];
        auto* words = rootWords.data() + i * 12;
        writeFloat(words[0], root.position[0]);
        writeFloat(words[1], root.position[1]);
        writeFloat(words[2], root.position[2]);
        writeFloat(words[3], root.scale);
        writeFloat(words[4], root.yaw);
        words[5] = root.clusterFirst;
        words[6] = root.clusterCount;
        words[7] = root.bucketBase;
        words[8] = root.outputFirst;
        words[9] = root.payload;
        words[10] = root.familyFirst;
        words[11] = 0;
    }
    Device::Uploader uploader(*frame.device);
    uploader.rewrite(roots_.get(), rootWords.data(), rootWords.size() * sizeof(std::uint32_t));
    if (!uploader.finish()) return false;

    float own[24]{};
    const std::uint32_t counts[2]{static_cast<std::uint32_t>(roots.size()),
                                  static_cast<std::uint32_t>(candidateCount)};
    std::memcpy(own, counts, sizeof(counts));
    if (dag_) {
        std::memcpy(own + 4, selection.rowX, sizeof(selection.rowX));
        std::memcpy(own + 8, selection.rowY, sizeof(selection.rowY));
        std::memcpy(own + 12, selection.rowW, sizeof(selection.rowW));
        own[16] = selection.pixelError;
        own[17] = selection.halfWidth;
        own[18] = selection.halfHeight;
        own[19] = selection.focal;
    }
    ComputeDispatch job;
    job.pipeline = expand_;
    job.groupsX = groupsFor(roots.size());
    job.reads = {roots_.get(), staticClusters_.get()};
    if (dag_) {
        job.reads.push_back(families_.get());
        job.reads.push_back(familyChildren_.get());
    }
    job.writes = {candidates_.get()};
    std::memcpy(job.own, own, sizeof(own));
    into.dispatch(std::move(job));
    candidateCount_ = candidateCount;
    return true;
}

} // namespace engine
