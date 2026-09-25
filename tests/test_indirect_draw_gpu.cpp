#include "framework.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <vector>

#include "engine/render/device.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/cluster_cull.hpp"
#include "engine/render/render_pipeline.hpp"
#include "engine/render/mesh.hpp"
#include "engine/render/systems/cluster_draws.hpp"

// Proving the draw path before anything is built on it.
//
// One indexed indirect multi-draw is what turns a forest of batches into a
// single call, and what a GPU cluster cull hands back. Three of the five
// numbers in a draw argument have never been used by this engine: a vertex
// offset, a first index into a shared buffer, and a first instance. The lesson
// from the grass detour is written into the plan - check a new device
// capability with its own GPU test BEFORE the first thing depends on it,
// because while the picture is blank every probe says nothing is wrong.
namespace {
struct Gpu {
    SDL_Window* window = nullptr;
    engine::Device device;
    bool ready = false;
    Gpu() {
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        window = SDL_CreateWindow("indirect draw tests", 64, 64, SDL_WINDOW_HIDDEN);
        const auto assets =
                std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/sprites";
        ready = window && device.open(window, assets);
        if (!ready) std::cerr << device.error() << " " << SDL_GetError() << '\n';
    }
    ~Gpu() {
        device.close();
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
};

constexpr std::uint32_t kWidth = 128, kHeight = 64;

struct Instance {
    float place[2];
    float tint[4];
};

// Everything is a full-height stripe, so which way the target counts its rows
// never enters the answer - only where a column lands, which the clip x says
// on its own.
struct Picture {
    std::vector<std::uint8_t> pixels;
    [[nodiscard]] std::array<int, 3> at(double clipX) const {
        const auto column = std::uint32_t((clipX * 0.5 + 0.5) * kWidth);
        const auto row = kHeight / 2;
        const auto offset = (std::size_t(row) * kWidth + std::min(column, kWidth - 1)) * 4;
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
    }
    [[nodiscard]] std::array<int, 3> at(double clipX, double fraction) const {
        const auto column = std::uint32_t((clipX * 0.5 + 0.5) * kWidth);
        const auto row = std::uint32_t(fraction * kHeight);
        const auto offset = (std::size_t(std::min(row, kHeight - 1)) * kWidth +
                             std::min(column, kWidth - 1)) * 4;
        return {pixels[offset], pixels[offset + 1], pixels[offset + 2]};
    }
};

bool near(std::array<int, 3> got, std::array<int, 3> wanted) {
    for (int i = 0; i < 3; ++i)
        if (std::abs(got[i] - wanted[i]) > 6) return false;
    return true;
}

// Draws `draws` with one indirect call and hands back what landed on the target.
bool render(Gpu& gpu, std::span<const engine::DrawArguments> draws, Picture& into) {
    engine::VertexLayout layout;
    layout.buffers = {{0, sizeof(float) * 2, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                      {1, sizeof(Instance), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    layout.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
                         {1, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(Instance, place)},
                         {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(Instance, tint)}};
    engine::PipelineWanted wanted;
    wanted.shaderFile = "indirect_probe.hlsl";
    wanted.vertexEntry = "ProbeVS";
    wanted.fragmentEntry = "ProbePS";
    wanted.buffers = layout.buffers;
    wanted.attributes = layout.attributes;
    wanted.depthTest = wanted.depthWrite = false;
    auto graphics = gpu.device.makePipeline(wanted);
    if (!graphics) {
        std::cerr << gpu.device.error() << '\n';
        return false;
    }

    // Two shapes in one buffer. The second is twice as wide, so a draw that
    // ignored its vertex offset paints the first one and says so.
    const float vertices[]{0.0f, -1.0f, 0.10f, -1.0f, 0.0f, 1.0f, 0.10f, 1.0f,
                           0.0f, -1.0f, 0.20f, -1.0f, 0.0f, 1.0f, 0.20f, 1.0f};
    // A whole stripe, then - at index six - the lower triangle of one.
    const std::uint32_t indices[]{0, 1, 2, 2, 1, 3, 0, 1, 2};
    const Instance instances[]{{{-0.95f, 0}, {1, 0, 0, 1}},  {{-0.70f, 0}, {0, 1, 0, 1}},
                               {{-0.40f, 0}, {0, 0, 1, 1}},  {{-0.10f, 0}, {1, 1, 0, 1}},
                               {{0.20f, 0}, {0, 1, 1, 1}},   {{0.60f, 0}, {1, 0, 1, 1}}};

    auto vertexBuffer = gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices, sizeof(vertices));
    auto indexBuffer = gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices, sizeof(indices));
    auto instanceBuffer =
            gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, instances, sizeof(instances));
    auto arguments = gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDIRECT, draws.data(),
                                             draws.size() * sizeof(engine::DrawArguments));
    if (!vertexBuffer || !indexBuffer || !instanceBuffer || !arguments) return false;

