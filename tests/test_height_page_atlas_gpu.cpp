#include "framework.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <limits>
#include <chrono>
#include <thread>
#include "engine/render/frame.hpp"

#include "game/generation/world_map_gen.hpp"
#include "game/render/height_page_atlas.hpp"
#include "game/render/height_page_stream.hpp"
#include "game/render/gpu_terrain.hpp"
#include "game/world/world_system.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/world/terrain_lod.hpp"

namespace {
struct Gpu {
    SDL_Window* window = nullptr;
    engine::Device device;
    bool ready = false;
    Gpu() {
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        window = SDL_CreateWindow("height atlas tests", 64, 64, SDL_WINDOW_HIDDEN);
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

world::streaming::BaseTile makePage(const game::HeightPageAtlas::Layout& layout, int x) {
    world::streaming::BaseTile page;
    page.key = {x, -3, layout.level()};
    page.sampleMetres = layout.sampleMetres();
    page.width = page.height = world::streaming::interiorSamples(page.sampleMetres);
    page.padding = world::streaming::kDefaultPaddingSamples;
    page.elevationMin = core::Fixed::fromInt(-200);
    page.elevationMax = core::Fixed::fromInt(1800);
    page.heightQuantized.resize(page.sampleCount());
    for (std::size_t i = 0; i < page.heightQuantized.size(); ++i)
        page.heightQuantized[i] = static_cast<std::uint16_t>(i * 257u + std::uint32_t(x) * 997u);
    return page;
}

bool matches(engine::Device& device, const game::HeightPageAtlas& atlas,
             const world::streaming::BaseTile& page) {
    const auto address = atlas.find(page.key);
    if (!address) return false;
    const Uint32 side = atlas.layout().storedSamples();
    // A padded transfer stride also exercises odd R16 page widths on Metal.
    const Uint32 pitch = ((side * 2 + 255) / 256) * 256;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = pitch * side;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
            device.handle(), SDL_CreateGPUTransferBuffer(device.handle(), &info));
    if (!transfer) return false;
    auto* commands = SDL_AcquireGPUCommandBuffer(device.handle());
    if (!commands) return false;
    auto* copy = SDL_BeginGPUCopyPass(commands);
    if (!copy) { SDL_CancelGPUCommandBuffer(commands); return false; }
    SDL_GPUTextureRegion source{atlas.binding().texture, 0, 0, address->x, address->y, 0,
                                side, side, 1};
    SDL_GPUTextureTransferInfo destination{transfer.get(), 0, pitch / 2, side};
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) return false;
    const bool completed = SDL_WaitForGPUFences(device.handle(), true, &fence, 1);
    SDL_ReleaseGPUFence(device.handle(), fence);
    if (!completed) return false;
    const auto* bytes = static_cast<const std::byte*>(
            SDL_MapGPUTransferBuffer(device.handle(), transfer.get(), false));
    if (!bytes) return false;
    bool same = true;
    for (Uint32 row = 0; row < side; ++row)
        same = same && std::memcmp(bytes + row * pitch,
                                   page.heightQuantized.data() + row * side, side * 2) == 0;
    SDL_UnmapGPUTransferBuffer(device.handle(), transfer.get());
    return same;
}
}

TEST(height_page_stream_progress_tracks_the_actual_queue_and_completion) {
    generation::WorldMapData world;
    world.width = world.height = 1;
    world.cells.resize(1);
    world.cells[0].elevation = 100;
    const world::streaming::HydrologyGraph graph;
    world::streaming::PageStore pages(world, graph,
        {core::Fixed::fromInt(-200), core::Fixed::fromInt(1800)},
        {128u << 20, world::streaming::kDefaultPaddingSamples, 1});
    game::HeightPageStream stream(pages);
    stream.frozen(true);
    stream.wants({{0, 0, 4}, {0, 0, 4}}, {{0, 0, 2}});
    CHECK_EQ(stream.stats().workers, std::size_t(1));
    CHECK_EQ(stream.stats().busy, std::size_t(0));
    CHECK_EQ(stream.stats().queued, std::size_t(2));
    stream.wants({{0, 0, 4}}, {}); // obsolete prediction must disappear
    CHECK_EQ(stream.stats().queued, std::size_t(1));
    stream.frozen(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (stream.stats().ready == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    CHECK_EQ(stream.stats().ready, std::size_t(1));
    CHECK_EQ(stream.stats().busy, std::size_t(0));
    CHECK_EQ(stream.stats().queued, std::size_t(0));
    CHECK_EQ(stream.stats().failed, std::size_t(0));
#if ASR_ENABLE_PROFILING
    // Only a diagnostics build measures how long a page took. Asserting it
    // unconditionally asserts that the timers were not compiled out, which is
    // the opposite of what the flag is for.
    CHECK(stream.stats().lastBuildMs > 0);
    CHECK(stream.stats().longestBuildMs >= stream.stats().lastBuildMs);
#else
    CHECK_EQ(stream.stats().lastBuildMs, 0.0);
#endif
    CHECK_EQ(stream.collect().size(), std::size_t(1));
    CHECK_EQ(stream.stats().ready, std::size_t(0));
}

TEST(height_page_stream_h8_and_background_share_one_budget_without_eager_h4) {
    generation::WorldMapData world;
    world.width = world.height = 1;
    world.cells.resize(1);
    world.cells[0].elevation = 100;
    const world::streaming::HydrologyGraph graph;
    world::streaming::PageStore pages(world, graph,
        {core::Fixed::fromInt(-200), core::Fixed::fromInt(1800)},
        {128u << 20, world::streaming::kDefaultPaddingSamples, 1});
    game::HeightPageStream first(pages), second(pages);
    pages.prebakeInBackground({2, 4});
    first.wants({{0, 0, 1}}, {});
    second.wants({{1, 0, 1}}, {});
    const auto wait = [&](const auto& ready) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return ready();
    };
    CHECK(wait([&] { return first.stats().ready == 1 && second.stats().ready == 1; }));
    CHECK_EQ(first.stats().workers, std::size_t(1));
    CHECK_EQ(second.stats().workers, std::size_t(1));
    CHECK_EQ(pages.workerPool().stats().workers, std::size_t(1));
    CHECK(!pages.resident({0, 0, 0}));
    CHECK(!pages.resident({1, 0, 0}));
    const auto h8 = first.collect();
    CHECK_EQ(h8.size(), std::size_t(1));
    if (h8.empty()) return;
    CHECK_EQ(h8.front().source->base.sampleMetres, 8);
    CHECK(h8.front().source->medium.deltaQuantized.empty());
    first.wants({{0, 0, 0}}, {});
    CHECK(wait([&] { return first.stats().ready == 1; }));
    const auto h4 = first.collect();
    CHECK_EQ(h4.size(), std::size_t(1));
    if (h4.empty()) return;
    CHECK_EQ(h4.front().source->base.sampleMetres, 4);
    CHECK(h4.front().source->reusedSamples > 0);
    CHECK(!h4.front().source->medium.deltaQuantized.empty());
    CHECK(wait([&] { return !pages.prebakeProgress().running; }));
    CHECK_EQ(pages.prebakeProgress().failed, std::size_t(0));
}

TEST(height_atlas_gpu_preserves_all_r16_samples_and_neighbours_across_eviction) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    for (const int step : {4, 8, 16, 64}) {
        game::HeightPageAtlas atlas(gpu.device, {step, 2, 2});
        CHECK(bool(atlas));
        if (!atlas) return;
        const auto a = makePage(atlas.layout(), -1);
        const auto b = makePage(atlas.layout(), 0);
        const auto c = makePage(atlas.layout(), 1);
        const auto d = makePage(atlas.layout(), 2);
        const auto e = makePage(atlas.layout(), 3);
        {
            engine::Device::Uploader uploader(gpu.device);
            for (const auto* page : {&a, &b, &c, &d}) {
                CHECK(atlas.upload(uploader, *page, 1).has_value());
                CHECK(atlas.pin(page->key, 1));
                CHECK(!atlas.find(page->key).has_value()); // not submitted yet
            }
            CHECK(!atlas.upload(uploader, e, 2).has_value()); // all slots pinned
            const bool submitted = uploader.finish();
            CHECK(submitted);
            atlas.publishUploads(submitted);
        }
        for (const auto* page : {&a, &b, &c, &d}) CHECK(matches(gpu.device, atlas, *page));
        const auto old = atlas.find(b.key);
        CHECK(old.has_value());
        if (!old) return;
        CHECK(old->x > 0);
        CHECK(atlas.unpin(b.key));
        {
            engine::Device::Uploader uploader(gpu.device);
            const auto reused = atlas.upload(uploader, e, 3);
            CHECK(reused.has_value());
            if (reused) CHECK_EQ(reused->slot, old->slot);
            CHECK(!atlas.find(b.key).has_value());
            const bool submitted = uploader.finish();
            CHECK(submitted);
            atlas.publishUploads(submitted);
        }
        for (const auto* page : {&a, &c, &d, &e}) CHECK(matches(gpu.device, atlas, *page));
        {
            engine::Device::Uploader uploader(gpu.device);
            const auto same = atlas.upload(uploader, a, 4);
            CHECK(same.has_value());
            CHECK(atlas.find(a.key).has_value()); // cache hit needs no new publication
            CHECK(uploader.finish());
        }
        CHECK(!atlas.find({a.key.x, a.key.y, std::uint8_t(atlas.layout().level() + 1)}));
        atlas.clear();
        CHECK(!atlas.find(a.key));
        CHECK(!atlas.find(e.key));
    }
}

