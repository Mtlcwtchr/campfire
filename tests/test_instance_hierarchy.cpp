#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "engine/geometry/instance_hierarchy.hpp"
#include "engine/render/systems/instance_hierarchy_select.hpp"

namespace {
using engine::geometry::InstanceHierarchyNode;
using engine::geometry::InstanceRepresentation;
using engine::geometry::InstanceSource;
using engine::geometry::buildInstanceHierarchy;
using engine::geometry::cutAtInstanceHierarchy;

std::vector<InstanceSource> grove() {
    std::vector<InstanceSource> result;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            InstanceSource source;
            source.position[0] = float(x * 2);
            source.position[1] = float(y * 2);
            source.position[2] = float((x + y) % 3);
            source.scale = 1;
            source.radius = 0.75f;
            source.variant = std::uint32_t((x + y) % 2);
            result.push_back(source);
        }
    return result;
}
}

TEST(instance_hierarchy_builds_near_mid_far_and_very_far_representations) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    const auto hierarchy = buildInstanceHierarchy(grove(), options);
    CHECK_EQ(hierarchy.sources.size(), std::size_t(64));
    CHECK(hierarchy.levels >= 3);

    bool individual = false, merged = false, canopy = false, density = false;
    for (const auto& node : hierarchy.nodes) {
        individual |= node.representation == InstanceRepresentation::Individual;
        merged |= node.representation == InstanceRepresentation::MergedGeometry;
        canopy |= node.representation == InstanceRepresentation::CanopyProxy;
        density |= node.representation == InstanceRepresentation::DensityShading;
        CHECK(node.memberCount > 0);
        CHECK(std::uint64_t(node.memberFirst) + node.memberCount <= hierarchy.members.size());
        CHECK(std::uint64_t(node.childFirst) + node.childCount <= hierarchy.children.size());
        for (const auto child : hierarchy.childrenOf(node)) CHECK(child < hierarchy.nodes.size());
        CHECK(node.radius > 0);
        CHECK(std::isfinite(node.error));
        CHECK(node.parentError >= node.error);
    }
    CHECK(individual);
    CHECK(merged);
    CHECK(canopy);
    CHECK(density);
}

TEST(instance_hierarchy_cuts_one_representation_per_branch) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    const auto hierarchy = buildInstanceHierarchy(grove(), options);
    for (const double allowance : {0.0, 0.1, 1.0, 10.0, 100.0}) {
        std::vector<std::uint32_t> cut;
        CHECK(cutAtInstanceHierarchy(hierarchy.nodes, allowance, cut));
        CHECK(!cut.empty());
        std::vector<bool> covered(hierarchy.sources.size(), false);
        for (const auto nodeId : cut) {
            CHECK(nodeId < hierarchy.nodes.size());
            const auto& node = hierarchy.nodes[nodeId];
            for (const auto source : hierarchy.membersOf(node)) {
                CHECK(source < covered.size());
                CHECK(!covered[source]);
                covered[source] = true;
            }
        }
        CHECK(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }));
    }
}

TEST(instance_hierarchy_explicit_adjacency_cut_matches_family_cut) {
    const auto hierarchy = buildInstanceHierarchy(grove());
    std::vector<double> allowances(hierarchy.nodes.size(), 1.0);
    std::vector<std::uint32_t> familyCut, adjacencyCut;
    CHECK(cutAtInstanceHierarchy(hierarchy.nodes, allowances, familyCut));
    CHECK(cutAtInstanceHierarchy(hierarchy, allowances, adjacencyCut));
    CHECK_EQ(familyCut, adjacencyCut);
}

TEST(instance_hierarchy_supports_per_node_screen_allowances) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    const auto hierarchy = buildInstanceHierarchy(grove(), options);

    std::vector<double> allowances(hierarchy.nodes.size(), 0.0);
    std::vector<std::uint32_t> fine;
    CHECK(cutAtInstanceHierarchy(hierarchy.nodes, allowances, fine));
    CHECK_EQ(fine.size(), hierarchy.sources.size());
    for (const auto id : fine)
        CHECK(hierarchy.nodes[id].representation == InstanceRepresentation::Individual);

    std::fill(allowances.begin(), allowances.end(), 1e9);
    std::vector<std::uint32_t> coarse;
    CHECK(cutAtInstanceHierarchy(hierarchy.nodes, allowances, coarse));
    CHECK(coarse.size() < fine.size());
    CHECK(std::any_of(coarse.begin(), coarse.end(), [&](std::uint32_t id) {
        return hierarchy.nodes[id].representation == InstanceRepresentation::DensityShading;
    }));
}

