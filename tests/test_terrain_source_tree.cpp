#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

#include "game/world/terrain_source_tree.hpp"

namespace {
using namespace world::terrain;

// A world with one coast running diagonally across it and flat ocean and flat
// plain either side. Everything the tree is for shows up here: most of the
// world has nothing to find, and one thin band has everything.
double distanceToCoast(const SourceBounds& node) {
    // The coast is the line x + y = 8192, in metres.
    const double d = (double(node.centreX()) + double(node.centreY()) - 8192.0) / std::sqrt(2.0);
    return std::abs(d) - double(node.metres) * 0.71;   // to the nearest point of the square
}

// The coast is a SEGMENT, not an endless line: it runs inside [0, 16384] in
// both axes and there is open water beyond it. That is what makes "cost follows
// content" a thing a test can measure - a larger world adds ocean, not coast.
bool onCoast(const SourceBounds& node) {
    if (node.x + node.metres < 0 || node.x > 16384) return false;
    if (node.y + node.metres < 0 || node.y > 16384) return false;
    return distanceToCoast(node) <= 0;
}

RefinementHints coastWorld(const SourceBounds& node) {
    RefinementHints hints;
    if (!onCoast(node)) return hints;            // flat, and provably so
    // Inside the coastal band: shape that a coarse step cannot carry, a shore
    // to draw, and water finding its way to it.
    hints.geometryError = float(std::min(40.0, double(node.metres) * 0.12));
    hints.boundaryComplexity = 0.9f;
    hints.erosionPotential = float(std::min(8.0, double(node.metres) * 0.02));
    return hints;
}

// Nothing anywhere: the answer a bounded analysis gives over open ocean.
RefinementHints emptyWorld(const SourceBounds&) { return {}; }

std::vector<const SourceNode*> leavesOf(const SourceTree& tree) {
    std::vector<const SourceNode*> out;
    for (const auto& node : tree.nodes)
        if (node.leaf()) out.push_back(&node);
    return out;
}
}

TEST(source_tree_has_no_mandatory_chain_and_lets_siblings_live_at_different_depths) {
    // The property the whole design rests on. If every node had to be opened to
    // the bottom, ten thousand kilometres of ocean would cost what ten thousand
    // kilometres of broken coast costs.
    const auto tree = refine({0, 0, 16384}, coastWorld);
    CHECK(tree.complete());
    CHECK(tree.leaves > 1);
    CHECK(tree.deepest > 3);

    // At least one parent has children that did not all stop together.
    std::size_t uneven = 0;
    for (const auto& node : tree.nodes) {
        if (node.leaf()) continue;
        std::set<bool> childIsLeaf;
        for (std::uint32_t i = 0; i < 4; ++i)
            childIsLeaf.insert(tree.nodes[node.firstChild + i].leaf());
        if (childIsLeaf.size() > 1) ++uneven;
    }
    CHECK(uneven > 0);

    // And the depths of the leaves really do span a range, rather than the tree
    // being uniform at whatever depth the coast forced.
    std::set<int> depths;
    for (const auto* leaf : leavesOf(tree)) depths.insert(leaf->depth);
    CHECK(depths.size() > 2);
}

TEST(source_tree_costs_what_the_world_contains_not_what_it_measures) {
    // The same coast in a world four times as wide costs about the same: the
    // extra area is ocean, and ocean is one node. A mandatory chain would cost
    // sixteen times as much for the same coastline.
    const auto small = refine({0, 0, 16384}, coastWorld);
    const auto large = refine({-24576, -24576, 65536}, coastWorld);
    CHECK(small.complete());
    CHECK(large.complete());
    // Four times the side, sixteen times the area, and not four times the tree.
    CHECK(large.nodes.size() < small.nodes.size() * 4);
    CHECK(large.analysed < small.analysed * 4);

    // An empty world is one node however large it is, and the analysis is asked
    // exactly once - which is the claim that the tree is cheap, measured.
    for (const std::int64_t side : {4096, 65536, 1048576}) {
        const auto nothing = refine({0, 0, side}, emptyWorld);
        CHECK_EQ(nothing.nodes.size(), std::size_t(1));
        CHECK_EQ(nothing.leaves, std::size_t(1));
        CHECK_EQ(nothing.analysed, std::size_t(1));
        CHECK(nothing.complete());
    }
}

