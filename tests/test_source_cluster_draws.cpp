#include "framework.hpp"
#include "engine/geometry/cluster_asset.hpp"
#include "engine/render/systems/source_cluster_draws.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace {
using namespace engine::geometry;
using namespace engine::render;
MeshCluster piece(std::uint32_t first, std::uint32_t count, float error, float parent) {
    MeshCluster c;
    c.indices = {first, count}; c.error = error; c.parentError = parent; c.radius = 0.1f;
    return c;
}
constexpr float infinity = std::numeric_limits<float>::infinity();
ScreenScale ortho() {
    ScreenScale s;
    s.rowX[0] = 1; s.rowY[1] = 1; s.rowW[3] = 1;
    s.scale = 1; s.allowance = 1;
    return s;
}
GatheredInstances instances(std::size_t count) {
    GatheredInstances g;
    g.instances.resize(count);
    g.batches.push_back({0, 0, 0, std::uint32_t(count)});
    return g;
}
}

TEST(source_clusters_preserve_foliage_and_imported_attribute_splits) {
    // Coincident corners intentionally have different SOURCE indices (UV/material
    // splits). Position welding would silently change what texture a face uses.
    const std::vector<float> positions{0,0,0, 1,0,0, 0,1,0, 0,0,0, 0,1,0, -1,0,0,
                                      3,0,0, 4,0,0, 4,1,0, 3,1,0};
    const std::vector<std::uint32_t> indices{0,1,2, 3,4,5, 6,7,8, 6,8,9};
    auto asset = buildSourceClusterAsset(positions, indices);
    CHECK(!asset.empty());
    CHECK_EQ(asset.sourceTriangles, 4u);
    CHECK_EQ(asset.cardTriangles, 0u);
    CHECK(asset.cardIndices.empty());
    CHECK(asset.crownClusters.empty());
    std::multiset<std::array<std::uint32_t, 3>> finest, original;
    for (std::size_t i = 0; i < indices.size(); i += 3)
        original.insert({indices[i], indices[i+1], indices[i+2]});
    for (const auto& c : asset.clusters)
        if (c.level == 0)
            for (std::uint32_t i = 0; i < c.indices.count; i += 3) {
                const auto at = c.indices.first + i;
                finest.insert({asset.indices[at], asset.indices[at+1], asset.indices[at+2]});
            }
    CHECK(finest == original); // includes face connectivity and winding, not just vertex counts
    SourceClusterIndex tree(asset.clusters);
    std::vector<std::uint32_t> cut;
    for (double allowance : {0.0, 0.1, 100.0}) {
        tree.select(allowance, cut);
        std::size_t triangles = 0;
        for (auto id : cut) triangles += asset.clusters[id].indices.count / 3;
        // The finest cut must retain every source face. At a coarse allowance
        // the valid replacement may simplify the disconnected patch, but it
        // must never become empty or exceed the source triangle count.
        CHECK(triangles > 0);
        CHECK(triangles <= std::size_t(4));
        if (allowance <= 0.1) CHECK_EQ(triangles, std::size_t(4));
    }
    std::string why;
    const auto restored = decodeClusters(encodeClusters(asset), why);
    CHECK(why.empty());
    CHECK(restored.indices == asset.indices);
}

TEST(source_clusters_coplanar_changes_are_not_zero_error) {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    constexpr int side = 9;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) positions.insert(positions.end(), {float(x), float(y), 0});
    for (int y = 0; y + 1 < side; ++y)
        for (int x = 0; x + 1 < side; ++x) {
            const auto a = std::uint32_t(y * side + x), b = a + side;
            indices.insert(indices.end(), {a, a+1, b, a+1, b+1, b});
        }
    const auto asset = buildSourceClusterAsset(positions, indices);
    CHECK(asset.levels > 1);
    for (const auto& c : asset.clusters) {
        if (c.level > 0) CHECK(c.error > 0);
        if (c.level == 0) CHECK(c.parentError > 0);
    }
}

TEST(source_cluster_index_matches_exhaustive_cut_including_boundaries) {
    std::vector<MeshCluster> clusters;
    for (int i = 0; i < 200; ++i)
        clusters.push_back(piece(std::uint32_t(i*3), 3, float(i%11), i%7 ? float(i%11+i%5) : infinity));
    SourceClusterIndex index(clusters);
    std::vector<std::uint32_t> actual, expected;
    for (int a = 0; a < 80; ++a) {
        const double allowance = a * 0.25;
        index.select(allowance, actual);
        expected.clear();
        for (std::uint32_t i = 0; i < clusters.size(); ++i)
            if (clusters[i].error <= allowance && allowance < clusters[i].parentError) expected.push_back(i);
        std::sort(actual.begin(), actual.end());
        CHECK(actual == expected);
    }
}

