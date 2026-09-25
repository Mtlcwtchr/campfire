#include "engine/render/systems/source_cluster_draws.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_map>

namespace engine::render {
SourceClusterIndex::SourceClusterIndex(std::span<const geometry::MeshCluster> clusters) {
    std::vector<std::uint32_t> order;
    for (std::uint32_t i = 0; i < clusters.size(); ++i)
        if (clusters[i].indices.count && clusters[i].error >= 0 &&
            clusters[i].error < clusters[i].parentError) order.push_back(i);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return clusters[a].error == clusters[b].error ? a < b : clusters[a].error < clusters[b].error;
    });
    nodes_.reserve(order.size());
    const auto build = [&](auto&& self, std::size_t first, std::size_t end) -> int {
        if (first == end) return -1;
        const auto middle = first + (end - first) / 2;
        const int at = int(nodes_.size());
        const auto id = order[middle];
        nodes_.push_back({id, -1, -1, clusters[id].error, clusters[id].parentError,
                          clusters[id].parentError});
        const int left = self(self, first, middle), right = self(self, middle + 1, end);
        auto& node = nodes_[std::size_t(at)];
        node.left = left; node.right = right;
        for (int child : {left, right})
            if (child >= 0) node.subtreeUpper = std::max(node.subtreeUpper, nodes_[std::size_t(child)].subtreeUpper);
        return at;
    };
    root_ = build(build, 0, order.size());
}
void SourceClusterIndex::visit(int at, double allowance, std::vector<std::uint32_t>& result) const {
    if (at < 0) return;
    const auto& node = nodes_[std::size_t(at)];
    if (!(allowance < node.subtreeUpper)) return;
    visit(node.left, allowance, result);
    if (node.lower > allowance) return;
    if (allowance < node.upper) result.push_back(node.cluster);
    visit(node.right, allowance, result);
}
void SourceClusterIndex::select(double allowance, std::vector<std::uint32_t>& result) const {
    result.clear();
    // +infinity means the coarsest complete cut, not an empty scene. Roots
    // themselves have an infinite upper bound, so query below that sentinel.
    if (allowance >= 0) visit(root_, std::min(allowance, std::numeric_limits<double>::max()), result);
}

bool sourceFitsGpuCapacity(const GatheredInstances& instances,
                          std::span<const SourceGeometry> assets,
                          std::size_t candidateCapacity, std::size_t instancesPerCluster) {
    std::vector<std::size_t> counts(assets.size());
    std::size_t remaining = candidateCapacity;
    for (const auto& batch : instances.batches) {
        if (batch.mesh >= assets.size() ||
            std::uint64_t(batch.first) + batch.count > instances.instances.size()) return false;
        const auto clusters = assets[batch.mesh].clusters.size();
        if (!batch.count) continue;
        if (!clusters || batch.count > instancesPerCluster - counts[batch.mesh] ||
            clusters > remaining / batch.count) return false;
        counts[batch.mesh] += batch.count;
        remaining -= clusters * batch.count;
    }
    return true;
}