TEST(source_tree_is_the_same_tree_whatever_asked_for_it) {
    // It is a property of the world, not of a camera or a queue. Two builds
    // that disagreed would put two different worlds in one save.
    const auto first = refine({0, 0, 16384}, coastWorld);
    const auto again = refine({0, 0, 16384}, coastWorld);
    CHECK_EQ(first.nodes.size(), again.nodes.size());
    CHECK_EQ(first.analysed, again.analysed);
    for (std::size_t i = 0; i < first.nodes.size(); ++i) {
        CHECK_EQ(first.nodes[i].bounds.x, again.nodes[i].bounds.x);
        CHECK_EQ(first.nodes[i].bounds.y, again.nodes[i].bounds.y);
        CHECK_EQ(first.nodes[i].bounds.metres, again.nodes[i].bounds.metres);
        CHECK_EQ(first.nodes[i].firstChild, again.nodes[i].firstChild);
        CHECK_EQ(first.nodes[i].materialMetres, again.nodes[i].materialMetres);
    }
    // And the analysis is asked once per node, never twice: the whole approach
    // only pays if looking is cheaper than opening.
    CHECK_EQ(first.analysed, first.nodes.size());

    // The set of leaves does not depend on the order they were reached in,
    // which is what lets the build be spread over workers later.
    std::multiset<std::tuple<std::int64_t, std::int64_t, std::int64_t>> one, two;
    for (const auto* leaf : leavesOf(first))
        one.insert({leaf->bounds.x, leaf->bounds.y, leaf->bounds.metres});
    for (const auto* leaf : leavesOf(again))
        two.insert({leaf->bounds.x, leaf->bounds.y, leaf->bounds.metres});
    CHECK(one == two);
}

TEST(source_tree_treats_an_unknown_bound_as_something_to_look_at_not_as_nothing) {
    // A certificate of no error is a claim, and a claim about a shape nobody
    // has looked at is a guess. The one reading that loses features in silence
    // is "I could not bound it" read as "there is nothing there".
    const auto unknownEverywhere = [](const SourceBounds&) {
        RefinementHints hints;
        hints.unknown = true;
        return hints;
    };
    RefinementPolicy policy;
    policy.finestMetres = 64;
    const auto tree = refine({0, 0, 1024}, unknownEverywhere, policy);
    CHECK(tree.leaves > 1);
    for (const auto* leaf : leavesOf(tree)) CHECK_EQ(leaf->bounds.metres, std::int64_t(64));

    // A bound that is not a number is the same answer, not a quiet zero.
    const auto broken = [](const SourceBounds&) {
        RefinementHints hints;
        hints.geometryError = std::numeric_limits<float>::quiet_NaN();
        return hints;
    };
    CHECK(refine({0, 0, 1024}, broken, policy).leaves > 1);
    // And a flat world stops at once under the same policy, so the test above
    // is about the unknown rather than about the policy.
    CHECK_EQ(refine({0, 0, 1024}, emptyWorld, policy).leaves, std::size_t(1));
}

