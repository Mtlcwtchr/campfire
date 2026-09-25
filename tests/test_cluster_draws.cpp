#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/render/systems/cluster_draws.hpp"

namespace {
using namespace engine::render;
using engine::IndexRange;
using engine::geometry::MeshCluster;

// A chain of four levels in one shared index buffer, the way a model's ranges
// actually sit after several models are concatenated.
constexpr IndexRange kChain[]{{1000, 18000}, {1000 + 18000, 4632}, {22632, 1860}, {24492, 744}};

// A two-level DAG: four clusters of the model, replaced by two coarser ones.
// Errors are in model metres, as the offline build measures them.
std::vector<MeshCluster> dag() {
    std::vector<MeshCluster> clusters;
    for (int i = 0; i < 4; ++i) {
        MeshCluster cluster;
        cluster.indices = {std::uint32_t(i * 384), 384};
        cluster.error = 0;
        cluster.parentError = 0.25f;
        cluster.level = 0;
        cluster.replacedBy = 0;
        cluster.radius = 3;
        clusters.push_back(cluster);
    }
    for (int i = 0; i < 2; ++i) {
        MeshCluster cluster;
        cluster.indices = {std::uint32_t(1536 + i * 384), 384};
        cluster.error = 0.25f;
        cluster.parentError = std::numeric_limits<float>::infinity();
        cluster.level = 1;
        cluster.bornOf = 0;
        cluster.radius = 5;
        clusters.push_back(cluster);
    }
    return clusters;
}
}

TEST(cluster_draws_turn_every_run_into_one_argument_per_piece_of_geometry) {
    const auto clusters = dag();
    const MeshGeometry assets[]{
            {0, kChain, {}, 0},                       // chain only
            {4096, kChain, clusters, 40000}};         // and a DAG beside it
    const DrawRun runs[]{{0, 2, 0, 50, 0},            // chain: level two, fifty instances
                         {1, 0, 50, 8, 0.10}};        // DAG: an allowance the finest level fits
    const auto plan = planDraws(runs, assets);

    CHECK_EQ(plan.rejected, std::size_t(0));
    CHECK_EQ(plan.fromChain, std::size_t(1));
    CHECK_EQ(plan.fromClusters, std::size_t(4));      // all four of the finest level
    CHECK_EQ(plan.draws.size(), std::size_t(5));

    // The chain run: one draw of that level's range, at that asset's vertices.
    CHECK_EQ(plan.draws[0].indexCount, kChain[2].count);
    CHECK_EQ(plan.draws[0].firstIndex, kChain[2].first);
    CHECK_EQ(plan.draws[0].instanceCount, 50u);
    CHECK_EQ(plan.draws[0].vertexOffset, 0);
    CHECK_EQ(plan.draws[0].firstInstance, 0u);

    // The cut: every cluster gets the whole run, at the second asset's bases.
    for (std::size_t i = 1; i < plan.draws.size(); ++i) {
        CHECK_EQ(plan.draws[i].instanceCount, 8u);
        CHECK_EQ(plan.draws[i].vertexOffset, 4096);
        CHECK_EQ(plan.draws[i].firstInstance, 50u);
        CHECK_EQ(plan.draws[i].indexCount, 384u);
        CHECK(plan.draws[i].firstIndex >= 40000u);
        CHECK(plan.draws[i].firstIndex < 40000u + 1536u);   // the finest level's range
    }
    CHECK_EQ(plan.triangles, std::size_t(kChain[2].count / 3) * 50 + std::size_t(128) * 4 * 8);
}