TEST(height_page_stream_completion_budget_is_bytes_not_eight_pages) {
    using Stream = game::HeightPageStream;
    CHECK(Stream::kCompletionBytes / Stream::uploadBytes({0, 0, 4}) > 256);
    CHECK(Stream::kCompletionBytes / Stream::uploadBytes({0, 0, 2}) > 100);
    CHECK(Stream::kCompletionBytes / Stream::uploadBytes({0, 0, 0}) < 16);
    CHECK(Stream::uploadBytes({0, 0, 0}) > Stream::uploadBytes({0, 0, 4}) * 100);
}

TEST(height_atlas_gpu_does_not_publish_a_failed_batch_and_can_retry_it) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    game::HeightPageAtlas atlas(gpu.device, {64, 1, 1});
    CHECK(bool(atlas));
    if (!atlas) return;
    const auto page = makePage(atlas.layout(), 1);
    {
        engine::Device::Uploader uploader(gpu.device);
        CHECK(atlas.upload(uploader, page, 1).has_value());
        // A later transfer fails: none of this batch may be published.
        CHECK(!uploader.refillRegion(nullptr, page.heightQuantized.data(), 0, 0, 1, 1, 2));
        const bool submitted = uploader.finish();
        CHECK(!submitted);
        atlas.publishUploads(submitted);
        CHECK(!atlas.find(page.key));
    }
    {
        engine::Device::Uploader uploader(gpu.device);
        CHECK(atlas.upload(uploader, page, 2).has_value());
        const bool submitted = uploader.finish();
        CHECK(submitted);
        atlas.publishUploads(submitted);
    }
    CHECK(matches(gpu.device, atlas, page));
    {
        engine::Device::Uploader uploader(gpu.device);
        auto invalid = makePage(atlas.layout(), 2);
        invalid.heightQuantized.pop_back();
        CHECK(!atlas.upload(uploader, invalid, 3));
        CHECK(atlas.find(page.key).has_value()); // invalid input never evicts good data
        CHECK(uploader.finish());
    }
}

TEST(height_atlas_gpu_rejects_overflow_before_reading_pixels) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    game::HeightPageAtlas atlas(gpu.device, {64, 1, 1});
    CHECK(bool(atlas));
    if (!atlas) return;
    const std::uint16_t pixel = 0;
    engine::Device::Uploader uploader(gpu.device);
    CHECK(!uploader.refillRegion(atlas.binding().texture, &pixel, 0, 0,
                                 std::numeric_limits<Uint32>::max(), 2, 2));
    CHECK(!uploader.finish());
    CHECK(gpu.device.error().find("exceeds transfer buffer size") != std::string::npos);
}

TEST(height_page_configuration_is_loaded_before_gpu_setup) {
    generation::WorldMapData map;
    map.width = map.height = 2;
    map.cells.resize(4);
    for (auto& cell : map.cells) cell.elevation = 10;
    world::streaming::PageStore::Config settings;
    settings.workerCount = 1;
    world::WorldSystem system(settings);
    system.publish(std::move(map));
    const auto preparation = system.prepare(system.read());
    CHECK(preparation != nullptr);
    if (!preparation) return;
    // No Device, window or ensure(): pipelines consult this configuration in setup.
    game::GpuTerrain terrain(preparation);
    world::terrain::TerrainConfig expected;
    std::string error;
    CHECK(world::terrain::readTerrainConfig(
        std::filesystem::path(__FILE__).parent_path().parent_path() / "content/config/terrain.json",
        expected, error));
    CHECK(terrain.config() == expected);
}

TEST(height_page_packed_water_proof_includes_padding_and_inland_kind) {
    using namespace world::streaming;
    auto source = std::make_shared<BakedPage>();
    source->base = makePage({16, 1, 1}, 0);
    CHECK(!game::packHeightPage(source).mayHaveWater); // actual packed defaults
    CHECK(game::PackedHeightPage{}.mayHaveWater); // no proof
    auto& water = source->water;
    water.width = water.height = source->base.width;
    water.padding = source->base.padding;
    water.sampleMetres = source->base.sampleMetres;
    const auto count = source->base.sampleCount();
    water.surfaceQuantized.resize(count);
    water.waterBodyId.assign(count, kInvalidWaterBodyId);
    water.riverId.resize(count); water.shoreDecimetres.resize(count);
    water.coverage.resize(count); water.flowX.resize(count); water.flowY.resize(count);
    CHECK(water.valid());
    CHECK(!game::packHeightPage(source).mayHaveWater);
    // A single padded corner is enough to invalidate dryness. Cover alone is
    // not proof: lake/river shaders can use depth at close zoom with cover=0.
    for (const auto i : {std::size_t(0), count / 2, count - 1}) {
        water.coverage[i] = 1;
        CHECK(game::packHeightPage(source).mayHaveWater);
        water.coverage[i] = 0;
        water.surfaceQuantized[i] = 1;
        CHECK(!water.wet(i)); // an extended bank head is not coverage
        CHECK(!game::packHeightPage(source).mayHaveWater);
        water.riverId[i] = 1;
        CHECK(game::packHeightPage(source).mayHaveWater);
        water.riverId[i] = kInvalidRiverId;
        water.surfaceQuantized[i] = 0;
        water.waterBodyId[i] = 7; // lake, zero coverage/head
        CHECK(game::packHeightPage(source).mayHaveWater);
        water.waterBodyId[i] = kOceanWaterBodyId;
        CHECK(!game::packHeightPage(source).mayHaveWater); // ocean cover clip is unconditional
        water.coverage[i] = 1; // ocean swash/apron must stay
        CHECK(game::packHeightPage(source).mayHaveWater);
        water.coverage[i] = 0;
        water.waterBodyId[i] = kInvalidWaterBodyId;
    }
    CHECK(!game::packHeightPage(source).mayHaveWater);
}

TEST(height_page_packed_water_preserves_heads_without_inventing_rivers) {
    using namespace world::streaming;
    auto source = std::make_shared<BakedPage>();
    source->base = makePage({16, 1, 1}, 0);
    const HsimQuantisation quant{source->base.elevationMin, source->base.elevationMax};
    const auto sea = quant.quantise(core::kZero);
    auto& water = source->water;
    water.width = water.height = source->base.width;
    water.padding = source->base.padding;
    water.sampleMetres = source->base.sampleMetres;
    const auto count = source->base.sampleCount();
    water.surfaceQuantized.assign(count, sea);
    water.waterBodyId.resize(count); water.riverId.resize(count);
    water.shoreDecimetres.resize(count); water.coverage.resize(count);
    water.flowX.resize(count); water.flowY.resize(count);
    CHECK(water.valid());
    CHECK(!game::packHeightPage(source).mayHaveWater);
    const int side = source->base.width + 2 * source->base.padding;
    const std::size_t bank = std::size_t(side) * (side / 2) + side / 2;
    water.coverage[bank] = 51; // stencil sees ocean, centre itself is dry
    auto packed = game::packHeightPage(source);
    CHECK(packed.mayHaveWater);
    CHECK_EQ(packed.fields[3][bank * 4 + 2], 0); // not a river
    CHECK_EQ(packed.fields[3][bank * 4 + 3], 0);
    water.coverage[bank] = 0;
    const auto head = quant.quantise(core::Fixed::fromInt(45));
    water.surfaceQuantized[bank] = head;
    water.riverId[bank] = 1;
    packed = game::packHeightPage(source);
    CHECK(!water.wet(bank));
    CHECK_EQ(packed.fields[0][bank * 4 + 2], head);
    CHECK_EQ(packed.fields[3][bank * 4 + 2], 65535);

    water.riverId[bank] = 0;
    water.waterBodyId[bank + 1] = 2;
    water.surfaceQuantized[bank + 1] = head;
    packed = game::packHeightPage(source);
    CHECK_EQ(packed.fields[3][bank * 4 + 3], 65535);
    // Linear filtering against a dry bank retains the lake's level.
    for (const double blend : {0.0, 0.1, 0.5, 0.9, 1.0})
        CHECK_EQ(std::lerp(double(packed.fields[0][bank * 4 + 2]),
                           double(packed.fields[0][(bank + 1) * 4 + 2]), blend), double(head));

    // A different, earlier diagonal candidate must not hide the closest body.
    water.surfaceQuantized[bank] = sea;
    water.waterBodyId[bank - side - 1] = 3;
    water.surfaceQuantized[bank - side - 1] = quant.quantise(core::Fixed::fromInt(80));
    packed = game::packHeightPage(source);
    CHECK_EQ(packed.fields[0][bank * 4 + 2], head);
    // A dry river bank with a different head is not relabelled as that lake.
    const auto riverHead = quant.quantise(core::Fixed::fromInt(70));
    water.surfaceQuantized[bank] = riverHead;
    water.riverId[bank] = 1;
    packed = game::packHeightPage(source);
    CHECK_EQ(packed.fields[0][bank * 4 + 2], riverHead);
    CHECK_EQ(packed.fields[3][bank * 4 + 2], 65535);
    CHECK_EQ(packed.fields[3][bank * 4 + 3], 0);
}

