#include "engine/geometry/cluster_asset.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <array>
#include <unordered_map>

namespace engine::geometry {
namespace {
constexpr char kMagic[4]{'S', 'C', 'C', '6'};
constexpr char kHierarchyLegacyMagic[4]{'S', 'C', 'C', '5'};
constexpr char kLegacyMagic[4]{'S', 'C', 'C', '4'};
constexpr char kLegacyMagicV3[4]{'S', 'C', 'C', '3'};
// firstIndex, indexCount, centre[3], radius, error, parentError, level,
// bornOf, replacedBy.
constexpr std::size_t kClusterBytes = 44;
constexpr std::size_t kLegacyClusterBytes = 36;
// Fifteen numbers after the magic; see encodeClusters, in that order.
constexpr std::size_t kHeaderBytes = 4 + 15 * 4;
// A limit rather than a guess: a sidecar is content, and content arrives
// corrupted. Twelve million indices is four million triangles, which is more
// than any prop in this catalogue and still allocatable if a file lies.
constexpr std::uint32_t kMaxIndices = 12u << 20;
constexpr std::uint32_t kMaxClusters = 1u << 20;

template <class T>
void put(std::vector<std::byte>& into, T value) {
    std::byte bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    into.insert(into.end(), bytes, bytes + sizeof(T));
}
template <class T>
T take(std::span<const std::byte> bytes, std::size_t& at) {
    T value{};
    std::memcpy(&value, bytes.data() + at, sizeof(T));
    at += sizeof(T);
    return value;
}

// Which triangles hang together, over WELDED vertices: an importer's split for
// a UV would otherwise make one leaf look like two pieces.
struct Components {
    std::vector<std::uint32_t> of;      // per triangle
    std::vector<std::uint32_t> size;    // triangles in each
};

Components components(std::span<const std::uint32_t> welded,
                      const std::vector<std::array<std::uint32_t, 3>>& triangles) {
    std::vector<std::uint32_t> parent(welded.empty() ? 0 : *std::max_element(welded.begin(), welded.end()) + 1);
    std::iota(parent.begin(), parent.end(), 0u);
    const auto find = [&](std::uint32_t v) {
        while (parent[v] != v) v = parent[v] = parent[parent[v]];
        return v;
    };
    for (const auto& t : triangles) {
        const auto root = find(t[0]);
        parent[find(t[1])] = root;
        parent[find(t[2])] = root;
    }
    Components result;
    result.of.resize(triangles.size());
    std::unordered_map<std::uint32_t, std::uint32_t> numbered;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const auto root = find(triangles[i][0]);
        const auto [entry, fresh] = numbered.try_emplace(root, std::uint32_t(result.size.size()));
        if (fresh) result.size.push_back(0);
        result.of[i] = entry->second;
        ++result.size[entry->second];
    }
    return result;
}

void appendClusterPositions(ClusterAsset& asset, const ClusterDag& dag) {
    asset.clusterPositions.reserve(dag.indices.size() * 3);
    for (const auto index : dag.indices) {
        if (std::size_t(index) * 3 + 2 >= dag.positions.size()) {
            asset.clusterPositions.clear();
            return;
        }
        for (int axis = 0; axis < 3; ++axis)
            asset.clusterPositions.push_back(dag.positions[std::size_t(index) * 3 + axis]);
    }
}
}

