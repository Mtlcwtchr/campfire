#include "terrain.hlsl"

float4 TerrainErosionProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex==1 ? 3.0 : -1.0, vertex==2 ? 3.0 : -1.0,0.0,1.0);
}
float4 TerrainErosionProbePS(float4 pixel : SV_Position) : SV_Target0
{
    const float2 p=cameraPS.xy+float2(floor(pixel.x)*7.3,floor(pixel.x)*3.7);
    const float3 normal=normalize(float3(extraPS.x,extraPS.x*0.7,1.0));
    const ErosionRelief erosion=erosionRelief(p,normal,cameraPS.w,cameraPS.z);
    return float4(-erosion.depth/16.0,erosion.cut,0.0,1.0);
}