TEST(cluster_draws_pick_one_representation_along_every_path_of_the_dag) {
    // The rule the shader states and the offline build measured against. A cut
    // that took two levels of a path would draw the same surface twice; one
    // that took none would leave a hole.
    const auto clusters = dag();
    const MeshGeometry assets[]{{0, kChain, clusters, 0}};
    for (const double allowance : {0.001, 0.05, 0.2499, 0.25, 0.4, 3.0, 40.0}) {
        const DrawRun runs[]{{0, 0, 0, 1, allowance}};
        const auto plan = planDraws(runs, assets);
        CHECK(!plan.draws.empty());
        // Every cluster of the model is covered once: the finest four, or the
        // two that replace them, never a mixture of a parent and its children.
        std::size_t fine = 0, coarse = 0;
        for (const auto& draw : plan.draws) (draw.firstIndex < 1536 ? fine : coarse) += 1;
        CHECK((fine == 4 && coarse == 0) || (fine == 0 && coarse == 2));
        CHECK_EQ(fine + coarse, plan.draws.size());
        if (allowance < 0.25) CHECK_EQ(fine, std::size_t(4));
        else CHECK_EQ(coarse, std::size_t(2));
    }
}

TEST(cluster_draws_prefer_explicit_family_links_over_stale_interval_metadata) {
    auto clusters = dag();
    for (int i = 0; i < 4; ++i) clusters[i].parentError = 0;
    const MeshGeometry assets[]{{0, kChain, clusters, 0}};
    const DrawRun run[]{{0, 0, 0, 1, 0.1}};
    const auto plan = planDraws(run, assets);

    // The interval fields are deliberately stale, but the family says that
    // the two coarse clusters replace the four fine ones. Runtime must follow
    // the explicit hierarchy instead of silently drawing the chain.
    CHECK_EQ(plan.fromClusters, std::size_t(4));
    CHECK_EQ(plan.fromChain, std::size_t(0));
    CHECK_EQ(plan.draws.size(), std::size_t(4));
}

TEST(cluster_draws_fall_back_to_the_chain_rather_than_leaving_a_hole) {
    // An allowance outside what the DAG measured selects nothing. Drawing the
    // chain instead draws the model; drawing nothing draws a gap in a forest,
    // which is the one outcome worth ruling out.
    std::vector<MeshCluster> broken = dag();
    for (auto& cluster : broken) {
        cluster.parentError = cluster.error;   // nothing is selectable
        cluster.bornOf = MeshCluster::kNoGroup;
        cluster.replacedBy = MeshCluster::kNoGroup;
    }
    const MeshGeometry assets[]{{0, kChain, broken, 0}};
    const DrawRun runs[]{{0, 1, 0, 4, 0.1}};
    const auto plan = planDraws(runs, assets);
    CHECK_EQ(plan.draws.size(), std::size_t(1));
    CHECK_EQ(plan.fromClusters, std::size_t(0));
    CHECK_EQ(plan.fromChain, std::size_t(1));
    CHECK_EQ(plan.draws[0].firstIndex, kChain[1].first);
    CHECK_EQ(plan.rejected, std::size_t(0));

    // An allowance of zero or a number that is not one says "use the chain"
    // outright, which is what a run whose instances have no size on screen is.
    const auto clusters = dag();
    const MeshGeometry ordinary[]{{0, kChain, clusters, 0}};
    for (const double allowance : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity()}) {
        const DrawRun one[]{{0, 3, 0, 2, allowance}};
        const auto fallback = planDraws(one, ordinary);
        CHECK_EQ(fallback.draws.size(), std::size_t(1));
        CHECK_EQ(fallback.fromChain, std::size_t(1));
        CHECK_EQ(fallback.draws[0].firstIndex, kChain[3].first);
    }
}

TEST(cluster_draws_keep_each_runs_instances_to_itself) {
    // Every argument carries where its run starts and how long it is. Getting
    // that wrong does not draw nothing - it draws somebody else's trees at this
    // asset's shape, which looks like a bug in the scatter.
    const auto clusters = dag();
    const MeshGeometry assets[]{{0, kChain, {}, 0}, {2048, kChain, clusters, 9000}};
    std::vector<DrawRun> runs;
    std::uint32_t at = 0;
    for (std::uint32_t i = 0; i < 6; ++i) {
        const std::uint32_t count = 3 + i * 7;
        // Levels where the cut is the cheaper answer; the level where it is
        // not has its own test below.
        runs.push_back({i % 2, i % 3, at, count, i % 2 ? 0.05 : 0.0});
        at += count;
    }
    const auto plan = planDraws(runs, assets);
    CHECK(plan.draws.size() > runs.size());   // the DAG runs became several each

    // Walk the arguments back to the run each came from and check the pairing.
    std::size_t argument = 0;
    for (const auto& run : runs) {
        const auto expected = run.asset == 1 ? std::size_t(4) : std::size_t(1);
        for (std::size_t i = 0; i < expected; ++i, ++argument) {
            CHECK_EQ(plan.draws[argument].firstInstance, run.first);
            CHECK_EQ(plan.draws[argument].instanceCount, run.count);
            CHECK_EQ(plan.draws[argument].vertexOffset, assets[run.asset].vertexBase);
        }
    }
    CHECK_EQ(argument, plan.draws.size());
}

