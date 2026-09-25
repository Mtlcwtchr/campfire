#include "framework.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include "engine/pipeline/pipeline.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/cluster_cull.hpp"
#include "engine/render/geometry/mesh_root_cull.hpp"
#include "engine/render/geometry/instance_root_cull.hpp"
#include "engine/render/render_pipeline.hpp"

namespace {
struct Gpu {
    SDL_Window* window = nullptr;
    engine::Device device;
    bool ready = false;
    Gpu() {
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        window = SDL_CreateWindow("cluster cull tests", 64, 64, SDL_WINDOW_HIDDEN);
        const auto assets = std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/sprites";
        ready = window && device.open(window, assets);
        if (!ready) std::cerr << device.error() << " " << SDL_GetError() << '\n';
    }
    ~Gpu() {
        device.close();
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

// What the shader is supposed to decide, decided again on the CPU. Comparing
// a GPU result against a second implementation of the same rule is the only
// way a GPU-driven path is tested rather than believed: a cull that silently
// keeps everything and a cull that silently keeps nothing both draw a picture.
std::vector<std::vector<std::uint32_t>> reference(std::span<const engine::GeometryCluster> clusters,
                                                  const engine::ClusterSelection& selection,
                                                  std::size_t buckets, std::size_t capacity) {
    std::vector<std::vector<std::uint32_t>> kept(buckets);
    const auto dot = [](const float* row, const float* point) {
        return row[0] * point[0] + row[1] * point[1] + row[2] * point[2] + row[3];
    };
    for (const auto& cluster : clusters) {
        const float depth = dot(selection.rowW, cluster.centre);
        const auto outside = [&](const float* axis, float sign) {
            std::array<double,4> plane{};
            for (int i=0;i<4;++i) plane[i]=selection.rowW[i]+sign*axis[i];
            const double distance=plane[0]*cluster.centre[0]+plane[1]*cluster.centre[1]+
                                  plane[2]*cluster.centre[2]+plane[3];
            return distance < -cluster.radius*std::hypot(std::hypot(plane[0],plane[1]),plane[2]);
        };
        if (outside(selection.rowX,0) || outside(selection.rowX,1) || outside(selection.rowX,-1) ||
            outside(selection.rowY,1) || outside(selection.rowY,-1)) continue;
        const float scale = selection.focal > 0 ? selection.focal / std::max(depth, 1e-3f)
                                                : selection.halfWidth;
        if (cluster.error * scale > selection.pixelError) continue;
        if (cluster.parentError * scale <= selection.pixelError) continue;
        if (cluster.bucket >= buckets) continue;
        if (kept[cluster.bucket].size() >= capacity) continue;
        kept[cluster.bucket].push_back(cluster.payload);
    }
    for (auto& bucket : kept) std::sort(bucket.begin(), bucket.end());
    return kept;
}

// A perspective-ish selection: the rows a camera would hand over.
engine::ClusterSelection viewAt(float pixelError) {
    engine::ClusterSelection selection;
    selection.rowX[0] = 1;
    selection.rowY[1] = 1;
    selection.rowW[2] = 1;      // depth is z
    selection.rowW[3] = 0;
    selection.pixelError = pixelError;
    selection.halfWidth = 640;
    selection.halfHeight = 400;
    selection.focal = 900;
    return selection;
}

struct Harness {
    Gpu gpu;
    // Declared after the device and therefore destroyed before it. A pipeline
    // owns compute pipelines, and releasing one against a device that has
    // already gone is a crash at exit rather than anywhere near the mistake -
    // which is what a shared static one did.
    engine::RenderPipeline pipeline{0};
    engine::ClusterCuller culler;
    // Wide enough that the selection tests never overflow a bucket. They must
    // not: past the capacity the survivors are whichever threads won the
    // atomic, which is a different set from the first N in input order and
    // legitimately so. Overflow has its own test, which checks the clamping
    // rather than the membership.
    std::size_t buckets = 4, capacity = 1024;

    bool start() {
        if (!gpu.ready) return false;
        if (!culler.setup(gpu.device, pipeline, 4096, capacity, buckets)) {
            std::cerr << "cluster culler setup: " << gpu.device.error() << '\n';
            return false;
        }
        CHECK(culler.ready()); // reset occupies slot 0, which is a valid slot
        std::vector<engine::DrawArguments> draws(buckets);
        for (std::size_t i = 0; i < buckets; ++i) draws[i] = {6, 0, 0, 0, 0};
        return culler.describe(gpu.device, draws);
    }

    // Runs one selection and reads back what the card decided.
    struct Result {
        std::vector<engine::DrawArguments> arguments;
        std::vector<std::vector<std::uint32_t>> kept;
        std::vector<std::uint32_t> feedback;
    };
    Result run(std::span<const engine::GeometryCluster> clusters,
               const engine::ClusterSelection& selection, bool compact = false) {
        culler.clusters(clusters);
        engine::Frame frame{};
        frame.device = &gpu.device;
        frame.commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        // The pipeline runs dispatches from run(); here the dispatches are
        // recorded directly so the test needs no window, no targets and no
        // swapchain - only the compute half of the frame.
        culler.dispatch(frame, pipeline, selection, compact);
        pipeline.runDispatches(frame);
        gpu.device.submitFrame(frame.commands);

        Result result;
        result.arguments.resize(buckets);
        gpu.device.readBuffer(culler.arguments(), result.arguments.data(),
                              result.arguments.size() * sizeof(engine::DrawArguments));
        std::vector<std::uint32_t> visible(buckets * capacity);
        gpu.device.readBuffer(culler.visible(), visible.data(), visible.size() * sizeof(std::uint32_t));
        result.kept.resize(buckets);
        for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
            const auto count = std::min<std::size_t>(result.arguments[bucket].instanceCount, capacity);
            result.kept[bucket].assign(visible.begin() + std::ptrdiff_t(bucket * capacity),
                                       visible.begin() + std::ptrdiff_t(bucket * capacity + count));
            std::sort(result.kept[bucket].begin(), result.kept[bucket].end());
        }
        result.feedback.resize(buckets);
        CHECK(culler.readPageFeedback(gpu.device, result.feedback));
        return result;
    }
};

std::vector<engine::GeometryCluster> scatter(std::size_t count, std::size_t buckets) {
    std::vector<engine::GeometryCluster> clusters;
    clusters.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        engine::GeometryCluster cluster;
        const double t = double(i);
        cluster.centre[0] = float(std::sin(t * 0.37) * 900);
        cluster.centre[1] = float(std::cos(t * 0.29) * 700);
        cluster.centre[2] = float(40 + std::fmod(t * 13.0, 1800.0));   // in front of the eye
        cluster.radius = float(2 + std::fmod(t, 7.0));
        cluster.error = float(0.05 + std::fmod(t * 0.017, 1.4));
        cluster.parentError = std::numeric_limits<float>::infinity();
        cluster.bucket = std::uint32_t(i % buckets);
        cluster.payload = std::uint32_t(i);
        clusters.push_back(cluster);
    }
    return clusters;
}
}

