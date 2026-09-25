// Regression probe: use the production material-border noise on a turning slope.
#include "terrain.hlsl"

float4 TerrainEdgeProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 TerrainEdgeProbePS(float4 pixel : SV_Position) : SV_Target0
{
    const float x = floor(pixel.x);
    const float2 p = cameraPS.xy + float2(x * 7.3, x * 3.7);
    const float angle = 0.7 + x * 0.013;
    const float2 before = float2(cos(angle), sin(angle));
    const float2 after = float2(cos(angle + 0.001), sin(angle + 0.001));
    const float metres = 1.25;
    return float4(groundEdgeNoise(p, before, metres, 1.0),
                  groundEdgeNoise(p, after, metres, 1.0),
                  groundEdgeNoise(p, before, metres, 0.0), noiseAt(p / metres));
}

