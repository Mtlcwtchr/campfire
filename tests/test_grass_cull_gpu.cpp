#include "framework.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/grass_cull.hpp"

namespace {
struct Gpu {
    SDL_Window* window = nullptr;
    engine::Device device;
    bool ready = false;
    Gpu() {
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        window = SDL_CreateWindow("grass cull tests",64,64,SDL_WINDOW_HIDDEN);
        ready = window && device.open(window,std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites");
        if (!ready) std::cerr << device.error() << ' ' << SDL_GetError() << '\n';
    }
    ~Gpu() { device.close();if (window) SDL_DestroyWindow(window);SDL_Quit(); }
};
world::PageGrassRoot root(float x, float tag=0) {
    world::PageGrassRoot value{};
    value.position[0]=x;
    value.upright=tag;
    value.run=64; // cell = 2 m
    return value;
}
struct Probe : engine::DrawPass {
    game::GrassCuller culler;
    engine::RenderPipeline* pipeline=nullptr;
    engine::Buffer vertices,indices;
    engine::PipelineSlot graphics=0;
    std::vector<world::PageGrassRoot> roots;
    bool dispatched=false;
    engine::PassPlace setup(engine::Device& device,engine::RenderPipeline& into) override {
        pipeline=&into;
        if (!culler.setup(device,into)) return {};
        const float quad[]{-1,-1,1,-1,1,1,-1,1};
        const std::uint16_t ix[]{0,1,2,0,2,3};
        vertices=device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX,quad,sizeof(quad));
        indices=device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX,ix,sizeof(ix));
        engine::PipelineWanted wanted;
        wanted.shaderFile="grass_cull_probe.hlsl";wanted.vertexEntry="ProbeVS";wanted.fragmentEntry="ProbePS";
        wanted.depthTest=wanted.depthWrite=false;
        wanted.buffers={{0,8,SDL_GPU_VERTEXINPUTRATE_VERTEX,0},
                        {1,sizeof(world::PageGrassRoot),SDL_GPU_VERTEXINPUTRATE_INSTANCE,0}};
        wanted.attributes={{0,0,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2,0},
                           {1,1,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,0}};
        auto gpu=device.makePipeline(wanted);
        if (!gpu || !vertices || !indices) return {};
        graphics=into.take(std::move(gpu));
        return {1,0,0};
    }
    bool anything(const engine::Frame&) const override { return true; }
    void collect(const engine::Frame& frame,engine::DrawQueue& queue) override {
        const std::array<float,7> prefix{9999,9999,9999,9999,9999,9999,9999};
        frame.instances->add(std::span(prefix));
        const auto at=roots.empty()?std::uint32_t(0):frame.instances->add(std::span(roots));
        if (!roots.empty()) CHECK_EQ(at,32u); // typed add pads the 28-byte prefix
        dispatched=culler.dispatch(frame,*pipeline,at,roots.size());
        if (!dispatched || roots.empty()) return;
        engine::DrawItem item;
        item.pipeline=graphics;item.vertexStreams=2;
        item.vertex[0]=vertices.get();item.vertex[1]=culler.instances();
        item.index=indices.get();item.indexSize=SDL_GPU_INDEXELEMENTSIZE_16BIT;
        item.indirect=culler.arguments();item.indirectDraws=1;
        queue.push(item);
    }
};
struct Harness {
    Gpu gpu;
    engine::RenderPipeline pipeline{0};
    engine::InstanceArena arena;
    engine::Texture target;
    Probe* pass=nullptr;
    bool start() {
        if (!gpu.ready) return false;
        engine::StageInfo stage;stage.useDepth=false;stage.clearColour=true;
        pipeline.stages({stage});
        pass=pipeline.add(std::make_unique<Probe>());
        SDL_GPUTextureCreateInfo info{};
        info.type=SDL_GPU_TEXTURETYPE_2D;info.format=engine::Device::kColourFormat;
        info.width=info.height=64;info.layer_count_or_depth=info.num_levels=1;
        info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        target=gpu.device.makeTexture(info);
        return target && pipeline.build(gpu.device);
    }
    bool run(float windStrength=0) {
        arena.begin();
        engine::Frame frame{};frame.device=&gpu.device;frame.instances=&arena;
        frame.width=frame.height=64;frame.colour=target.get();
        frame.scene.viewProjection[0]=frame.scene.viewProjection[5]=0.01f;
        frame.scene.viewProjection[15]=1;frame.scene.camera[3]=1;
        frame.scene.wind[0]=1;frame.scene.wind[2]=windStrength;
        frame.commands=SDL_AcquireGPUCommandBuffer(gpu.device.handle());
        if (!frame.commands) return false;
        const bool ok=pipeline.run(frame);
        return gpu.device.submitFrame(frame.commands) && ok && pass->dispatched;
    }
    std::uint32_t count() {
        std::uint32_t value=0;
        CHECK(pass->culler.readCount(gpu.device,value));
        return value;
    }
};
}