TEST(cluster_cull_gpu_selects_exactly_what_the_rule_says) {
    Harness harness;
    if (!harness.start()) return;   // no GPU in this environment
    const auto clusters = scatter(1500, harness.buckets);
    for (const float allowance : {0.25f, 1.0f, 4.0f}) {
        const auto selection = viewAt(allowance);
        const auto result = harness.run(clusters, selection);
        const auto expected = reference(clusters, selection, harness.buckets, harness.capacity);
        for (std::size_t bucket = 0; bucket < harness.buckets; ++bucket) {
            CHECK_EQ(result.kept[bucket], expected[bucket]);
            CHECK_EQ(std::size_t(result.arguments[bucket].instanceCount), expected[bucket].size());
        }
        // And the rest of each draw's arguments is untouched by the cull: the
        // count is the only thing the card is allowed to decide.
        for (const auto& arguments : result.arguments) {
            CHECK_EQ(arguments.indexCount, 6u);
            CHECK_EQ(arguments.firstIndex, 0u);
            CHECK_EQ(arguments.firstInstance, 0u);
        }
    }
}

TEST(mesh_root_gpu_expands_one_record_per_object_into_world_clusters) {
    Gpu gpu;
    if (!gpu.ready) return;
    engine::RenderPipeline pipeline{0};
    engine::MeshRootCuller roots;
    const float infinity = std::numeric_limits<float>::infinity();
    const engine::GeometryCluster local[] = {
        {{1, 2, 3}, .5f, .2f, infinity, 0, 0},
        {{-1, 0, 1}, 1.0f, .4f, .8f, 1, 0}};
    CHECK(roots.setup(gpu.device, pipeline, local, 4, 8));
    const engine::MeshRootInstance input[] = {
        {{10, 20, 30}, 2.0f, 1.57079632679f, 0, 2, 4, 0, 64},
        {{-10, -20, 5}, 1.0f, 0.0f, 0, 2, 8, 2, 128}};
    engine::Frame frame{};
    frame.device = &gpu.device;
    frame.commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(roots.dispatch(frame, pipeline, input));
    pipeline.runDispatches(frame);
    CHECK(gpu.device.submitFrame(frame.commands));
    std::vector<engine::GeometryCluster> output(4);
    CHECK(gpu.device.readBuffer(roots.candidates(), output.data(),
                                output.size() * sizeof(engine::GeometryCluster)));
    CHECK(std::abs(output[0].centre[0] - 6.0f) < 0.01f);
    CHECK(std::abs(output[0].centre[1] - 22.0f) < 0.01f);
    CHECK(std::abs(output[0].centre[2] - 36.0f) < 0.01f);
    CHECK(std::abs(output[0].radius - 1.0f) < 0.01f);
    CHECK_EQ(output[0].bucket, 4u);
    CHECK_EQ(output[0].payload, 64u);
    CHECK(std::abs(output[2].centre[0] + 9.0f) < 0.01f);
    CHECK(std::abs(output[2].centre[1] + 18.0f) < 0.01f);
    CHECK(std::abs(output[2].centre[2] - 8.0f) < 0.01f);
    CHECK_EQ(output[2].bucket, 8u);
    CHECK_EQ(output[2].payload, 128u);
}

