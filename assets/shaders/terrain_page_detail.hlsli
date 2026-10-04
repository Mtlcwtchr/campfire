#ifndef TERRAIN_PAGE_DETAIL_HLSLI
#define TERRAIN_PAGE_DETAIL_HLSLI
#include "noise.hlsli"
#include "page_levels.hlsli"
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
// The table entry of the page holding p at a dataset, and that page's corner.
float4 detailEntryAt(float2 p, int level, out float2 origin)
{
    float2 page;
    const float3 texel = pageTableTexel(p, level, ringReserved.xy, origin, page);
    return pageEntryFrom(detailTable.SampleLevel(detailTableSampler, texel, 0), page, ringReserved.xy);
}
PageDetail pageDetailAt(float2 p, int level)
{
    PageDetail s = (PageDetail)0;
    s.bed = -60.0; s.cover = 1.0; s.weights0.z = 1.0;
    float2 origin;
    const float4 entry = detailEntryAt(p, level, origin);
    if (entry.z == 0.0) return s;
    const float2 uv = entry.xy + (p - origin) * entry.zw;
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
// The procedural environment at p (environment_style.hlsli): planes 4 and 5
// are the eight mask channels, filtered; plane 6 holds the zone ids, which a
// filter would blend into ids nobody has, so they are loaded from the nearest
// texel.
float4 detailFieldsLoad(float2 uv, int level, int plane)
{
    uint w, h, layers;
    if (level == 0) { detailFields4.GetDimensions(w, h, layers); return detailFields4.Load(int4(int2(uv * float2(w, h)), plane, 0)); }
    if (level == 1) { detailFields8.GetDimensions(w, h, layers); return detailFields8.Load(int4(int2(uv * float2(w, h)), plane, 0)); }
    if (level == 2) { detailFields16.GetDimensions(w, h, layers); return detailFields16.Load(int4(int2(uv * float2(w, h)), plane, 0)); }
    detailFields64.GetDimensions(w, h, layers);
    return detailFields64.Load(int4(int2(uv * float2(w, h)), plane, 0));
}
EnvironmentPoint pageEnvironment(float2 p)
{
    EnvironmentPoint e = (EnvironmentPoint)0;
    if (envSwitchesPS.x < 0.5) return e;
    const int level = (int)ringState.z;
    float2 origin;
    const float4 entry = detailEntryAt(p, level, origin);
    if (entry.z == 0.0) return e;
    const float2 uv = entry.xy + (p - origin) * entry.zw;
    e.masksA = detailFields(uv, level, 4);
    e.masksB = detailFields(uv, level, 5);
    const float4 zones = detailFieldsLoad(uv, level, 6);
    e.zone = (uint)round(zones.x * 255.0);
    e.second = (uint)round(zones.y * 255.0);
    e.share = zones.z;
    e.cover = zones.w * 2.0;
    return e;
}
// Only the two material planes, for the blur below.
void pageWeightsAt(float2 p, int level, out float4 weights0, out float2 weights1)
{
    weights0 = float4(0, 0, 1, 0); weights1 = 0;
    float2 origin;
    const float4 entry = detailEntryAt(p, level, origin);
    if (entry.z == 0.0) return;
    const float2 uv = entry.xy + (p - origin) * entry.zw;
    weights0 = detailFields(uv, level, 1);
    weights1 = detailFields(uv, level, 2).xy;
}
// Material weights live on 4..64 m texels. Read bilinearly, the line where
// two materials tie is a polygon along the texel grid - teeth - and a step
// from one material to the next inside one texel is a hard edge. Fixed at the
// source rather than in the blend: the lookup point is warped by
// low-frequency noise at about two texels, and the field is reconstructed
// with a C2 cubic B-spline (a one-texel step becomes a smooth ramp about
// three texels wide). The whole footprint resolves inside one page: it needs
// texels -1..+2 and pages carry two samples of padding.
//
// Evaluated from exact texel loads, in float, not through the filtering
// hardware: the sampler interpolates with 8 bits of sub-texel position, so a
// 4 m texel is 256 flat terraces 1.6 cm wide. Close up each terrace spans
// many pixels, ddx() of the weights is zero on it and a spike at its edge,
// and the metre band divided by that derivative drew the terraces as
// stripes and the 2x2 pixel quads as teeth. The spline's own derivative is
// returned instead, in weight per metre, so nothing downstream has to
// differentiate the field in screen space.
#ifndef TERRAIN_WEIGHT_GRADIENTS
#define TERRAIN_WEIGHT_GRADIENTS
static bool gWeightGradients = false;
static float4 gWeightDX0 = 0, gWeightDY0 = 0;
static float2 gWeightDX1 = 0, gWeightDY1 = 0;
#endif
float4 detailFieldsLoad(int2 t, int level, int plane)
{
    const int4 at = int4(t, plane, 0);
    if (level == 0) return detailFields4.Load(at);
    if (level == 1) return detailFields8.Load(at);
    if (level == 2) return detailFields16.Load(at);
    return detailFields64.Load(at);
}
struct PageWeights {
    float4 w0; float2 w1;
    float4 dx0, dy0; float2 dx1, dy1;
};
PageWeights pageWeightsCubic(float2 p, int wanted)
{
    PageWeights r = (PageWeights)0;
    r.w0 = float4(0, 0, 1, 0);
    // The wanted level where this page has it, else the next coarser one it
    // does: the warped lookup can land in a neighbour page that lacks the
    // level, and a missing page used to read as solid sand.
    int level = kPageDatasets - 1;
    float4 entry = 0;
    float2 pageCorner = 0;
    [unroll] for (int l = 0; l < kPageDatasets; ++l) {
        if (l < wanted || entry.z != 0.0) continue;
        float2 corner;
        const float4 e = detailEntryAt(p, l, corner);
        if (e.z != 0.0) { entry = e; level = l; pageCorner = corner; }
    }
    if (entry.z == 0.0) return r;
    const float step = pageStepOf(level);
    // uv = (texel + 0.5) / size and uvPerMetre = 1 / (step * size): the
    // page's first interior texel, as an integer.
    const float2 size = 1.0 / (entry.zw * step);
    const int2 origin = int2(round(entry.xy * size - 0.5));
    // Texel centres sit at page-local multiples of the step.
    const float2 local = (p - pageCorner) / step;
    const float2 base = floor(local), f = local - base;
    const float2 f2 = f * f, f3 = f2 * f, g = 1.0 - f;
    const float2 b[4] = {g * g * g / 6.0, (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0,
                         (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, f3 / 6.0};
    const float2 d[4] = {-0.5 * g * g, 1.5 * f2 - 2.0 * f, 0.5 + f - 1.5 * f2, 0.5 * f2};
    const int2 corner = origin + int2(base) - 1;
    r.w0 = 0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 t = corner + int2(i, j);
            const float4 a = detailFieldsLoad(t, level, 1);
            const float2 c = detailFieldsLoad(t, level, 2).xy;
            const float k = b[i].x * b[j].y, kx = d[i].x * b[j].y, ky = b[i].x * d[j].y;
            r.w0 += a * k;  r.dx0 += a * kx; r.dy0 += a * ky;
            r.w1 += c * k;  r.dx1 += c * kx; r.dy1 += c * ky;
        }
    }
    const float perMetre = 1.0 / step;
    r.dx0 *= perMetre; r.dy0 *= perMetre; r.dx1 *= perMetre; r.dy1 *= perMetre;
    return r;
}

float2 pageWeightWarp(float2 p, float step)
{
    return float2(noiseAt(p / (step * 2.3)) - 0.5, noiseAt(p / (step * 2.3) + 17.3) - 0.5) * step * 1.3 +
           float2(noiseAt(p / (step * 0.9) + 5.1) - 0.5, noiseAt(p / (step * 0.9) + 41.7) - 0.5) * step * 0.35;
}

// The finest data level resident at p. Borders read from it rather than from
// the level the patch happens to be drawn at: a patch changing level (zoom,
// morph, a neighbour refining) then never moves a material border or the
// waterline. Only streaming a finer page in can change it, once.
int finestResidentLevel(float2 p)
{
    [unroll] for (int level = 0; level < kPageDatasets; ++level) {
        float2 origin;
        if (detailEntryAt(p, level, origin).z != 0.0) return level;
    }
    return (int)ringState.z;
}

// Which levels a border reads near p, and how far toward the coarser one.
// Pages are resident at different finest levels; switching level at the page
// line itself was a straight seam every 512 m. Within `band` metres of an
// edge whose neighbour is coarser, the reading blends to that neighbour's
// level, reaching it exactly at the edge - where the neighbour, which has
// nothing coarser around it, reads the same level. Continuous both sides.
struct BorderLevels { int fine; int coarse; float toCoarse; };
BorderLevels borderLevels(float2 p)
{
    BorderLevels b;
    b.fine = finestResidentLevel(p);
    b.coarse = b.fine;
    b.toCoarse = 0.0;
    const float metres = pageMetresOf(b.fine);
    // Wide enough for the page it is in: 48 m on the 512 m pages, a tenth of
    // the page on the coarse ones - 48 m of an 8 km page was a seam.
    const float band = max(48.0, metres * 0.094);
    const float2 local = p - floor(p / metres) * metres;
    const float4 distance = float4(metres - local.x, local.x, metres - local.y, local.y);
    const float2 direction[4] = {float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1)};
    [unroll] for (int k = 0; k < 4; ++k) {
        if (distance[k] >= band) continue;
        const int other = finestResidentLevel(p + direction[k] * (distance[k] + 1.0));
        if (other > b.fine) {
            b.coarse = max(b.coarse, other);
            b.toCoarse = max(b.toCoarse, 1.0 - smoothstep(0.0, band, distance[k]));
        }
    }
    return b;
}

