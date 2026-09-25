#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"
#include "engine/geometry/cluster_asset.hpp"
#include "engine/geometry/quad_mass.hpp"

namespace {
using namespace engine::geometry;

// A crown: alpha cards scattered over a shell, each facing outward, which is
// what an imported tree's foliage actually is.
std::vector<Quad> crown(int count, double radius = 4.0, double card = 1.2, std::uint32_t seed = 7) {
    std::mt19937 noise(seed);
    std::uniform_real_distribution<double> unit(-1.0, 1.0);
    std::vector<Quad> cards;
    while (int(cards.size()) < count) {
        double n[3]{unit(noise), unit(noise), unit(noise)};
        const double size = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (size < 0.2) continue;
        for (double& v : n) v /= size;
        // A frame on the sphere, so the card lies in the surface.
        double u[3]{-n[1], n[0], 0};
        double axis = std::sqrt(u[0] * u[0] + u[1] * u[1]);
        if (axis < 1e-6) { u[0] = 1; u[1] = 0; axis = 1; }
        for (double& v : u) v /= axis;
        const double w[3]{n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2],
                          n[0] * u[1] - n[1] * u[0]};
        Quad quad;
        quad.coverage = 0.45f;   // alpha-cut: most of a leaf card is hole
        const double signs[4][2]{{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int c = 0; c < 4; ++c)
            for (int axisIndex = 0; axisIndex < 3; ++axisIndex)
                quad.corner[c][axisIndex] =
                        float(n[axisIndex] * radius + u[axisIndex] * signs[c][0] * card * 0.5 +
                              w[axisIndex] * signs[c][1] * card * 0.5);
        cards.push_back(quad);
    }
    return cards;
}

std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeUse(const QuadMass& mass) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    for (std::size_t t = 0; t + 2 < mass.indices.size(); t += 3)
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(mass.indices[t + e], mass.indices[t + (e + 1) % 3]);
            ++uses[{key.first, key.second}];
        }
    return uses;
}

double distanceFromCentre(const QuadMass& mass, std::size_t v) {
    double sum = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const double d = mass.positions[v * 3 + axis];
        sum += d * d;
    }
    return std::sqrt(sum);
}
}

TEST(quad_mass_turns_a_crown_of_cards_into_one_surface) {
    // The whole point. 960 separate two-triangle quads have nothing to group
    // and nothing to collapse; the surface of the MASS they form has both.
    const auto cards = crown(960);
    const auto mass = mergeQuadMass(cards, {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2});
    CHECK_EQ(mass.quads, std::size_t(960));
    CHECK(!mass.empty());
    CHECK(mass.solidCells > 50);
    // NOT fewer triangles at the finest level, and the test says so rather than
    // implying otherwise: the shell of a four-metre crown at sixty-centimetre
    // cells is 2 200 triangles against the cards' 1 920. That is the wrong
    // claim to make for this.
    //
    // What the merge buys is that this is ONE surface. It simplifies - through
    // the ordinary DAG, to 68 triangles, measured below - and 960 separate
    // quads do not simplify at all, they only thin. And 68 triangles of a real
    // surface are right from every angle, where a billboard is a picture of one
    // side of the tree seen from wherever it was baked.
    CHECK(mass.triangles() < cards.size() * 4);
    CHECK(mass.filledCells > 0);   // the inside is closed, so it is not drawn twice
    CHECK_EQ(mass.positions.size() / 3, mass.normals.size() / 3);
    CHECK_EQ(mass.positions.size() / 3, mass.coverage.size());
    for (const float value : mass.positions) CHECK(std::isfinite(value));
    for (const float value : mass.normals) CHECK(std::isfinite(value));

    // And it really is the shell the cards sat on: every vertex near the
    // sphere they were scattered over, none inside it and none far outside.
    double lowest = 1e9, highest = 0;
    for (std::size_t v = 0; v < mass.positions.size() / 3; ++v) {
        const double d = distanceFromCentre(mass, v);
        lowest = std::min(lowest, d);
        highest = std::max(highest, d);
    }
    CHECK(lowest > 2.0);
    CHECK(highest < 6.5);
}