TEST(cluster_draws_refuse_a_run_that_names_nothing_instead_of_drawing_it) {
    const auto clusters = dag();
    const MeshGeometry assets[]{{0, kChain, clusters, 0}};
    const IndexRange none[]{{0, 0}};
    const MeshGeometry empty[]{{0, {}, {}, 0}, {0, none, {}, 0}};
    const DrawRun runs[]{{0, 0, 0, 0, 0.1},      // no instances
                         {7, 0, 0, 5, 0.1},      // an asset that is not there
                         {0, 99, 0, 5, 0.0}};    // a level past the end of the chain
    const auto plan = planDraws(runs, assets);
    CHECK_EQ(plan.rejected, std::size_t(2));
    CHECK_EQ(plan.draws.size(), std::size_t(1));
    // The level past the end clamps to the coarsest rather than reading past it.
    CHECK_EQ(plan.draws[0].firstIndex, kChain[3].first);

    const DrawRun bare[]{{0, 0, 0, 5, 0.0}, {1, 0, 0, 5, 0.0}};
    const auto nothing = planDraws(bare, empty);
    CHECK_EQ(nothing.draws.size(), std::size_t(0));
    CHECK_EQ(nothing.rejected, std::size_t(2));
    CHECK_EQ(planDraws({}, assets).draws.size(), std::size_t(0));
}

TEST(cluster_draws_keep_the_leaves_of_a_run_they_drew_from_a_cut) {
    // A DAG covers the solid part of a tree. If the plan forgot the cards, a
    // clustered tree would be a bare trunk - and it would be the models WITH
    // leaves that broke, which is all of them that matter.
    const auto clusters = dag();
    // Thinned the way the offline chain thins them, and small enough beside
    // the chain's own levels that the cut is the cheaper answer at each - the
    // comparison itself has its own test below.
    const IndexRange cards[]{{0, 2880}, {2880, 720}, {3600, 180}, {3780, 45}};
    const MeshGeometry assets[]{{0, kChain, clusters, 0, cards, 60000}};

    for (std::uint32_t level = 0; level < 3; ++level) {
        const DrawRun runs[]{{0, level, 0, 12, 0.1}};
        const auto plan = planDraws(runs, assets);
        CHECK_EQ(plan.fromClusters, std::size_t(4));
        CHECK_EQ(plan.fromCards, std::size_t(1));
        CHECK_EQ(plan.draws.size(), std::size_t(5));
        const auto& leaves = plan.draws.back();
        // The cards of THIS level, not of the finest: a tree at a distance
        // draws the leaves the offline thinning left for that distance.
        CHECK_EQ(leaves.indexCount, cards[level].count);
        CHECK_EQ(leaves.firstIndex, 60000u + cards[level].first);
        CHECK_EQ(leaves.instanceCount, 12u);
        CHECK_EQ(leaves.firstInstance, 0u);
    }

    // A run that fell back to the chain draws the chain's level, which already
    // holds its cards - adding them again would double every leaf.
    const MeshGeometry bare[]{{0, kChain, {}, 0, cards, 60000}};
    const DrawRun chain[]{{0, 1, 0, 3, 0.1}};
    const auto fallback = planDraws(chain, bare);
    CHECK_EQ(fallback.draws.size(), std::size_t(1));
    CHECK_EQ(fallback.fromCards, std::size_t(0));

    // An asset with a DAG and no cards at all stays one draw per cluster.
    const MeshGeometry solid[]{{0, kChain, clusters, 0, {}, 0}};
    const DrawRun one[]{{0, 0, 0, 5, 0.1}};
    CHECK_EQ(planDraws(one, solid).fromCards, std::size_t(0));
    CHECK_EQ(planDraws(one, solid).draws.size(), std::size_t(4));
}