    SDL_GPUTextureCreateInfo colourInfo{};
    colourInfo.type = SDL_GPU_TEXTURETYPE_2D;
    colourInfo.format = engine::Device::kColourFormat;
    colourInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    colourInfo.width = kWidth;
    colourInfo.height = kHeight;
    colourInfo.layer_count_or_depth = 1;
    colourInfo.num_levels = 1;
    auto colour = gpu.device.makeTexture(colourInfo);
    SDL_GPUTextureCreateInfo depthInfo = colourInfo;
    depthInfo.format = engine::Device::kDepthFormat;
    depthInfo.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    auto depth = gpu.device.makeTexture(depthInfo);
    if (!colour || !depth) return false;

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    if (!commands) return false;
    SDL_GPUColorTargetInfo target{};
    target.texture = colour.get();
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    target.clear_color = {0, 0, 0, 1};
    SDL_GPUDepthStencilTargetInfo depthTarget{};
    depthTarget.texture = depth.get();
    depthTarget.load_op = SDL_GPU_LOADOP_CLEAR;
    depthTarget.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depthTarget.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    depthTarget.clear_depth = 1.0f;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(commands, &target, 1, &depthTarget);
    if (!pass) return false;
    SDL_BindGPUGraphicsPipeline(pass, graphics.get());
    const SDL_GPUBufferBinding streams[]{{vertexBuffer.get(), 0}, {instanceBuffer.get(), 0}};
    SDL_BindGPUVertexBuffers(pass, 0, streams, 2);
    const SDL_GPUBufferBinding index{indexBuffer.get(), 0};
    SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
    // The whole point: one call, however many draws the arguments describe.
    SDL_DrawGPUIndexedPrimitivesIndirect(pass, arguments.get(), 0, std::uint32_t(draws.size()));
    SDL_EndGPURenderPass(pass);
    if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands)) {
        SDL_WaitForGPUFences(gpu.device.handle(), true, &fence, 1);
        SDL_ReleaseGPUFence(gpu.device.handle(), fence);
    } else {
        return false;
    }
    into.pixels.assign(std::size_t(kWidth) * kHeight * 4, 0);
    return gpu.device.readTexture(colour.get(), into.pixels.data(), kWidth, kHeight);
}
}

TEST(indirect_draw_reads_every_number_its_arguments_carry) {
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    const engine::DrawArguments draws[]{
            // A whole stripe of the narrow shape, for the first instance.
            {6, 1, 0, 0, 0},
            // The same six indices against the second shape: twice as wide.
            {6, 1, 0, 4, 1},
            // Three indices from the middle of the buffer: half a stripe.
            {3, 1, 6, 0, 2},
            // One draw, two instances, starting at the fourth.
            {6, 2, 0, 0, 3},
            // And a draw of nothing, which is how a selection turns one off.
            {6, 0, 0, 0, 5}};
    Picture picture;
    CHECK(render(gpu, draws, picture));
    if (picture.pixels.empty()) return;

    // The first instance: red, and only as wide as the narrow shape.
    CHECK(near(picture.at(-0.90), {255, 0, 0}));
    CHECK(near(picture.at(-0.80), {0, 0, 0}));       // past its 0.1 of width
    // The second: green, and twice as wide - so the vertex offset was read.
    CHECK(near(picture.at(-0.60), {0, 255, 0}));
    CHECK(near(picture.at(-0.55), {0, 255, 0}));
    CHECK(near(picture.at(-0.45), {0, 0, 0}));
    // The third started six indices in, so it is a triangle, not a stripe:
    // present against its leading edge and gone against its trailing one.
    CHECK(near(picture.at(-0.39), {0, 0, 255}));
    CHECK(near(picture.at(-0.315, 0.5), {0, 0, 0}));
    // The fourth draw painted two instances from the fourth onward.
    CHECK(near(picture.at(-0.05), {255, 255, 0}));
    CHECK(near(picture.at(0.25), {0, 255, 255}));
    // And the draw of nothing painted nothing, rather than one of everything.
    CHECK(near(picture.at(0.65), {0, 0, 0}));
}