void pageBlurredWeights(float2 p, PageDetail centre, out float4 weights0, out float2 weights1)
{
    const BorderLevels levels = borderLevels(p);
    // The lookup is warped by smooth noise so the tie line is an organic
    // curve that no longer follows the texel grid. The warp is the SAME at
    // every data level (it used to scale with the level's step): a zoom that
    // changes level, or the morph between two, must not move every border
    // by metres - that was the swimming of transitions with zoom.
    // Except on the coarse datasets, whose texels are 256 m and 1024 m: an
    // 8 m warp is nothing there, and the borders drew the texel grid. Those
    // take half their own step - fixed by what is resident, not by the zoom.
    const float dataStepHere = pageStepOf(levels.fine);
    const float warpStep = dataStepHere > 64.0 ? dataStepHere * 0.5 : 8.0;
    const float2 warp = pageWeightWarp(p, warpStep);
    const float2 q = p + warp;
    // The warp's Jacobian, by a world-space difference well above float
    // resolution: the warp stretches the field by up to about 2x, and the
    // slope that sizes the blend band has to be the slope on the ground.
    const float h = warpStep * 0.15;
    const float2 jx = float2(1, 0) + (pageWeightWarp(p + float2(h, 0), warpStep) - warp) / h;
    const float2 jy = float2(0, 1) + (pageWeightWarp(p + float2(0, h), warpStep) - warp) / h;
    PageWeights r = pageWeightsCubic(q, levels.fine);
    [branch] if (levels.toCoarse > 0.001) {
        const PageWeights c = pageWeightsCubic(q, levels.coarse);
        const float t = levels.toCoarse;
        r.w0 = lerp(r.w0, c.w0, t);   r.w1 = lerp(r.w1, c.w1, t);
        r.dx0 = lerp(r.dx0, c.dx0, t); r.dy0 = lerp(r.dy0, c.dy0, t);
        r.dx1 = lerp(r.dx1, c.dx1, t); r.dy1 = lerp(r.dy1, c.dy1, t);
    }
    weights0 = r.w0; weights1 = r.w1;
    // Chain rule through the warp: d/dpx = dq/dpx . grad_q.
    gWeightGradients = true;
    gWeightDX0 = r.dx0 * jx.x + r.dy0 * jx.y; gWeightDY0 = r.dx0 * jy.x + r.dy0 * jy.y;
    gWeightDX1 = r.dx1 * jx.x + r.dy1 * jx.y; gWeightDY1 = r.dx1 * jy.x + r.dy1 * jy.y;
}