TEST(height_atlas_gpu_dry_proof_follows_publication_eviction_and_clear) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    game::HeightPageAtlas atlas(gpu.device, {16, 1, 1}, true);
    auto source = std::make_shared<world::streaming::BakedPage>();
    source->base = makePage(atlas.layout(), 0);
    const auto dry = game::packHeightPage(source);
    CHECK(!dry.mayHaveWater);
    const auto key = source->base.key;
    CHECK(atlas.mayHaveWater(key));
    for (const bool fail : {true, false}) {
        engine::Device::Uploader upload(gpu.device);
        CHECK(atlas.upload(upload, dry, 1));
        CHECK(atlas.mayHaveWater(key)); // staged fields are not published
        if (fail) CHECK(!upload.refillRegion(nullptr, source->base.heightQuantized.data(), 0, 0, 1, 1, 2));
        const bool submitted = upload.finish();
        CHECK_EQ(submitted, !fail);
        atlas.publishUploads(submitted);
        CHECK_EQ(atlas.mayHaveWater(key), fail);
    }
    auto replacement = std::make_shared<world::streaming::BakedPage>();
    replacement->base = makePage(atlas.layout(), 1);
    auto unknown = game::packHeightPage(replacement);
    unknown.mayHaveWater = true; // conservative metadata is permitted
    engine::Device::Uploader upload(gpu.device);
    CHECK(atlas.upload(upload, unknown, 2));
    CHECK(atlas.mayHaveWater(key)); // evicted: don't inherit another slot's proof
    const bool submitted = upload.finish();
    CHECK(submitted); atlas.publishUploads(submitted);
    CHECK(atlas.mayHaveWater(replacement->base.key));
    atlas.clear();
    CHECK(atlas.mayHaveWater(key));
    CHECK(atlas.mayHaveWater(replacement->base.key));
    game::HeightPageAtlas heightsOnly(gpu.device, {16, 1, 1});
    engine::Device::Uploader heights(gpu.device);
    CHECK(heightsOnly.upload(heights, source->base, 3));
    const bool heightSubmitted = heights.finish();
    CHECK(heightSubmitted); heightsOnly.publishUploads(heightSubmitted);
    CHECK(heightsOnly.mayHaveWater(key));
}

TEST(height_atlas_gpu_packed_fields_and_height_publish_together) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    game::HeightPageAtlas atlas(gpu.device, {16, 1, 1}, true);
    CHECK(bool(atlas));
    auto source = std::make_shared<world::streaming::BakedPage>();
    source->base = makePage(atlas.layout(), 0);
    auto packed = game::packHeightPage(source);
    CHECK_EQ(packed.fields[0][0], std::uint16_t(32768));
    CHECK_EQ(packed.fields[1][0], std::uint16_t(65535));
    engine::Device::Uploader upload(gpu.device);
    CHECK(atlas.upload(upload, packed, 1).has_value());
    CHECK(!atlas.find(source->base.key));
    const bool submitted = upload.finish();
    CHECK(submitted);
    atlas.publishUploads(submitted);
    CHECK(atlas.find(source->base.key).has_value());
    CHECK(matches(gpu.device, atlas, source->base));
    packed.fields[2].pop_back();
    engine::Device::Uploader invalid(gpu.device);
    CHECK(!atlas.upload(invalid, packed, 2));
    CHECK(atlas.find(source->base.key).has_value());
}

TEST(height_atlas_gpu_h4_waits_for_reverse_morph_and_submission_fence) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    game::HeightPageAtlas atlas(gpu.device, {4, 1, 1});
    const auto page = makePage(atlas.layout(), 0), replacement = makePage(atlas.layout(), 1);
    engine::Device::Uploader upload(gpu.device);
    CHECK(atlas.upload(upload, page, 1).has_value());
    CHECK(atlas.pin(page.key, 1)); // transition ownership
    CHECK(atlas.pin(page.key, 1)); // in-flight GPU lease
    const bool submitted = upload.finish();
    CHECK(submitted); atlas.publishUploads(submitted);
    const auto serial = gpu.device.nextSubmission();
    auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(commands != nullptr);
    if (!commands) return;
    CHECK(gpu.device.submitFrame(commands));
    world::terrain::RefinementTransition transition(0.25);
    transition.requestChildren(true); transition.tick(1);
    transition.requestParent(); transition.tick(0.1);
    engine::Device::Uploader retry(gpu.device);
    CHECK(!transition.fineDataMayEvict());
    CHECK(!atlas.upload(retry, replacement, 2));
    transition.tick(1);
    CHECK(transition.fineDataMayEvict());
    CHECK(atlas.unpin(page.key));
    CHECK(!atlas.upload(retry, replacement, 3)); // reverse morph alone is insufficient
    CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
    CHECK(gpu.device.completedSubmission() >= serial);
    CHECK(atlas.unpin(page.key));
    CHECK(atlas.upload(retry, replacement, 4).has_value());
    const bool done = retry.finish();
    CHECK(done); atlas.publishUploads(done);
    CHECK(matches(gpu.device, atlas, replacement));
}

TEST(height_atlas_gpu_page_vertex_shaders_compile_on_the_active_backend) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    for (const bool water : {false, true}) {
        engine::PipelineWanted wanted;
        wanted.shaderFile = water ? "water_pages.hlsl" : "terrain_pages.hlsl";
        wanted.vertexEntry = water ? "WaterPageVS" : "TerrainPageVS";
        wanted.fragmentEntry = water ? "WaterPagePS" : "TerrainPagePS";
        wanted.buffers = {{0, sizeof(world::terrain::GridVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
        wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, 0}};
        auto pipeline = gpu.device.makePipeline(wanted);
        if (!pipeline) std::cerr << gpu.device.error() << '\n';
        CHECK(bool(pipeline));
    }
}

TEST(height_atlas_gpu_adaptive_vertex_shaders_compile_on_the_active_backend) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    for (const bool water : {false,true}) {
        engine::PipelineWanted wanted;
        wanted.shaderFile = water ? "water_pages.hlsl" : "terrain_pages.hlsl";
        wanted.vertexEntry = water ? "AdaptiveWaterVS" : "AdaptiveTerrainVS";
        wanted.fragmentEntry = water ? "WaterPagePS" : "TerrainPagePS";
        wanted.buffers = {{0,sizeof(world::terrain::AdaptiveVertex),SDL_GPU_VERTEXINPUTRATE_VERTEX,0}};
        wanted.attributes = {{0,0,SDL_GPU_VERTEXELEMENTFORMAT_USHORT4,0},
            {1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,8},
            {2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,16},
            {3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,24},
            {4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(world::terrain::AdaptiveVertex,priorBed)},
            {5,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(world::terrain::AdaptiveVertex,diagnostic)},
            {6,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(world::terrain::AdaptiveVertex,drainage)},
            {7,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(world::terrain::AdaptiveVertex,displayFrom)}};
        auto pipeline = gpu.device.makePipeline(wanted);
        if (!pipeline) std::cerr << gpu.device.error() << '\n';
        CHECK(bool(pipeline));
    }
}