TEST(mesh_dag_gpu_walks_from_a_coarse_root_to_the_fitting_child) {
    Gpu gpu;
    if (!gpu.ready) return;
    engine::RenderPipeline pipeline{0};
    engine::MeshRootCuller roots;
    const float infinity = std::numeric_limits<float>::infinity();
    const engine::MeshStaticCluster clusters[] = {
        {{{0, 0, 10}, 1, 0, 2, 0, 0}, 0xffffffffu, 0},
        {{{0, 0, 10}, 2, 2, infinity, 1, 0}, 0, 0xffffffffu}};
    const engine::MeshFamilyRange families[]{{0, 1}};
    const std::uint32_t children[]{0};
    CHECK(roots.setup(gpu.device, pipeline, clusters, families, children, 2, 2));
    const engine::MeshRootInstance input[]{{{0, 0, 0}, 1, 0, 0, 2, 0, 0, 77, 0}};
    engine::Frame frame{};
    frame.device = &gpu.device;
    frame.commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(roots.dispatch(frame, pipeline, input, viewAt(1.0f)));
    pipeline.runDispatches(frame);
    CHECK(gpu.device.submitFrame(frame.commands));
    std::array<engine::GeometryCluster, 2> output{};
    CHECK(gpu.device.readBuffer(roots.candidates(), output.data(), sizeof(output)));
    CHECK_EQ(output[0].bucket, 0u);
    CHECK_EQ(output[0].payload, 77u);
    CHECK_EQ(output[1].bucket, 0xffffffffu);
}