TEST(source_tree_does_not_add_vertices_for_a_material_edge) {
    // P4's gate. A shore inside an otherwise gentle slope wants an exact edge
    // and no extra geometry. Asking for vertices here is how a coastline ends
    // up costing what a mountain range costs and looking no better.
    const auto edgeOnly = [](const SourceBounds&) {
        RefinementHints hints;
        hints.boundaryComplexity = 1.0f;   // as complex as it gets
        hints.geometryError = 0.1f;        // and flat
        return hints;
    };
    const auto tree = refine({0, 0, 4096}, edgeOnly);
    CHECK_EQ(tree.nodes.size(), std::size_t(1));
    CHECK_EQ(tree.leaves, std::size_t(1));

    // It is not ignored, though. The material field is asked for a finer step
    // than the geometry has, and the leaf records what it owes its shader.
    const auto& leaf = tree.nodes.front();
    CHECK(leaf.materialMetres < leaf.bounds.metres);
    CHECK_EQ(leaf.materialMetres, std::int64_t(4096) / 8);   // three halvings
    CHECK_EQ(leaf.debt.boundary, 1.0f);
    CHECK(leaf.debt.any());
    CHECK_EQ(tree.indebted, std::size_t(1));

    // A boundary below the threshold asks for nothing at all.
    const auto faint = [](const SourceBounds&) {
        RefinementHints hints;
        hints.boundaryComplexity = 0.05f;
        return hints;
    };
    const auto plain = refine({0, 0, 4096}, faint);
    CHECK_EQ(plain.nodes.front().materialMetres, std::int64_t(4096));
    CHECK(!plain.nodes.front().debt.any());
}

TEST(source_tree_hands_on_every_detail_it_declined_to_shape) {
    // The other half of the rule. "Did not fit" without handing the detail on
    // is a feature lost in silence, and that is exactly how ground comes out
    // looking like smooth plastic.
    RefinementPolicy policy;
    policy.finestMetres = 32;       // the tree cannot go below this
    const auto stubborn = [](const SourceBounds&) {
        RefinementHints hints;
        hints.geometryError = 7.5f;        // far past any tolerance
        hints.erosionPotential = 3.25f;
        hints.drainagePotential = 0.8f;
        hints.boundaryComplexity = 0.6f;
        return hints;
    };
    const auto tree = refine({0, 0, 256}, stubborn, policy);
    CHECK(tree.leaves > 1);
    CHECK(!tree.complete());                    // it stopped because it had to
    CHECK_EQ(tree.truncated, tree.leaves);

    for (const auto* leaf : leavesOf(tree)) {
        CHECK_EQ(leaf->bounds.metres, std::int64_t(32));
        // Every amplitude the step could not carry, at the size it asked with.
        CHECK(std::abs(double(leaf->debt.relief) - (7.5 - policy.geometryTolerance)) < 1e-5);
        CHECK(std::abs(double(leaf->debt.erosion) - (3.25 - policy.erosionTolerance)) < 1e-5);
        CHECK_EQ(leaf->debt.drainage, 0.8f);
        CHECK_EQ(leaf->debt.boundary, 0.6f);
    }
    CHECK_EQ(tree.indebted, tree.leaves);

    // A node that DID split owes nothing: the detail became shape instead.
    for (const auto& node : tree.nodes)
        if (!node.leaf()) CHECK(!node.debt.any());
}

TEST(source_tree_stops_coarse_on_a_plane_and_is_forced_fine_along_a_ridge) {
    // The gate, as two worlds that differ in one thing. A uniformly steep
    // plane is not on its own a reason for a dense mesh; a ridge running
    // between nodes is.
    const auto slope = [](const SourceBounds&) {
        RefinementHints hints;
        hints.geometryError = 0.2f;        // a plane is exactly representable
        hints.featureImportance = 0.1f;
        return hints;
    };
    CHECK_EQ(refine({0, 0, 8192}, slope).nodes.size(), std::size_t(1));

    const auto ridge = [](const SourceBounds& node) {
        RefinementHints hints;
        // A crest along y = 3000, one node wide wherever it is looked at.
        const double reach = std::abs(double(node.centreY()) - 3000.0) - double(node.metres) * 0.5;
        if (reach > 0) return hints;
        // A hint is a statement about THIS node at THIS size. The crest is a
        // feature while a node is too coarse to hold it and stops being one
        // once a node can - which is what makes refinement finite.
        hints.featureImportance = node.metres > 32 ? 0.95f : 0.1f;
        hints.geometryError = float(double(node.metres) * 0.02);
        return hints;
    };
    RefinementPolicy policy;
    policy.finestMetres = 16;
    const auto tree = refine({0, 0, 8192}, ridge, policy);
    CHECK(tree.complete());
    CHECK(tree.leaves > 1);

    // Every leaf that the ridge runs through is at the finest step, and the
    // leaves away from it are not - the tree spent its nodes where the feature is.
    std::size_t fine = 0, coarse = 0;
    for (const auto* leaf : leavesOf(tree)) {
        const bool onRidge = std::abs(double(leaf->bounds.centreY()) - 3000.0) <
                             double(leaf->bounds.metres) * 0.5;
        if (onRidge) {
            CHECK_EQ(leaf->bounds.metres, policy.finestMetres);
            ++fine;
        } else if (leaf->bounds.metres >= 1024) {
            ++coarse;
        }
    }
    CHECK(fine > 0);
    CHECK(coarse > 0);
}