TEST(cluster_draws_submit_source_card_companions_without_a_solid_dag) {
    const IndexRange cards[]{{0, 288}, {288, 72}};
    const MeshGeometry companions[]{{2048, {}, {}, 0, cards, 60000}};
    const DrawRun run[]{{0, 1, 37, 12, 0}};
    const auto plan = planDraws(run, companions);

    CHECK_EQ(plan.rejected, std::size_t(0));
    CHECK_EQ(plan.fromCards, std::size_t(1));
    CHECK_EQ(plan.fromClusters, std::size_t(0));
    CHECK_EQ(plan.draws.size(), std::size_t(1));
    CHECK_EQ(plan.draws[0].indexCount, 72u);
    CHECK_EQ(plan.draws[0].firstIndex, 60288u);
    CHECK_EQ(plan.draws[0].vertexOffset, 2048);
    CHECK_EQ(plan.draws[0].firstInstance, 37u);
    CHECK_EQ(plan.draws[0].instanceCount, 12u);
}

TEST(cluster_draws_take_the_chain_when_the_cut_would_cost_more_at_the_same_allowance) {
    // A DAG whose coarse end stalls is worse than the chain it replaced at
    // exactly the distance where most of a forest is. The two are measured
    // against the same rule, so at one allowance either is a valid answer and
    // the cheaper one is the right one. Falling back costs one pop; not falling
    // back costs every frame.
    const auto clusters = dag();                       // 4 x 384 indices in the finest cut
    const IndexRange thrifty[]{{0, 18000}, {18000, 4632}, {22632, 1860}, {24492, 744}};
    const MeshGeometry assets[]{{0, thrifty, clusters, 0}};

    // Levels whose whole mesh is dearer than the cut: the cut is taken.
    for (std::uint32_t level = 0; level < 3; ++level) {
        const DrawRun runs[]{{0, level, 0, 7, 0.1}};
        const auto plan = planDraws(runs, assets);
        CHECK_EQ(plan.fromClusters, std::size_t(4));
        CHECK_EQ(plan.declined, std::size_t(0));
        CHECK_EQ(plan.triangles, std::size_t(4 * 128) * 7);
    }
    // And the level that is cheaper than the cut: the chain, and it says so.
    const DrawRun coarse[]{{0, 3, 0, 7, 0.1}};
    const auto plan = planDraws(coarse, assets);
    CHECK_EQ(plan.fromClusters, std::size_t(0));
    CHECK_EQ(plan.fromChain, std::size_t(1));
    CHECK_EQ(plan.declined, std::size_t(1));
    CHECK_EQ(plan.draws.size(), std::size_t(1));
    CHECK_EQ(plan.draws[0].indexCount, thrifty[3].count);
    CHECK_EQ(plan.triangles, std::size_t(thrifty[3].count / 3) * 7);

    // The leaves count towards the comparison: a cut that is cheaper on its own
    // and dearer with its cards is dearer.
    const IndexRange cards[]{{0, 30}, {30, 30}, {60, 30}, {90, 30}};
    const IndexRange tight[]{{0, 1700}, {1700, 1700}, {3400, 1700}, {5100, 1700}};
    const MeshGeometry close[]{{0, tight, clusters, 0, cards, 9000}};
    const DrawRun one[]{{0, 0, 0, 1, 0.1}};
    const auto with = planDraws(one, close);
    CHECK_EQ(with.fromClusters, std::size_t(4));    // 1536 + 30 still under 1700
    CHECK_EQ(with.fromCards, std::size_t(1));
    const IndexRange tighter[]{{0, 1560}, {1560, 1560}, {3120, 1560}, {4680, 1560}};
    const MeshGeometry over[]{{0, tighter, clusters, 0, cards, 9000}};
    const auto declined = planDraws(one, over);
    CHECK_EQ(declined.fromClusters, std::size_t(0));   // 1536 + 30 is over 1560
    CHECK_EQ(declined.declined, std::size_t(1));
}