std::vector<std::byte> encodeClusters(const ClusterAsset& asset) {
    std::vector<std::byte> bytes;
    if (asset.indices.size() > kMaxIndices || asset.clusters.size() > kMaxClusters) return bytes;
    bytes.reserve(kHeaderBytes + asset.indices.size() * 4 + asset.clusters.size() * kClusterBytes);
    bytes.insert(bytes.end(), reinterpret_cast<const std::byte*>(kMagic),
                 reinterpret_cast<const std::byte*>(kMagic) + 4);
    put<std::uint32_t>(bytes, asset.sourceVertices);
    put<std::uint32_t>(bytes, asset.sourceTriangles);
    put<std::uint32_t>(bytes, asset.cardTriangles);
    put<std::uint32_t>(bytes, asset.levels);
    put<std::uint32_t>(bytes, asset.dominated);
    put<std::uint32_t>(bytes, std::uint32_t(asset.indices.size()));
    put<std::uint32_t>(bytes, std::uint32_t(asset.clusters.size()));
    put<std::uint32_t>(bytes, std::uint32_t(asset.cardIndices.size()));
    put<std::uint32_t>(bytes, std::uint32_t(asset.cardLevels.size()));
    put<std::uint32_t>(bytes, std::uint32_t(asset.crownPositions.size() / 3));
    put<std::uint32_t>(bytes, std::uint32_t(asset.crownIndices.size()));
    put<std::uint32_t>(bytes, std::uint32_t(asset.crownClusters.size()));
    put<std::uint32_t>(bytes, asset.crownLevels);
    put<float>(bytes, asset.crownLayer);
    put<float>(bytes, float(asset.crownCellMetres));
    for (const auto index : asset.indices) put<std::uint32_t>(bytes, index);
    // SCC6 carries the actual replacement positions in a stream parallel to
    // the cluster index stream. Empty is a valid compatibility value for
    // hand-built/test assets; generated DAG assets always provide it.
    if (asset.clusterPositions.size() == asset.indices.size() * 3)
        for (const auto value : asset.clusterPositions) put<float>(bytes, value);
    for (const auto index : asset.cardIndices) put<std::uint32_t>(bytes, index);
    for (const auto& range : asset.cardLevels) {
        put<std::uint32_t>(bytes, range.first);
        put<std::uint32_t>(bytes, range.count);
    }
    for (const auto& cluster : asset.clusters) {
        put<std::uint32_t>(bytes, cluster.indices.first);
        put<std::uint32_t>(bytes, cluster.indices.count);
        for (const float value : cluster.centre) put<float>(bytes, value);
        put<float>(bytes, cluster.radius);
        put<float>(bytes, cluster.error);
        put<float>(bytes, cluster.parentError);
        put<std::uint32_t>(bytes, cluster.level);
        put<std::uint32_t>(bytes, cluster.bornOf);
        put<std::uint32_t>(bytes, cluster.replacedBy);
    }
    for (std::size_t v = 0; v < asset.crownPositions.size() / 3; ++v) {
        for (int axis = 0; axis < 3; ++axis) put<float>(bytes, asset.crownPositions[v * 3 + axis]);
        for (int axis = 0; axis < 3; ++axis)
            put<float>(bytes, v * 3 + 2 < asset.crownNormals.size()
                                      ? asset.crownNormals[v * 3 + axis] : 0.0f);
        put<float>(bytes, v < asset.crownCoverage.size() ? asset.crownCoverage[v] : 1.0f);
        // The layer this vertex wears. Written beside the coverage rather than
        // as a second array, because the two are read together and a vertex
        // that has one without the other has never been useful.
        put<float>(bytes, v < asset.crownLayers.size() ? asset.crownLayers[v] : asset.crownLayer);
    }
    for (const auto index : asset.crownIndices) put<std::uint32_t>(bytes, index);
    for (const auto& cluster : asset.crownClusters) {
        put<std::uint32_t>(bytes, cluster.indices.first);
        put<std::uint32_t>(bytes, cluster.indices.count);
        for (const float value : cluster.centre) put<float>(bytes, value);
        put<float>(bytes, cluster.radius);
        put<float>(bytes, cluster.error);
        put<float>(bytes, cluster.parentError);
        put<std::uint32_t>(bytes, cluster.level);
        put<std::uint32_t>(bytes, cluster.bornOf);
        put<std::uint32_t>(bytes, cluster.replacedBy);
    }
    return bytes;
}

