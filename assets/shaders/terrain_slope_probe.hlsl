// Exercise the real adaptive-page fragment path with a constant material and
// analytic inclined plane. No world generation or texture grain can hide the
// slope-only procedural lighting layer in this regression.
#define TERRAIN_PAGE_MATERIALS
#include "terrain.hlsl"
float4 SlopeProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0,0,1);
}
float4 SlopeProbePS(float4 pixel : SV_Position) : SV_Target0
{
    const float2 local=(pixel.xy-32.5)*cameraPS.w;
    const float c=cos(extraPS.x),s=sin(extraPS.x);
    const float2 xy=float2(c*local.x-s*local.y,s*local.x+c*local.y);
    const float slope=extraPS.y;
    TerrainOut input=(TerrainOut)0;
    input.position=pixel;
    input.worldXY=cameraPS.xy+xy;
    input.frameXY=input.worldXY;   // frame origin nought: the world as it is
    input.worldHeight=cameraPS.z+xy.x*slope;
    input.normal=normalize(float3(-slope,0,1));
    input.weights0=float4(0,0,0,1);
    input.waterDepth=-100;
    input.stageDiagnostic.y=extraPS.z;
    return TerrainPS(input);
}