TEST(source_cluster_draws_select_per_instance_not_by_nearest_batch_member) {
    const std::vector<MeshCluster> clusters{piece(0, 24, 0, 1), piece(24, 6, 1, infinity)};
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters, &index, 100, 17, {1, 0}, 0}};
    auto g = instances(2);
    g.instances[0].position[2] = 5;
    g.instances[1].position[2] = 100;
    auto s = ortho(); s.rowW[3] = 0; s.rowW[2] = 1; s.focal = 10;
    const auto result = planSourceDraws(g, assets, s);
    CHECK_EQ(result.plan.draws.size(), std::size_t(2));
    CHECK_EQ(result.plan.draws[0].indexCount, 24u);
    CHECK_EQ(result.plan.draws[1].indexCount, 6u);
    CHECK_EQ(result.plan.draws[0].firstInstance, 0u);
    CHECK_EQ(result.plan.draws[1].firstInstance, 1u);
    CHECK_EQ(result.plan.draws[0].vertexOffset, 17);
    CHECK_EQ(result.plan.triangles, std::size_t(10));
    CHECK_EQ(result.plan.fromCrown + result.plan.fromCards + result.plan.fromChain, std::size_t(0));
}

TEST(source_cluster_draws_cull_clusters_and_keep_single_instance_records) {
    std::vector<MeshCluster> clusters{piece(0, 6, 0, infinity), piece(6, 6, 0, infinity)};
    clusters[1].centre[0] = 10;
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters, &index, 0, 0, {11, 0}, 0}};
    const auto g = instances(3);
    const auto result = planSourceDraws(g, assets, ortho());
    CHECK_EQ(result.culledClusters, std::size_t(3));
    CHECK_EQ(result.plan.draws.size(), std::size_t(1));
    CHECK_EQ(result.plan.draws[0].instanceCount, 3u);
    CHECK_EQ(result.plan.triangles, std::size_t(6));
    CHECK_EQ(g.instances.size(), std::size_t(3));
}

TEST(source_cluster_draws_bound_vertex_animation_and_eye_plane_intersections) {
    std::vector<MeshCluster> clusters{piece(0, 6, 0, infinity)};
    clusters[0].centre[0] = 1.3f;
    SourceClusterIndex index(clusters);
    SourceGeometry assets[]{{clusters, &index, 0, 0, {2, 0}, 0}};
    auto g = instances(1);
    CHECK(planSourceDraws(g, assets, ortho()).plan.draws.empty());
    assets[0].deformation = 0.5;
    CHECK_EQ(planSourceDraws(g, assets, ortho()).plan.draws.size(), std::size_t(1));
    clusters[0].centre[0] = 0; clusters[0].centre[2] = 1;
    g.instances[0].position[2] = -0.5f;
    auto s = ortho(); s.rowW[3] = 0; s.rowW[2] = 1; s.focal = 10;
    CHECK_EQ(planSourceDraws(g, assets, s).plan.draws.size(), std::size_t(1));
}

TEST(source_clusters_reject_nonfinite_source_positions) {
    const std::vector<std::uint32_t> indices{0,1,2};
    for (float bad : {infinity, -infinity, std::numeric_limits<float>::quiet_NaN()}) {
        const std::vector<float> positions{0,0,0, 1,0,0, 0,bad,0};
        CHECK(buildSourceClusterAsset(positions, indices).empty());
    }
}

TEST(source_cluster_index_unbounded_allowance_selects_roots_not_nothing) {
    const std::vector<MeshCluster> clusters{piece(0, 24, 0, 1), piece(24, 6, 1, infinity)};
    SourceClusterIndex index(clusters);
    std::vector<std::uint32_t> selected;
    index.select(infinity, selected);
    CHECK(selected == std::vector<std::uint32_t>{1});
    const SourceGeometry assets[]{{clusters, &index, 0, 0, {1, 0}, 0}};
    auto screen = ortho();
    screen.allowance = std::numeric_limits<double>::max();
    auto g = instances(1);
    g.instances[0].scale = 0.25f; // the finite pixel allowance overflows in model units
    const auto plan = planSourceDraws(g, assets, screen);
    CHECK_EQ(plan.plan.draws.size(), std::size_t(1));
    CHECK_EQ(plan.plan.draws.front().firstIndex, 24u);
}

TEST(source_cluster_draws_bound_off_axis_depth_displacement) {
    const std::vector<MeshCluster> clusters{piece(0, 24, 0, 1), piece(24, 6, 1, infinity)};
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters, &index, 0, 0, {1, 0}, 0}};
    auto g = instances(1);
    g.instances[0].position[0] = 9;
    g.instances[0].position[2] = 10;
    auto screen = ortho();
    screen.rowW[3] = 0; screen.rowW[2] = 1;
    screen.focal = 100; screen.allowance = 12;
    // A unit displacement fits focal*error/nearestDepth, yet exceeds the
    // allowance after perspective division because it also changes depth.
    const double step = std::sqrt(0.5);
    const double measured = 100 * (9.0/10.0 - (9-step)/(10+step));
    CHECK(measured > screen.allowance);
    const auto plan = planSourceDraws(g, assets, screen);
    CHECK_EQ(plan.plan.draws.size(), std::size_t(1));
    CHECK_EQ(plan.plan.draws.front().firstIndex, 0u);
}