ClusterAsset decodeClusters(std::span<const std::byte> bytes, std::string& why) {
    ClusterAsset asset;
    const auto refuse = [&](const char* reason) {
        why = reason;
        return ClusterAsset{};
    };
    if (bytes.size() < kHeaderBytes) return refuse("cluster sidecar is shorter than its header");
    const bool hierarchy = std::memcmp(bytes.data(), kMagic, 4) == 0 ||
                           std::memcmp(bytes.data(), kHierarchyLegacyMagic, 4) == 0;
    const bool clusterPositionsFormat = std::memcmp(bytes.data(), kMagic, 4) == 0;
    const bool legacy = std::memcmp(bytes.data(), kLegacyMagic, 4) == 0;
    const bool legacyV3 = std::memcmp(bytes.data(), kLegacyMagicV3, 4) == 0;
    if (!hierarchy && !legacy && !legacyV3)
        return refuse("not a cluster sidecar");
    std::size_t at = 4;
    asset.sourceVertices = take<std::uint32_t>(bytes, at);
    asset.sourceTriangles = take<std::uint32_t>(bytes, at);
    asset.cardTriangles = take<std::uint32_t>(bytes, at);
    asset.levels = take<std::uint32_t>(bytes, at);
    asset.dominated = take<std::uint32_t>(bytes, at);
    const auto indexCount = take<std::uint32_t>(bytes, at);
    const auto clusterCount = take<std::uint32_t>(bytes, at);
    const auto cardCount = take<std::uint32_t>(bytes, at);
    const auto cardLevels = take<std::uint32_t>(bytes, at);
    const auto crownVertices = take<std::uint32_t>(bytes, at);
    const auto crownCount = take<std::uint32_t>(bytes, at);
    const auto crownClusters = take<std::uint32_t>(bytes, at);
    asset.crownLevels = take<std::uint32_t>(bytes, at);
    asset.crownLayer = take<float>(bytes, at);
    asset.crownCellMetres = take<float>(bytes, at);
    if (indexCount > kMaxIndices || clusterCount > kMaxClusters || cardCount > kMaxIndices ||
        cardLevels > 8 || crownCount > kMaxIndices || crownClusters > kMaxClusters ||
        crownVertices > kMaxIndices || asset.crownLevels > 32)
        return refuse("cluster sidecar claims more geometry than any model has");
    if (indexCount % 3 || cardCount % 3 || crownCount % 3)
        return refuse("cluster sidecar index count is not whole triangles");
    // SCC3 shipped with seven floats per crown vertex, then gained a layer in
    // SCC4. SCC5 additionally carries the family ids on every cluster.
    // Accept all exact legacy layouts; never interpret a truncated newer file
    // as an older one.
    const auto clusterBytes = hierarchy ? kClusterBytes : kLegacyClusterBytes;
    const std::size_t fixedBytes = kHeaderBytes + std::size_t(indexCount) * 4 +
        std::size_t(cardCount) * 4 + std::size_t(cardLevels) * 8 +
        std::size_t(clusterCount) * clusterBytes + std::size_t(crownCount) * 4 +
        std::size_t(crownClusters) * clusterBytes;
    const std::size_t crownBytes = std::size_t(crownVertices) * 32;
    const std::size_t legacyCrownBytes = std::size_t(crownVertices) * 28;
    const std::size_t positionBytes = std::size_t(indexCount) * 12;
    const bool hasClusterPositions = clusterPositionsFormat &&
        bytes.size() == fixedBytes + positionBytes + crownBytes;
    const bool vertexLayers = bytes.size() == fixedBytes + (hasClusterPositions ? positionBytes : 0) + crownBytes;
    const bool legacyVertexLayers = bytes.size() == fixedBytes + legacyCrownBytes;
    if (!vertexLayers && !(legacyV3 && legacyVertexLayers))
        return refuse("cluster sidecar size does not match what it declares");
    // A model that is entirely alpha cards - a bush - has no solid geometry to
    // cluster and is exactly the case the crown answers, so a sidecar with a
    // crown and nothing else is a complete answer rather than an empty one.
    if ((clusterCount == 0) != (indexCount == 0))
        return refuse("cluster sidecar has clusters without indices, or the other way about");
    if (clusterCount == 0 && crownClusters == 0)
        return refuse("cluster sidecar has neither clusters nor a crown");

    asset.indices.reserve(indexCount);
    for (std::uint32_t i = 0; i < indexCount; ++i) {
        const auto index = take<std::uint32_t>(bytes, at);
        if (index >= asset.sourceVertices) return refuse("cluster sidecar indexes past the mesh");
        asset.indices.push_back(index);
    }
    if (hasClusterPositions) {
        asset.clusterPositions.reserve(std::size_t(indexCount) * 3);
        for (std::uint32_t i = 0; i < indexCount * 3; ++i) {
            const auto value = take<float>(bytes, at);
            if (!std::isfinite(value)) return refuse("a cluster replacement position is not a number");
            asset.clusterPositions.push_back(value);
        }
    }
    asset.cardIndices.reserve(cardCount);
    for (std::uint32_t i = 0; i < cardCount; ++i) {
        const auto index = take<std::uint32_t>(bytes, at);
        if (index >= asset.sourceVertices) return refuse("cluster sidecar indexes past the mesh");
        asset.cardIndices.push_back(index);
    }
    asset.cardLevels.reserve(cardLevels);
    for (std::uint32_t i = 0; i < cardLevels; ++i) {
        IndexRange range;
        range.first = take<std::uint32_t>(bytes, at);
        range.count = take<std::uint32_t>(bytes, at);
        if (range.count % 3 || std::uint64_t(range.first) + range.count > cardCount)
            return refuse("a card level names a range the sidecar does not hold");
        asset.cardLevels.push_back(range);
    }
    const auto readClusters = [&](std::uint32_t count, std::uint32_t indices, std::uint32_t levels,
                                  std::vector<MeshCluster>& into) {
        for (std::uint32_t i = 0; i < count; ++i) {
            MeshCluster cluster;
            cluster.indices.first = take<std::uint32_t>(bytes, at);
            cluster.indices.count = take<std::uint32_t>(bytes, at);
            for (float& value : cluster.centre) value = take<float>(bytes, at);
            cluster.radius = take<float>(bytes, at);
            cluster.error = take<float>(bytes, at);
            cluster.parentError = take<float>(bytes, at);
            cluster.level = take<std::uint32_t>(bytes, at);
            if (hierarchy) {
                cluster.bornOf = take<std::uint32_t>(bytes, at);
                cluster.replacedBy = take<std::uint32_t>(bytes, at);
            }
            if (cluster.indices.count == 0 || cluster.indices.count % 3 ||
                std::uint64_t(cluster.indices.first) + cluster.indices.count > indices)
                return false;
            if (!(cluster.radius > 0) || !std::isfinite(cluster.radius) ||
                !std::isfinite(cluster.error) || cluster.error < 0 ||
                !(cluster.parentError >= cluster.error))
                return false;
            if (cluster.level >= levels) return false;
            if ((cluster.bornOf != MeshCluster::kNoGroup && cluster.bornOf >= kMaxClusters) ||
                (cluster.replacedBy != MeshCluster::kNoGroup && cluster.replacedBy >= kMaxClusters))
                return false;
            into.push_back(cluster);
        }
        return true;
    };

    asset.clusters.reserve(clusterCount);
    for (std::uint32_t i = 0; i < clusterCount; ++i) {
        MeshCluster cluster;
        cluster.indices.first = take<std::uint32_t>(bytes, at);
        cluster.indices.count = take<std::uint32_t>(bytes, at);
        for (float& value : cluster.centre) value = take<float>(bytes, at);
        cluster.radius = take<float>(bytes, at);
        cluster.error = take<float>(bytes, at);
        cluster.parentError = take<float>(bytes, at);
        cluster.level = take<std::uint32_t>(bytes, at);
        if (hierarchy) {
            cluster.bornOf = take<std::uint32_t>(bytes, at);
            cluster.replacedBy = take<std::uint32_t>(bytes, at);
        }
        if (cluster.indices.count == 0 || cluster.indices.count % 3 ||
            std::uint64_t(cluster.indices.first) + cluster.indices.count > indexCount)
            return refuse("a cluster names a range the sidecar does not hold");
        if (!(cluster.radius > 0) || !std::isfinite(cluster.radius) ||
            !std::isfinite(cluster.error) || cluster.error < 0 ||
            !(cluster.parentError >= cluster.error))
            return refuse("a cluster's bound or error is not a usable number");
        if (cluster.level >= asset.levels) return refuse("a cluster names a level past the end");
        if ((cluster.bornOf != MeshCluster::kNoGroup && cluster.bornOf >= kMaxClusters) ||
            (cluster.replacedBy != MeshCluster::kNoGroup && cluster.replacedBy >= kMaxClusters))
            return refuse("a cluster names a family past the sidecar limit");
        asset.clusters.push_back(cluster);
    }
    asset.crownPositions.reserve(std::size_t(crownVertices) * 3);
    asset.crownNormals.reserve(std::size_t(crownVertices) * 3);
    asset.crownCoverage.reserve(crownVertices);
    asset.crownLayers.reserve(crownVertices);
    for (std::uint32_t v = 0; v < crownVertices; ++v) {
        for (int axis = 0; axis < 3; ++axis) asset.crownPositions.push_back(take<float>(bytes, at));
        for (int axis = 0; axis < 3; ++axis) asset.crownNormals.push_back(take<float>(bytes, at));
        asset.crownCoverage.push_back(take<float>(bytes, at));
        asset.crownLayers.push_back(vertexLayers ? take<float>(bytes, at) : asset.crownLayer);
    }
    for (const float value : asset.crownPositions)
        if (!std::isfinite(value)) return refuse("a crown vertex is not a number");
    asset.crownIndices.reserve(crownCount);
    for (std::uint32_t i = 0; i < crownCount; ++i) {
        const auto index = take<std::uint32_t>(bytes, at);
        if (index >= crownVertices) return refuse("the crown indexes past its own vertices");
        asset.crownIndices.push_back(index);
    }

    asset.crownClusters.reserve(crownClusters);
    if (!readClusters(crownClusters, crownCount, asset.crownLevels, asset.crownClusters))
        return refuse("a crown cluster names something the sidecar does not hold");
    why.clear();
    return asset;
}