static void checkPageSurfaceGpu(bool adaptive, bool water = false, bool stages = false, bool display = false) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    std::array<std::unique_ptr<game::HeightPageAtlas>, 4> atlases;
    std::array<std::array<float, 4>, 16 * 4> table{};
    engine::Device::Uploader upload(gpu.device);
    for (int level = 0; level < 4; ++level) {
        const int step = level == 0 ? 4 : level == 1 ? 8 : level == 2 ? 16 : 64;
        atlases[level] = std::make_unique<game::HeightPageAtlas>(gpu.device,
            game::HeightPageAtlas::Layout(step, 1, 1), true);
        auto source = std::make_shared<world::streaming::BakedPage>();
        auto& page = source->base;
        page = makePage(atlases[level]->layout(), 0);
        page.key.y = 0;
        page.elevationMin = core::kZero;
        page.elevationMax = core::Fixed::fromInt(1024);
        const world::streaming::HsimQuantisation quant{page.elevationMin, page.elevationMax};
        std::fill(page.heightQuantized.begin(), page.heightQuantized.end(), quant.quantise(core::Fixed::fromInt(100)));
        if (level <= 1) {
            // At (8,8): h=100, gradient=(0.25,0.5). Shading uses the
            // selected dataset's stencil, even on the same test grid.
            const int side = static_cast<int>(atlases[level]->layout().storedSamples());
            for (int y = 0; y < side; ++y)
                for (int x = 0; x < side; ++x) {
                    const double dx = (x - page.padding) * step - 8.0;
                    const double dy = (y - page.padding) * step - 8.0;
                    const double height = 100 + 0.25 * dx + 0.5 * dy + 0.01 * (dx * dx + dy * dy);
                    page.heightQuantized[y * side + x] = quant.quantise(core::Fixed::fromDoubleForContent(height));
                }
        }
        if (level == 2) {
            const auto side = atlases[level]->layout().storedSamples();
            const auto at = std::size_t(page.padding) * side + page.padding;
            page.heightQuantized[at] = quant.quantise(core::Fixed::fromInt(50));
            page.heightQuantized[at + 1] = quant.quantise(core::Fixed::fromInt(200));
            page.heightQuantized[at + side] = quant.quantise(core::Fixed::fromInt(800));
            page.heightQuantized[at + side + 1] = quant.quantise(core::Fixed::fromInt(100));
        }
        const auto packed = game::packHeightPage(source);
        const auto address = atlases[level]->upload(upload, packed, 1);
        CHECK(address.has_value());
        if (!address) return;
        // Neighbour lookups near (0,0) resolve to this fixture's actual padded
        // samples. H8's two-step shading stencil reaches -8 m from the probe.
        for (int y = 1; y <= 2; ++y)
            for (int x = 1; x <= 2; ++x)
                table[level * 16 + y * 4 + x] = {
                    address->uvOrigin[0] + (x - 2) * 512.0f * address->uvPerMetre[0],
                    address->uvOrigin[1] + (y - 2) * 512.0f * address->uvPerMetre[1],
                    address->uvPerMetre[0], address->uvPerMetre[1]};
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 4;
    info.layer_count_or_depth = 4; info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto mapping = gpu.device.makeTexture(info);
    CHECK(bool(mapping));
    for (int layer = 0; layer < 4; ++layer)
        CHECK(upload.refillRegion(mapping.get(), table.data() + layer * 16, 0, 0, 4, 4, 16, layer));
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = engine::Device::kColourFormat;
    info.width = info.height = info.layer_count_or_depth = 1;
    auto climate = gpu.device.makeTexture(info);
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    auto waterTexture = gpu.device.makeTexture(info);
    info.type = SDL_GPU_TEXTURETYPE_2D;
    const std::uint32_t zero = 0;
    CHECK(upload.refillRegion(climate.get(), &zero, 0, 0, 1, 1, 4));
    CHECK(upload.refillRegion(waterTexture.get(), &zero, 0, 0, 1, 1, 4));
    const bool submitted = upload.finish();
    CHECK(submitted);
    for (auto& atlas : atlases) atlas->publishUploads(submitted);
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    auto target = gpu.device.makeTexture(info);
    CHECK(bool(target));
    SDL_GPUSamplerCreateInfo samplerInfo{};
    auto sampler = gpu.device.makeSampler(samplerInfo);
    std::vector<SDL_GPUTextureSamplerBinding> bindings(3, {climate.get(), sampler.get()});
    for (const auto& atlas : atlases) bindings.push_back(atlas->binding());
    bindings.push_back({mapping.get(), sampler.get()});
    for (const auto& atlas : atlases) bindings.push_back(atlas->fieldBinding());
    std::vector<SDL_GPUTextureSamplerBinding> fragmentBindings{{water?waterTexture.get():climate.get(), sampler.get()}};
    fragmentBindings.insert(fragmentBindings.end(), bindings.begin() + 3, bindings.end());

    engine::PipelineWanted wanted;
    wanted.shaderFile = water ? "water_page_probe.hlsl" : "terrain_page_probe.hlsl";
    wanted.vertexEntry = water ? "WaterDetailProbeVS" : adaptive ? "AdaptivePageProbeVS" : "TerrainPageProbeVS";
    wanted.fragmentEntry = water ? "WaterDetailProbePS" : adaptive ? "AdaptivePageProbePS" : "TerrainPageProbePS";
    wanted.buffers = {{0, sizeof(world::terrain::AdaptiveVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, 0}};
    if (adaptive) {
        wanted.attributes.push_back({1,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,8});
        wanted.attributes.push_back({2,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,16});
        wanted.attributes.push_back({3,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,24});
        wanted.attributes.push_back({4,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(world::terrain::AdaptiveVertex,priorBed)});
        wanted.attributes.push_back({5,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(world::terrain::AdaptiveVertex,diagnostic)});
        wanted.attributes.push_back({6,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,offsetof(world::terrain::AdaptiveVertex,drainage)});
        wanted.attributes.push_back({7,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(world::terrain::AdaptiveVertex,displayFrom)});
    }
    wanted.depthTest = wanted.depthWrite = false;
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    CHECK(bool(pipeline));
    if (!pipeline) return;
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = 256;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(), SDL_CreateGPUTransferBuffer(gpu.device.handle(), &transferInfo));
    CHECK(bool(transfer));
    for (const std::uint16_t baseFlags : {0,2,3,4,6,7}) {
    const std::uint16_t flags = baseFlags | (display ? 8 : 0);
    if (!adaptive && flags != 0) continue;
    world::terrain::AdaptiveVertex point{1,1,flags,0,350,450,600,700,250,300,80,120,200,{}};
    point.displayFrom = {150,320,60};
    const world::terrain::AdaptiveVertex points[3]{point,point,point};
    auto vertices = gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, points, sizeof(points));
    CHECK(bool(vertices));
    for (const int childDataset : {0, 1})
    for (const float morph : {0.0f, 0.5f, 1.0f})
    for (const float displayPhase : {0.0f,0.25f,0.5f,0.75f,1.0f})
    for (const float phase : {0.0f,0.5f,1.0f}) {
        if (!stages && phase!=1.0f) continue;
        if (!display && displayPhase!=1.0f) continue;
        auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        CHECK(commands != nullptr);
        engine::Scene scene{};
        const float parameters[16]{morph, stages?3.0f+phase:2.0f, float(childDataset), 8, 0, 0, 0, 1024, 2, adaptive?displayPhase:16.0f, 32, 14, 4, 4, 100, 1};
        SDL_PushGPUVertexUniformData(commands, 0, &scene, sizeof(scene));
        SDL_PushGPUVertexUniformData(commands, 1, parameters, sizeof(parameters));
        SDL_PushGPUFragmentUniformData(commands, 0, &scene, sizeof(scene));
        SDL_PushGPUFragmentUniformData(commands, 1, parameters, sizeof(parameters));
        SDL_GPUColorTargetInfo colour{};
        colour.texture = target.get();
        colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
        auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        CHECK(render != nullptr);
        SDL_BindGPUGraphicsPipeline(render, pipeline.get());
        const SDL_GPUBufferBinding buffer{vertices.get(), 0};
        SDL_BindGPUVertexBuffers(render, 0, &buffer, 1);
        SDL_BindGPUVertexSamplers(render, 0, bindings.data(), static_cast<Uint32>(bindings.size()));
        SDL_BindGPUFragmentSamplers(render, 0, fragmentBindings.data(), static_cast<Uint32>(fragmentBindings.size()));
        SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render);
        auto* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureRegion region{target.get(), 0, 0, 0, 0, 0, 1, 1, 1};
        const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, 64, 1};
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
        SDL_EndGPUCopyPass(copy);
        const auto serial = gpu.device.nextSubmission();
        CHECK(gpu.device.submitFrame(commands));
        CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
        CHECK(gpu.device.completedSubmission() >= serial);
        const auto* pixel = static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
        CHECK(pixel != nullptr);
        if (!pixel) return;
        const int actual = pixel[0];
        const int shadedHeight = pixel[1], normalZ = pixel[2], curvature = pixel[3];
        SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
        if (adaptive) {
            float bed = flags & 2 ? 600.0f : std::lerp(flags & 4 ? 250.0f : 100.0f,350.0f,morph);
            float head = flags & 2 ? 700.0f : std::lerp(flags & 4 ? 300.0f : 0.0f,450.0f,morph);
            if (stages) {
                const float prior=flags&2?200.0f:std::lerp(80.0f,120.0f,morph);
                bed=std::lerp(prior,bed,phase*phase*(3-2*phase));
                if (phase<1) head=-6000;
            }
            if (display) {
                const float from = stages ? std::lerp(60.0f,150.0f,phase*phase*(3-2*phase)) : 150.0f;
                bed = std::lerp(from,bed,displayPhase);
                if (!stages || phase>=1) head=std::lerp(320.0f,head,displayPhase);
            }
            if (flags&1) bed=std::min(bed-32,100.0f);
            if (water) {
                if (flags & 4) {
                    CHECK(std::abs(actual-int(std::lround(std::clamp((head-bed)/1024+0.5f,0.0f,1.0f)*255)))<=1);
                    CHECK_EQ(shadedHeight,flags&1?0:255);
                } else {
                    CHECK_EQ(shadedHeight,0); // legacy grid still uses the dry page
                }
                continue;
            }
            CHECK(std::abs(actual - int(std::lround(bed/1024*255))) <= 1);
            CHECK(std::abs(shadedHeight - int(std::lround(std::clamp(head/1024,0.0f,1.0f)*255))) <= 1);
            continue;
        }
        // NE--SW at (8,8): (200+800)/2 = 500, not bilinear 287.5 or NW--SE 75.
        const int expected = int(std::lround(std::lerp(100.0f, 500.0f, morph) / 1024.0f * 255.0f));
        CHECK(std::abs(actual - expected) <= 1);
        // Pixel height follows the full field, not the parent triangle plane.
        const int expectedShading = int(std::lround(std::lerp(100.0f, 287.5f, morph) / 1024.0f * 255.0f));
        CHECK(std::abs(shadedHeight - expectedShading) <= 1);
        if (morph == 0.0f) {
            CHECK(std::abs(normalZ - int(std::lround(255.0 / std::sqrt(1.3125)))) <= 1);
            const double openness = childDataset == 0 ? 0.46 : 0.42;
            CHECK(std::abs(curvature - int(std::lround(openness * 255.0))) <= 1);
        }
    }
    }
}

TEST(height_atlas_gpu_shared_grid_reads_heights_and_morphs_to_parent_triangles) {
    checkPageSurfaceGpu(false);
}

TEST(height_atlas_gpu_adaptive_reads_actual_parent_and_stitched_bed_and_water) {
    checkPageSurfaceGpu(true);
}

TEST(height_atlas_gpu_generation_stages_morph_both_shared_edge_endpoints) {
    checkPageSurfaceGpu(true,false,true);
}

TEST(height_atlas_gpu_publication_morphs_geometry_water_and_stage_on_every_frame) {
    checkPageSurfaceGpu(true,false,false,true);
    checkPageSurfaceGpu(true,false,true,true);
    checkPageSurfaceGpu(true,true,false,true);
}

TEST(height_atlas_gpu_water_fragment_preserves_explicit_channel_when_coarse_page_is_dry) {
    checkPageSurfaceGpu(true,true);
}

