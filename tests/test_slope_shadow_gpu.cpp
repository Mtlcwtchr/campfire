#include "framework.hpp"
#include "engine/render/device.hpp"
#include "engine/render/frame.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace {
struct Probe {
    engine::Device device;
    engine::Texture target;
    engine::Sampler sampler;
    bool ready=false;
    Probe() {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP,"1");
        if (!SDL_Init(SDL_INIT_VIDEO)) return;
        const auto assets=std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites";
        if (!device.openHeadless(64,64,assets)) return;
        SDL_GPUTextureCreateInfo info{};
        info.type=SDL_GPU_TEXTURETYPE_2D;info.format=engine::Device::kColourFormat;
        info.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        info.width=info.height=64;info.layer_count_or_depth=info.num_levels=1;
        target=device.makeTexture(info);
        SDL_GPUSamplerCreateInfo sample{};
        sample.min_filter=sample.mag_filter=SDL_GPU_FILTER_NEAREST;
        sample.address_mode_u=sample.address_mode_v=sample.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler=device.makeSampler(sample);
        ready=bool(target) && bool(sampler);
    }
    ~Probe() { sampler={};target={};device.close();SDL_Quit(); }
    engine::Texture texture(SDL_GPUTextureFormat format,int side,int layers) {
        SDL_GPUTextureCreateInfo info{};
        info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;info.format=format;
        info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width=info.height=Uint32(side);info.layer_count_or_depth=Uint32(layers);info.num_levels=1;
        return device.makeTexture(info);
    }
    engine::GraphicsPipeline pipeline(const char* file,const char* vertex,const char* fragment) {
        engine::PipelineWanted wanted;
        wanted.shaderFile=file;wanted.vertexEntry=vertex;wanted.fragmentEntry=fragment;
        wanted.depthTest=wanted.depthWrite=false;
        auto result=device.makePipeline(wanted);
        if (!result) std::cerr<<device.error()<<'\n';
        return result;
    }
    std::array<Uint8,64*64*4> draw(engine::GraphicsPipeline& pipeline,const engine::Scene& scene,
                                 const std::vector<SDL_GPUTextureSamplerBinding>& bindings,const char* name) {
        std::array<Uint8,64*64*4> pixels{};
        auto* commands=SDL_AcquireGPUCommandBuffer(device.handle());
        CHECK(commands!=nullptr);
        if (!commands) return pixels;
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
        CHECK(device.submitFrame(commands));
        CHECK(device.readTexture(target.get(),pixels.data(),64,64));
        if (const auto* directory=std::getenv("ASR_SLOPE_SHADOW_PREVIEWS")) {
            std::filesystem::create_directories(directory);
            auto* image=SDL_CreateSurfaceFrom(64,64,SDL_PIXELFORMAT_RGBA32,pixels.data(),64*4);
            if (image) {
                CHECK(SDL_SaveBMP(image,(std::filesystem::path(directory)/(std::string(name)+".bmp")).string().c_str()));
                SDL_DestroySurface(image);
            }
        }
        return pixels;
    }
};
}