ClusterAsset buildSourceClusterAsset(std::span<const float> positions,
                                    std::span<const std::uint32_t> indices,
                                    ClusterDagOptions options) {
    options.preserveSourceVertices = true;
    options.conservativeError = true;
    const auto dag = buildClusterDag(positions, indices, options);
    if (dag.clusters.empty()) return {};
    ClusterAsset asset;
    asset.sourceVertices = std::uint32_t(positions.size() / 3);
    asset.sourceTriangles = std::uint32_t(indices.size() / 3);
    asset.levels = std::uint32_t(dag.levels);
    asset.dominated = std::uint32_t(dag.dominated);
    asset.clusters = dag.clusters;
    asset.indices.reserve(dag.indices.size());
    for (const auto index : dag.indices) asset.indices.push_back(dag.sourceVertex[index]);
    appendClusterPositions(asset, dag);
    return asset;
}

ClusterAsset buildClusterAsset(std::span<const float> positions,
                               std::span<const std::uint32_t> indices,
                               std::span<const IndexRange> levels,
                               const ClusterDagOptions& options,
                               std::span<const std::uint64_t> attributeKeys,
                               std::span<const std::uint64_t> hardBoundaryKeys) {
    ClusterAsset asset;
    if (positions.size() < 3 || positions.size() % 3 || indices.size() < 3 || indices.size() % 3 ||
        levels.empty() || levels.size() > 8)
        return asset;
    const auto vertices = std::uint32_t(positions.size() / 3);
    const auto welded = weldPositions(positions);
    if (welded.empty()) return asset;
    for (const auto& level : levels)
        if (level.count == 0 || level.count % 3 ||
            std::uint64_t(level.first) + level.count > indices.size())
            return asset;

    // Which triangles are alpha cards, level by level. A coarse level's cards
    // are not the fine one's: the offline thinning already removed most of them
    // and grew the survivors to compensate, and that is the answer wanted at
    // the distance where the coarse level is drawn.
    const auto split = [&](const IndexRange& level, std::vector<std::uint32_t>& solid,
                           std::vector<std::uint32_t>& cards) {
        std::vector<std::array<std::uint32_t, 3>> welding, original;
        for (std::uint32_t i = 0; i + 2 < level.count; i += 3) {
            const auto at = level.first + i;
            if (indices[at] >= vertices || indices[at + 1] >= vertices ||
                indices[at + 2] >= vertices)
                return false;
            const std::array<std::uint32_t, 3> t{welded[indices[at]], welded[indices[at + 1]],
                                                 welded[indices[at + 2]]};
            if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;   // a line, not a face
            welding.push_back(t);
            original.push_back({indices[at], indices[at + 1], indices[at + 2]});
        }
        if (welding.empty()) return true;
        const auto pieces = components(welded, welding);
        for (std::size_t i = 0; i < welding.size(); ++i) {
            auto& into = pieces.size[pieces.of[i]] <= 2 ? cards : solid;
            into.insert(into.end(), original[i].begin(), original[i].end());
        }
        return true;
    };

    asset.sourceVertices = vertices;
    asset.sourceTriangles = levels.front().count / 3;

    // A DAG over 960 separate two-triangle quads has nothing to group and
    // nothing to collapse. The cards come out; the solid part is clustered.
    std::vector<std::uint32_t> solid;
    for (std::uint32_t level = 0; level < levels.size(); ++level) {
        std::vector<std::uint32_t> mine, cards;
        if (!split(levels[level], mine, cards)) return {};
        if (level == 0) {
            solid = std::move(mine);
            asset.cardTriangles = std::uint32_t(cards.size() / 3);
        }
        asset.cardLevels.push_back({std::uint32_t(asset.cardIndices.size()),
                                    std::uint32_t(cards.size())});
        asset.cardIndices.insert(asset.cardIndices.end(), cards.begin(), cards.end());
    }
    if (solid.empty()) {
        asset.cardIndices.clear();
        asset.cardLevels.clear();
        return asset;
    }

    // The production mesh carries normals, UVs, colours and texture-layer
    // selectors beside these positions. When the importer supplied exact
    // attribute identities, weld only matching identities; without them keep
    // source vertices as hard boundaries rather than guessing which attribute
    // should survive. `welded` above remains intentionally limited to
    // topology/card analysis.
    auto hierarchyOptions = options;
    hierarchyOptions.preserveSourceVertices = attributeKeys.size() != vertices;
    hierarchyOptions.attributeKeys = attributeKeys;
    hierarchyOptions.hardBoundaryKeys = hardBoundaryKeys;
    hierarchyOptions.conservativeError = true;
    const auto dag = buildClusterDag(positions, solid, hierarchyOptions);
    if (dag.clusters.empty()) {
        asset.cardIndices.clear();
        asset.cardLevels.clear();
        return asset;
    }
    asset.levels = std::uint32_t(dag.levels);
    asset.dominated = std::uint32_t(dag.dominated);
    asset.clusters = dag.clusters;
    // Back into the caller's own numbering, so the renderer keeps one vertex
    // buffer and every level shares it.
    asset.indices.reserve(dag.indices.size());
    for (const auto index : dag.indices) asset.indices.push_back(dag.sourceVertex[index]);
    appendClusterPositions(asset, dag);
    return asset;
}