TEST(indirect_draw_of_a_single_argument_is_the_same_picture_as_one_of_many) {
    // A cluster cull writes the instance counts and leaves the rest alone, so
    // the same arguments must mean the same thing whether five draws are
    // submitted together or one at a time. If they did not, a frame would
    // change when the selection merely emptied a bucket.
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    const engine::DrawArguments together[]{{6, 1, 0, 0, 0}, {6, 1, 0, 4, 1}, {6, 2, 0, 0, 3}};
    Picture all;
    CHECK(render(gpu, together, all));
    if (all.pixels.empty()) return;

    Picture piece;
    std::vector<std::uint8_t> summed(all.pixels.size(), 0);
    for (const auto& one : together) {
        const engine::DrawArguments single[]{one};
        CHECK(render(gpu, single, piece));
        for (std::size_t i = 0; i < summed.size(); ++i)
            summed[i] = std::uint8_t(std::min(255, summed[i] + piece.pixels[i]));
    }
    // The stripes do not overlap, so drawing them apart and adding is the same
    // picture as drawing them together - except for the alpha the clear left.
    std::size_t differing = 0;
    for (std::size_t i = 0; i < summed.size(); ++i)
        if (i % 4 != 3 && std::abs(int(summed[i]) - int(all.pixels[i])) > 6) ++differing;
    CHECK_EQ(differing, std::size_t(0));
}

TEST(indirect_draw_paints_what_the_cluster_plan_said_it_would) {
    // The end of the chain, checked as one thing: an offline cut, turned into
    // draw arguments by planDraws, issued as one command, and read back as
    // pixels. Each piece is tested on its own; this is the test that says they
    // were wired to each other rather than each to nothing.
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    // Two clusters of one asset - the near half of a stripe and the far half -
    // replaced by one coarser cluster that covers the whole stripe.
    std::vector<engine::geometry::MeshCluster> clusters(3);
    clusters[0].indices = {0, 6};
    clusters[0].error = 0;
    clusters[0].parentError = 0.5f;
    clusters[1].indices = {0, 6};
    clusters[1].error = 0;
    clusters[1].parentError = 0.5f;
    clusters[2].indices = {0, 6};
    clusters[2].error = 0.5f;
    clusters[2].parentError = std::numeric_limits<float>::infinity();
    for (auto& cluster : clusters) cluster.radius = 1;

    // No chain beside it, so the plan cannot decide the chain is cheaper - what
    // is under test here is the cut reaching the card, and that rule has its
    // own test in asr_render_system_tests.
    const engine::render::MeshGeometry assets[]{{0, {}, clusters, 0}};

    // Fine enough for the two: both draw, at the narrow shape.
    const engine::render::DrawRun fine[]{{0, 0, 0, 1, 0.1}};
    const auto detailed = engine::render::planDraws(fine, assets);
    CHECK_EQ(detailed.fromClusters, std::size_t(2));
    CHECK_EQ(detailed.draws.size(), std::size_t(2));
    // Coarse enough that only the replacement draws.
    const engine::render::DrawRun coarse[]{{0, 0, 1, 1, 0.9}};
    const auto simple = engine::render::planDraws(coarse, assets);
    CHECK_EQ(simple.fromClusters, std::size_t(1));

    // The plan names instance zero and instance one, which sit at different
    // places, so which cut was issued is visible rather than inferred.
    Picture cutFine, cutCoarse;
    CHECK(render(gpu, detailed.draws, cutFine));
    CHECK(render(gpu, simple.draws, cutCoarse));
    if (cutFine.pixels.empty() || cutCoarse.pixels.empty()) return;
    CHECK(near(cutFine.at(-0.90), {255, 0, 0}));       // the fine cut, at instance zero
    CHECK(near(cutFine.at(-0.65), {0, 0, 0}));
    CHECK(near(cutCoarse.at(-0.65), {0, 255, 0}));     // the coarse cut, at instance one
    CHECK(near(cutCoarse.at(-0.90), {0, 0, 0}));
}