TEST(source_cluster_draws_do_not_merge_across_culled_instance_or_material_batch) {
    const std::vector<MeshCluster> clusters{piece(0, 6, 0, infinity)};
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters, &index, 0, 0, {0.2, 0}, 0}};
    auto g = instances(4);
    g.instances[1].position[0] = 10;
    g.batches[0].count = 3;
    g.batches.push_back({0, 1, 3, 1});
    const auto original = g.instances;
    const auto result = planSourceDraws(g, assets, ortho());
    CHECK_EQ(result.plan.draws.size(), std::size_t(3));
    CHECK_EQ(result.plan.draws[0].firstInstance, 0u);
    CHECK_EQ(result.plan.draws[1].firstInstance, 2u);
    CHECK_EQ(result.plan.draws[2].firstInstance, 3u);
    for (const auto& draw : result.plan.draws) CHECK_EQ(draw.instanceCount, 1u);
    for (std::size_t i = 0; i < g.instances.size(); ++i) {
        CHECK_EQ(g.instances[i].position[0], original[i].position[0]);
        CHECK_EQ(g.instances[i].scale, original[i].scale);
        CHECK_EQ(g.instances[i].yaw, original[i].yaw);
    }
}

TEST(source_cluster_draws_reserve_error_for_uncorrelated_vertex_animation) {
    const std::vector<MeshCluster> clusters{piece(0, 24, 0, 1), piece(24, 6, 1, infinity)};
    SourceClusterIndex index(clusters);
    SourceGeometry assets[]{{clusters, &index, 0, 0, {1, 0}, 0}};
    auto screen = ortho(); screen.allowance = 1.5;
    const auto g = instances(1);
    const auto still = planSourceDraws(g, assets, screen);
    CHECK_EQ(still.plan.draws.size(), std::size_t(1));
    CHECK_EQ(still.plan.draws.front().firstIndex, 24u);
    assets[0].deformation = 0.4; // error <= 1 + 2*0.4 exceeds 1.5 pixels
    const auto moving = planSourceDraws(g, assets, screen);
    CHECK_EQ(moving.plan.draws.size(), std::size_t(1));
    CHECK_EQ(moving.plan.draws.front().firstIndex, 0u);
}

TEST(source_cluster_draws_transform_cluster_bounds_and_scale_error_per_instance) {
    std::vector<MeshCluster> clusters{piece(0, 24, 0, 1), piece(24, 6, 1, infinity)};
    for (auto& c : clusters) c.centre[0] = 2;
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters, &index, 0, 0, {3, 0}, 0}};
    auto g = instances(2);
    // Rotate the off-centre cluster onto Y, then translate it into the view.
    g.instances[0].yaw = g.instances[1].yaw = float(std::acos(-1.0)/2);
    g.instances[0].scale = 0.5;
    g.instances[1].scale = 2;
    g.instances[0].position[1] = -1;
    g.instances[1].position[1] = -4;
    const auto result = planSourceDraws(g, assets, ortho());
    CHECK_EQ(result.plan.draws.size(), std::size_t(2));
    CHECK_EQ(result.plan.draws[0].firstIndex, 24u);
    CHECK_EQ(result.plan.draws[1].firstIndex, 0u);
    CHECK_EQ(result.plan.draws[0].firstInstance, 0u);
    CHECK_EQ(result.plan.draws[1].firstInstance, 1u);
}

TEST(source_gpu_capacity_overflow_preserves_every_instance_via_cpu) {
    const std::vector<MeshCluster> clusters{piece(0,6,0,infinity)};
    SourceClusterIndex index(clusters);
    const SourceGeometry assets[]{{clusters,&index,0,0,{0.1,0},0}};
    CHECK(sourceFitsGpuCapacity(instances(2048),assets,262144,2048));
    const auto wide=instances(2049);
    CHECK(!sourceFitsGpuCapacity(wide,assets,262144,2048));
    const auto cpu=planSourceDraws(wide,assets,ortho());
    CHECK_EQ(cpu.plan.rejected,std::size_t(0));
    CHECK_EQ(cpu.plan.draws.size(),std::size_t(1));
    CHECK_EQ(cpu.plan.draws.front().instanceCount,2049u);
    CHECK_EQ(cpu.plan.triangles,std::size_t(4098));
}

TEST(source_gpu_capacity_counts_shared_model_buckets_across_batches) {
    const std::vector<MeshCluster> clusters{piece(0,6,0,infinity),piece(6,6,0,infinity)};
    const SourceGeometry assets[]{{clusters,nullptr,0,0,{1,0},0},{clusters,nullptr,0,0,{1,0},0}};
    auto source=instances(3000);
    source.batches={{0,0,0,1500},{0,1,1500,1500}};
    CHECK(!sourceFitsGpuCapacity(source,assets,262144,2048));
    source.batches.back().mesh=1;
    CHECK(sourceFitsGpuCapacity(source,assets,6000,2048));
    CHECK(!sourceFitsGpuCapacity(source,assets,5999,2048));
    CHECK(!sourceFitsGpuCapacity(source,assets,0,2048));
    source.batches.back().first=3000;
    CHECK(!sourceFitsGpuCapacity(source,assets,262144,2048));
}
