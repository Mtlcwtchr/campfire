#ifndef TERRAIN_PAGES_HLSLI
#define TERRAIN_PAGES_HLSLI
#include "page_levels.hlsli"
// t0..2 are the shared climate field. H4/H8/H16/H64 are datasets.
Texture2D<float> height4 : register(t3, space0);
SamplerState height4Sampler : register(s3, space0);
Texture2D<float> height8 : register(t4, space0);
SamplerState height8Sampler : register(s4, space0);
Texture2D<float> height16 : register(t5, space0);
SamplerState height16Sampler : register(s5, space0);
Texture2D<float> height64 : register(t6, space0);
SamplerState height64Sampler : register(s6, space0);
Texture2DArray<float4> heightTable : register(t7, space0);
SamplerState heightTableSampler : register(s7, space0);
Texture2DArray<float4> fields4 : register(t8, space0);
SamplerState fields4Sampler : register(s8, space0);
Texture2DArray<float4> fields8 : register(t9, space0);
SamplerState fields8Sampler : register(s9, space0);
Texture2DArray<float4> fields16 : register(t10, space0);
SamplerState fields16Sampler : register(s10, space0);
Texture2DArray<float4> fields64 : register(t11, space0);
SamplerState fields64Sampler : register(s11, space0);

struct PageGridIn { uint4 grid : TEXCOORD0; };
struct AdaptivePageIn {
    uint4 grid : TEXCOORD0;
    float2 parentHeight : TEXCOORD1;
    float2 edgeHeight : TEXCOORD2;
    float2 sourceHeight : TEXCOORD3;
    float3 priorHeight : TEXCOORD4;
    float4 diagnostic : TEXCOORD5;
    float2 drainage : TEXCOORD6;
    float3 displayFrom : TEXCOORD7;
};
struct PageSurface {
    float3 position;
    float3 normal;
    float4 weights0;
    float2 weights1;
    float waterHeight;
    float waterCover;
    float4 waterMotion;
    float2 relief;
};