TEST(source_tree_keeps_its_work_inside_a_budget_and_says_when_it_ran_out) {
    // A bound on the work, not a guess at it. A tree that silently stopped is a
    // world with a flat patch nobody ordered, so stopping is reported.
    const auto everywhere = [](const SourceBounds&) {
        RefinementHints hints;
        hints.geometryError = 100.0f;
        return hints;
    };
    RefinementPolicy policy;
    policy.finestMetres = 1;
    policy.maxNodes = 500;
    const auto tree = refine({0, 0, 65536}, everywhere, policy);
    CHECK(tree.nodes.size() <= policy.maxNodes);
    CHECK(!tree.complete());
    CHECK(tree.truncated > 0);
    CHECK_EQ(tree.analysed, tree.nodes.size());
    // Breadth first, so a budget that runs out leaves the tree even rather than
    // deep down one corner and coarse everywhere else.
    std::set<int> depths;
    for (const auto* leaf : leavesOf(tree)) depths.insert(leaf->depth);
    CHECK(depths.size() <= 2);

    // And every leaf still carries what it could not shape.
    for (const auto* leaf : leavesOf(tree)) CHECK(leaf->debt.relief > 0);
}

TEST(source_tree_refuses_a_root_it_cannot_describe) {
    CHECK_EQ(refine({0, 0, 0}, emptyWorld).nodes.size(), std::size_t(0));
    CHECK_EQ(refine({0, 0, -256}, emptyWorld).nodes.size(), std::size_t(0));
    CHECK_EQ(refine({0, 0, 1024}, {}).nodes.size(), std::size_t(0));
    RefinementPolicy policy;
    policy.maxNodes = 0;
    CHECK_EQ(refine({0, 0, 1024}, emptyWorld, policy).nodes.size(), std::size_t(0));
    policy = {};
    policy.finestMetres = 0;
    CHECK_EQ(refine({0, 0, 1024}, emptyWorld, policy).nodes.size(), std::size_t(0));
}

TEST(source_tree_children_tile_their_parent_exactly) {
    // Not a detail: a gap between siblings is a hole in the world, and an
    // overlap is a place two subsystems each think they own.
    const auto tree = refine({-4096, 2048, 8192}, coastWorld);
    CHECK(tree.nodes.size() > 4);
    for (const auto& node : tree.nodes) {
        if (node.leaf()) continue;
        std::int64_t area = 0;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto& child = tree.nodes[node.firstChild + i];
            CHECK_EQ(child.bounds.metres, node.bounds.metres / 2);
            CHECK(child.bounds.x >= node.bounds.x);
            CHECK(child.bounds.y >= node.bounds.y);
            CHECK(child.bounds.x + child.bounds.metres <= node.bounds.x + node.bounds.metres);
            CHECK(child.bounds.y + child.bounds.metres <= node.bounds.y + node.bounds.metres);
            CHECK_EQ(tree.nodes[child.parent].bounds.metres, node.bounds.metres);
            area += child.bounds.metres * child.bounds.metres;
        }
        CHECK_EQ(area, node.bounds.metres * node.bounds.metres);
        // And no two children start at the same corner.
        std::set<std::pair<std::int64_t, std::int64_t>> corners;
        for (std::uint32_t i = 0; i < 4; ++i)
            corners.insert({tree.nodes[node.firstChild + i].bounds.x,
                            tree.nodes[node.firstChild + i].bounds.y});
        CHECK_EQ(corners.size(), std::size_t(4));
    }
}
