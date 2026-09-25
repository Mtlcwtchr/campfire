#include "engine/geometry/cluster_morph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace engine::geometry {
namespace {

std::array<float,3> closestPointTriangle(const float p[3], const float a[3],
                                         const float b[3], const float c[3]) {
    const auto sub=[](const float* x,const float* y) {
        return std::array<double,3>{double(x[0])-y[0],double(x[1])-y[1],double(x[2])-y[2]};
    };
    const auto dot=[](const std::array<double,3>& x,const std::array<double,3>& y) {
        return x[0]*y[0]+x[1]*y[1]+x[2]*y[2];
    };
    const auto ab=sub(b,a), ac=sub(c,a), ap=sub(p,a);
    const double d1=dot(ab,ap),d2=dot(ac,ap);
    if (d1<=0 && d2<=0) return {a[0],a[1],a[2]};
    const auto bp=sub(p,b);
    const double d3=dot(ab,bp),d4=dot(ac,bp);
    if (d3>=0 && d4<=d3) return {b[0],b[1],b[2]};
    const double vc=d1*d4-d3*d2;
    if (vc<=0 && d1>=0 && d3<=0) {
        const double t=d1/(d1-d3);
        return {float(a[0]+t*ab[0]),float(a[1]+t*ab[1]),float(a[2]+t*ab[2])};
    }
    const auto cp=sub(p,c);
    const double d5=dot(ab,cp),d6=dot(ac,cp);
    if (d6>=0 && d5<=d6) return {c[0],c[1],c[2]};
    const double vb=d5*d2-d1*d6;
    if (vb<=0 && d2>=0 && d6<=0) {
        const double t=d2/(d2-d6);
        return {float(a[0]+t*ac[0]),float(a[1]+t*ac[1]),float(a[2]+t*ac[2])};
    }
    const double va=d3*d6-d5*d4;
    if (va<=0 && d4-d3>=0 && d5-d6>=0) {
        const double t=(d4-d3)/((d4-d3)+(d5-d6));
        return {float(b[0]+t*(double(c[0])-b[0])),
                float(b[1]+t*(double(c[1])-b[1])),
                float(b[2]+t*(double(c[2])-b[2]))};
    }
    const double inverse=1.0/(va+vb+vc);
    const double v=vb*inverse,w=vc*inverse;
    return {float(a[0]+ab[0]*v+ac[0]*w),float(a[1]+ab[1]*v+ac[1]*w),
            float(a[2]+ab[2]*v+ac[2]*w)};
}

double squaredDistance(const float* a,const std::array<float,3>& b) {
    const double x=double(a[0])-b[0],y=double(a[1])-b[1],z=double(a[2])-b[2];
    return x*x+y*y+z*z;
}

} // namespace

ClusterMorphAsset buildClusterMorph(std::span<const float> sourcePositions,
                                    std::span<const std::uint32_t> clusterIndices,
                                    std::span<const MeshCluster> clusters,
                                    std::span<const float> clusterPositions) {
    ClusterMorphAsset result;
    if (sourcePositions.empty() || sourcePositions.size()%3 ||
        clusterIndices.empty() || clusterIndices.size()%3 || clusters.empty() ||
        (!clusterPositions.empty() && clusterPositions.size()!=clusterIndices.size()*3))
        return result;
    const auto sourceCount=sourcePositions.size()/3;
    result.indices.reserve(clusterIndices.size());
    result.targetNormals.reserve(clusterIndices.size());
    for (const auto& cluster:clusters) {
        if (std::uint64_t(cluster.indices.first)+cluster.indices.count>clusterIndices.size() ||
            cluster.indices.count%3) return {};
        std::vector<std::uint32_t> sourceIds,localIds;
        sourceIds.reserve(cluster.indices.count);
        localIds.reserve(cluster.indices.count);
        for (std::uint32_t at=0;at<cluster.indices.count;++at) {
            const auto source=clusterIndices[cluster.indices.first+at];
            if (source>=sourceCount) return {};
            const auto found=std::find(sourceIds.begin(),sourceIds.end(),source);
            std::uint32_t local=0;
            if (found==sourceIds.end()) {
                sourceIds.push_back(source);
                local=std::uint32_t(result.sourceVertices.size());
                localIds.push_back(local);
            const float* position=&sourcePositions[std::size_t(source)*3];
                std::array<float,3> target{position[0],position[1],position[2]};
                std::array<float,3> targetNormal{0,0,0};
                if (cluster.replacedBy!=MeshCluster::kNoGroup) {
                    double best=std::numeric_limits<double>::infinity();
                    for (const auto& parent:clusters) {
                        if (parent.bornOf!=cluster.replacedBy) continue;
                        for (std::uint32_t p=0;p+2<parent.indices.count;p+=3) {
                            const auto base=parent.indices.first+p;
                            const auto ia=clusterIndices[base],ib=clusterIndices[base+1],
                                       ic=clusterIndices[base+2];
                            if (ia>=sourceCount || ib>=sourceCount || ic>=sourceCount) return {};
                            const float* parentA = clusterPositions.empty()
                                    ? &sourcePositions[std::size_t(ia)*3]
                                    : &clusterPositions[std::size_t(base)*3];
                            const float* parentB = clusterPositions.empty()
                                    ? &sourcePositions[std::size_t(ib)*3]
                                    : &clusterPositions[std::size_t(base+1)*3];
                            const float* parentC = clusterPositions.empty()
                                    ? &sourcePositions[std::size_t(ic)*3]
                                    : &clusterPositions[std::size_t(base+2)*3];
                            const auto candidate=closestPointTriangle(
                                position,parentA,parentB,parentC);
                            const double distance=squaredDistance(position,candidate);
                            if (distance<best) {
                                best=distance;target=candidate;
                                const auto ax=parentA;
                                const auto bx=parentB;
                                const auto cx=parentC;
                                const double ux=double(bx[0])-ax[0],uy=double(bx[1])-ax[1],uz=double(bx[2])-ax[2];
                                const double vx=double(cx[0])-ax[0],vy=double(cx[1])-ax[1],vz=double(cx[2])-ax[2];
                                const double nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
                                const double length=std::sqrt(nx*nx+ny*ny+nz*nz);
                                if (length>1e-12)
                                    targetNormal={float(nx/length),float(ny/length),float(nz/length)};
                            }
                        }
                    }
                }
                result.sourceVertices.push_back(source);
                result.targetPositions.insert(result.targetPositions.end(),
                                              target.begin(),target.end());
                result.targetNormals.insert(result.targetNormals.end(),
                                            targetNormal.begin(),targetNormal.end());
            } else {
                local=localIds[std::size_t(found-sourceIds.begin())];
            }
            result.indices.push_back(local);
        }
    }
    return result.empty() || result.targetNormals.size()!=result.sourceVertices.size()*3
            ? ClusterMorphAsset{} : result;
}

} // namespace engine::geometry
