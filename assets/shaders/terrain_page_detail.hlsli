#ifndef TERRAIN_PAGE_DETAIL_HLSLI
#define TERRAIN_PAGE_DETAIL_HLSLI
// Fragment-only page fields. t0 belongs to the material/water texture.
// Geometry and pixel shading share data residency, not a sampling lattice.
Texture2D<float> detailHeight4 : register(t1, space2);
SamplerState detailHeight4Sampler : register(s1, space2);
Texture2D<float> detailHeight8 : register(t2, space2);
SamplerState detailHeight8Sampler : register(s2, space2);
Texture2D<float> detailHeight16 : register(t3, space2);
SamplerState detailHeight16Sampler : register(s3, space2);
Texture2D<float> detailHeight64 : register(t4, space2);
SamplerState detailHeight64Sampler : register(s4, space2);
Texture2DArray<float4> detailTable : register(t5, space2);
SamplerState detailTableSampler : register(s5, space2);
Texture2DArray<float4> detailFields4 : register(t6, space2);
SamplerState detailFields4Sampler : register(s6, space2);
Texture2DArray<float4> detailFields8 : register(t7, space2);
SamplerState detailFields8Sampler : register(s7, space2);
Texture2DArray<float4> detailFields16 : register(t8, space2);
SamplerState detailFields16Sampler : register(s8, space2);
Texture2DArray<float4> detailFields64 : register(t9, space2);
SamplerState detailFields64Sampler : register(s9, space2);

struct PageDetail {
    float bed;
    float head;
    float4 weights0;
    float2 weights1;
    float cover;
    float4 motion;
};
float4 detailFields(float2 uv, int level, int plane)
{
    if (level == 0) return detailFields4.SampleLevel(detailFields4Sampler, float3(uv, plane), 0);
    if (level == 1) return detailFields8.SampleLevel(detailFields8Sampler, float3(uv, plane), 0);
    if (level == 2) return detailFields16.SampleLevel(detailFields16Sampler, float3(uv, plane), 0);
    return detailFields64.SampleLevel(detailFields64Sampler, float3(uv, plane), 0);
}
PageDetail pageDetailAt(float2 p, int level)
{
    PageDetail s = (PageDetail)0;
    s.bed = -60.0; s.cover = 1.0; s.weights0.z = 1.0;
    const float2 page = floor(p / 512.0), index = page + 2.0;
    if (any(index < 0.0) || any(index >= ringReserved.xy)) return s;
    const float4 entry = detailTable.SampleLevel(detailTableSampler,
        float3((index + 0.5) / ringReserved.xy, level), 0);
    if (entry.z == 0.0) return s;
    const float2 uv = entry.xy + (p - page * 512.0) * entry.zw;
    float h;
    if (level == 0) h = detailHeight4.SampleLevel(detailHeight4Sampler, uv, 0);
    else if (level == 1) h = detailHeight8.SampleLevel(detailHeight8Sampler, uv, 0);
    else if (level == 2) h = detailHeight16.SampleLevel(detailHeight16Sampler, uv, 0);
    else h = detailHeight64.SampleLevel(detailHeight64Sampler, uv, 0);
    const float4 bands = detailFields(uv, level, 0);
    s.bed = ringWindow.z + h * ringWindow.w + dot((bands.xy * 65535.0 - 32768.0) * 0.01, float2(1, 1));
    s.head = ringWindow.z + bands.z * ringWindow.w;
    s.weights0 = detailFields(uv, level, 1);
    const float4 material = detailFields(uv, level, 2);
    s.weights1 = material.xy; s.cover = material.z;
    s.motion = detailFields(uv, level, 3);
    s.motion.xy = s.motion.xy * 2.0 - 1.0;
    return s;
}
PageDetail pageDetail(float2 p)
{
    PageDetail s = pageDetailAt(p, (int)ringState.z);
    // No triangle interpolation: a 32 m grid can shade the complete H16 field.
    // Same-dataset splits never postpone shading until their morph finishes.
    if (ringState.x > 0.0 && ringState.z != ringReplacement.x) {
        const PageDetail parent = pageDetailAt(p, (int)ringReplacement.x);
        const float t = saturate(ringState.x);
        s.bed = lerp(s.bed, parent.bed, t);
        s.head = lerp(s.head, parent.head, t);
        s.weights0 = lerp(s.weights0, parent.weights0, t);
        s.weights1 = lerp(s.weights1, parent.weights1, t);
        s.cover = lerp(s.cover, parent.cover, t);
        s.motion = lerp(s.motion, parent.motion, t);
    }
    return s;
}
// Height-only reads for the shading stencil: never fetch water/material planes
// at every neighbour. Its radius belongs to the DATA, not the triangle grid.
float detailBedAt(float2 p, int level)
{
    const float2 page = floor(p / 512.0), index = page + 2.0;
    if (any(index < 0.0) || any(index >= ringReserved.xy)) return -60.0;
    const float4 entry = detailTable.SampleLevel(detailTableSampler,
        float3((index + 0.5) / ringReserved.xy, level), 0);
    if (entry.z == 0.0) return -60.0;
    const float2 uv = entry.xy + (p - page * 512.0) * entry.zw;
    float h;
    if (level == 0) h = detailHeight4.SampleLevel(detailHeight4Sampler, uv, 0);
    else if (level == 1) h = detailHeight8.SampleLevel(detailHeight8Sampler, uv, 0);
    else if (level == 2) h = detailHeight16.SampleLevel(detailHeight16Sampler, uv, 0);
    else h = detailHeight64.SampleLevel(detailHeight64Sampler, uv, 0);
    const float2 bands = (detailFields(uv, level, 0).xy * 65535.0 - 32768.0) * 0.01;
    return ringWindow.z + h * ringWindow.w + bands.x + bands.y;
}

// xy = height gradient, z = normalized openness (positive at a crest).
float3 pageShapeAt(float2 p, int level)
{
    const float step = level == 0 ? 4.0 : level == 1 ? 8.0 : level == 2 ? 16.0 : 64.0;
    const float left = detailBedAt(p - float2(step, 0), level);
    const float right = detailBedAt(p + float2(step, 0), level);
    const float down = detailBedAt(p - float2(0, step), level);
    const float up = detailBedAt(p + float2(0, step), level);
    const float around = detailBedAt(p - float2(2.0 * step, 0), level) +
        detailBedAt(p + float2(2.0 * step, 0), level) +
        detailBedAt(p - float2(0, 2.0 * step), level) +
        detailBedAt(p + float2(0, 2.0 * step), level);
    return float3(float2(right - left, up - down) / (2.0 * step),
        (detailBedAt(p, level) - around * 0.25) / (4.0 * step));
}

float3 pageShape(float2 p)
{
    // Differentiating quantized/filtered heights over a fraction of a pixel
    // amplifies sampler rounding when zoomed in. A world-space stencil stays
    // stable and uses the selected data level's normals/curvature.
    float3 shape = pageShapeAt(p, (int)ringState.z);
    if (ringState.x > 0.0 && ringState.z != ringReplacement.x)
        shape = lerp(shape, pageShapeAt(p, (int)ringReplacement.x), saturate(ringState.x));
    return shape;
}
#endif

