#include "engine/render/geometry/instance_root_cull.hpp"

#include <cstring>
#include <limits>
#include <vector>

#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {
namespace {
void writeFloat(std::uint32_t& destination, float value) {
    std::memcpy(&destination, &value, sizeof(value));
}
}

bool InstanceRootCuller::setup(Device& device, RenderPipeline& into, std::size_t maxRoots) {
    if (!maxRoots || maxRoots > std::numeric_limits<std::uint32_t>::max()) return false;
    auto expand = device.makeCompute({"scene_instance_root_expand.hlsl", "ExpandCS"});
    if (!expand) return false;
    roots_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                               maxRoots * 8 * sizeof(std::uint32_t));
    candidates_ = device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                    maxRoots * sizeof(GeometryCluster));
    if (!roots_ || !candidates_) return false;
    expand_ = into.take(std::move(expand));
    maxRoots_ = maxRoots;
    ready_ = true;
    return true;
}

bool InstanceRootCuller::dispatch(const Frame& frame, RenderPipeline& into,
                                  std::span<const InstanceHierarchyRoot> roots) {
    if (!ready_ || !frame.device || roots.empty() || roots.size() > maxRoots_) return false;
    std::vector<std::uint32_t> words(roots.size() * 8);
    for (std::size_t i = 0; i < roots.size(); ++i) {
        const auto& root = roots[i];
        auto* dst = words.data() + i * 8;
        writeFloat(dst[0], root.position[0]);
        writeFloat(dst[1], root.position[1]);
        writeFloat(dst[2], root.position[2]);
        writeFloat(dst[3], root.radius);
        writeFloat(dst[4], root.error);
        writeFloat(dst[5], root.parentError);
        dst[6] = root.bucket;
        dst[7] = root.payload;
    }
    Device::Uploader uploader(*frame.device);
    uploader.rewrite(roots_.get(), words.data(), words.size() * sizeof(std::uint32_t));
    if (!uploader.finish()) return false;

    float own[24]{};
    const auto count = static_cast<std::uint32_t>(roots.size());
    std::memcpy(own, &count, sizeof(count));
    ComputeDispatch job;
    job.pipeline = expand_;
    job.groupsX = static_cast<std::uint32_t>((roots.size() + 63) / 64);
    job.reads = {roots_.get()};
    job.writes = {candidates_.get()};
    std::memcpy(job.own, own, sizeof(own));
    into.dispatch(std::move(job));
    candidateCount_ = roots.size();
    return true;
}

bool InstanceRootCuller::setupHierarchy(Device& device, RenderPipeline& into,
                                        std::size_t maxNodes, std::size_t maxRoots,
                                        std::size_t maxCandidates) {
    if (!maxNodes || !maxRoots || !maxCandidates ||
        maxNodes > std::numeric_limits<std::uint32_t>::max() ||
        maxRoots > std::numeric_limits<std::uint32_t>::max() ||
        maxCandidates > std::numeric_limits<std::uint32_t>::max()) return false;
    auto expand=device.makeCompute({"scene_instance_dag_expand.hlsl","ExpandCS"});
    if (!expand) return false;
    hierarchyNodes_=device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                      maxNodes*12*sizeof(std::uint32_t));
    hierarchyChildren_=device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                         maxNodes*sizeof(std::uint32_t));
    hierarchyRoots_=device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                      maxRoots*4*sizeof(std::uint32_t));
    hierarchyCandidates_=device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE |
                                                SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                            maxCandidates*sizeof(GeometryCluster));
    if (!hierarchyNodes_ || !hierarchyChildren_ || !hierarchyRoots_ || !hierarchyCandidates_)
        return false;
    hierarchyExpand_=into.take(std::move(expand));
    maxHierarchyNodes_=maxNodes;maxHierarchyRoots_=maxRoots;maxHierarchyCandidates_=maxCandidates;
    hierarchyReady_=true;
    return true;
}

bool InstanceRootCuller::dispatchHierarchy(
        const Frame& frame, RenderPipeline& into,
        std::span<const InstanceHierarchyStaticNode> nodes,
        std::span<const std::uint32_t> children,
        std::span<const InstanceHierarchyTraversalRoot> roots,
        const ClusterSelection& selection) {
    if (!hierarchyReady_ || !frame.device || nodes.empty() || roots.empty() ||
        nodes.size()>maxHierarchyNodes_ || roots.size()>maxHierarchyRoots_ ||
        children.size()>maxHierarchyNodes_) return false;
    std::size_t candidateCount=0;
    for (const auto& root:roots) {
        if (std::size_t(root.outputFirst)+root.nodeCount>maxHierarchyCandidates_) return false;
        candidateCount=std::max(candidateCount,std::size_t(root.outputFirst)+root.nodeCount);
    }
    if (!candidateCount) return false;
    std::vector<std::uint32_t> nodeWords(nodes.size()*12);
    for (std::size_t i=0;i<nodes.size();++i) {
        const auto& node=nodes[i];auto* dst=nodeWords.data()+i*12;
        writeFloat(dst[0],node.bounds.centre[0]);writeFloat(dst[1],node.bounds.centre[1]);
        writeFloat(dst[2],node.bounds.centre[2]);writeFloat(dst[3],node.bounds.radius);
        writeFloat(dst[4],node.bounds.error);writeFloat(dst[5],node.bounds.parentError);
        dst[6]=node.bounds.bucket;dst[7]=node.bounds.payload;
        dst[8]=node.childFirst;dst[9]=node.childCount;dst[10]=0;dst[11]=node.payload;
    }
    std::vector<std::uint32_t> rootWords(roots.size()*4);
    for (std::size_t i=0;i<roots.size();++i) {
        rootWords[i*4+0]=roots[i].nodeFirst;rootWords[i*4+1]=roots[i].nodeCount;
        rootWords[i*4+2]=roots[i].outputFirst;rootWords[i*4+3]=0;
    }
    Device::Uploader uploader(*frame.device);
    uploader.rewrite(hierarchyNodes_.get(),nodeWords.data(),nodeWords.size()*sizeof(std::uint32_t));
    uploader.rewrite(hierarchyChildren_.get(),children.data(),children.size()*sizeof(std::uint32_t));
    uploader.rewrite(hierarchyRoots_.get(),rootWords.data(),rootWords.size()*sizeof(std::uint32_t));
    if (!uploader.finish()) return false;
    float own[24]{};
    const std::uint32_t counts[2]{static_cast<std::uint32_t>(roots.size()),
                                  static_cast<std::uint32_t>(candidateCount)};
    std::memcpy(own,counts,sizeof(counts));
    std::memcpy(own+4,selection.rowX,sizeof(selection.rowX));
    std::memcpy(own+8,selection.rowY,sizeof(selection.rowY));
    std::memcpy(own+12,selection.rowW,sizeof(selection.rowW));
    own[16]=selection.pixelError;own[17]=selection.halfWidth;
    own[18]=selection.halfHeight;own[19]=selection.focal;
    ComputeDispatch job;job.pipeline=hierarchyExpand_;job.groupsX=static_cast<std::uint32_t>((roots.size()+63)/64);
    job.reads={hierarchyNodes_.get(),hierarchyChildren_.get(),hierarchyRoots_.get()};
    job.writes={hierarchyCandidates_.get()};std::memcpy(job.own,own,sizeof(own));
    into.dispatch(std::move(job));
    candidateCount_=candidateCount;
    return true;
}

} // namespace engine
