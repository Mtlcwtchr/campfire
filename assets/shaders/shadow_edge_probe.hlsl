#define SHADOW_TEXTURE_SLOT t0
#define SHADOW_SAMPLER_SLOT s0
#include "shadow_field.hlsli"
float4 ShadowEdgeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0,0,1);
}
float4 ShadowEdgePS(float4 pixel : SV_Position) : SV_Target0
{
    // Sub-texel receiver motion must yield a smooth penumbra, not PCF plateaus.
    const float x=(pixel.x-32.0)*cameraPS.w;
    const float3 p=float3(x,cameraPS.y,x*extraPS.x);
    const float visibility=proceduralShadow(p,normalize(float3(-extraPS.x,0,1)));
    return float4(visibility,visibility,visibility,1);
}