TEST(terrain_material_border_uses_continuous_value_noise_at_world_coordinates) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    constexpr Uint32 width = 128;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = width; info.height = info.layer_count_or_depth = info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto target = gpu.device.makeTexture(info);
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_edge_probe.hlsl";
    wanted.vertexEntry = "TerrainEdgeProbeVS";
    wanted.fragmentEntry = "TerrainEdgeProbePS";
    wanted.depthTest = wanted.depthWrite = false;
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = width * 4;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(), SDL_CreateGPUTransferBuffer(gpu.device.handle(), &transferInfo));
    CHECK(bool(target) && bool(pipeline) && bool(transfer));
    if (!target || !pipeline || !transfer) return;

    for (const auto origin : {std::array<float, 2>{0, 0}, {10000, 30000},
                              {200000, 300000}, {-100000, -200000}}) {
        auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        CHECK(commands != nullptr);
        if (!commands) return;
        engine::Scene scene{};
        scene.camera[0] = origin[0]; scene.camera[1] = origin[1];
        SDL_PushGPUFragmentUniformData(commands, 0, &scene, sizeof(scene));
        SDL_GPUColorTargetInfo colour{};
        colour.texture = target.get();
        colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
        auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        SDL_BindGPUGraphicsPipeline(render, pipeline.get());
        SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render);
        auto* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureRegion region{target.get(), 0, 0, 0, 0, 0, width, 1, 1};
        const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, width, 1};
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
        SDL_EndGPUCopyPass(copy);
        CHECK(gpu.device.submitFrame(commands));
        CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
        const auto* pixels = static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
        CHECK(pixels != nullptr);
        if (!pixels) return;
        int boundaryJump = 0, low = 255, high = 0;
        bool matchesValueNoise = true;
        for (Uint32 x = 0; x < width; ++x) {
            matchesValueNoise = matchesValueNoise && pixels[x * 4] == pixels[x * 4 + 1];
            boundaryJump = std::max(boundaryJump, std::abs(int(pixels[x * 4 + 2]) - int(pixels[x * 4 + 3])));
            low = std::min(low, int(pixels[x * 4]));
            high = std::max(high, int(pixels[x * 4]));
        }
        SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
        CHECK(matchesValueNoise);
        CHECK(boundaryJump <= 2); // no lattice seam, including negative/far coordinates
        CHECK(high - low > 32); // real variation, not a constant mask
    }
}

TEST(terrain_material_all_pairs_lerp_multiscale_noise_without_slope_or_height_streaks) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 64;
    info.layer_count_or_depth = 6; info.num_levels = 7;
    auto map = gpu.device.makeTexture(info);
    CHECK(bool(map));
    if (!map) return;
    engine::Device::Uploader upload(gpu.device);
    for (int layer = 0; layer < 6; ++layer) {
        for (int mip = 0, side = 64; mip < 7; ++mip, side /= 2) {
            // Deliberate stripes and different mip heights must not leak into
            // the transition, even through a steep triplanar projection.
            std::vector<std::array<Uint8, 4>> pixels(side * side);
            for (int y = 0; y < side; ++y) for (int x = 0; x < side; ++x)
                pixels[y * side + x] = {128, 128, Uint8((x + layer + mip) % 2 ? 255 : 0), 255};
            CHECK(upload.refillRegion(map.get(), pixels.data(), 0, 0, side, side, 4, layer, mip));
        }
    }
    CHECK(upload.finish());
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.max_lod = 6;
    auto sampler = gpu.device.makeSampler(samplerInfo);
    const std::array<SDL_GPUTextureSamplerBinding, 4> bindings{{
        {map.get(), sampler.get()}, {map.get(), sampler.get()},
        {map.get(), sampler.get()}, {map.get(), sampler.get()}}};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.layer_count_or_depth = info.num_levels = 1;
    auto target = gpu.device.makeTexture(info);
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_material_probe.hlsl";
    wanted.vertexEntry = "TerrainMaterialProbeVS"; wanted.fragmentEntry = "TerrainMaterialProbePS";
    wanted.depthTest = wanted.depthWrite = false;
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    CHECK(bool(target) && bool(pipeline) && bool(sampler));
    if (!target || !pipeline || !sampler) return;
    const auto draw = [&](int a, int b, float slope, float footprint, float rotation, float bias) {
        std::array<Uint8, 64 * 64 * 4> pixels{};
        auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        CHECK(commands != nullptr);
        if (!commands) return pixels;
        engine::Scene scene{};
        scene.camera[0] = 47.3f; scene.camera[1] = -28.7f;
        scene.extra[0] = 21; scene.extra[1] = footprint; scene.extra[2] = rotation;
        scene.parameters[20][0] = float(a); scene.parameters[20][1] = float(b);
        scene.parameters[20][2] = slope; scene.parameters[20][3] = bias;
        for (int i = 0; i < 6; ++i) {
            scene.table[i][0] = 7.0f; scene.table[i][1] = 0.2f + float(i) * 0.01f;
            scene.table[i][2] = 0.15f; scene.table[i][3] = 3.0f + float(i);
        }
        SDL_PushGPUFragmentUniformData(commands, 0, &scene, sizeof(scene));
        const std::array<float, 16> own{};
        SDL_PushGPUFragmentUniformData(commands, 1, own.data(), sizeof(own));
        SDL_GPUColorTargetInfo colour{};
        colour.texture = target.get();
        colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
        auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
        CHECK(render != nullptr);
        if (!render) { SDL_CancelGPUCommandBuffer(commands); return pixels; }
        SDL_BindGPUGraphicsPipeline(render, pipeline.get());
        SDL_BindGPUFragmentSamplers(render, 0, bindings.data(), Uint32(bindings.size()));
        SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_EndGPURenderPass(render);
        CHECK(gpu.device.submitFrame(commands));
        CHECK(gpu.device.readTexture(target.get(), pixels.data(), 64, 64));
        return pixels;
    };
    for (int a = 0; a < 6; ++a) for (int b = a + 1; b < 6; ++b) {
        for (const float footprint : {0.08f, 1.5f, 8.0f}) for (const float angle : {0.0f, 0.7f}) {
            const auto flat = draw(a, b, 0.0f, footprint, angle, 0.0f);
            const auto slope = draw(a, b, 3.0f, footprint, angle, 0.0f);
            int error = 0, slopeChange = 0, low = 255, high = 0, blended = 0;
            for (std::size_t p = 0; p < flat.size(); p += 4) {
                error = std::max(error, std::abs(int(flat[p]) - int(flat[p + 1])));
                error = std::max(error, std::abs(int(slope[p]) - int(slope[p + 1])));
                slopeChange = std::max(slopeChange, std::abs(int(flat[p]) - int(slope[p])));
                low = std::min(low, int(flat[p])); high = std::max(high, int(flat[p]));
                blended += flat[p] > 16 && flat[p] < 239;
            }
            if (error > 1 || slopeChange > 1) {
                std::cerr << "Organic pair=" << a << ',' << b << " pixel=" << footprint
                          << " error=" << error << " slope change=" << slopeChange << " row32(actual/expected):";
                for (int x = 0; x < 64; x += 6) std::cerr << ' ' << int(flat[(32 * 64 + x) * 4]) << '/' << int(flat[(32 * 64 + x) * 4 + 1]);
                std::cerr << '\n';
            }
            CHECK(error <= 1);
            CHECK(slopeChange <= 1);
            CHECK(blended > 64); // a soft transition band, not a binary noisy contour
            // No intrusion into pure interiors - where the 64 px window is
            // wider than the metre-scale band (at 0.08 m/px it spans 5 m).
            if (footprint >= 1.0f) { CHECK_EQ(low, 0); CHECK_EQ(high, 255); }
        }
        const auto before = draw(a, b, 3.0f, 0.08f, 0.7f, -0.00001f);
        const auto after = draw(a, b, 3.0f, 0.08f, 0.7f, 0.00001f);
        int swapJump = 0;
        for (int y = 0; y < 64; ++y) {
            const int p = (y * 64 + 32) * 4; // top/under swaps at this column
            swapJump = std::max(swapJump, std::abs(int(before[p]) - int(after[p])));
        }
        CHECK(swapJump <= 1);
    }
}

