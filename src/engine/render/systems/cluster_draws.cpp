#include "engine/render/systems/cluster_draws.hpp"

#include <cmath>

namespace engine::render {
namespace {
std::vector<std::uint32_t> crownCut(std::span<const geometry::MeshCluster> clusters,
                                    double allowance, bool& explicitHierarchy) {
    std::vector<std::uint32_t> selected;
    explicitHierarchy = geometry::cutAtHierarchy(clusters, allowance, selected);
    if (explicitHierarchy) return selected;
    for (std::uint32_t i = 0; i < clusters.size(); ++i) {
        const auto& cluster = clusters[i];
        if (double(cluster.error) <= allowance && double(cluster.parentError) > allowance)
            selected.push_back(i);
    }
    return selected;
}
} // namespace

bool crownFits(std::span<const geometry::MeshCluster> clusters, double allowance) {
    if (!(allowance > 0) || !std::isfinite(allowance)) return false;
    bool finest = false;
    for (const auto& cluster : clusters) {
        if (!cluster.indices.count) continue;
        if (cluster.level == 0) {
            finest = true;
            if (!(cluster.error <= allowance)) return false;
        }
    }
    if (!finest) return false;
    bool explicitHierarchy = false;
    return !crownCut(clusters, allowance, explicitHierarchy).empty();
}

double proxyShellAllowance(double pixelsPerMetre, double scale, double height, double pixelError) {
    if (!(pixelsPerMetre > 0) || !(scale > 0) || !(height > 0) || !(pixelError > 0) ||
        !std::isfinite(pixelsPerMetre + scale + height + pixelError)) return 0;
    const double remaining = pixelError - std::abs(scale - 1) * height * pixelsPerMetre;
    return remaining > 0 ? remaining / (pixelsPerMetre * scale) : 0;
}

DrawPlan planDraws(std::span<const DrawRun> runs, std::span<const MeshGeometry> assets) {
    DrawPlan plan;
    plan.draws.reserve(runs.size());
    for (const DrawRun& run : runs) {
        if (run.count == 0 || run.asset >= assets.size()) {
            ++plan.rejected;
            continue;
        }
        const MeshGeometry& asset = assets[run.asset];
        const bool cut = !asset.clusters.empty() && run.allowance > 0 &&
                         std::isfinite(run.allowance);
        const std::size_t level =
                asset.levels.empty() ? 0 : std::min<std::size_t>(run.level, asset.levels.size() - 1);
        // Three answers for one run, all measured against the same allowance,
        // and the cheapest is the right one.
        //
        //   the cut      this asset's clusters, plus its cards at this level
        //   the shell    the whole model merged into one surface and cut
        //   the chain    a whole level of the discrete chain
        //
        // The shell REPLACES the cut rather than joining it: it contains the
        // solid geometry as well as the foliage, so drawing both would draw the
        // trunk twice.
        std::size_t shellIndices = 0;
        bool crownHierarchy = false;
        std::vector<std::uint32_t> crownSelection;
        if (crownFits(asset.crownClusters, run.allowance)) {
            crownSelection = crownCut(asset.crownClusters, run.allowance, crownHierarchy);
            for (const auto index : crownSelection)
                shellIndices += asset.crownClusters[index].indices.count;
        }
        const std::size_t chainIndices = asset.levels.empty() ? 0 : asset.levels[level].count;
        const IndexRange cards =
                run.level < asset.cardLevels.size() ? asset.cardLevels[run.level] : IndexRange{};
        if (cut) {
            // The rule the shader states, and the offline build measured
            // against: this representation fits, the coarser one does not.
            const auto before = plan.draws.size();
            std::size_t cutIndices = 0;
            std::vector<std::uint32_t> hierarchyCut;
            const bool hasHierarchy =
                    geometry::cutAtHierarchy(asset.clusters, run.allowance, hierarchyCut);
            if (hasHierarchy) {
                for (const auto index : hierarchyCut) {
                    const auto& cluster = asset.clusters[index];
                    plan.draws.push_back({cluster.indices.count, run.count,
                                          asset.clusterBase + cluster.indices.first, asset.vertexBase,
                                          run.first});
                    cutIndices += cluster.indices.count;
                }
            } else {
                for (const auto& cluster : asset.clusters) {
                    if (!(double(cluster.error) <= run.allowance) ||
                        !(double(cluster.parentError) > run.allowance))
                        continue;
                    plan.draws.push_back({cluster.indices.count, run.count,
                                          asset.clusterBase + cluster.indices.first, asset.vertexBase,
                                          run.first});
                    cutIndices += cluster.indices.count;
                }
            }

            const std::size_t whole = cutIndices + cards.count;
            const bool cheapest = plan.draws.size() != before &&
                                  (chainIndices == 0 || whole <= chainIndices) &&
                                  (shellIndices == 0 || whole <= shellIndices);
            if (cheapest) {
                plan.fromClusters += plan.draws.size() - before;
                plan.triangles += std::size_t(cutIndices / 3) * run.count;
                if (cards.count) {
                    plan.draws.push_back({cards.count, run.count, asset.cardBase + cards.first,
                                          asset.vertexBase, run.first});
                    plan.triangles += std::size_t(cards.count / 3) * run.count;
                    ++plan.fromCards;
                }
                continue;
            }
            plan.draws.resize(before);
            if (cutIndices) ++plan.declined;
            // A cut of nothing means the allowance fell outside what the DAG
            // measured. Falling through to the chain draws the model rather
            // than a hole, which is the only useful answer here.
        }
        // The shell, when it is cheaper than the chain - or when there is no
        // chain to fall back to, which is what a model made entirely of cards is.
        if (shellIndices && (chainIndices == 0 || shellIndices <= chainIndices)) {
            (void)crownHierarchy;
            for (const auto index : crownSelection) {
                const auto& cluster = asset.crownClusters[index];
                plan.draws.push_back({cluster.indices.count, run.count,
                                      asset.crownBase + cluster.indices.first,
                                      asset.crownVertexBase, run.first});
                ++plan.fromCrown;
            }
            plan.triangles += std::size_t(shellIndices / 3) * run.count;
            continue;
        }
        // A source-cluster companion run may intentionally contain only the
        // alpha-card ranges. It has no chain or solid DAG of its own, but the
        // cards are still real indexed geometry and must not be rejected as an
        // empty asset. This is how the GPU clustered path keeps leaves beside
        // the solid cluster cut without duplicating the solid triangles.
        if (asset.levels.empty() && cards.count) {
            plan.draws.push_back({cards.count, run.count, asset.cardBase + cards.first,
                                  asset.vertexBase, run.first});
            plan.triangles += std::size_t(cards.count / 3) * run.count;
            ++plan.fromCards;
            continue;
        }
        if (asset.levels.empty()) {
            ++plan.rejected;
            continue;
        }
        const IndexRange& range = asset.levels[level];
        if (range.count == 0) {
            ++plan.rejected;
            continue;
        }
        plan.draws.push_back({range.count, run.count, range.first, asset.vertexBase, run.first});
        plan.triangles += std::size_t(range.count / 3) * run.count;
        ++plan.fromChain;
    }
    return plan;
}

} // namespace engine::render
