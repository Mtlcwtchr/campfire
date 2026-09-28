#include "game/render/passes/frustum_pass.hpp"
#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include <algorithm>
#include <array>
#include <cmath>

namespace game {
engine::PassPlace FrustumPass::setup(engine::Device& device,engine::RenderPipeline& into) {
    engine::PipelineWanted wanted;
    wanted.shaderFile="frustum.hlsl";
    wanted.vertexEntry="FrustumVS";
    wanted.fragmentEntry="FrustumPS";
    wanted.primitive=SDL_GPU_PRIMITIVETYPE_LINELIST;
    wanted.blend=false;
    wanted.depthTest=false; // visible through the ground: it is a marker, not an object
    wanted.depthWrite=false;
    wanted.depthClip=true;
    auto pipeline=device.makePipeline(wanted);
    if (!pipeline) return {};
    pipeline_=into.take(std::move(pipeline));
    return {engine::passOf(Pass::Frustum),engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(Order::Cover)};
}
bool FrustumPass::anything(const engine::Frame& frame) const {
    const auto* m=frame.scene.cullViewProjection;
    return frame.scene.cullState[1]>0.5f && (m[12]!=0 || m[13]!=0 || m[14]!=0);
}
void FrustumPass::collect(const engine::Frame& frame,engine::DrawQueue& queue) {
    const auto& s=frame.scene;const float* m=s.cullViewProjection;
    // The ray through an NDC corner: (row0 - x row3).d = 0 and (row1 - y row3).d = 0.
    const auto ray=[&](double x,double y) {
        const double a[3]{m[0]-x*m[12],m[1]-x*m[13],m[2]-x*m[14]};
        const double b[3]{m[4]-y*m[12],m[5]-y*m[13],m[6]-y*m[14]};
        double d[3]{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        if (d[0]*m[12]+d[1]*m[13]+d[2]*m[14]<0) for (double& v:d) v=-v;
        const double n=std::sqrt(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]);
        for (double& v:d) v/=std::max(n,1e-12);
        return std::array<double,3>{d[0],d[1],d[2]};
    };
    // Long enough to read, never past where the frozen view stops drawing.
    const double length=std::clamp(double(s.fog[0])*0.25,300.0,4000.0);
    float points[16]{};
    for (int k=0;k<3;++k) points[k]=s.cullCamera[k];
    const double corners[4][2]{{-1,-1},{1,-1},{1,1},{-1,1}};
    for (int c=0;c<4;++c) {
        const auto d=ray(corners[c][0],corners[c][1]);
        for (int k=0;k<3;++k) points[3+c*3+k]=float(s.cullCamera[k]+d[std::size_t(k)]*length);
    }
    engine::DrawItem item;
    item.author=6;item.pipeline=pipeline_;item.vertexCount=16;
    item.ownToVertex=true;
    std::copy(std::begin(points),std::end(points),item.own);
    queue.push(item);
}
}