TEST(terrain_depth_clamp_does_not_pull_buried_skirts_in_front) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    struct Vertex { float position[3], colour[3]; };
    const Vertex points[]{
        {{-1, -1, 0.7f}, {0, 1, 0}}, {{3, -1, 0.7f}, {0, 1, 0}}, {{-1, 3, 0.7f}, {0, 1, 0}},
        // At screen centre the wall is behind the green surface. Clamping
        // its far vertices to 1 BEFORE interpolation incorrectly brings it forward.
        {{-1, -1, 0.1f}, {1, 0, 0}}, {{3, -1, 3.0f}, {1, 0, 0}}, {{-1, 3, 3.0f}, {1, 0, 0}},
        // Ground wholly beyond the depth slab must still render, not be clipped.
        {{-1, -1, 2.0f}, {1, 0, 0}}, {{3, -1, 2.0f}, {1, 0, 0}}, {{-1, 3, 2.0f}, {1, 0, 0}}};
    auto vertices = gpu.device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, points, sizeof(points));
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_depth_probe.hlsl";
    wanted.vertexEntry = "TerrainDepthProbeVS";
    wanted.fragmentEntry = "TerrainDepthProbePS";
    wanted.buffers = {{0, sizeof(Vertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},
                         {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)}};
    wanted.depthTest = wanted.depthWrite = true;
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = info.height = 32; info.layer_count_or_depth = info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto target = gpu.device.makeTexture(info);
    info.format = engine::Device::kDepthFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    auto depthTarget = gpu.device.makeTexture(info);
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = 256;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(), SDL_CreateGPUTransferBuffer(gpu.device.handle(), &transferInfo));
    CHECK(bool(vertices) && bool(pipeline) && bool(target) && bool(depthTarget) && bool(transfer));
    if (!vertices || !pipeline || !target || !depthTarget || !transfer) return;
    for (const bool farOnly : {false, true}) {
        auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        CHECK(commands != nullptr);
        if (!commands) return;
        engine::Scene scene{};
        scene.viewProjection[0] = scene.viewProjection[5] = scene.viewProjection[10] = scene.viewProjection[15] = 1;
        SDL_PushGPUVertexUniformData(commands, 0, &scene, sizeof(scene));
        SDL_GPUColorTargetInfo colour{};
        colour.texture = target.get();
        colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = depthTarget.get(); depth.clear_depth = 1;
        depth.load_op = SDL_GPU_LOADOP_CLEAR; depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
        CHECK(render != nullptr);
        if (!render) { SDL_CancelGPUCommandBuffer(commands); return; }
        SDL_BindGPUGraphicsPipeline(render, pipeline.get());
        const SDL_GPUBufferBinding buffer{vertices.get(), 0};
        SDL_BindGPUVertexBuffers(render, 0, &buffer, 1);
        if (!farOnly) SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
        SDL_DrawGPUPrimitives(render, 3, 1, farOnly ? 6 : 3, 0);
        SDL_EndGPURenderPass(render);
        auto* copy = SDL_BeginGPUCopyPass(commands);
        const SDL_GPUTextureRegion region{target.get(), 0, 0, 16, 16, 0, 1, 1, 1};
        const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, 64, 1};
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
        SDL_EndGPUCopyPass(copy);
        CHECK(gpu.device.submitFrame(commands));
        CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
        const auto* pixel = static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
        CHECK(pixel != nullptr);
        if (!pixel) return;
        CHECK_EQ(pixel[0], farOnly ? 255 : 0);
        CHECK_EQ(pixel[1], farOnly ? 0 : 255);
        SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
    }
}

