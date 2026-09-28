#define IMPOSTOR_DEPTH_TEXTURE_SLOT t0
#define IMPOSTOR_DEPTH_SAMPLER_SLOT s0
#include "scene_models.hlsl"
float4 DepthProbeVS(uint id : SV_VertexID) : SV_Position {
    return float4(id==1?3:-1,id==2?3:-1,0,1);
}
float4 DepthProbePS(float4 position : SV_Position) : SV_Target0 {
    ModelOut i=(ModelOut)0;
    i.world=i.plane=float3(0,0,3);
    i.uvLayer.xy=float2(0.5,0.5);
    i.frame=float3(4,6,1);
    if (parametersPS[20].x>0.5) {
        i.world=i.plane=0;
        i.flags.x=3;
        i.material.x=-parametersPS[20].y; // single-layer probe; production uses a real base layer
    }
    float2 uv;float3 world;
    const bool valid=reprojectModelDepth(i,0,uv,world);
    return float4(uv,depthTex.SampleLevel(depthSampler,float3(0.5,0.5,0),0).a,valid?1:0);
}
float4 HemisphereWeightsPS(float4 position : SV_Position) : SV_Target0 {
    float4 views,weights;
    hemisphereSelection(parametersPS[19].xyz,views,weights);
    const uint at=min(3u,(uint)floor(position.x));
    return float4(views[at]/20.0,weights[at],0,1);
}
float4 HemispherePlanePS(ModelOut i) : SV_Target0 { return float4(0,1,0,1); }