TEST(instance_hierarchy_is_deterministic_and_rejects_bad_sources) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    const auto sources = grove();
    const auto first = buildInstanceHierarchy(sources, options);
    const auto second = buildInstanceHierarchy(sources, options);
    CHECK(first.members == second.members);
    CHECK(first.nodes.size() == second.nodes.size());
    for (std::size_t i = 0; i < first.nodes.size(); ++i) {
        CHECK(first.nodes[i].level == second.nodes[i].level);
        CHECK(first.nodes[i].representation == second.nodes[i].representation);
        CHECK(first.nodes[i].bornOf == second.nodes[i].bornOf);
        CHECK(first.nodes[i].replacedBy == second.nodes[i].replacedBy);
        CHECK(first.nodes[i].childFirst == second.nodes[i].childFirst);
        CHECK(first.nodes[i].childCount == second.nodes[i].childCount);
    }
    CHECK(first.children == second.children);
    auto broken = sources;
    broken[3].scale = std::numeric_limits<float>::quiet_NaN();
    CHECK(buildInstanceHierarchy(broken, options).nodes.empty());
}

TEST(instance_hierarchy_does_not_make_a_canopy_shell_for_one_source) {
    InstanceSource source;
    source.radius = 1;
    const auto hierarchy = buildInstanceHierarchy(std::span<const InstanceSource>(&source, 1));
    CHECK(std::none_of(hierarchy.nodes.begin(), hierarchy.nodes.end(), [](const auto& node) {
        return node.representation == InstanceRepresentation::CanopyProxy;
    }));
    CHECK(std::any_of(hierarchy.nodes.begin(), hierarchy.nodes.end(), [](const auto& node) {
        return node.representation == InstanceRepresentation::DensityShading;
    }));
}

TEST(instance_hierarchy_renderer_bridge_uses_each_nodes_depth) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    auto sources = grove();
    for (std::size_t i = 0; i < sources.size(); ++i)
        sources[i].position[2] = i < 32 ? 8.0f : 800.0f;
    const auto hierarchy = buildInstanceHierarchy(sources, options);

    engine::render::InstanceHierarchyScreen screen;
    screen.focal = 100;
    screen.rowW[2] = 1;
    screen.rowW[3] = 1;
    const auto selected = engine::render::selectInstanceHierarchy(hierarchy, screen, 1.0);
    CHECK(!selected.fallbackToIndividual);
    CHECK(!selected.nodes.empty());
    CHECK_EQ(selected.allowances.size(), hierarchy.nodes.size());
    CHECK(std::any_of(selected.nodes.begin(), selected.nodes.end(), [&](std::uint32_t id) {
        return hierarchy.nodes[id].representation == InstanceRepresentation::Individual;
    }));
    CHECK(std::any_of(selected.nodes.begin(), selected.nodes.end(), [&](std::uint32_t id) {
        return hierarchy.nodes[id].representation != InstanceRepresentation::Individual;
    }));
}

TEST(instance_hierarchy_skips_representations_without_region_geometry) {
    engine::geometry::InstanceHierarchyOptions options;
    options.midCellMetres = 5;
    options.farCellMetres = 20;
    options.veryFarCellMetres = 100;
    const auto hierarchy = buildInstanceHierarchy(grove(), options);
    engine::render::InstanceHierarchyScreen screen;
    screen.scale = 0.01;
    const auto available =
        engine::render::representationBit(InstanceRepresentation::Individual) |
        engine::render::representationBit(InstanceRepresentation::DensityShading);
    const auto selected = engine::render::selectInstanceHierarchy(hierarchy, screen, 1.0, available);

    CHECK(!selected.fallbackToIndividual);
    CHECK(selected.merged.empty());
    CHECK(selected.canopy.empty());
    CHECK(!selected.density.empty());
    std::vector<bool> covered(hierarchy.sources.size(), false);
    for (const auto nodeId : selected.nodes) {
        const auto representation = hierarchy.nodes[nodeId].representation;
        CHECK(representation == InstanceRepresentation::Individual ||
              representation == InstanceRepresentation::DensityShading);
        for (const auto source : hierarchy.membersOf(hierarchy.nodes[nodeId])) {
            CHECK(!covered[source]);
            covered[source] = true;
        }
    }
    CHECK(std::all_of(covered.begin(), covered.end(), [](bool value) { return value; }));
}

TEST(instance_hierarchy_replacement_weight_is_a_monotonic_transition) {
    engine::geometry::InstanceHierarchyNode node;
    node.error = 2;
    node.parentError = 6;
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 1), 0.0);
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 2), 0.0);
    const auto middle = engine::render::instanceHierarchyReplacementWeight(node, 4);
    CHECK(middle > 0.0 && middle < 1.0);
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 6), 1.0);
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 12), 1.0);
    node.parentError = std::numeric_limits<float>::infinity();
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 2), 0.0);
    CHECK(engine::render::instanceHierarchyReplacementWeight(node, 2.25) > 0.0);
    CHECK_EQ(engine::render::instanceHierarchyReplacementWeight(node, 2.5), 1.0);
}

TEST(instance_hierarchy_replacement_and_finer_coverage_are_complementary) {
    engine::geometry::InstanceHierarchyNode node;
    node.error = 1;
    node.parentError = 5;
    double previous = 0;
    for (double allowance = 0; allowance <= 6; allowance += 0.25) {
        const double replacement = engine::render::instanceHierarchyReplacementWeight(node, allowance);
        CHECK(replacement >= previous);
        CHECK(replacement >= 0 && replacement <= 1);
        CHECK(std::abs((1.0 - replacement) + replacement - 1.0) < 1e-12);
        previous = replacement;
    }
}