TEST(terrain_materials_preserve_identity_channels_and_normals_across_zoom) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = info.height = 64;
    info.layer_count_or_depth = 6; info.num_levels = 7;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    std::array<engine::Texture, 3> maps;
    engine::Device::Uploader upload(gpu.device);
    for (int map = 0; map < 3; ++map) {
        maps[map] = gpu.device.makeTexture(info);
        CHECK(bool(maps[map]));
        if (!maps[map]) return;
        for (int layer = 0; layer < 6; ++layer) {
            for (int mip = 0, side = 64; mip < 7; ++mip, side /= 2) {
                std::array<Uint8, 4> value{48, 112, 36, 255};
                if (map == 0 && layer == 3) value = {140, 128, 120, 255};
                if (map == 1) value = {128, 128, 255, 255};
                // Opposite displacement at each end of the mip chain. Implicit
                // height LOD used to replace even pure grass with rock up close.
                if (map == 2) value = {64, 220,
                    static_cast<Uint8>(layer == 3 ? 255 - mip * 32 : mip * 32), 255};
                std::vector<std::array<Uint8, 4>> pixels(side * side, value);
                CHECK(upload.refillRegion(maps[map].get(), pixels.data(), 0, 0,
                    side, side, 4, layer, mip));
            }
        }
    }
    CHECK(upload.finish());
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u = samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.max_lod = 6;
    auto sampler = gpu.device.makeSampler(samplerInfo);
    std::array<SDL_GPUTextureSamplerBinding, 3> bindings;
    for (int i = 0; i < 3; ++i) bindings[i] = {maps[i].get(), sampler.get()};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.layer_count_or_depth = info.num_levels = 1;
    auto target = gpu.device.makeTexture(info);
    engine::PipelineWanted wanted;
    wanted.shaderFile = "terrain_material_probe.hlsl";
    wanted.vertexEntry = "TerrainMaterialProbeVS";
    wanted.fragmentEntry = "TerrainMaterialProbePS";
    wanted.depthTest = wanted.depthWrite = false;
    auto pipeline = gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr << gpu.device.error() << '\n';
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    transferInfo.size = 64 * 64 * 4;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(), SDL_CreateGPUTransferBuffer(gpu.device.handle(), &transferInfo));
    CHECK(bool(target) && bool(pipeline) && bool(transfer) && bool(sampler));
    if (!target || !pipeline || !transfer || !sampler) return;
    for (int mode = 0; mode <= 17; ++mode) {
        if (mode == 9 || mode == 10) continue; // real material arrays are tested below
        std::array<int, 4> reference{};
        bool first = true;
        for (const float footprint : {0.03f, 0.08f, 0.25f, 0.75f, 1.5f, 3.2f}) {
            for (const float rotation : {0.0f, 0.7f, 1.8f}) {
                auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
                CHECK(commands != nullptr);
                if (!commands) return;
                engine::Scene scene{};
                scene.camera[0] = 47.3f; scene.camera[1] = -28.7f;
                scene.camera[2] = 100; scene.camera[3] = 2.83f / footprint;
                scene.viewport[0] = scene.viewport[1] = 64;
                scene.viewProjection[10] = 1;
                scene.extra[0] = static_cast<float>(mode);
                scene.extra[1] = footprint; scene.extra[2] = rotation;
                for (auto& profile : scene.table) {
                    profile[0] = 7; profile[1] = 0.2f; profile[2] = 0.15f; profile[3] = 3;
                }
                SDL_PushGPUFragmentUniformData(commands, 0, &scene, sizeof(scene));
                const std::array<float, 16> draw{};
                SDL_PushGPUFragmentUniformData(commands, 1, draw.data(), sizeof(draw));
                SDL_GPUColorTargetInfo colour{};
                colour.texture = target.get();
                colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
                auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
                CHECK(render != nullptr);
                if (!render) { SDL_CancelGPUCommandBuffer(commands); return; }
                SDL_BindGPUGraphicsPipeline(render, pipeline.get());
                SDL_BindGPUFragmentSamplers(render, 0, bindings.data(), 3);
                SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
                SDL_EndGPURenderPass(render);
                auto* copy = SDL_BeginGPUCopyPass(commands);
                const SDL_GPUTextureRegion region{target.get(), 0, 0, 32, 32, 0, 1, 1, 1};
                const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, 64, 1};
                SDL_DownloadFromGPUTexture(copy, &region, &destination);
                SDL_EndGPUCopyPass(copy);
                CHECK(gpu.device.submitFrame(commands));
                CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
                const auto* data = static_cast<const Uint8*>(
                    SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
                CHECK(data != nullptr);
                if (!data) return;
                const std::array<int, 4> actual{data[0], data[1], data[2], data[3]};
                SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
                if (first) { reference = actual; first = false; }
                if (mode == 0 || mode == 2) {
                    for (int i = 0; i < 4; ++i) CHECK(actual[i] >= 254);
                } else if (mode == 1) {
                    CHECK(std::abs(actual[0] - actual[1]) <= 1);
                    CHECK_EQ(actual[2], 0);
                } else if (mode == 3) {
                    CHECK_EQ(actual[0] + actual[1] + actual[2], 0);
                    CHECK_EQ(actual[3], 255);
                } else if (mode == 4) {
                    CHECK_EQ(actual[0], 48); CHECK_EQ(actual[1], 112);
                    CHECK_EQ(actual[2], 36); CHECK_EQ(actual[3], 255);
                } else if (mode == 6) {
                    const float expected[3]{-0.1f, -0.2f, 1.0f};
                    for (int i = 0; i < 3; ++i)
                        CHECK(std::abs(actual[i] - int(std::lround(
                            (expected[i] / std::sqrt(1.05f) * 0.5f + 0.5f) * 255))) <= 1);
                } else if (mode == 8) {
                    CHECK_EQ(actual[0], 64); CHECK_EQ(actual[1], 220);
                }
                if (mode == 11) CHECK(actual[1]>actual[0]+15 && actual[0]>actual[2]);
                if (mode == 12) CHECK(actual[0]>actual[1] && actual[1]>actual[2]);
                if (mode >= 13) {
                    CHECK(std::abs(actual[0]-140)<=1);CHECK(std::abs(actual[1]-102)<=1);
                    CHECK(std::abs(actual[2]-64)<=1);CHECK_EQ(actual[3],255);
                }
                if (mode == 5 && footprint > 3.0f * 0.35f) {
                    // Unresolved Perlin must converge to the noise-free blend,
                    // not preserve a subpixel pattern (and alias on zoom-out).
                    const float width = 0.2f * 0.62f; // fixture's grass/rock profile
                    const float u = (0.04f + width) / (2.0f * width);
                    const float mix = u * u * (3.0f - 2.0f * u);
                    const std::array<int, 4> mean{
                        int(std::lround(140 + (48 - 140) * mix)),
                        int(std::lround(128 + (112 - 128) * mix)),
                        int(std::lround(120 + (36 - 120) * mix)), int(std::lround(255 * mix))};
                    for (int i = 0; i < 4; ++i) {
                        CHECK(actual[i] >= std::min(reference[i], mean[i]) - 1);
                        CHECK(actual[i] <= std::max(reference[i], mean[i]) + 1);
                        if (footprint >= 3.0f) CHECK(std::abs(actual[i] - mean[i]) <= 1);
                    }
                } else if (mode == 5 || mode == 7 || mode >= 11) {
                    for (int i = 0; i < 4; ++i) {
                        const int tolerance = mode == 5 ? 1 : 3; // lighting/haze, not repaint
                        if (std::abs(actual[i] - reference[i]) > tolerance)
                            std::cerr << "material mode=" << mode << " footprint=" << footprint
                                      << " channel=" << i << " got=" << actual[i]
                                      << " reference=" << reference[i] << '\n';
                        CHECK(std::abs(actual[i] - reference[i]) <= tolerance);
                    }
                }
            }
        }
    }
    // Real packed maps through the same loader as TerrainPass. Check aggregate
    // albedo (not individual texels, which MUST be filtered with distance).
    const std::array<std::string, 6> materials{
        "grass/lush/grass_lush", "soil/base/soil_base", "sand/dry/sand_dry",
        "stone/rock/rock_ground", "soil/mud_wet/mud_wet", "snow/clean/snow_clean"};
    const std::array<std::string, 3> channels{"albedo", "normal", "properties"};
    const float metres[]{2.4f, 2.0f, 1.6f, 2.6f, 2.0f, 3.0f};
    const auto terrain = gpu.device.assets().parent_path() / "terrain";
    for (int map = 0; map < 3; ++map) {
        std::vector<std::vector<std::filesystem::path>> layers;
        for (const auto& material : materials)
            layers.push_back({terrain / (material + "_" + channels[map] + ".png")});
        maps[map] = gpu.device.loadArrayMipped(layers, true);
        CHECK(bool(maps[map]));
        if (!maps[map]) return;
    }
    samplerInfo.max_lod = 16;
    // Match TerrainPass on oblique faces as well as on flat ground.
    samplerInfo.enable_anisotropy = true;
    samplerInfo.max_anisotropy = 8.0f;
    sampler = gpu.device.makeSampler(samplerInfo);
    CHECK(bool(sampler));
    if (!sampler) return;
    for (int i = 0; i < 3; ++i) bindings[i] = {maps[i].get(), sampler.get()};
    const char* previews = std::getenv("ASR_MATERIAL_PREVIEWS");
    if (previews) std::filesystem::create_directories(previews);
    for (const int mode : {9, 10}) {
        for (int layer = 0; layer < 6; ++layer) {
            for (const float slope : {0.0f, 3.0f}) {
                std::array<double, 3> reference{};
                bool first = true;
                for (const float footprint : {0.08f, 0.35f, 1.5f, 3.2f}) {
                    auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
                    CHECK(commands != nullptr);
                    if (!commands) return;
                    engine::Scene scene{};
                    scene.camera[0] = 47.3f; scene.camera[1] = -28.7f;
                    scene.camera[2] = 100; scene.camera[3] = 2.83f / footprint;
                    scene.viewport[0] = scene.viewport[1] = 64;
                    scene.viewProjection[10] = 1;
                    scene.extra[0] = static_cast<float>(mode); scene.extra[1] = footprint;
                    scene.parameters[20][0] = static_cast<float>(layer);
                    scene.parameters[20][1] = slope;
                    for (int i = 0; i < 6; ++i) {
                        scene.table[i][0] = 14.0f / metres[i];
                        scene.table[i][1] = 0.2f; scene.table[i][3] = 3;
                    }
                    const std::array<float, 16> draw{};
                    SDL_PushGPUFragmentUniformData(commands, 0, &scene, sizeof(scene));
                    SDL_PushGPUFragmentUniformData(commands, 1, draw.data(), sizeof(draw));
                    SDL_GPUColorTargetInfo colour{};
                    colour.texture = target.get();
                    colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
                    auto* render = SDL_BeginGPURenderPass(commands, &colour, 1, nullptr);
                    CHECK(render != nullptr);
                    if (!render) { SDL_CancelGPUCommandBuffer(commands); return; }
                    SDL_BindGPUGraphicsPipeline(render, pipeline.get());
                    SDL_BindGPUFragmentSamplers(render, 0, bindings.data(), 3);
                    SDL_DrawGPUPrimitives(render, 3, 1, 0, 0);
                    SDL_EndGPURenderPass(render);
                    auto* copy = SDL_BeginGPUCopyPass(commands);
                    const SDL_GPUTextureRegion region{target.get(), 0, 0, 0, 0, 0, 64, 64, 1};
                    const SDL_GPUTextureTransferInfo destination{transfer.get(), 0, 64, 64};
                    SDL_DownloadFromGPUTexture(copy, &region, &destination);
                    SDL_EndGPUCopyPass(copy);
                    CHECK(gpu.device.submitFrame(commands));
                    CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
                    auto* data = static_cast<Uint8*>(
                        SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
                    CHECK(data != nullptr);
                    if (!data) return;
                    std::array<double, 3> mean{};
                    double luminanceSum = 0.0, luminanceSquared = 0.0;
                    for (int p = 0; p < 64 * 64; ++p) {
                        for (int c = 0; c < 3; ++c) mean[c] += data[p * 4 + c] / 4096.0;
                        const double luminance = data[p * 4] * 0.2126 +
                            data[p * 4 + 1] * 0.7152 + data[p * 4 + 2] * 0.0722;
                        luminanceSum += luminance / 4096.0;
                        luminanceSquared += luminance * luminance / 4096.0;
                        CHECK_EQ(data[p * 4 + 3], 255);
                    }
                    const double contrast = std::sqrt(std::max(0.0,
                        luminanceSquared - luminanceSum * luminanceSum)) /
                        std::max(luminanceSum, 1.0);
                    if (previews)
                        std::cout << "material contrast mode=" << mode << " layer=" << layer
                                  << " slope=" << slope << " pixel=" << footprint
                                  << " cv=" << contrast << '\n';
                    // Preserve physical close-up detail, not a minimum amount
                    // of noise at every distance. The macro-band regression
                    // below checks that unresolved grain does not come back.
                    if (mode == 9 && layer != 5 && footprint <= 0.08f) {
                        if (contrast < 0.025)
                            std::cerr << materials[layer] << " lost texture at pixel=" << footprint
                                      << " slope=" << slope << " contrast=" << contrast << '\n';
                        CHECK(contrast >= 0.025);
                    }
                    if (previews) {
                        auto* image = SDL_CreateSurfaceFrom(64, 64, SDL_PIXELFORMAT_RGBA32, data, 64 * 4);
                        CHECK(image != nullptr);
                        if (image) {
                            const auto file = std::filesystem::path(previews) /
                                (std::to_string(mode) + "_" + std::to_string(layer) + "_slope" +
                                 std::to_string(int(slope)) + "_pixel" + std::to_string(footprint) + ".bmp");
                            CHECK(SDL_SaveBMP(image, file.string().c_str()));
                            SDL_DestroySurface(image);
                        }
                    }
                    SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
                    if (first) { reference = mean; first = false; }
                    for (int c = 0; c < 3; ++c) {
                        CHECK(mean[c] > 2.0 && mean[c] < 253.0);
                        if (mode == 9) {
                            if (std::abs(mean[c] - reference[c]) > 12.0)
                                std::cerr << materials[layer] << " albedo shift=" << mean[c] - reference[c]
                                          << " at pixel=" << footprint << " slope=" << slope << '\n';
                            CHECK(std::abs(mean[c] - reference[c]) <= 12.0);
                        }
                    }
                }
            }
        }
    }
}