TEST(instance_hierarchy_root_gpu_preserves_proxy_bounds_and_payload) {
    Gpu gpu;
    if (!gpu.ready) return;
    engine::RenderPipeline pipeline{0};
    engine::InstanceRootCuller roots;
    CHECK(roots.setup(gpu.device, pipeline, 4));
    const engine::InstanceHierarchyRoot input[] = {
        {{4, 5, 6}, 2.0f, .25f, 3.0f, 1, 256},
        {{-4, -5, 7}, 8.0f, 2.0f, std::numeric_limits<float>::infinity(), 3, 512}};
    engine::Frame frame{};
    frame.device = &gpu.device;
    frame.commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(roots.dispatch(frame, pipeline, input));
    pipeline.runDispatches(frame);
    CHECK(gpu.device.submitFrame(frame.commands));
    std::array<engine::GeometryCluster, 2> output{};
    CHECK(gpu.device.readBuffer(roots.candidates(), output.data(), sizeof(output)));
    CHECK(std::abs(output[0].centre[0] - 4.0f) < 0.01f);
    CHECK(std::abs(output[0].centre[1] - 5.0f) < 0.01f);
    CHECK(std::abs(output[0].radius - 2.0f) < 0.01f);
    CHECK_EQ(output[0].bucket, 1u);
    CHECK_EQ(output[0].payload, 256u);
    CHECK(std::isinf(output[1].parentError));
    CHECK_EQ(output[1].bucket, 3u);
    CHECK_EQ(output[1].payload, 512u);
}

TEST(instance_hierarchy_dag_gpu_walks_explicit_children) {
    Gpu gpu;
    if (!gpu.ready) return;
    engine::RenderPipeline pipeline{0};
    engine::InstanceRootCuller roots;
    CHECK(roots.setupHierarchy(gpu.device, pipeline, 4, 2, 4));
    const float infinity = std::numeric_limits<float>::infinity();
    const engine::InstanceHierarchyStaticNode nodes[] = {
        {{{0, 0, 10}, 1, 0, 2, 7, 0}, 0, 0, 99},
        {{{0, 0, 10}, 2, 2, infinity, 0xffffffffu, 0}, 0, 1, 0}};
    const std::uint32_t children[]{0};
    const engine::InstanceHierarchyTraversalRoot root{0, 2, 0};
    engine::Frame frame{};
    frame.device = &gpu.device;
    frame.commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(roots.dispatchHierarchy(frame, pipeline, nodes, children, std::span(&root, 1),
                                  viewAt(1.0f)));
    pipeline.runDispatches(frame);
    CHECK(gpu.device.submitFrame(frame.commands));
    std::array<engine::GeometryCluster, 2> output{};
    CHECK(gpu.device.readBuffer(roots.hierarchyCandidates(), output.data(), sizeof(output)));
    CHECK_EQ(output[0].bucket, 7u);
    CHECK_EQ(output[0].payload, 99u);
    CHECK_EQ(output[1].bucket, 0xffffffffu);
    // The child is intentionally invalid here: this checks traversal and
    // sentinel safety independently from the representation bucket policy.
}

TEST(cluster_cull_gpu_is_a_cut_rather_than_a_threshold) {
    // A hierarchy: every cluster has a parent with twice its error. Exactly one
    // level along each path must be chosen - no gaps, no double coverage - and
    // that is what makes the cut crack-free.
    Harness harness;
    if (!harness.start()) return;
    std::vector<engine::GeometryCluster> clusters;
    constexpr int kPaths = 200, kLevels = 5;
    for (int path = 0; path < kPaths; ++path)
        for (int level = 0; level < kLevels; ++level) {
            engine::GeometryCluster cluster;
            cluster.centre[0] = float(path * 3 - 300);
            cluster.centre[1] = 0;
            cluster.centre[2] = float(200 + path);
            cluster.radius = 1;
            cluster.error = float(0.02 * std::pow(2.0, level));
            cluster.parentError = level + 1 < kLevels
                                          ? float(0.02 * std::pow(2.0, level + 1))
                                          : std::numeric_limits<float>::infinity();
            cluster.bucket = 0;
            cluster.payload = std::uint32_t(path);
            clusters.push_back(cluster);
        }
    for (const float allowance : {0.5f, 2.0f, 9.0f}) {
        const auto result = harness.run(clusters, viewAt(allowance));
        // One cluster per path that is on screen at all, and never two.
        auto kept = result.kept[0];
        const auto unique = std::unique(kept.begin(), kept.end());
        CHECK_EQ(unique, kept.end());
        CHECK(!kept.empty());
        const auto expected = reference(clusters, viewAt(allowance), harness.buckets, harness.capacity);
        CHECK_EQ(result.kept[0], expected[0]);
    }
}