TEST(terrain_slope_does_not_invent_noise_from_erosion_history) {
    Probe p;
    CHECK(p.ready);if (!p.ready) return;
    std::array<engine::Texture,3> maps;
    engine::Device::Uploader upload(p.device);
    for (int channel=0;channel<3;++channel) {
        maps[channel]=p.texture(engine::Device::kColourFormat,16,6);
        CHECK(bool(maps[channel]));if (!maps[channel]) return;
        const std::array<Uint8,4> value=channel==1?std::array<Uint8,4>{128,128,255,255}:
            channel==2?std::array<Uint8,4>{255,200,128,255}:std::array<Uint8,4>{128,128,128,255};
        const std::vector<std::array<Uint8,4>> pixels(16*16,value);
        for (int layer=0;layer<6;++layer) CHECK(upload.refillRegion(maps[channel].get(),pixels.data(),0,0,16,16,4,layer));
    }
    CHECK(upload.finish());
    auto pipeline=p.pipeline("terrain_slope_probe.hlsl","SlopeProbeVS","SlopeProbePS");
    CHECK(bool(pipeline));if (!pipeline) return;
    std::vector<SDL_GPUTextureSamplerBinding> bindings(13,{maps[0].get(),p.sampler.get()});
    bindings[10].texture=maps[1].get();bindings[11].texture=maps[2].get();
    for (const float slope:{0.5f,1.5f}) for (const float footprint:{0.08f,0.35f,1.5f}) {
        engine::Scene scene{};
        scene.camera[0]=47300;scene.camera[1]=28700;scene.camera[2]=100;scene.camera[3]=footprint;
        scene.extra[0]=0.7f;scene.extra[1]=slope;
        scene.viewport[0]=scene.viewport[1]=64;
        scene.viewProjection[10]=1;
        for (auto& material:scene.table) { material[0]=7;material[1]=0.2f;material[3]=3; }
        const std::string name="slope_"+std::to_string(slope)+"_pixel_"+std::to_string(footprint);
        const auto base=p.draw(pipeline,scene,bindings,(name+"_base").c_str());
        scene.extra[2]=120; // diagnostic removed-height, not new geometry
        const auto eroded=p.draw(pipeline,scene,bindings,(name+"_eroded").c_str());
        int difference=0,low=255,high=0;
        for (std::size_t i=0;i<base.size();i+=4) {
            difference=std::max(difference,std::abs(int(base[i])-int(eroded[i])));
            low=std::min(low,int(eroded[i]));high=std::max(high,int(eroded[i]));
        }
        std::cout<<name<<" diagnostic delta="<<difference<<" spatial range="<<high-low<<'\n';
        CHECK(difference<=1);
        CHECK(high-low<=4); // constant rock on a plane, not random etched grooves
    }
}

TEST(shadow_edges_are_soft_continuous_and_preserve_receiver_depth) {
    Probe p;
    CHECK(p.ready);if (!p.ready) return;
    auto field=p.texture(SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT,128,4);
    auto pipeline=p.pipeline("shadow_edge_probe.hlsl","ShadowEdgeVS","ShadowEdgePS");
    CHECK(bool(field) && bool(pipeline));if (!field || !pipeline) return;
    const std::vector<SDL_GPUTextureSamplerBinding> bindings{{field.get(),p.sampler.get()}};
    for (int kind=0;kind<4;++kind) {
        std::vector<std::array<float,4>> texels(128*128,{-1e20f,-1e20f,0,-1e20f});
        for (int y=0;y<128;++y) for (int x=0;x<128;++x) {
            auto& t=texels[std::size_t(y)*128+std::size_t(x)];
            if (kind==3) t[0]=(float(x)+0.5f-64)*0.8f; // inclined receiver itself
            else if (x<64) {
                if (kind==0) t[0]=10;
                if (kind==1) t[3]=10;
                if (kind==2) { t[1]=10;t[2]=1.38629436f; } // transmittance 1/4
            }
        }
        engine::Device::Uploader upload(p.device);
        for (int layer=0;layer<4;++layer) CHECK(upload.refillRegion(field.get(),texels.data(),0,0,128,128,16,layer));
        CHECK(upload.finish());
        engine::Scene scene{};
        scene.camera[3]=0.125f;scene.shadowSun[2]=scene.shadowSun[3]=1;
        scene.shadowClip[0][3]=128;
        scene.extra[0]=kind==3?0.8f:0;
        const auto pixels=p.draw(pipeline,scene,bindings,("shadow_"+std::to_string(kind)).c_str());
        const auto at=[&](int x){return int(pixels[std::size_t(32*64+x)*4]);};
        int fractional=0,maxJump=0;
        for (int x=1;x<64;++x) {
            maxJump=std::max(maxJump,std::abs(at(x)-at(x-1)));
            fractional+=at(x)>75 && at(x)<245;
            CHECK(at(x)+1>=at(x-1));
        }
        if (kind==3) { for (int x=0;x<64;++x) CHECK(at(x)>=254);continue; }
        std::cout<<"shadow kind="<<kind<<" fractional="<<fractional<<" max jump="<<maxJump<<'\n';
        CHECK(fractional>=12);CHECK(maxJump<=24);
        CHECK(at(0)<=(kind==2?66:1));CHECK(at(63)>=254);
        if (kind==2) CHECK(at(0)>=62);
        scene.shadowSun[3]=0;
        const auto disabled=p.draw(pipeline,scene,bindings,"shadow_disabled");
        CHECK_EQ(disabled[32*64*4],255);
    }
}