// Height-only reads for the shading stencil: never fetch water/material planes
// at every neighbour. Its radius belongs to the DATA, not the triangle grid.
// `origin` is the page's corner in metres (detailEntryAt).
float detailBedInPage(float2 p, int level, float2 origin, float4 entry)
{
    if (entry.z == 0.0) return -60.0;
    const float2 uv = entry.xy + (p - origin) * entry.zw;
    float h;
    if (level == 0) h = detailHeight4.SampleLevel(detailHeight4Sampler, uv, 0);
    else if (level == 1) h = detailHeight8.SampleLevel(detailHeight8Sampler, uv, 0);
    else if (level == 2) h = detailHeight16.SampleLevel(detailHeight16Sampler, uv, 0);
    else h = detailHeight64.SampleLevel(detailHeight64Sampler, uv, 0);
    const float2 bands = (detailFields(uv, level, 0).xy * 65535.0 - 32768.0) * 0.01;
    return ringWindow.z + h * ringWindow.w + bands.x + bands.y;
}

float detailBedAt(float2 p, int level)
{
    float2 origin;
    const float4 entry = detailEntryAt(p, level, origin);
    return detailBedInPage(p, level, origin, entry);
}
float detailStencilBed(float2 p, int level, float2 origin, float4 entry)
{
    // Never extend a cached mapping over a page boundary: its neighbour can
    // be absent or occupy a completely different atlas slot.
    const float metres = pageMetresOf(level);
    if (all(floor(p / metres) * metres == origin)) return detailBedInPage(p, level, origin, entry);
    return detailBedAt(p, level);
}