#include "game/render/world_materials.hpp"

TEST(scene_model_shader_still_builds_with_the_streams_the_pass_declares) {
    // A shader is compiled at run time, so a mistake in one is not a build
    // error - it is a pass that quietly draws nothing, on a machine that is
    // not this one. Asking the device to build it costs a second and is the
    // only thing that catches a stream the shader stopped agreeing with.
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    const auto layout = game::materials::sceneModelLayout();
    const auto material = game::materials::sceneModels();
    engine::PipelineWanted wanted = material->pipeline(layout);
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    CHECK(bool(pipeline));
}

TEST(scene_model_cluster_gather_shader_builds) {
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    auto gather=gpu.device.makeCompute({"scene_model_cluster_gather.hlsl","GatherCS"});
    if (!gather) std::cerr << gpu.device.error() << '\n';
    CHECK(bool(gather));
}

TEST(scene_instance_hierarchy_gather_shader_builds) {
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    auto gather=gpu.device.makeCompute({"scene_instance_hierarchy_gather.hlsl","GatherCS"});
    if (!gather) std::cerr << gpu.device.error() << '\n';
    CHECK(bool(gather));
}

TEST(scene_model_cluster_cull_and_gather_compacts_a_real_instance) {
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    engine::RenderPipeline pipeline{0};
    engine::ClusterCuller culler;
    CHECK(culler.setup(gpu.device,pipeline,16,8,1));
    const engine::DrawArguments description{6,0,0,0,0};
    CHECK(culler.describe(gpu.device,std::span(&description,1)));
    auto gather=gpu.device.makeCompute({"scene_model_cluster_gather.hlsl","GatherCS"});
    CHECK(bool(gather));
    const auto gatherSlot=pipeline.take(std::move(gather));

    // ModelInstance as the frame arena stores it: sixteen floats, with a
    // non-zero transform so a zero/garbled ByteAddressBuffer binding cannot
    // accidentally pass the check.
    std::array<float,16> source{3,4,5,2, 0.25f,0.5f,0.75f,1,
                                7,8,9,10, 11,12,13,0};
    const auto sourceBuffer=gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                                     source.data(),sizeof(source));
    const std::array<float,4> morph[]{ {0,0,0,0}, {std::numeric_limits<float>::infinity(),0,0,0} };
    const auto morphBuffer=gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                                    morph,sizeof(morph));
    const auto output=gpu.device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|
                                             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|
                                             SDL_GPU_BUFFERUSAGE_VERTEX,
                                             sizeof(source));
    CHECK(sourceBuffer && morphBuffer && output);

    engine::GeometryCluster cluster;
    cluster.centre[2]=10;
    cluster.radius=1;
    cluster.error=0;
    cluster.parentError=std::numeric_limits<float>::infinity();
    cluster.bucket=0;
    cluster.payload=0;
    culler.clusters(std::span(&cluster,1));
    engine::ClusterSelection selection;
    selection.rowX[0]=1; selection.rowY[1]=1; selection.rowW[2]=1;
    selection.pixelError=1; selection.halfWidth=1; selection.focal=0;
    engine::Frame frame{};
    frame.device=&gpu.device;
    frame.commands=SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    culler.dispatch(frame,pipeline,selection,true);
    engine::ComputeDispatch job;
    job.pipeline=gatherSlot;
    job.groupsX=1;
    job.reads={culler.visible(),culler.arguments(),morphBuffer.get(),sourceBuffer.get()};
    job.writes={output.get()};
    const std::uint32_t bucketCapacity=8, bucketCount=1;
    std::memcpy(&job.own[0],&bucketCapacity,sizeof(bucketCapacity));
    std::memcpy(&job.own[1],&bucketCount,sizeof(bucketCount));
    job.own[2]=1; job.own[3]=1;
    job.own[4]=0; job.own[8]=0; job.own[9]=0; job.own[10]=1; job.own[11]=0;
    pipeline.dispatch(std::move(job));
    pipeline.runDispatches(frame);
    gpu.device.submitFrame(frame.commands);

    std::array<float,16> compacted{};
    std::array<float,16> uploaded{};
    CHECK(gpu.device.readBuffer(sourceBuffer.get(),uploaded.data(),sizeof(uploaded)));
    CHECK_EQ(uploaded[0],source[0]);
    CHECK(gpu.device.readBuffer(output.get(),compacted.data(),sizeof(compacted)));
    engine::DrawArguments resultArguments{};
    CHECK(gpu.device.readBuffer(culler.arguments(),&resultArguments,sizeof(resultArguments)));
    std::uint32_t visiblePayload=0xffffffffu;
    CHECK(gpu.device.readBuffer(culler.visible(),&visiblePayload,sizeof(visiblePayload)));
    CHECK_EQ(compacted[0],source[0]);
    CHECK_EQ(compacted[1],source[1]);
    CHECK_EQ(compacted[2],source[2]);
    CHECK_EQ(compacted[3],source[3]);
    CHECK_EQ(compacted[12],source[12]);
}

