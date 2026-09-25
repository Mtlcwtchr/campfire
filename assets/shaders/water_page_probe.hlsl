// Read back the production adaptive water vertex and fragment source selection.
#include "water_pages.hlsl"
WaterOut WaterDetailProbeVS(AdaptivePageIn input, uint vertex : SV_VertexID)
{
    WaterOut output = AdaptiveWaterVS(input);
    output.position = float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
    const SurfaceClimate climate = climateAt(output.worldXY);
    output.depth += climate.foliage.x + climate.desert + climate.geography.w;
    return output;
}
float4 WaterDetailProbePS(WaterOut input) : SV_Target0
{
    const WaterOut resolved = resolveWaterPageDetail(input);
    // Keep the full fragment binding layout and scene uniform live.
    const float material = waterTex.SampleLevel(waterSampler,float3(0,0,0),0).r;
    return float4(resolved.depth / 1024.0 + 0.5 + material + cameraPS.x,
                  resolved.cover, resolved.motion.z, 1.0);
}