float4 pageAddress(float2 p, int level)
{
    float2 origin, page;
    const float3 texel = pageTableTexel(p, level, morphSettings.xy, origin, page);
    // Point-sampled table; zero scale is an explicitly absent ocean page.
    const float4 entry = pageEntryFrom(heightTable.SampleLevel(heightTableSampler, texel, 0), page, morphSettings.xy);
    return float4(entry.xy + (p - origin) * entry.zw, entry.zw);
}
float4 pageFields(float2 uv, int level, int plane)
{
    if (level == 0) return fields4.SampleLevel(fields4Sampler, float3(uv, plane), 0);
    if (level == 1) return fields8.SampleLevel(fields8Sampler, float3(uv, plane), 0);
    if (level == 2) return fields16.SampleLevel(fields16Sampler, float3(uv, plane), 0);
    return fields64.SampleLevel(fields64Sampler, float3(uv, plane), 0);
}
// The procedural environment's ground cover multiplier at a page address
// (engine/environment/masks.hpp, plane 6 .w): 0..2, one where nothing says
// otherwise. Vertex stages read it from the scene's `environment` switch.
float pageCoverAt(float4 address, int level)
{
    if (envSwitches.x < 0.5 || address.z == 0.0) return 1.0;
    return pageFields(address.xy, level, 6).w * 2.0;
}
float pageHeight(float2 p, int level)
{
    const float4 at = pageAddress(p, level);
    if (at.z == 0.0) return -60.0;
    float h;
    if (level == 0) h = height4.SampleLevel(height4Sampler, at.xy, 0);
    else if (level == 1) h = height8.SampleLevel(height8Sampler, at.xy, 0);
    else if (level == 2) h = height16.SampleLevel(height16Sampler, at.xy, 0);
    else h = height64.SampleLevel(height64Sampler, at.xy, 0);
    const float2 bands = (pageFields(at.xy, level, 0).xy * 65535.0 - 32768.0) * 0.01;
    return morphWindow.z + h * morphWindow.w + bands.x + bands.y;
}
float3 pageNormal(float2 p, int level, float step)
{
    const float dx = pageHeight(p + float2(step, 0), level) - pageHeight(p - float2(step, 0), level);
    const float dy = pageHeight(p + float2(0, step), level) - pageHeight(p - float2(0, step), level);
    return normalize(float3(-dx, -dy, 2.0 * step));
}
PageSurface pageSurface(float2 p, int level)
{
    PageSurface s = (PageSurface)0;
    s.position = float3(p, pageHeight(p, level));
    s.weights0.z = 1.0;
    s.waterHeight = 0.0;
    s.waterCover = 1.0;
    const float4 at = pageAddress(p, level);
    if (at.z != 0.0) {
        s.waterHeight = morphWindow.z + pageFields(at.xy, level, 0).z * morphWindow.w;
        s.weights0 = pageFields(at.xy, level, 1);
        const float4 material = pageFields(at.xy, level, 2);
        s.weights1 = material.xy;
        s.waterCover = material.z;
        s.waterMotion = pageFields(at.xy, level, 3);
        s.waterMotion.xy = s.waterMotion.xy * 2.0 - 1.0;
    }
    return s;
}
// Match the INDEX BUFFER's NE--SW diagonal, NOT a bilinear patch or NW--SE.
// At morph=1 every child vertex lies on the actual parent triangle plane.
void parentTriangle(float2 p, float step, out float2 a, out float2 b, out float2 c, out float3 weights)
{
    const float2 origin = floor(p / step) * step;
    const float2 f = (p - origin) / step;
    b = origin + float2(step, 0); c = origin + float2(0, step);
    if (f.x + f.y <= 1.0) { a = origin; weights = float3(1.0-f.x-f.y, f.x, f.y); }
    else { a = origin + step; weights = float3(f.x+f.y-1.0, 1.0-f.y, 1.0-f.x); }
}
PageSurface gridSurface(PageGridIn input)
{
    const float2 p = morphWindow.xy + float2(input.grid.xy) * morphing.w;
    const int level = (int)morphing.z;
    const int parentLevel = (int)morphReplacement.x;
    const float step = morphReplacement.y;
    PageSurface s = pageSurface(p, level);
    s.normal = pageNormal(p, level, morphing.w);
    s.relief = float2(1.0, 0.0);
    if (morphing.x > 0.0) {
        float2 a, b, c; float3 w;
        parentTriangle(p, step, a, b, c, w);
        const PageSurface sa = pageSurface(a, parentLevel);
        const PageSurface sb = pageSurface(b, parentLevel);
        const PageSurface sc = pageSurface(c, parentLevel);
        const float t = saturate(morphing.x);
        s.position.z = lerp(s.position.z, sa.position.z*w.x + sb.position.z*w.y + sc.position.z*w.z, t);
        const float3 parentNormal = pageNormal(a,parentLevel,step)*w.x + pageNormal(b,parentLevel,step)*w.y + pageNormal(c,parentLevel,step)*w.z;
        s.normal = normalize(lerp(s.normal, parentNormal, t));
        s.weights0 = lerp(s.weights0, sa.weights0*w.x + sb.weights0*w.y + sc.weights0*w.z, t);
        s.weights1 = lerp(s.weights1, sa.weights1*w.x + sb.weights1*w.y + sc.weights1*w.z, t);
        s.waterHeight = lerp(s.waterHeight, sa.waterHeight*w.x + sb.waterHeight*w.y + sc.waterHeight*w.z, t);
        s.waterCover = lerp(s.waterCover, sa.waterCover*w.x + sb.waterCover*w.y + sc.waterCover*w.z, t);
        s.waterMotion = lerp(s.waterMotion, sa.waterMotion*w.x + sb.waterMotion*w.y + sc.waterMotion*w.z, t);
    }
    if ((input.grid.z & 1) != 0) {
        s.position.z -= morphReplacement.z;
        if (morphSettings.w > 0.5) s.position.z = min(s.position.z, morphSettings.z);
    }
    return s;
}
PageSurface adaptiveSurface(AdaptivePageIn input)
{
    const float2 p = morphWindow.xy + float2(input.grid.xy) * morphing.w;
    const PageSurface fine = pageSurface(p, (int)morphing.z);
    PageSurface s = fine;
    s.normal = pageNormal(p, (int)morphing.z, morphing.w);
    s.relief = float2(1.0, 0.0);
    if (morphing.x > 0.0 && morphing.z != morphReplacement.x) {
        const PageSurface parent = pageSurface(p, (int)morphReplacement.x);
        s.weights0 = lerp(s.weights0, parent.weights0, saturate(morphing.x));
        s.weights1 = lerp(s.weights1, parent.weights1, saturate(morphing.x));
        s.waterCover = lerp(s.waterCover, parent.waterCover, saturate(morphing.x));
        s.waterMotion = lerp(s.waterMotion, parent.waterMotion, saturate(morphing.x));
    }
    const float2 height = (input.grid.z & 4) != 0 ? input.sourceHeight : float2(fine.position.z,fine.waterHeight);
    s.position.z = lerp(height.x, input.parentHeight.x, saturate(morphing.x));
    s.waterHeight = lerp(height.y, input.parentHeight.y, saturate(morphing.x));
    if ((input.grid.z & 2) != 0) {
        s.position.z = input.edgeHeight.x;
        s.waterHeight = input.edgeHeight.y;
    }
    if (morphing.y >= 3.0) {
        const float prior = (input.grid.z & 2) != 0 ? input.priorHeight.z :
            lerp(input.priorHeight.x, input.priorHeight.y, saturate(morphing.x));
        const float t = saturate(morphing.y - 3.0);
        s.position.z = lerp(prior, s.position.z, t*t*(3.0-2.0*t));
        if (t < 1.0) s.waterHeight = -6000.0;
    }
    if ((input.grid.z & 8) != 0) {
        // Adaptive meshes use replacement.y as publication-time progress; only
        // the legacy grid path uses it as a regular parent lattice spacing.
        float from = input.displayFrom.x;
        if (morphing.y >= 3.0) {
            const float stage = saturate(morphing.y - 3.0);
            from = lerp(input.displayFrom.z, from, stage*stage*(3.0-2.0*stage));
        }
        const float t = saturate(morphReplacement.y);
        s.position.z = lerp(from, s.position.z, t);
        if (morphing.y < 3.0 || morphing.y >= 4.0)
            s.waterHeight = lerp(input.displayFrom.y, s.waterHeight, t);
    }
    if ((input.grid.z & 1) != 0) {
        s.position.z -= morphReplacement.z;
        if (morphSettings.w > 0.5) s.position.z = min(s.position.z, morphSettings.z);
    }
    return s;
}
#endif