TEST(grass_cull_gpu_compacts_and_really_draws_indirect) {
    Harness h;const bool ready=h.start();CHECK(ready);if (!ready) return;
    h.pass->roots={root(-50),root(50),root(1000)};
    CHECK(h.run());CHECK_EQ(h.count(),2u);
    CHECK_EQ(h.pipeline.dispatchesLastFrame(),7u);
    std::array<world::PageGrassRoot,2> kept{};
    CHECK(h.gpu.device.readBuffer(h.pass->culler.instances(),kept.data(),sizeof(kept)));
    CHECK_EQ(kept[0].position[0],-50.0f);CHECK_EQ(kept[1].position[0],50.0f);
    SDL_GPUTransferBufferCreateInfo info{};info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;info.size=64*64*4;
    engine::Owned<SDL_GPUTransferBuffer,SDL_ReleaseGPUTransferBuffer> transfer(
        h.gpu.device.handle(),SDL_CreateGPUTransferBuffer(h.gpu.device.handle(),&info));
    CHECK(bool(transfer));if (!transfer) return;
    auto* commands=SDL_AcquireGPUCommandBuffer(h.gpu.device.handle());
    auto* copy=SDL_BeginGPUCopyPass(commands);
    const SDL_GPUTextureRegion region{h.target.get(),0,0,0,0,0,64,64,1};
    const SDL_GPUTextureTransferInfo destination{transfer.get(),0,64,64};
    SDL_DownloadFromGPUTexture(copy,&region,&destination);SDL_EndGPUCopyPass(copy);
    CHECK(h.gpu.device.submitFrame(commands));CHECK(SDL_WaitForGPUIdle(h.gpu.device.handle()));
    const auto* pixels=static_cast<const Uint8*>(SDL_MapGPUTransferBuffer(h.gpu.device.handle(),transfer.get(),false));
    CHECK(pixels!=nullptr);if (!pixels) return;
    CHECK(pixels[(32*64+16)*4+1]>200);CHECK(pixels[(32*64+48)*4+1]>200);
    CHECK_EQ(pixels[(32*64+32)*4+1],0);
    SDL_UnmapGPUTransferBuffer(h.gpu.device.handle(),transfer.get());
}

TEST(grass_cull_gpu_preserves_full_budget_bytes_growth_and_empty_reset) {
    Harness h;const bool ready=h.start();CHECK(ready);if (!ready) return;
    h.pass->roots={root(0)};CHECK(h.run());CHECK_EQ(h.count(),1u);
    const auto oldCapacity=h.arena.capacity();
    h.pass->roots.clear();h.pass->roots.reserve(game::GrassCuller::kCapacity);
    for (std::size_t i=0;i<game::GrassCuller::kCapacity;++i)
        h.pass->roots.push_back(root(i%2?1000.0f:0.0f,float(i)));
    CHECK(h.run());CHECK(h.arena.capacity()>oldCapacity);
    CHECK_EQ(h.count(),std::uint32_t(game::GrassCuller::kCapacity/2));
    std::vector<world::PageGrassRoot> kept(h.count());
    CHECK(h.gpu.device.readBuffer(h.pass->culler.instances(),kept.data(),kept.size()*sizeof(kept[0])));
    for (std::size_t i=0;i<kept.size();++i)
        CHECK_EQ(std::memcmp(&kept[i],&h.pass->roots[i*2],sizeof(kept[i])),0);
    for (auto& value:h.pass->roots) value.position[0]=0;
    CHECK(h.run());CHECK_EQ(h.count(),std::uint32_t(game::GrassCuller::kCapacity));
    h.pass->roots.clear();CHECK(h.run());CHECK_EQ(h.count(),0u);
    CHECK_EQ(h.pipeline.dispatchesLastFrame(),1u); // reset even with no DrawItems
    h.pass->roots={root(0),root(1000)};CHECK(h.run());CHECK_EQ(h.count(),1u);
}

TEST(grass_cull_gpu_retains_morph_envelope_and_rejects_outside) {
    Harness h;const bool ready=h.start();CHECK(ready);if (!ready) return;
    auto crossing=root(105); // root offscreen, but its tall morph envelope intersects a side plane
    crossing.parentHeight=80;crossing.priorHeight=-80;crossing.priorParentHeight=40;
    auto outside=root(1000);
    h.pass->roots={crossing,outside};CHECK(h.run());CHECK_EQ(h.count(),1u);
    world::PageGrassRoot kept{};
    CHECK(h.gpu.device.readBuffer(h.pass->culler.instances(),&kept,sizeof(kept)));
    CHECK_EQ(std::memcmp(&kept,&crossing,sizeof(kept)),0);
}

TEST(grass_cull_gpu_bounds_include_wind_and_coarse_card_width) {
    Harness h;const bool ready=h.start();CHECK(ready);if (!ready) return;
    h.pass->roots={root(110)};
    CHECK(h.run());CHECK_EQ(h.count(),0u);
    CHECK(h.run(20));CHECK_EQ(h.count(),1u);
    h.pass->roots[0].run=float(6<<6); // 64 m grid: wide clump, not a tall blade
    CHECK(h.run());CHECK_EQ(h.count(),1u);
}

TEST(grass_cull_gpu_rejects_invalid_input_before_dispatch) {
    Harness h;const bool ready=h.start();CHECK(ready);if (!ready) return;
    engine::Frame frame{};frame.device=&h.gpu.device;frame.instances=&h.arena;
    const std::array<world::PageGrassRoot,1> roots{root(0)};
    h.arena.add(std::span(roots));
    CHECK(!h.pass->culler.dispatch(frame,h.pipeline,1,1));
    CHECK(!h.pass->culler.dispatch(frame,h.pipeline,0,2));
    CHECK(!h.pass->culler.dispatch(frame,h.pipeline,0,game::GrassCuller::kCapacity+1));
}