TEST(cluster_cull_gpu_compacts_bucket_outputs_into_dense_first_instances) {
    Harness harness;
    if (!harness.start()) return;
    const float infinity = std::numeric_limits<float>::infinity();
    std::vector<engine::GeometryCluster> clusters;
    clusters.push_back({{0, 0, 10}, 1, 0, infinity, 0, 10});
    clusters.push_back({{0, 0, 10}, 1, 0, infinity, 0, 11});
    clusters.push_back({{0, 0, 10}, 1, 0, infinity, 2, 12});
    const auto result = harness.run(clusters, viewAt(1), true);
    CHECK_EQ(result.arguments[0].instanceCount, 2u);
    CHECK_EQ(result.arguments[0].firstInstance, 0u);
    CHECK_EQ(result.arguments[1].instanceCount, 0u);
    CHECK_EQ(result.arguments[1].firstInstance, 2u);
    CHECK_EQ(result.arguments[2].instanceCount, 1u);
    CHECK_EQ(result.arguments[2].firstInstance, 2u);
}

TEST(cluster_cull_gpu_bucket_overflow_is_clamped_and_never_written_past) {
    Harness harness;
    if (!harness.start()) return;
    // Three times what one bucket can hold, all of it visible and all of it in
    // one bucket. The count must stop at the capacity rather than promising the
    // draw more instances than were written.
    std::vector<engine::GeometryCluster> clusters;
    for (std::size_t i = 0; i < harness.capacity * 3; ++i) {
        engine::GeometryCluster cluster;
        cluster.centre[2] = 300;
        cluster.radius = 1;
        cluster.error = 0;
        cluster.parentError = std::numeric_limits<float>::infinity();
        cluster.bucket = 1;
        cluster.payload = std::uint32_t(i);
        clusters.push_back(cluster);
    }
    const auto result = harness.run(clusters, viewAt(1.0f));
    CHECK_EQ(std::size_t(result.arguments[1].instanceCount), harness.capacity);
    CHECK_EQ(result.kept[1].size(), harness.capacity);
    // Every survivor is one of the payloads offered, and no two are the same.
    auto kept = result.kept[1];
    CHECK_EQ(std::unique(kept.begin(), kept.end()), kept.end());
    for (const auto payload : kept) CHECK(payload < harness.capacity * 3);
    // The other buckets are untouched, so an overflow stays inside its slice.
    for (std::size_t bucket = 0; bucket < harness.buckets; ++bucket)
        if (bucket != 1) CHECK_EQ(result.arguments[bucket].instanceCount, 0u);
}

TEST(cluster_cull_gpu_starts_every_frame_from_nothing) {
    // The counts are reset by a dispatch, not by a fresh allocation. A frame
    // that inherited the previous one's counts would draw the previous frame's
    // instances out of a list this frame only partly overwrote.
    Harness harness;
    if (!harness.start()) return;
    const auto clusters = scatter(600, harness.buckets);
    const auto busy = harness.run(clusters, viewAt(4.0f));
    std::size_t total = 0;
    for (const auto& arguments : busy.arguments) total += arguments.instanceCount;
    CHECK(total > 0);
    // Now a selection that keeps nothing: every error is far too large.
    const auto empty = harness.run(clusters, viewAt(1e-6f));
    for (const auto& arguments : empty.arguments) CHECK_EQ(arguments.instanceCount, 0u);
    // And back again, to the same answer as the first time.
    const auto again = harness.run(clusters, viewAt(4.0f));
    for (std::size_t bucket = 0; bucket < harness.buckets; ++bucket) {
        CHECK_EQ(again.arguments[bucket].instanceCount, busy.arguments[bucket].instanceCount);
        CHECK_EQ(again.kept[bucket], busy.kept[bucket]);
    }
}