TEST(cluster_draws_weigh_the_shell_against_the_cut_and_the_chain) {
    // Three answers for one run at one allowance, and the cheapest is the right
    // one. The shell REPLACES the cut rather than joining it: it contains the
    // solid geometry as well as the foliage, so drawing both would draw the
    // trunk twice - which is the fault this shape of the code exists to rule out.
    const auto clusters = dag();                        // a cut of 4 x 384 indices
    std::vector<MeshCluster> shell(2);
    for (auto& piece : shell) {
        piece.indices = {std::uint32_t(&piece - shell.data()) * 300u, 300};
        piece.error = 0;
        piece.parentError = 1.0f;
        piece.radius = 5;
    }
    const IndexRange cards[]{{0, 900}, {900, 240}, {1140, 60}, {1200, 30}};

    // A chain level dearer than either: the cut plus its cards wins, because at
    // 1 536 + 60 it is cheaper than the shell's 600... no, it is not, and that
    // is the point of measuring rather than preferring.
    const IndexRange dear[]{{0, 9000}, {9000, 4000}, {13000, 2400}, {15400, 1800}};
    const MeshGeometry full[]{{0, dear, clusters, 0, cards, 60000, shell, 90000, 4096}};
    const DrawRun run[]{{0, 2, 0, 6, 0.1}};
    const auto plan = planDraws(run, full);
    // 1 536 + 60 for the cut and its cards, against 600 for the shell.
    CHECK_EQ(plan.fromCrown, std::size_t(2));
    CHECK_EQ(plan.fromClusters, std::size_t(0));
    CHECK_EQ(plan.fromCards, std::size_t(0));
    CHECK_EQ(plan.draws.size(), std::size_t(2));
    CHECK_EQ(plan.triangles, std::size_t(600 / 3) * 6);
    for (const auto& draw : plan.draws) {
        CHECK_EQ(draw.vertexOffset, 4096);           // the shell's own vertices
        CHECK(draw.firstIndex >= 90000u);
        CHECK_EQ(draw.instanceCount, 6u);
    }

    // Make the shell dear and the cut wins, with its cards beside it.
    std::vector<MeshCluster> heavy = shell;
    for (auto& piece : heavy) piece.indices.count = 3000;
    const MeshGeometry other[]{{0, dear, clusters, 0, cards, 60000, heavy, 90000, 4096}};
    const auto cutWins = planDraws(run, other);
    CHECK_EQ(cutWins.fromClusters, std::size_t(4));
    CHECK_EQ(cutWins.fromCards, std::size_t(1));
    CHECK_EQ(cutWins.fromCrown, std::size_t(0));

    // And make the chain cheap and it wins over both, which is what a model
    // whose DAG stalls at the coarse end actually needs.
    const IndexRange thrifty[]{{0, 9000}, {9000, 4000}, {13000, 300}, {15400, 180}};
    const MeshGeometry cheap[]{{0, thrifty, clusters, 0, cards, 60000, heavy, 90000, 4096}};
    const auto chainWins = planDraws(run, cheap);
    CHECK_EQ(chainWins.fromChain, std::size_t(1));
    CHECK_EQ(chainWins.fromCrown, std::size_t(0));
    CHECK_EQ(chainWins.fromClusters, std::size_t(0));
    CHECK_EQ(chainWins.draws.size(), std::size_t(1));
    CHECK_EQ(chainWins.draws[0].indexCount, thrifty[2].count);
}