// xy = height gradient, z = normalized openness (positive at a crest).
float3 pageShapeAt(float2 p, int level)
{
    const float step = pageStepOf(level);
    float2 page;
    const float4 entry = detailEntryAt(p, level, page);
    const float left = detailStencilBed(p - float2(step, 0), level, page, entry);
    const float right = detailStencilBed(p + float2(step, 0), level, page, entry);
    const float down = detailStencilBed(p - float2(0, step), level, page, entry);
    const float up = detailStencilBed(p + float2(0, step), level, page, entry);
    const float around = detailStencilBed(p - float2(2.0 * step, 0), level, page, entry) +
        detailStencilBed(p + float2(2.0 * step, 0), level, page, entry) +
        detailStencilBed(p - float2(0, 2.0 * step), level, page, entry) +
        detailStencilBed(p + float2(0, 2.0 * step), level, page, entry);
    return float3(float2(right - left, up - down) / (2.0 * step),
        (detailBedInPage(p, level, page, entry) - around * 0.25) / (4.0 * step));
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

// The waterline, drawn at one fixed step.
//
// Water depth is head minus bed. Read bilinearly at the patch's own data level
// its zero line is a polygon along that level's texel grid - 4 to 64 m
// squares, the teeth along every coast and bank - and it jumps to another
// polygon whenever the patch changes level, which is the shore sliding about
// as the ground refines. Here the depth always comes from the finest level
// resident at the point (whatever the patch is drawn at), reconstructed with
// an interpolating Catmull-Rom cubic from exact texel loads: the line passes
// through the same samples the data has, is smooth between them, and does
// not move with LOD. Each texel's depth is clamped to a few metres first so a
// deep channel or a high bank cannot overshoot the curve near the edge; far
// from the edge the plain value is kept for depth-dependent shading.
float detailHeightLoad(int2 t, int level)
{
    const int3 at = int3(t, 0);
    if (level == 0) return detailHeight4.Load(at);
    if (level == 1) return detailHeight8.Load(at);
    if (level == 2) return detailHeight16.Load(at);
    return detailHeight64.Load(at);
}
// Catmull-Rom depth at one level; `ok` false where the page lacks it.
float shoreDepthLevel(float2 p, int level, out bool ok)
{
    ok = false;
    const float step = pageStepOf(level);
    float2 page;
    const float4 entry = detailEntryAt(p, level, page);
    if (entry.z == 0.0) return 0.0;
    ok = true;
    const float2 size = 1.0 / (entry.zw * step);
    const int2 origin = int2(round(entry.xy * size - 0.5));
    const float2 local = (p - page) / step;
    const float2 base = floor(local), f = local - base;
    const float2 f2 = f * f, f3 = f2 * f;
    const float2 w[4] = {-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0,
                         -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2};
    const int2 corner = origin + int2(base) - 1;
    float depth = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 t = corner + int2(i, j);
            const float4 bands = detailFieldsLoad(t, level, 0);
            const float bed = ringWindow.z + detailHeightLoad(t, level) * ringWindow.w +
                              dot((bands.xy * 65535.0 - 32768.0) * 0.01, float2(1, 1));
            const float head = ringWindow.z + bands.z * ringWindow.w;
            depth += clamp(head - bed, -4.0, 4.0) * w[i].x * w[j].y;
        }
    }
    return depth;
}
float shoreDepthAt(float2 p, float fallback)
{
    // Only the shore band is redrawn: away from the waterline the patch's own
    // (morphing) depth is the answer, and the fixed-step curve is not asked.
    [branch] if (abs(fallback) >= 3.9) return fallback;
    // The same border rule as the materials: blend to a coarser neighbour's
    // level before its page edge, so the coast has no seam at 512 m lines.
    const BorderLevels levels = borderLevels(p);
    bool ok;
    float depth = shoreDepthLevel(p, levels.fine, ok);
    if (!ok) return fallback;
    [branch] if (levels.toCoarse > 0.001) {
        bool coarseOk;
        const float coarse = shoreDepthLevel(p, levels.coarse, coarseOk);
        if (coarseOk) depth = lerp(depth, coarse, levels.toCoarse);
    }
    // Near the edge the fixed-step curve; blending back to the patch's depth
    // before the band ends keeps deep water and dry land exactly as they were.
    return lerp(depth, fallback, smoothstep(3.0, 3.9, abs(fallback)));
}
#endif