TEST(quad_mass_filled_air_does_not_change_the_leaf_material) {
    auto cards = crown(960);
    for (auto& card : cards) card.layer = 7.0f;
    const auto mass = mergeQuadMass(cards, {.cellMetres = 0.6, .solidDensity = 0.12});
    CHECK(!mass.empty());
    CHECK(mass.filledCells > 0);
    for (const auto layer : mass.layer) CHECK_EQ(layer, 7.0f);
}

TEST(quad_mass_material_ids_are_not_blended_into_unrelated_textures) {
    auto cards = crown(960);
    for (std::size_t i = 0; i < cards.size(); ++i) cards[i].layer = i % 2 ? 1.0f : 7.0f;
    const auto mass = mergeQuadMass(cards, {.cellMetres = 0.6, .solidDensity = 0.12});
    CHECK(!mass.empty());
    for (const auto layer : mass.layer) CHECK(layer == 1.0f || layer == 7.0f);
}

TEST(crown_preserves_card_material_and_accounts_for_retopology_error) {
    const auto cards = crown(600);
    std::vector<float> positions, layers;
    std::vector<std::uint32_t> indices;
    for (const auto& card : cards) {
        const auto base = std::uint32_t(layers.size());
        for (const auto& corner : card.corner) {
            positions.insert(positions.end(), corner, corner + 3);
            layers.push_back(7.0f);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    const engine::IndexRange levels[]{{0, std::uint32_t(indices.size())}};
    ClusterAsset asset;
    buildCrown(asset, positions, indices, levels, 0.9, layers);
    CHECK(!asset.crownClusters.empty());
    for (const auto layer : asset.crownLayers) CHECK_EQ(layer, 7.0f);
    for (const auto& cluster : asset.crownClusters) {
        CHECK(cluster.error >= float(2 * asset.crownCellMetres));
        CHECK(cluster.parentError > cluster.error);
    }
}

TEST(quad_mass_closes_the_shell_it_builds) {
    // An open shell is a hole seen from the other side, and the cards it
    // replaced had no inside at all - so the merge must not invent one that is
    // visible. Every edge carries two faces.
    const auto mass = mergeQuadMass(crown(600), {.cellMetres = 0.7, .solidDensity = 0.12, .relax = 1});
    CHECK(!mass.empty());
    std::size_t open = 0, folded = 0;
    for (const auto& [edge, uses] : edgeUse(mass)) {
        if (uses < 2) ++open;
        if (uses > 2) ++folded;
    }
    CHECK_EQ(open, std::size_t(0));
    CHECK_EQ(folded, std::size_t(0));
}

TEST(quad_mass_keeps_a_lacy_crown_lacy_rather_than_making_a_potato) {
    // Coverage is what survives the merge. A canopy is mostly air; a surface
    // that claimed to be solid would read as a green blob at exactly the
    // distance where the merge is used.
    const auto sparse = mergeQuadMass(crown(200, 4.0, 1.2), {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2});
    const auto dense = mergeQuadMass(crown(1600, 4.0, 1.2), {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2});
    CHECK(!sparse.empty());
    CHECK(!dense.empty());
    const auto average = [](const QuadMass& mass) {
        double total = 0;
        for (const float value : mass.coverage) total += value;
        return mass.coverage.empty() ? 0.0 : total / double(mass.coverage.size());
    };
    // Denser foliage reads as more solid. Measured over the neighbourhood: at
    // the vertex itself the density is the threshold for every mass, because
    // that is what putting the vertex on the level set means.
    CHECK(average(dense) > average(sparse));
    for (const float value : sparse.coverage) CHECK(value >= 0 && value <= 1);
    for (const float value : dense.coverage) CHECK(value >= 0 && value <= 1);
    // A card that is mostly hole contributes less than a solid one of the same
    // size, or the shell encloses the bounding box of the foliage rather than
    // the foliage.
    auto solid = crown(400);
    for (auto& quad : solid) quad.coverage = 1.0f;
    const auto opaque = mergeQuadMass(solid, {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2});
    CHECK(average(opaque) > average(mergeQuadMass(crown(400), {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2})));
}

TEST(quad_mass_cell_size_is_the_scale_at_which_cards_stop_being_separate) {
    // The one control, and it behaves like one: coarser cells, fewer triangles,
    // same shell. That is what makes the same algorithm serve a crown and a
    // grove - the grove is this with a bigger number.
    std::size_t previous = 0;
    for (const double cell : {0.4, 0.8, 1.6, 3.2}) {
        const auto mass = mergeQuadMass(crown(800), {.cellMetres = cell, .solidDensity = 0.12, .relax = 1});
        CHECK(!mass.empty());
        CHECK_EQ(mass.cellMetres, cell);
        if (previous) CHECK(mass.triangles() < previous);
        previous = mass.triangles();
    }
    CHECK(previous > 0);
}

TEST(quad_mass_is_a_mesh_the_cluster_build_can_take_from_here) {
    // The reason this is worth doing at all: once the crown is a surface, the
    // DAG applies to it, and the coarse end of that DAG is what a billboard
    // used to be - not a special case beside the geometry, the last level of it.
    const auto mass = mergeQuadMass(crown(900), {.cellMetres = 0.6, .solidDensity = 0.12, .relax = 2});
    CHECK(mass.triangles() > 64);
    const auto dag = buildClusterDag(mass.positions, mass.indices);
    CHECK(dag.levels > 1);
    CHECK_EQ(dag.trianglesAt(0), mass.triangles());
    // The number that matters: a crown of 900 cards ends as this many triangles
    // of a surface, where the cards themselves could only be thinned and then
    // replaced by a billboard.
    CHECK(dag.trianglesAt(dag.levels - 1) < 128);
    CHECK(dag.trianglesAt(dag.levels - 1) * 8 < dag.trianglesAt(0));
    // And the cut stays watertight, which is the property the whole DAG rests
    // on - a shell that opened at a seam would show sky through a crown.
    for (int step = 0; step <= 40; ++step) {
        const auto cut = cutAt(dag, step * 0.05);
        CHECK(!cut.empty());
        std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
        for (const auto index : cut) {
            const auto& range = dag.clusters[index].indices;
            for (std::uint32_t i = 0; i < range.count; i += 3)
                for (int e = 0; e < 3; ++e) {
                    const auto a = dag.indices[range.first + i + e];
                    const auto b = dag.indices[range.first + i + (e + 1) % 3];
                    const auto key = std::minmax(a, b);
                    ++uses[{key.first, key.second}];
                }
        }
        for (const auto& [edge, count] : uses) CHECK_EQ(count, 2);
    }
}

TEST(quad_mass_finds_the_cards_in_a_mesh_that_has_them) {
    // The input the merge wants, out of the mesh an importer produced: every
    // connected component of exactly two triangles, as one quad.
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    for (int i = 0; i < 40; ++i) {
        const auto base = std::uint32_t(positions.size() / 3);
        const double x = i * 3.0;
        for (int c = 0; c < 4; ++c) {
            positions.push_back(float(x + (c & 1)));
            positions.push_back(float(c >> 1));
            positions.push_back(0.0f);
        }
        indices.insert(indices.end(), {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    // And a solid piece beside them, which is not a card and must not come out
    // as one: two triangles that are part of something larger.
    const auto solid = std::uint32_t(positions.size() / 3);
    for (int c = 0; c < 6; ++c) {
        positions.push_back(float(200 + c));
        positions.push_back(float(c % 3));
        positions.push_back(0.0f);
    }
    indices.insert(indices.end(), {solid, solid + 1, solid + 2, solid + 1, solid + 2, solid + 3,
                                   solid + 2, solid + 3, solid + 4, solid + 3, solid + 4, solid + 5});

    const auto cards = cardsOf(positions, indices);
    CHECK_EQ(cards.size(), std::size_t(40));
    for (const auto& quad : cards) {
        // The corners walk the quad's rim rather than coming out in index
        // order: opposite corners are the diagonal, not an edge.
        const double side = std::hypot(quad.corner[1][0] - quad.corner[0][0],
                                       quad.corner[1][1] - quad.corner[0][1]);
        const double diagonal = std::hypot(quad.corner[2][0] - quad.corner[0][0],
                                           quad.corner[2][1] - quad.corner[0][1]);
        CHECK(diagonal > side);
    }
}

TEST(quad_mass_uv_split_cards_keep_their_original_coordinates) {
    const auto source = crown(40);
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    // Importers duplicate vertices at UV seams. The fourth distinct position
    // then has welded ID 3 but original ID 5, not 3 (which repeats corner 0).
    for (const auto& card : source)
        for (const int corner : {0, 1, 2, 0, 2, 3}) {
            indices.push_back(std::uint32_t(positions.size() / 3));
            positions.insert(positions.end(), card.corner[corner], card.corner[corner] + 3);
        }
    std::vector<std::array<std::uint32_t, 4>> corners;
    auto cards = cardsOf(positions, indices, &corners);
    CHECK_EQ(cards.size(), source.size());
    CHECK_EQ(corners.size(), cards.size());
    if (cards.size() != source.size() || corners.size() != cards.size()) return;
    for (std::size_t q = 0; q < cards.size(); ++q) {
        cards[q].coverage = source[q].coverage;
        for (int c = 0; c < 4; ++c)
            for (int axis = 0; axis < 3; ++axis) {
                CHECK_EQ(cards[q].corner[c][axis], positions[std::size_t(corners[q][c]) * 3 + axis]);
                CHECK_EQ(cards[q].corner[c][axis], source[q].corner[c][axis]);
            }
    }
    const auto original = mergeQuadMass(source);
    const auto imported = mergeQuadMass(cards);
    CHECK(!original.empty());
    CHECK(original.positions == imported.positions);
    CHECK(original.indices == imported.indices);
    CHECK(original.coverage == imported.coverage);
}

TEST(quad_mass_is_the_same_mass_twice_and_refuses_what_it_cannot_merge) {
    const auto cards = crown(300);
    const auto first = mergeQuadMass(cards);
    const auto again = mergeQuadMass(cards);
    CHECK(first.positions == again.positions);
    CHECK(first.indices == again.indices);
    CHECK(first.coverage == again.coverage);

    CHECK(mergeQuadMass({}).empty());
    CHECK(mergeQuadMass(cards, {.cellMetres = 0.0, .solidDensity = 0.12, .relax = 2}).empty());
    CHECK(mergeQuadMass(cards, {.cellMetres = 0.6, .solidDensity = 0.0, .relax = 2}).empty());
    // A quad that is not a number is skipped and the rest are counted: three
    // of four here, all of them degenerate, so there is no mass and no surface.
    std::vector<Quad> broken(4);
    broken[0].corner[0][1] = std::numeric_limits<float>::quiet_NaN();
    const auto nothing = mergeQuadMass(broken);
    CHECK_EQ(nothing.quads, std::size_t(3));
    CHECK(nothing.empty());
    CHECK(cardsOf({}, {}).empty());

    // A budget too small for the cell size asked for coarsens rather than
    // failing: a bound on the work is not a reason to produce nothing.
    const auto cramped = mergeQuadMass(crown(400, 40.0, 2.0), {.cellMetres = 0.05, .solidDensity = 0.12, .relax = 1, .maxSamples = 1u << 16});
    CHECK(cramped.coarsened);
    CHECK(cramped.cellMetres > 0.05);
    CHECK(!cramped.empty());
}
