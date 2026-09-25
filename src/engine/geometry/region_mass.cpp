#include "engine/geometry/region_mass.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace engine::geometry {
namespace {

bool finite(const float* values, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i)
        if (!std::isfinite(values[i])) return false;
    return true;
}

// The member's own placement, applied to a model-local point. Uniform scale and
// a turn about +Z: exactly what the instance stream carries, so the mass is
// merged from the objects the renderer would otherwise have drawn one by one.
void place(const RegionMember& member, double cosYaw, double sinYaw,
           const float* local, float* out) {
    const double x = double(local[0]) * member.scale, y = double(local[1]) * member.scale;
    out[0] = float(member.position[0] + cosYaw * x - sinYaw * y);
    out[1] = float(member.position[1] + sinYaw * x + cosYaw * y);
    out[2] = float(member.position[2] + double(local[2]) * member.scale);
}

} // namespace

int chooseRegionMassTier(std::span<const RegionMassTier> tiers, double allowedError) {    if (!(allowedError > 0) || !std::isfinite(allowedError)) return -1;
    int chosen = -1;
    for (std::size_t at = 0; at < tiers.size(); ++at) {
        const auto& tier = tiers[at];
        if (!tier.slots || !tier.vertices || !tier.indices) continue;
        if (!(tier.cellMetres > 0) || !std::isfinite(tier.cellMetres)) continue;
        if (tier.error() > allowedError) continue;
        if (chosen < 0 || tier.cellMetres > tiers[std::size_t(chosen)].cellMetres)
            chosen = int(at);
    }
    return chosen;
}

std::span<const RegionMassTier> defaultRegionMassTiers() {
    // Capacities measured, not guessed: tests/test_region_mass.cpp bakes a real
    // 128 m forest region at each of these cells and prints what it comes to.
    //
    // The SLOT COUNTS are measured too, and the first attempt got them wrong.
    // Spreading them evenly assumed the far tiers would carry most of the
    // demand, because there is more world out there. In a real frame they
    // carried none: a region only reaches the 16 m tier once it is about seven
    // pixels across, and by then it is past the horizon or behind the terrain.
    // The finest tier is where the demand actually is, and giving it sixteen
    // slots made the pool cover fewer regions than the flat pool it replaced.
    // So the finest tier keeps the whole of the old pool, and the coarser tiers
    // are what the extra bytes buy.
    static constexpr RegionMassTier tiers[]{
        {4.0, 48, 6144, 65536},
        {8.0, 64, 1536, 16384},
        {16.0, 64, 768, 8192},
    };
    return tiers;
}

