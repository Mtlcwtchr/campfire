// A long buried wall crosses the far depth bound. Use production projection.
#include "world.hlsli"

struct DepthProbeIn {
    float3 position : TEXCOORD0;
    float3 colour : TEXCOORD1;
};
struct DepthProbeOut {
    float4 position : SV_Position;
    float3 colour : TEXCOORD0;
};
DepthProbeOut TerrainDepthProbeVS(DepthProbeIn input)
{
    DepthProbeOut output;
    output.position = project(input.position);
    output.colour = input.colour;
    return output;
}
float4 TerrainDepthProbePS(DepthProbeOut input) : SV_Target0
{
    return float4(input.colour, 1.0);
}

