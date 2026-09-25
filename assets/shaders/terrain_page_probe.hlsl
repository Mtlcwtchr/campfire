// GPU regression probe. Uses the production page/grid/morph implementation.
#include "ground.hlsli"
#include "climate_field.hlsli"
#include "terrain_pages.hlsli"
#include "ring_reveal.hlsli"
#include "terrain_page_detail.hlsli"

Texture2D<float4> probeMaterial : register(t0, space2);
SamplerState probeMaterialSampler : register(s0, space2);

struct ProbeOut {
    float4 position : SV_Position;
    float height : TEXCOORD0;
};
ProbeOut TerrainPageProbeVS(PageGridIn input, uint vertex : SV_VertexID)
{
    const PageSurface surface = gridSurface(input);
    const SurfaceClimate climate = climateAt(surface.position.xy);
    ProbeOut output;
    output.position = float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
    // Keep the full production binding layout live; test climate textures are zero.
    output.height = surface.position.z / 1024.0 + climate.foliage.x + climate.desert + climate.geography.w;
    return output;
}
float4 TerrainPageProbePS(ProbeOut input) : SV_Target0
{
    const PageDetail detail = pageDetail(float2(8, 8));
    // Keep fragment t0 live, matching the material/page binding layout.
    const float material = probeMaterial.SampleLevel(probeMaterialSampler, float2(0, 0), 0).r;
    const float3 shape = pageShape(float2(8, 8));
    const float3 normal = normalize(float3(-shape.xy, 1.0));
    // Keep scene b0 live as in production, so Metal does not compact b1 to b0.
    return float4(input.height + material + cameraPS.x, detail.bed / 1024.0, normal.z, shape.z + 0.5);
}

struct AdaptiveProbeOut {
    float4 position : SV_Position;
    float2 heights : TEXCOORD0;
};
AdaptiveProbeOut AdaptivePageProbeVS(AdaptivePageIn input, uint vertex : SV_VertexID)
{
    const PageSurface surface = adaptiveSurface(input);
    const SurfaceClimate climate = climateAt(surface.position.xy);
    AdaptiveProbeOut output;
    output.position = float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
    output.heights = float2(surface.position.z, surface.waterHeight) / 1024.0
        + climate.foliage.x + climate.desert + climate.geography.w;
    return output;
}
float4 AdaptivePageProbePS(AdaptiveProbeOut input) : SV_Target0
{
    const PageDetail detail = pageDetail(float2(8, 8));
    const float material = probeMaterial.SampleLevel(probeMaterialSampler, float2(0, 0), 0).r;
    return float4(input.heights + material + cameraPS.x, detail.bed / 1024.0, 1.0);
}