TEST(scene_instance_hierarchy_gather_compacts_a_real_instance) {
    Gpu gpu;
    if (!gpu.ready) {
        std::cerr << "no GPU device; skipping\n";
        return;
    }
    engine::RenderPipeline pipeline{0};
    engine::ClusterCuller culler;
    CHECK(culler.setup(gpu.device,pipeline,16,8,1));
    const engine::DrawArguments description{6,0,0,0,0};
    CHECK(culler.describe(gpu.device,std::span(&description,1)));
    auto gather=gpu.device.makeCompute({"scene_instance_hierarchy_gather.hlsl","GatherCS"});
    CHECK(bool(gather));
    const auto gatherSlot=pipeline.take(std::move(gather));

    const std::array<float,16> source{13,14,15,2, 0.25f,0.5f,0.75f,1,
                                      7,8,9,10, 11,12,13,0};
    const auto sourceBuffer=gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ,
                                                     source.data(),sizeof(source));
    const auto output=gpu.device.makeBuffer(SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|
                                             SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|
                                             SDL_GPU_BUFFERUSAGE_VERTEX,
                                             sizeof(source));
    CHECK(sourceBuffer && output);

    engine::GeometryCluster cluster;
    cluster.centre[2]=10;
    cluster.radius=1;
    cluster.error=0;
    cluster.parentError=std::numeric_limits<float>::infinity();
    cluster.bucket=0;
    cluster.payload=0;
    culler.clusters(std::span(&cluster,1));
    engine::ClusterSelection selection;
    selection.rowX[0]=1; selection.rowY[1]=1; selection.rowW[2]=1;
    selection.pixelError=1; selection.halfWidth=1; selection.focal=0;
    engine::Frame frame{};
    frame.device=&gpu.device;
    frame.commands=SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    culler.dispatch(frame,pipeline,selection,true);
    engine::ComputeDispatch job;
    job.pipeline=gatherSlot;
    job.groupsX=1;
    job.reads={culler.visible(),culler.arguments(),sourceBuffer.get()};
    job.writes={output.get()};
    const std::uint32_t bucketCapacity=8, bucketCount=1;
    std::memcpy(&job.own[0],&bucketCapacity,sizeof(bucketCapacity));
    std::memcpy(&job.own[1],&bucketCount,sizeof(bucketCount));
    pipeline.dispatch(std::move(job));
    pipeline.runDispatches(frame);
    gpu.device.submitFrame(frame.commands);

    std::array<float,16> compacted{};
    CHECK(gpu.device.readBuffer(output.get(),compacted.data(),sizeof(compacted)));
    CHECK_EQ(compacted[0],source[0]);
    CHECK_EQ(compacted[1],source[1]);
    CHECK_EQ(compacted[2],source[2]);
    CHECK_EQ(compacted[3],source[3]);
    CHECK_EQ(compacted[12],source[12]);
}