TEST(cluster_draws_use_a_shell_when_there_is_no_chain_to_fall_back_to) {
    // A model made entirely of alpha cards has no solid geometry to cluster and
    // no chain in the sidecar. Its shell is the only answer there is, and a
    // plan that refused it would draw nothing at all.
    std::vector<MeshCluster> shell(3);
    for (std::size_t i = 0; i < shell.size(); ++i) {
        shell[i].indices = {std::uint32_t(i) * 120u, 120};
        shell[i].error = 0;
        shell[i].parentError = 0.5f;
        shell[i].radius = 2;
    }
    const MeshGeometry bush[]{{0, {}, {}, 0, {}, 0, shell, 7000, 512}};
    const DrawRun run[]{{0, 0, 0, 9, 0.2}};
    const auto plan = planDraws(run, bush);
    CHECK_EQ(plan.fromCrown, std::size_t(3));
    CHECK_EQ(plan.rejected, std::size_t(0));
    CHECK_EQ(plan.triangles, std::size_t(3 * 40) * 9);

    // Past the shell's own error there is nothing left to draw, and the plan
    // says so rather than emitting a draw of nothing.
    const DrawRun far[]{{0, 0, 0, 9, 4.0}};
    const auto beyond = planDraws(far, bush);
    CHECK_EQ(beyond.draws.size(), std::size_t(0));
    CHECK_EQ(beyond.rejected, std::size_t(1));
}

TEST(cluster_draws_can_submit_a_coarsest_canopy_proxy_without_a_chain) {
    // The far instance representation is not the merged shell cut: it is a
    // separate, already-coarsened cluster asset with no individual chain.
    std::vector<MeshCluster> canopy(2);
    for (std::size_t i = 0; i < canopy.size(); ++i) {
        canopy[i].indices = {std::uint32_t(i) * 90u, 90};
        canopy[i].error = 3;
        canopy[i].parentError = std::numeric_limits<float>::infinity();
        canopy[i].radius = 8;
    }
    const MeshGeometry asset[]{{2048, {}, canopy, 12000}};
    const DrawRun run[]{{0, 0, 0, 1, 1.0e9}};
    const auto plan = planDraws(run, asset);
    CHECK_EQ(plan.fromClusters, std::size_t(2));
    CHECK_EQ(plan.fromChain, std::size_t(0));
    CHECK_EQ(plan.rejected, std::size_t(0));
    CHECK_EQ(plan.triangles, std::size_t(60));
    for (const auto& draw : plan.draws) CHECK_EQ(draw.vertexOffset, 2048);
}

TEST(cluster_draws_never_replace_a_whole_tree_with_a_partial_crown) {
    std::vector<MeshCluster> shell(2);
    shell[0].indices = {0, 60};
    shell[1].indices = {60, 60};
    shell[0].error = 1;
    shell[1].error = 2;
    for (auto& piece : shell) piece.parentError = 10;
    const IndexRange chain[]{{0, 900}};
    const MeshGeometry assets[]{{0, chain, {}, 0, {}, 0, shell, 9000, 512}};
    const DrawRun near[]{{0, 0, 0, 1, 1.5}};
    const auto plan = planDraws(near, assets);
    CHECK(!crownFits(shell, 1.5));
    CHECK_EQ(plan.fromCrown, std::size_t(0));
    CHECK_EQ(plan.fromChain, std::size_t(1));
    CHECK(crownFits(shell, 2));
    CHECK(!crownFits(shell, 10));
    CHECK(!crownFits(shell, 0));
    CHECK(!crownFits({}, 2));
}

TEST(cluster_draws_grove_proxy_scale_cannot_grow_giant_trunks) {
    CHECK_EQ(proxyShellAllowance(1, 1, 12, 2), 2.0);
    // Doubling a twelve-metre tree changes the height by twelve pixels.
    CHECK_EQ(proxyShellAllowance(1, 2, 12, 2), 0.0);
    CHECK_EQ(proxyShellAllowance(1, 0.5, 12, 2), 0.0);
    // At sufficient distance that height difference is subpixel, the remaining
    // budget is converted to MODEL metres, including the instance scale.
    CHECK(std::abs(proxyShellAllowance(0.1, 2, 12, 2) - 4.0) < 1e-9);
    CHECK_EQ(proxyShellAllowance(0, 1, 12, 2), 0.0);
    CHECK_EQ(proxyShellAllowance(1, 0, 12, 2), 0.0);
}