SourceDrawPlan planSourceDraws(const GatheredInstances& instances,
                              std::span<const SourceGeometry> assets, const ScreenScale& screen) {
    SourceDrawPlan result;
    // Plane order and normalization are independent of the model and instance.
    std::array<std::array<double, 4>, 5> planes{};
    for (int p = 0; p < 4; ++p)
        for (int axis = 0; axis < 4; ++axis)
            planes[p][axis] = screen.rowW[axis] + (p % 2 ? -1 : 1) *
                double(p < 2 ? screen.rowX[axis] : screen.rowY[axis]);
    for (int axis = 0; axis < 4; ++axis) planes[4][axis] = screen.rowW[axis];
    std::array<double, 5> norms{};
    for (int p = 0; p < 5; ++p)
        norms[p] = std::hypot(planes[p][0], planes[p][1], planes[p][2]);
    const double normX = std::hypot(double(screen.rowX[0]), screen.rowX[1], screen.rowX[2]);
    const double normY = std::hypot(double(screen.rowY[0]), screen.rowY[1], screen.rowY[2]);
    double projectionNorm = 0;
    if (normX > 0 && normY > 0) {
        double product = 0;
        for (int axis = 0; axis < 3; ++axis)
            product += (screen.rowX[axis]/normX) * (screen.rowY[axis]/normY);
        // Operator norm of the two unit-length projection rows.
        projectionNorm = std::sqrt(1 + std::abs(product));
    }
    const auto visible = [&](const double centre[3], double radius) {
        for (int p = 0; p < 5; ++p) {
            const auto& plane = planes[p];
            if (plane[0]*centre[0] + plane[1]*centre[1] + plane[2]*centre[2] + plane[3] +
                radius*norms[p] < 0) return false;
        }
        return true;
    };
    std::vector<std::uint32_t> selected;
    for (const auto& batch : instances.batches) {
        if (batch.mesh >= assets.size() ||
            std::uint64_t(batch.first) + batch.count > instances.instances.size()) {
            ++result.plan.rejected;
            continue;
        }
        const auto& asset = assets[batch.mesh];
        if (!asset.index) { ++result.plan.rejected; continue; }
        // Extend an existing draw when consecutive source instances need the
        // same cluster. No per-cluster copy of instance transforms is uploaded.
        std::unordered_map<std::uint32_t, std::size_t> lastDraw;
        for (std::uint32_t i = batch.first; i < batch.first + batch.count; ++i) {
            const auto& instance = instances.instances[i];
            const double scale = instance.scale;
            if (!(scale > 0) || !std::isfinite(scale)) { ++result.plan.rejected; continue; }
            const double centre[]{instance.position[0], instance.position[1],
                                   instance.position[2] + asset.bounds.rise*scale};
            const double radius = (asset.bounds.radius + asset.deformation)*scale;
            if (!visible(centre, radius)) continue;
            const double depth = screen.rowW[0]*centre[0] + screen.rowW[1]*centre[1] +
                                 screen.rowW[2]*centre[2] + screen.rowW[3];
            // A sphere crossing the eye plane requires the finest representation,
            // not rejection of geometry that remains visible in front of the eye.
            const double nearest = depth - radius*norms[4];
            double allowance = 0;
            if (screen.focal > 0) {
                if (nearest > 0 && projectionNorm > 0) {
                    double x = screen.rowX[3], y = screen.rowY[3];
                    for (int axis = 0; axis < 3; ++axis) {
                        x += screen.rowX[axis]*centre[axis];
                        y += screen.rowY[axis]*centre[axis];
                    }
                    // For P,Q inside the sphere, bound |A(P)/w(P)-A(Q)/w(Q)|.
                    // The second term covers motion ALONG depth, which changes
                    // screen position off-axis even without transverse motion.
                    const double transverse = std::hypot(x/normX, y/normY) + projectionNorm*radius;
                    const double factor = projectionNorm + transverse*norms[4]/nearest;
                    allowance = (screen.allowance/screen.focal) * (nearest/scale) / factor;
                }
            } else if (screen.scale > 0) allowance = screen.allowance/(screen.scale*scale);
            // With no animation correspondence, two vertices can move apart by
            // twice the displacement bound. A negative remainder keeps the exact
            // source cut; it never removes geometry to satisfy the allowance.
            asset.index->select(std::max(0.0, allowance - 2*asset.deformation), selected);
            const double c = std::cos(instance.yaw), s = std::sin(instance.yaw);
            for (const auto id : selected) {
                if (id >= asset.clusters.size()) { ++result.plan.rejected; continue; }
                const auto& cluster = asset.clusters[id];
                ++result.selectedClusters;
                const double at[]{instance.position[0] + scale*(c*cluster.centre[0] - s*cluster.centre[1]),
                                  instance.position[1] + scale*(s*cluster.centre[0] + c*cluster.centre[1]),
                                  instance.position[2] + scale*cluster.centre[2]};
                if (!visible(at, (cluster.radius + asset.deformation)*scale)) {
                    ++result.culledClusters;
                    continue;
                }
                auto found = lastDraw.find(id);
                if (found != lastDraw.end() && result.plan.draws[found->second].firstInstance +
                    result.plan.draws[found->second].instanceCount == i) {
                    ++result.plan.draws[found->second].instanceCount;
                } else {
                    lastDraw[id] = result.plan.draws.size();
                    result.plan.draws.push_back({cluster.indices.count, 1,
                        asset.indexBase + cluster.indices.first,
                        asset.clusterVertexBase ? asset.clusterVertexBase : asset.vertexBase, i});
                    ++result.plan.fromClusters;
                }
                result.plan.triangles += cluster.indices.count / 3;
            }
        }
    }
    return result;
}
} // namespace engine::render
