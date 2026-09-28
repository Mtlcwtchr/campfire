#include "framework.hpp"
#include "engine/render/impostor_gpu_cache.hpp"
#include <SDL3_image/SDL_image.h>
#include <cstring>
#include <chrono>
#include <thread>

TEST(impostor_data_png_preserves_packed_bytes_under_partial_alpha) {
    struct Video {Video(){CHECK(SDL_Init(SDL_INIT_VIDEO));}~Video(){SDL_Quit();}} video;
    struct Temporary {
        std::filesystem::path path=std::filesystem::temp_directory_path()/("campfire-data-png-"+std::to_string(SDL_GetTicksNS())+".png");
        ~Temporary(){std::error_code error;std::filesystem::remove(path,error);}
    } file;
    const std::array<engine::render::ImpostorPixel,4> bytes{{{159,255,1,123},{128,7,20,0},{100,55,7,67},{1,2,3,255}}};
    auto* image=SDL_CreateSurfaceFrom(2,2,SDL_PIXELFORMAT_RGBA32,const_cast<void*>(static_cast<const void*>(bytes.data())),8);
    CHECK(image);if (!image) return;
    CHECK(IMG_SavePNG(image,file.path.string().c_str()));SDL_DestroySurface(image);
    image=engine::Device::loadDataPng(file.path);CHECK(image);if (!image) return;
    CHECK(std::memcmp(image->pixels,bytes.data(),sizeof(bytes))==0);SDL_DestroySurface(image);
    engine::Device device;CHECK(device.openHeadless(2,2,std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites"));
    if (!device.handle()) return;
    auto texture=device.loadDataArray({file.path});CHECK(bool(texture));if (!texture) return;
    std::array<std::uint8_t,16> read{};CHECK(device.readTexture(texture.get(),read.data(),2,2));
    CHECK(std::memcmp(read.data(),bytes.data(),sizeof(bytes))==0);
}

TEST(impostor_gpu_cache_uploads_bounded_views_before_publication) {
    struct Video {Video(){CHECK(SDL_Init(SDL_INIT_VIDEO));}~Video(){SDL_Quit();}} video;
    engine::Device device;
    CHECK(device.openHeadless(16,16,std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites"));
    if (!device.handle()) return;
    engine::render::ImpostorGpuCache gpu;CHECK(gpu.setup(device));
    auto atlas=std::make_shared<engine::render::ImpostorAtlas>();
    atlas->resolution=8;atlas->side=2;atlas->members=1;
    atlas->colour.resize(21*64,{60,180,90,255});atlas->normal.resize(21*64,{128,128,255,255});
    atlas->depth.resize(21*64);
    for (unsigned view=0;view<21;++view) for (unsigned i=0;i<64;++i) atlas->depth[view*64+i]={128,0,std::uint8_t(view),255};
    const std::vector<engine::render::ImpostorPlacement> children{{atlas,{0,0,0}}};
    gpu.cache.frame(0,0);CHECK_EQ(gpu.cache.request({0,0},1,children),0);gpu.cache.poll();
    for (int i=0;i<5000 && gpu.cache.busy();++i) {std::this_thread::sleep_for(std::chrono::milliseconds(1));gpu.cache.poll();}
    CHECK(!gpu.cache.busy());CHECK_EQ(gpu.cache.uploadCandidate(),0);
    for (unsigned i=0;i<21;++i) {
        CHECK(!gpu.cache.resident({0,0},1));
        gpu.cache.frame(i*.01,device.completedSubmission());
        CHECK(gpu.upload(device));CHECK_EQ(gpu.uploadedViews(),std::uint64_t(i+1));
        auto* commands=SDL_AcquireGPUCommandBuffer(device.handle());CHECK(commands);if (!commands) return;
        CHECK(device.submitFrame(commands));device.waitInFlight(0);
    }
    CHECK(gpu.cache.resident({0,0},1));
    const auto bindings=gpu.bindings({});std::vector<std::uint8_t> pixels(64*64*4);
    CHECK(device.readTexture(bindings[0].texture,pixels.data(),64,64));
    const auto at=(32*64+32)*4;CHECK_EQ(pixels[at],60);CHECK_EQ(pixels[at+1],180);CHECK_EQ(pixels[at+3],255);
    gpu.cache.protect(0,device.nextSubmission());gpu.cache.invalidate({0,0});
    CHECK(!gpu.cache.resident({0,0},1));
}

TEST(impostor_gpu_cache_far_shape_uploads_adopted_atlas_in_budgeted_batches) {
    struct Video {Video(){CHECK(SDL_Init(SDL_INIT_VIDEO));}~Video(){SDL_Quit();}} video;
    engine::Device device;
    CHECK(device.openHeadless(16,16,std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites"));
    if (!device.handle()) return;
    engine::render::ImpostorGpuCache gpu(4,32,1<<22);CHECK(gpu.setup(device));
    CHECK_EQ(gpu.mipLevels(),6u);CHECK_EQ(gpu.slots(),4u);
    auto atlas=std::make_shared<engine::render::ImpostorAtlas>();
    atlas->resolution=32;atlas->side=128;atlas->members=40;
    atlas->colour.resize(21*1024,{30,90,40,255});atlas->normal.resize(21*1024,{128,128,255,255});
    atlas->depth.resize(21*1024);
    for (unsigned view=0;view<21;++view) for (unsigned i=0;i<1024;++i) atlas->depth[view*1024+i]={128,0,std::uint8_t(view),255};
    engine::render::buildImpostorMips(*atlas);
    const engine::render::ImpostorKey key{0,0,2};
    gpu.cache.frame(0,0);CHECK_EQ(gpu.cache.adopt(key,9,atlas),0);CHECK(!gpu.cache.busy());
    // Sixteen views in one submission, then the rest only after its fence.
    CHECK(gpu.upload(device,16));CHECK_EQ(gpu.uploadedViews(),std::uint64_t(16));
    CHECK(!gpu.cache.resident(key,9));
    CHECK(gpu.upload(device,16));CHECK_EQ(gpu.uploadedViews(),std::uint64_t(16));
    auto* commands=SDL_AcquireGPUCommandBuffer(device.handle());CHECK(commands);if (!commands) return;
    CHECK(device.submitFrame(commands));device.waitInFlight(0);
    gpu.cache.frame(.1,device.completedSubmission());
    CHECK(gpu.upload(device,16));CHECK_EQ(gpu.uploadedViews(),std::uint64_t(21));
    CHECK(gpu.cache.resident(key,9));
    const auto bindings=gpu.bindings({});std::vector<std::uint8_t> pixels(32*32*4);
    CHECK(device.readTexture(bindings[0].texture,pixels.data(),32,32));
    CHECK_EQ(pixels[(16*32+16)*4+1],90);
}
