// Regression probe: world-anchored value noise must remain continuous.
#include "terrain.hlsl"

float4 TerrainEdgeProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 TerrainEdgeProbePS(float4 pixel : SV_Position) : SV_Target0
{
    const float x = floor(pixel.x);
    const float2 p = cameraPS.xy + float2(x * 7.3, x * 3.7);
    const float metres = 1.25;
    const float2 cell = floor(p / metres);
    return float4(groundEdgeNoise(p, metres, 0.0), noiseAt(p / metres),
                  noiseAt(cell + float2(-0.001, 0.37)),
                  noiseAt(cell + float2(0.001, 0.37)));
}