void buildCrown(ClusterAsset& asset, std::span<const float> positions,
                std::span<const std::uint32_t> indices, std::span<const IndexRange> levels,
                double cellMetres, std::span<const float> layers,
                const ClusterDagOptions& options) {
    asset.crownPositions.clear();
    asset.crownNormals.clear();
    asset.crownCoverage.clear();
    asset.crownLayers.clear();
    asset.crownIndices.clear();
    asset.crownClusters.clear();
    asset.crownLevels = 0;
    asset.crownCellMetres = 0;
    if (levels.empty() || !(cellMetres > 0)) return;
    const IndexRange& finest = levels.front();
    if (finest.count == 0 || std::uint64_t(finest.first) + finest.count > indices.size()) return;

    std::vector<std::array<std::uint32_t, 4>> cardCorners;
    auto cards = cardsOf(positions, indices.subspan(finest.first, finest.count), &cardCorners);
    const auto welded = weldPositions(positions);
    if (welded.empty()) return;
    std::vector<bool> cardVertex(*std::max_element(welded.begin(), welded.end()) + 1, false);
    for (std::size_t i = 0; i < cards.size(); ++i) {
        cards[i].layer = cardCorners[i][0] < layers.size() ? layers[cardCorners[i][0]] : asset.crownLayer;
        for (const auto v : cardCorners[i]) cardVertex[welded[v]] = true;
    }
    const auto cardCount = cards.size();
    // And the solid geometry, as quads with a doubled corner. A trunk is part
    // of the mass at the distance this is used, and leaving it out is how the
    // shell ends up costing more than the chain it was meant to replace.
    {
        std::vector<std::array<std::uint32_t, 3>> faces;
        if (!welded.empty()) {
            for (std::uint32_t i = 0; i + 2 < finest.count; i += 3) {
                const auto at = finest.first + i;
                const std::array<std::uint32_t, 3> t{welded[indices[at]], welded[indices[at + 1]],
                                                     welded[indices[at + 2]]};
                if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
                // These faces already contributed their leaf area above.
                if (cardVertex[t[0]] && cardVertex[t[1]] && cardVertex[t[2]]) continue;
                faces.push_back({indices[at], indices[at + 1], indices[at + 2]});
            }
        }
        // Only solids: duplicating foliage doubles its density and closes gaps.
        for (const auto& face : faces) {
            Quad quad;
            for (int i = 0; i < 4; ++i) {
                const auto v = face[std::min(i, 2)];
                for (int axis = 0; axis < 3; ++axis)
                    quad.corner[i][axis] = positions[std::size_t(v) * 3 + axis];
            }
            quad.layer = face[0] < layers.size() ? layers[face[0]] : asset.crownLayer;
            cards.push_back(quad);
        }
    }
    // Below a handful there is no mass to merge - a model with three leaves is
    // better served by its three leaves.
    if (cards.size() < 16) return;
    // Leaves stop being separate leaves at about the size of a leaf. A cell
    // much finer than a card resolves each one again and the merge produces
    // more geometry than it replaced - a nine-hundred-triangle bush came out as
    // thirty-five thousand that way, at a cell a fortieth of its own height.
    double card = 0;
    for (std::size_t at = 0; at < cardCount; ++at) {
        const Quad& quad = cards[at];
        double longest = 0;
        for (int i = 0; i < 4; ++i) {
            double side = 0;
            for (int axis = 0; axis < 3; ++axis) {
                const double d = quad.corner[(i + 1) % 4][axis] - quad.corner[i][axis];
                side += d * d;
            }
            longest = std::max(longest, std::sqrt(side));
        }
        card += longest;
    }
    // Only the real cards set the floor. Solid triangles were added to the
    // input as degenerate quads, and a triangle is not the scale at which
    // anything stops being separate - counting them dragged a pine's cell to a
    // quarter of a metre and produced a shell twice the size of the model.
    card = cardCount ? card / double(cardCount) : 0.0;
    QuadMassOptions merge;
    merge.cellMetres = std::max(cellMetres, card * 0.75);
    const auto mass = mergeQuadMass(cards, merge);
    if (mass.empty()) return;

    auto hierarchyOptions = options;
    hierarchyOptions.conservativeError = true;
    const auto dag = buildClusterDag(mass.positions, mass.indices, hierarchyOptions);
    if (dag.clusters.empty()) return;
    asset.crownCellMetres = mass.cellMetres;
    asset.crownLevels = std::uint32_t(dag.levels);
    asset.crownClusters = dag.clusters;
    // DAG error is measured against the reconstructed shell, NOT the model.
    // Splatting, surface extraction and relaxation already move its surface
    // by a cell-scale distance. Never offer that approximation as zero-error
    // geometry merely because it has fewer triangles than the original tree.
    const float reconstructionError = float(2.0 * mass.cellMetres);
    for (auto& cluster : asset.crownClusters) {
        cluster.error += reconstructionError;
        cluster.parentError += reconstructionError;
        // Adding the reconstruction term can round a one-ulp child/parent
        // interval back to the same float. The cluster cut needs a strict
        // interval after every error contribution, not only before it.
        if (std::isfinite(cluster.parentError) && cluster.parentError <= cluster.error)
            cluster.parentError = std::nextafter(cluster.error,
                                                 std::numeric_limits<float>::infinity());
    }
    // The DAG welds, so its vertices are its own numbering; the crown travels
    // with its geometry, so the vertices it kept are the ones written out.
    asset.crownPositions = dag.positions;
    asset.crownNormals.resize(dag.positions.size(), 0.0f);
    asset.crownCoverage.resize(dag.positions.size() / 3, 1.0f);
    asset.crownLayers.assign(dag.positions.size() / 3, asset.crownLayer);
    for (std::size_t v = 0; v < dag.sourceVertex.size(); ++v) {
        const auto source = dag.sourceVertex[v];
        if (std::size_t(source) * 3 + 2 < mass.normals.size())
            for (int axis = 0; axis < 3; ++axis)
                asset.crownNormals[v * 3 + axis] = mass.normals[std::size_t(source) * 3 + axis];
        if (source < mass.coverage.size()) asset.crownCoverage[v] = mass.coverage[source];
        if (!layers.empty() && source < mass.layer.size())
            asset.crownLayers[v] = mass.layer[source];
    }
    asset.crownIndices = dag.indices;
}

} // namespace engine::geometry
