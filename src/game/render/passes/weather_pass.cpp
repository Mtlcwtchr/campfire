#include "game/render/passes/weather_pass.hpp"
#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include <algorithm>
#include <cmath>

namespace game {
engine::PassPlace WeatherPass::setup(engine::Device& device,engine::RenderPipeline& into) {
    renderer_=&into;
    previousTime_=-1;
    previousClock_=-1;
    drift_={};
    rainWind_={};snowWind_={};
    engine::PipelineWanted wanted;
    wanted.shaderFile="precipitation.hlsl";
    wanted.vertexEntry="PrecipitationVS";
    wanted.fragmentEntry="PrecipitationPS";
    wanted.blend=true;
    wanted.depthTest=false;
    wanted.depthWrite=false;
    auto graphics=device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_=into.take(std::move(graphics));
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter=sampling.mag_filter=SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u=sampling.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_=device.makeSampler(sampling);
    if (!sampler_) return {};
    bindings_=into.take(std::vector<SDL_GPUTextureSamplerBinding>{{nullptr,sampler_.get()}});
    return {engine::passOf(Pass::Weather),engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(240)};
}
bool WeatherPass::anything(const engine::Frame& frame) const {
    // Rain is drawn from under the clouds: from five kilometres up the eye is
    // above the weather, and the streaks over a whole continent were a screen
    // of white dashes in front of it.
    return frame.grab && frame.scene.extra[3]<0.5f && frame.scene.parameters[0][0]>0.5f &&
           frame.scene.parameters[2][1]>0.005f && frame.scene.camera[2] < 5000.0f;
}
void WeatherPass::collect(const engine::Frame& frame,engine::DrawQueue& queue) {
    const auto& scene=frame.scene;
    const auto axis=[&](int row) {
        std::array<double,3> a{scene.viewProjection[row*4],scene.viewProjection[row*4+1],scene.viewProjection[row*4+2]};
        const double n=std::hypot(a[0],a[1],a[2]);
        if (n>1e-10) for (auto& v:a) v/=n;
        return a;
    };
    const auto right=axis(0),up=axis(1),forward=axis(3);
    const auto dot=[](const auto& a,const auto& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    const bool perspective=dot(forward,forward)>0.5;
    const double reference=1080.0/std::max(1u,frame.height);
    const double focalX=std::hypot(scene.viewProjection[0],scene.viewProjection[1],scene.viewProjection[2])*frame.width*reference*0.5;
    const double focalY=std::hypot(scene.viewProjection[4],scene.viewProjection[5],scene.viewProjection[6])*540.0;
    const std::array<double,3> eye{scene.camera[0],scene.camera[1],scene.camera[2]};
    std::array<double,3> delta{};
    for (int i=0;i<3;++i) delta[i]=eye[i]-previousEye_[i];
    const bool continuous=previousTime_>=0 && frame.seconds-previousTime_<0.25 && dot(delta,delta)<10000;
    // Integrate camera deltas instead of multiplying absolute world positions
    // into screen pixels (which made the old pattern jump on tiny movements).
    if (continuous) {
        const double yaw=perspective?std::atan2(dot(previousForward_,right),dot(previousForward_,forward)):0;
        const double pitch=perspective?std::atan2(dot(previousForward_,up),dot(previousForward_,forward)):0;
        constexpr double depths[]{6,18,48};
        for (int layer=0;layer<3;++layer) {
            const double distance=perspective?depths[layer]:1.0;
            drift_[layer][0]+=focalX*(dot(delta,right)/distance-yaw);
            drift_[layer][1]-=focalY*(dot(delta,up)/distance-pitch);
        }
    }
    const double clock=scene.viewport[2];
    const std::array<double,3> wind{scene.wind[0]*scene.wind[2],scene.wind[1]*scene.wind[2],0};
    const double gust=1+std::clamp(double(scene.wind[3]),0.0,1.0)*
        (0.22*std::sin(clock*0.43)+0.11*std::sin(clock*0.17));
    const double windX=dot(wind,right)*gust,windY=-dot(wind,up)*gust;
    const double elapsed=clock-previousClock_;
    // Integrate wind velocity. Multiplying the current gust by total uptime
    // makes each change jump farther as the session gets longer.
    if (continuous && elapsed>=0 && elapsed<0.25) {
        for (int layer=0;layer<3;++layer) {
            const double near=1-layer*0.34;
            const double rainSpeed=270+(690-270)*near;
            const double snowSpeed=19+(64-19)*near;
            rainWind_[layer][0]+=windX*0.24*rainSpeed*elapsed;
            rainWind_[layer][1]+=windY*0.14*rainSpeed*elapsed;
            snowWind_[layer][0]+=windX*0.28*snowSpeed*elapsed;
            snowWind_[layer][1]+=windY*0.10*snowSpeed*elapsed;
        }
    }
    previousEye_=eye;previousForward_=forward;previousTime_=frame.seconds;previousClock_=clock;
    renderer_->replace(bindings_,{{frame.grab,sampler_.get()}});
    engine::DrawItem item;
    item.author = 6;
    item.pipeline=pipeline_;
    item.bindings=bindings_;
    item.vertexCount=3;
    item.hasOwnData=true;
    for (int layer=0;layer<3;++layer) {
        item.own[layer*4]=float(drift_[layer][0]-rainWind_[layer][0]);
        item.own[layer*4+1]=float(drift_[layer][1]-rainWind_[layer][1]);
        item.own[layer*4+2]=float(rainWind_[layer][0]-snowWind_[layer][0]);
        item.own[layer*4+3]=float(rainWind_[layer][1]-snowWind_[layer][1]);
    }
    item.own[12]=float(windX);
    item.own[13]=float(windY);
    item.own[14]=perspective?1.0f:0.0f;
    item.own[15]=float(std::abs(up[2]));
    queue.push(item);
}
}