TEST(cluster_cull_gpu_keeps_spheres_crossing_perspective_side_planes) {
    Harness h;
    const bool ready=h.start();CHECK(ready);if (!ready) return;
    const float infinity=std::numeric_limits<float>::infinity();
    // Side-plane distance is (x-z)/sqrt(2): at x=11.2,z=10,r=1,
    // part of the sphere is visible although x > z+r. The old test lost it.
    const std::vector<engine::GeometryCluster> clusters{
        {{11.2f,0,10},1,0,infinity,0,0}, {{-11.2f,0,10},1,0,infinity,0,1},
        {{0,11.2f,10},1,0,infinity,0,2}, {{0,-11.2f,10},1,0,infinity,0,3},
        {{12,0,10},1,0,infinity,0,4}, {{0,0,-0.5f},1,0,infinity,0,5},
        {{0,0,-2},1,0,infinity,0,6}};
    const auto result=h.run(clusters,viewAt(1));
    CHECK_EQ(result.kept[0],(std::vector<std::uint32_t>{0,1,2,3,5}));
    auto scaled=viewAt(1);
    for (int i=0;i<4;++i) { scaled.rowX[i]*=3;scaled.rowY[i]*=3;scaled.rowW[i]*=3; }
    CHECK_EQ(h.run(clusters,scaled).kept[0],result.kept[0]);
}

TEST(cluster_cull_gpu_orthographic_sides_and_empty_input) {
    Harness h;
    const bool ready=h.start();CHECK(ready);if (!ready) return;
    auto view=viewAt(1);view.focal=0;view.halfWidth=1;view.rowW[2]=0;view.rowW[3]=1;
    const float infinity=std::numeric_limits<float>::infinity();
    const std::vector<engine::GeometryCluster> clusters{
        {{1.1f,0,100},0.2f,0,infinity,0,0}, {{1.3f,0,100},0.2f,0,infinity,0,1}};
    CHECK_EQ(h.run(clusters,view).kept[0],(std::vector<std::uint32_t>{0}));
    const auto empty=h.run({},view);
    for (const auto& args:empty.arguments) CHECK_EQ(args.instanceCount,0u);
}

TEST(cluster_cull_gpu_requests_missing_page_without_drawing_a_hole) {
    Harness h;
    const bool ready = h.start(); CHECK(ready); if (!ready) return;
    const std::uint32_t missing[] = {engine::ClusterCuller::kPageMissing,
                                     engine::ClusterCuller::kPageResident,
                                     engine::ClusterCuller::kPageResident,
                                     engine::ClusterCuller::kPageResident};
    CHECK(h.culler.updatePageTable(h.gpu.device, missing));
    const float infinity = std::numeric_limits<float>::infinity();
    const std::vector<engine::GeometryCluster> clusters{{{0, 0, 10}, 1, 0, infinity, 0, 77}};
    const auto result = h.run(clusters, viewAt(1));
    CHECK(result.kept[0].empty());
    CHECK_EQ(result.arguments[0].instanceCount, 0u);
    CHECK_EQ(result.feedback[0], 1u);
}

TEST(cluster_cull_gpu_fallback_page_bypasses_the_cut_but_stays_visible) {
    Harness h;
    const bool ready = h.start(); CHECK(ready); if (!ready) return;
    const std::uint32_t fallback[] = {engine::ClusterCuller::kPageFallback,
                                      engine::ClusterCuller::kPageResident,
                                      engine::ClusterCuller::kPageResident,
                                      engine::ClusterCuller::kPageResident};
    CHECK(h.culler.updatePageTable(h.gpu.device, fallback));
    const std::vector<engine::GeometryCluster> clusters{
        {{0, 0, 10}, 1, 1000, std::numeric_limits<float>::infinity(), 0, 91}};
    const auto result = h.run(clusters, viewAt(1));
    CHECK_EQ(result.kept[0], (std::vector<std::uint32_t>{91}));
    CHECK_EQ(result.feedback[0], 0u);
}