TEST(terrain_material_macro_bands_do_not_reintroduce_unresolved_grain) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format=engine::Device::kColourFormat;
    info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width=info.height=64;
    info.layer_count_or_depth=6;info.num_levels=7;
    std::array<engine::Texture,3> maps;
    engine::Device::Uploader upload(gpu.device);
    for (int map=0;map<3;++map) {
        maps[map]=gpu.device.makeTexture(info);
        CHECK(bool(maps[map]));
        if (!maps[map]) return;
        for (int layer=0;layer<6;++layer) for (int mip=0,side=64;mip<7;++mip,side/=2) {
            std::vector<std::array<Uint8,4>> pixels(std::size_t(side*side));
            for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
                const bool light=((x+y)&1)!=0;
                auto& p=pixels[std::size_t(y)*std::size_t(side)+std::size_t(x)];
                const Uint8 value=mip?128:light?224:32;
                p=map==1?std::array<Uint8,4>{Uint8(mip?128:light?208:48),128,218,255}:
                    std::array<Uint8,4>{value,value,value,255};
            }
            CHECK(upload.refillRegion(maps[map].get(),pixels.data(),0,0,side,side,4,layer,mip));
        }
    }
    CHECK(upload.finish());
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter=samplerInfo.mag_filter=SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    samplerInfo.address_mode_u=samplerInfo.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.max_lod=6;
    auto sampler=gpu.device.makeSampler(samplerInfo);
    // Shadow sampling is disabled by Scene; bind a valid array in its slot too.
    const std::array<SDL_GPUTextureSamplerBinding,4> bindings{{
        {maps[0].get(),sampler.get()},{maps[1].get(),sampler.get()},
        {maps[2].get(),sampler.get()},{maps[0].get(),sampler.get()}}};
    info.type=SDL_GPU_TEXTURETYPE_2D;
    info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.layer_count_or_depth=info.num_levels=1;
    auto target=gpu.device.makeTexture(info);
    engine::PipelineWanted wanted;
    wanted.shaderFile="terrain_material_probe.hlsl";
    wanted.vertexEntry="TerrainMaterialProbeVS";wanted.fragmentEntry="TerrainMaterialProbePS";
    wanted.depthTest=wanted.depthWrite=false;
    auto pipeline=gpu.device.makePipeline(wanted);
    CHECK(bool(target) && bool(pipeline) && bool(sampler));
    if (!target || !pipeline || !sampler) return;
    const auto draw=[&](int mode,float footprint,float rotation) {
        std::array<Uint8,64*64*4> pixels{};
        auto* commands=SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        CHECK(commands!=nullptr);
        if (!commands) return pixels;
        engine::Scene scene{};
        scene.camera[0]=47.3f;scene.camera[1]=-28.7f;
        scene.extra[0]=float(mode);scene.extra[1]=footprint;scene.extra[2]=rotation;
        scene.viewport[0]=scene.viewport[1]=64;
        SDL_PushGPUFragmentUniformData(commands,0,&scene,sizeof(scene));
        const std::array<float,16> own{};
        SDL_PushGPUFragmentUniformData(commands,1,own.data(),sizeof(own));
        SDL_GPUColorTargetInfo colour{};
        colour.texture=target.get();colour.load_op=SDL_GPU_LOADOP_CLEAR;colour.store_op=SDL_GPU_STOREOP_STORE;
        auto* render=SDL_BeginGPURenderPass(commands,&colour,1,nullptr);
        CHECK(render!=nullptr);
        if (!render) { SDL_CancelGPUCommandBuffer(commands);return pixels; }
        SDL_BindGPUGraphicsPipeline(render,pipeline.get());
        SDL_BindGPUFragmentSamplers(render,0,bindings.data(),Uint32(bindings.size()));
        SDL_DrawGPUPrimitives(render,3,1,0,0);
        SDL_EndGPURenderPass(render);
        CHECK(gpu.device.submitFrame(commands));
        CHECK(gpu.device.readTexture(target.get(),pixels.data(),64,64));
        return pixels;
    };
    for (int mode:{18,19,20}) for (float footprint:{0.125f,0.25f,0.5f}) for (float angle:{0.0f,0.7f}) {
        const auto pixels=draw(mode,footprint,angle);
        int low=255,high=0;
        for (std::size_t p=0;p<pixels.size();p+=4) {
            low=std::min(low,int(pixels[p]));high=std::max(high,int(pixels[p]));
        }
        if (high-low>3) std::cerr<<"macro grain mode="<<mode<<" footprint="<<footprint<<" range="<<high-low<<'\n';
        CHECK(high-low<=3);
    }
    const auto near=draw(18,0.001f,0.0f);
    int low=255,high=0;
    for (std::size_t p=0;p<near.size();p+=4) {
        low=std::min(low,int(near[p]));high=std::max(high,int(near[p]));
    }
    CHECK(high-low>20); // preserve actual close-up texture, don't replace it by a flat fill
}

TEST(terrain_material_array_generates_mip_tail_down_to_one_texel) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    struct Temporary {
        std::filesystem::path path;
        ~Temporary() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } temporary{std::filesystem::temp_directory_path() /
                ("asr-material-mips-" + std::to_string(SDL_GetTicksNS()))};
    std::filesystem::create_directories(temporary.path);
    const auto path = temporary.path / "checker.bmp";
    auto* surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA32);
    CHECK(surface != nullptr);
    if (!surface) return;
    for (int y = 0; y < 64; ++y) {
        auto* row = static_cast<Uint8*>(surface->pixels) + y * surface->pitch;
        for (int x = 0; x < 64; ++x) {
            row[x * 4] = row[x * 4 + 1] = row[x * 4 + 2] = (x + y) % 2 ? 255 : 0;
            row[x * 4 + 3] = 255;
        }
    }
    CHECK(SDL_SaveBMP(surface, path.string().c_str()));
    SDL_DestroySurface(surface);
    auto texture = gpu.device.loadArrayMipped({{path}, {path}}, true);
    CHECK(bool(texture));
    if (!texture) return;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; info.size = 512;
    engine::Owned<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(), SDL_CreateGPUTransferBuffer(gpu.device.handle(), &info));
    CHECK(bool(transfer));
    if (!transfer) return;
    auto* commands = SDL_AcquireGPUCommandBuffer(gpu.device.handle());
    CHECK(commands != nullptr);
    if (!commands) return;
    auto* copy = SDL_BeginGPUCopyPass(commands);
    for (Uint32 layer = 0; layer < 2; ++layer) {
        const SDL_GPUTextureRegion region{texture.get(), 6, layer, 0, 0, 0, 1, 1, 1};
        const SDL_GPUTextureTransferInfo destination{transfer.get(), layer * 256, 64, 1};
        SDL_DownloadFromGPUTexture(copy, &region, &destination);
    }
    SDL_EndGPUCopyPass(copy);
    CHECK(gpu.device.submitFrame(commands));
    CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
    const auto* pixels = static_cast<const Uint8*>(
        SDL_MapGPUTransferBuffer(gpu.device.handle(), transfer.get(), false));
    CHECK(pixels != nullptr);
    if (!pixels) return;
    for (int layer = 0; layer < 2; ++layer) {
        for (int c = 0; c < 3; ++c) CHECK(std::abs(int(pixels[layer * 256 + c]) - 128) <= 1);
        CHECK_EQ(pixels[layer * 256 + 3], 255);
    }
    SDL_UnmapGPUTransferBuffer(gpu.device.handle(), transfer.get());
}

TEST(terrain_erosion_gpu_survives_distance_but_filters_unresolved_bands) {
    Gpu gpu;
    CHECK(gpu.ready);
    if (!gpu.ready) return;
    constexpr Uint32 width=256;
    SDL_GPUTextureCreateInfo info{};
    info.type=SDL_GPU_TEXTURETYPE_2D; info.format=engine::Device::kColourFormat;
    info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET; info.width=width;
    info.height=info.layer_count_or_depth=info.num_levels=1;
    info.sample_count=SDL_GPU_SAMPLECOUNT_1;
    auto target=gpu.device.makeTexture(info);
    engine::PipelineWanted wanted;
    wanted.shaderFile="terrain_erosion_probe.hlsl";
    wanted.vertexEntry="TerrainErosionProbeVS"; wanted.fragmentEntry="TerrainErosionProbePS";
    wanted.depthTest=wanted.depthWrite=false;
    auto pipeline=gpu.device.makePipeline(wanted);
    if (!pipeline) std::cerr<<gpu.device.error()<<'\n';
    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; transferInfo.size=width*4;
    engine::Owned<SDL_GPUTransferBuffer,SDL_ReleaseGPUTransferBuffer> transfer(
        gpu.device.handle(),SDL_CreateGPUTransferBuffer(gpu.device.handle(),&transferInfo));
    CHECK(bool(target) && bool(pipeline) && bool(transfer));
    if (!target || !pipeline || !transfer) return;
    for (const float origin : {0.0f,70000.0f,-200000.0f})
    for (int mask=0;mask<3;++mask) {
        std::array<Uint8,width*4> previous;
        previous.fill(255);
        for (const float footprint : {0.25f,2.0f,8.0f,40.0f}) {
            auto* commands=SDL_AcquireGPUCommandBuffer(gpu.device.handle());
            CHECK(commands!=nullptr);
            if (!commands) return;
            engine::Scene scene{};
            scene.camera[0]=origin;scene.camera[1]=origin*0.3f;
            scene.camera[2]=mask==1?0.0f:30.0f;scene.camera[3]=footprint;
            scene.extra[0]=mask==0?0.0f:0.9f;
            SDL_PushGPUFragmentUniformData(commands,0,&scene,sizeof(scene));
            SDL_GPUColorTargetInfo colour{};
            colour.texture=target.get();colour.load_op=SDL_GPU_LOADOP_CLEAR;colour.store_op=SDL_GPU_STOREOP_STORE;
            auto* render=SDL_BeginGPURenderPass(commands,&colour,1,nullptr);
            SDL_BindGPUGraphicsPipeline(render,pipeline.get());
            SDL_DrawGPUPrimitives(render,3,1,0,0);
            SDL_EndGPURenderPass(render);
            auto* copy=SDL_BeginGPUCopyPass(commands);
            const SDL_GPUTextureRegion region{target.get(),0,0,0,0,0,width,1,1};
            const SDL_GPUTextureTransferInfo destination{transfer.get(),0,width,1};
            SDL_DownloadFromGPUTexture(copy,&region,&destination);
            SDL_EndGPUCopyPass(copy);
            CHECK(gpu.device.submitFrame(commands));
            CHECK(SDL_WaitForGPUIdle(gpu.device.handle()));
            const auto* pixels=static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(gpu.device.handle(),transfer.get(),false));
            CHECK(pixels!=nullptr);
            if (!pixels) return;
            int depth=0,cut=0;
            for (std::size_t i=0;i<width;++i) {
                depth+=pixels[i*4];cut+=pixels[i*4+1];
                CHECK(pixels[i*4]<=previous[i*4]);
                CHECK(pixels[i*4+1]<=previous[i*4+1]);
                CHECK_EQ(pixels[i*4+3],255);
            }
            if (mask<2 || footprint==40) { CHECK_EQ(depth,0);CHECK_EQ(cut,0); }
            else { CHECK(depth>100);CHECK(cut>100); }
            std::copy_n(pixels,previous.size(),previous.begin());
            SDL_UnmapGPUTransferBuffer(gpu.device.handle(),transfer.get());
        }
    }
}