RegionMass bakeRegionMass(std::span<const RegionMember> members,
                          std::span<const RegionSource> sources,
                          const RegionMassOptions& options) {
    RegionMass result;
    if (members.size() < std::max<std::size_t>(options.minimumMembers, 1) ||
        !(options.cellMetres > 0) || !std::isfinite(options.cellMetres) || !options.maxQuads)
        return result;

    // Every model referenced is decomposed once, not once per member: a region
    // holds many copies of a handful of models, and cards are found by walking
    // connected components.
    struct Decomposed {
        std::vector<Quad> cards;              // the model's own alpha quads
        std::vector<std::array<std::uint32_t, 3>> solids;
        std::vector<float> layerOfCard, layerOfSolid;
        double cardSide = 0;                  // mean longest side of a card
        bool built = false, usable = false;
    };
    std::vector<Decomposed> models(sources.size());
    const auto decompose = [&](std::uint32_t model) -> const Decomposed& {
        Decomposed& out = models[model];
        if (out.built) return out;
        out.built = true;
        const RegionSource& source = sources[model];
        if (source.positions.size() < 9 || source.indices.size() < 3 ||
            source.positions.size() % 3) return out;
        for (const auto index : source.indices)
            if (std::size_t(index) * 3 + 2 >= source.positions.size()) return out;
        if (!finite(source.positions.data(), source.positions.size())) return out;

        std::vector<std::array<std::uint32_t, 4>> corners;
        out.cards = cardsOf(source.positions, source.indices, &corners);
        out.layerOfCard.reserve(out.cards.size());
        std::vector<bool> cardVertex(source.positions.size() / 3, false);
        for (std::size_t i = 0; i < out.cards.size(); ++i) {
            const auto first = corners[i][0];
            out.layerOfCard.push_back(first < source.layers.size() ? source.layers[first]
                                                                   : source.layer);
            for (const auto v : corners[i]) if (v < cardVertex.size()) cardVertex[v] = true;
            double longest = 0;
            for (int c = 0; c < 4; ++c) {
                double side = 0;
                for (int axis = 0; axis < 3; ++axis) {
                    const double d = out.cards[i].corner[(c + 1) % 4][axis] -
                                     out.cards[i].corner[c][axis];
                    side += d * d;
                }
                longest = std::max(longest, std::sqrt(side));
            }
            out.cardSide += longest;
        }
        out.cardSide = out.cards.empty() ? 0.0 : out.cardSide / double(out.cards.size());
        // Solid triangles join the mass as degenerate quads, as a single crown
        // does: a trunk is part of what a region looks like from far away, and
        // a shell of foliage alone floats above bare ground.
        for (std::size_t i = 0; i + 2 < source.indices.size(); i += 3) {
            const std::array<std::uint32_t, 3> face{source.indices[i], source.indices[i + 1],
                                                    source.indices[i + 2]};
            if (face[0] == face[1] || face[1] == face[2] || face[0] == face[2]) continue;
            if (face[0] < cardVertex.size() && face[1] < cardVertex.size() &&
                face[2] < cardVertex.size() && cardVertex[face[0]] && cardVertex[face[1]] &&
                cardVertex[face[2]])
                continue; // already counted as a card
            out.solids.push_back(face);
            out.layerOfSolid.push_back(face[0] < source.layers.size() ? source.layers[face[0]]
                                                                     : source.layer);
        }
        out.usable = !out.cards.empty() || !out.solids.empty();
        return out;
    };

    std::size_t quads = 0;
    for (const auto& member : members) {
        if (member.model >= sources.size()) return {};
        if (!std::isfinite(member.scale) || !(member.scale > 0) ||
            !std::isfinite(member.yaw) || !finite(member.position, 3)) return {};
        const auto& model = decompose(member.model);
        if (!model.usable) return {};
        quads += model.cards.size() + model.solids.size();
        if (quads > options.maxQuads) break; // the cell grows below; nothing is dropped
    }

    std::vector<Quad> mass;
    mass.reserve(std::min<std::size_t>(quads, options.maxQuads));
    double cardSide = 0;
    std::size_t cardCount = 0;
    float low[3]{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::infinity()};
    float high[3]{-low[0], -low[1], -low[2]};
    for (const auto& member : members) {
        const auto& model = models[member.model];
        const double cosYaw = std::cos(double(member.yaw)), sinYaw = std::sin(double(member.yaw));
        const auto keep = [&](const Quad& quad) {
            mass.push_back(quad);
            for (int c = 0; c < 4; ++c)
                for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], quad.corner[c][axis]);
                    high[axis] = std::max(high[axis], quad.corner[c][axis]);
                }
        };
        for (std::size_t i = 0; i < model.cards.size(); ++i) {
            Quad quad;
            for (int c = 0; c < 4; ++c) place(member, cosYaw, sinYaw, model.cards[i].corner[c],
                                              quad.corner[c]);
            quad.coverage = model.cards[i].coverage;
            quad.layer = model.layerOfCard[i];
            keep(quad);
        }
        cardSide += model.cardSide * model.cards.size() * member.scale;
        cardCount += model.cards.size();
        const RegionSource& source = sources[member.model];
        for (std::size_t i = 0; i < model.solids.size(); ++i) {
            Quad quad;
            for (int c = 0; c < 4; ++c) {
                const auto v = model.solids[i][std::size_t(std::min(c, 2))];
                place(member, cosYaw, sinYaw, &source.positions[std::size_t(v) * 3],
                      quad.corner[c]);
            }
            quad.layer = model.layerOfSolid[i];
            keep(quad);
        }
    }
    if (mass.empty()) return result;

    QuadMassOptions merge;
    // Never finer than the cards themselves: below that the merge resolves each
    // leaf again and a region comes out heavier than the trees it replaces.
    const double card = cardCount ? cardSide / double(cardCount) : 0.0;
    merge.cellMetres = std::max(options.cellMetres, card * 0.75);
    const auto merged = mergeQuadMass(mass, merge);
    if (merged.empty()) return result;

    auto dagOptions = options.dag;
    dagOptions.conservativeError = true;
    if (options.clusterTriangles) dagOptions.clusterTriangles = options.clusterTriangles;
    const auto dag = buildClusterDag(merged.positions, merged.indices, dagOptions);
    if (dag.clusters.empty()) return result;

    result.clusters = dag.clusters;
    // What the surface already lost before a single triangle was simplified:
    // splatting, extraction and relaxation move it by a cell, and the members
    // themselves are gone. Offering that as zero-error geometry is how an
    // aggregate gets selected in place of objects it does not resemble.
    const float reconstruction = float(2.0 * merged.cellMetres);
    for (auto& cluster : result.clusters) {
        cluster.error += reconstruction;
        cluster.parentError += reconstruction;
        if (std::isfinite(cluster.parentError) && cluster.parentError <= cluster.error)
            cluster.parentError = std::nextafter(cluster.error,
                                                 std::numeric_limits<float>::infinity());
    }
    result.positions = dag.positions;
    result.indices = dag.indices;
    result.levels = std::uint32_t(dag.levels);
    result.normals.assign(dag.positions.size(), 0.0f);
    result.coverage.assign(dag.positions.size() / 3, 1.0f);
    result.layer.assign(dag.positions.size() / 3, 0.0f);
    for (std::size_t v = 0; v < dag.sourceVertex.size(); ++v) {
        const auto source = dag.sourceVertex[v];
        if (std::size_t(source) * 3 + 2 < merged.normals.size())
            for (int axis = 0; axis < 3; ++axis)
                result.normals[v * 3 + axis] = merged.normals[std::size_t(source) * 3 + axis];
        if (source < merged.coverage.size()) result.coverage[v] = merged.coverage[source];
        if (source < merged.layer.size()) result.layer[v] = merged.layer[source];
    }
    // Measured from the merged input, so the bound covers the members even
    // where the extracted surface pulled inside them.
    for (int axis = 0; axis < 3; ++axis) result.centre[axis] = (low[axis] + high[axis]) * 0.5f;
    double radius = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const double half = (double(high[axis]) - double(low[axis])) * 0.5;
        radius += half * half;
    }
    result.radius = float(std::sqrt(radius));
    result.members = members.size();
    result.quads = mass.size();
    for (const auto& cluster : result.clusters)
        if (cluster.level == 0) result.finestTriangles += cluster.indices.count / 3;
    result.cellMetres = merged.cellMetres;
    result.coarsened = merged.coarsened || merge.cellMetres > options.cellMetres;
    return result;
}

} // namespace engine::geometry

