// Trees from the edge of the placed objects to the horizon.
//
// Placed objects (meshes near, impostors past the mesh line) come from the CPU
// scatter and stop at the object reach; streaming and gathering them further
// is the cost that does not scale. Past it every tree is a few pixels tall, and
// what a far hillside needs is not those exact trees but a forest of the same
// kind in the same places. So nothing is placed on the CPU at all: one draw
// runs a fixed grid of candidate cells in rings around the eye, each ring twice
// the spacing of the one inside it (so every ring costs the same and the
// screen density stays roughly even), and the vertex stage decides per cell
// whether a tree stands there - from the same climate field and ground weights
// the terrain draws, and a forest mass/clearing field of the same scales the
// CPU scatter uses - how tall, which species, and which of the baked impostor
// views faces the eye. A cell that holds no tree collapses to nothing.
//
// Rings overlap by a band and hand over by dithered coverage, and the first
// ring fades in over the band where the placed objects fade out.
#include "world.hlsli"
#include "ground.hlsli"
#include "noise.hlsli"
#include "landscape_look.hlsli"
#include "climate_field.hlsli"
#include "terrain_pages.hlsli"
#define SHADOW_TEXTURE_SLOT t1
#define SHADOW_SAMPLER_SLOT s1
#include "shadow_field.hlsli"

Texture2DArray treeAtlas : register(t0, space2);
SamplerState treeSampler : register(s0, space2);

// The per-draw block (PatchVertex, 16 floats) carries the page window in
// morphWindow/morphSettings as every page draw does; the two vectors the page
// helpers do not read carry this pass's own numbers:
//   morphing          = start (m), base spacing (m), cells per ring side, rings
//   morphReplacement  = eye x, y, z, hand-over band (m)
//   morphSettings     = page table size (x, y), ground data level (z)
// Species: layer 0..7 broadleaf views, 8..15 conifer views.
static const float2 kTreeSize[2] = {float2(15.11, 26.79), float2(11.77, 29.77)};

struct FarTreeOut {
    float4 position : SV_Position;
    float3 uvLayer : TEXCOORD0;
    float3 world : TEXCOORD1;
    float coverage : TEXCOORD2;
    float3 tint : TEXCOORD3;
    float crown : TEXCOORD4;   // the tree's own number for landscapeCrownTint
};

float farHash(float2 cell, float salt)
{
    return hashAt(cell * 1.0 + salt * 37.0);
}

// The CPU scatter's forest mass: two broad octaves, clearings and groves.
float farForestMass(float2 p)
{
    const float mass = 0.7 * noiseAt(p / 512.0 + 11.3) + 0.3 * noiseAt(p / 192.0 - 7.9);
    const float clearing = smoothstep(0.58, 0.79, noiseAt(p / 80.0 + 3.7));
    const float grove = 0.72 + 0.4 * noiseAt(p / 32.0 + 29.1);
    return saturate(smoothstep(0.43, 0.66, mass) * (1.0 - 0.97 * clearing) * grove);
}

FarTreeOut hidden()
{
    FarTreeOut o = (FarTreeOut)0;
    o.position = float4(0, 0, 0, 1);
    return o;
}

FarTreeOut FarTreesVS(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    const float start = morphing.x, baseSpacing = morphing.y;
    const uint side = (uint)morphing.z, rings = (uint)morphing.w;
    const float3 eye = morphReplacement.xyz;
    const float band = morphReplacement.w;
    const uint perRing = side * side;
    const uint ring = instance / perRing;
    if (ring >= rings) return hidden();
    const uint local = instance - ring * perRing;
    const float spacing = baseSpacing * exp2((float)ring);
    const float half = (float)side * spacing * 0.5;
    // Snapped to twice the spacing so the grid is stable as the eye moves.
    const float2 origin = floor(eye.xy / (2.0 * spacing)) * (2.0 * spacing) - half;
    const float2 cell = origin + float2((float)(local % side), (float)(local / side)) * spacing;
    const float2 cellId = floor(cell / spacing + 0.5);
    const float2 p = cell + (0.15 + 0.7 * float2(farHash(cellId, 1.0 + ring), farHash(cellId, 2.0 + ring))) * spacing;
    const float distance = length(p - eye.xy);
    // Ring k owns [inner, outer); neighbours overlap by `band` and hand over.
    const float inner = ring == 0 ? start : start * exp2((float)ring);
    const float outer = start * exp2((float)ring + 1.0);
    const float fadeIn = ring == 0 ? smoothstep(start - band, start, distance)
                                   : smoothstep(inner - band, inner, distance);
    const float fadeOut = ring + 1 == rings ? 1.0 - smoothstep(fog.x * 0.92, fog.x, distance)
                                            : 1.0 - smoothstep(outer - band, outer, distance);
    const float presence = fadeIn * fadeOut;
    if (presence <= 0.001 || (fog.z > 0.5 && distance > fog.x)) return hidden();
    // Cheap sideways cull before any texture is read: six vertices run per
    // cell, and most cells of a ring are behind or beside the eye. The height
    // is only a guess (the middle of the page window), so the margin is wide.
    {
        const float4 guess = project(float3(p, morphWindow.z + morphWindow.w * 0.5));
        if (guess.w <= -spacing || abs(guess.x) > abs(guess.w) * 1.4 + 60.0) return hidden();
    }

    // The ground: the coarsest data resident here (far pages are H64).
    int level = 3;
    float4 address = pageAddress(p, 3);
    if (address.z == 0.0) { level = 2; address = pageAddress(p, 2); }
    if (address.z == 0.0) return hidden();
    const float4 w0 = pageFields(address.xy, level, 1);
    const float4 plane2 = pageFields(address.xy, level, 2);
    const float2 w1 = plane2.xy;
    const float bed = pageHeight(p, level);
    const float head = morphWindow.z + pageFields(address.xy, level, 0).z * morphWindow.w;
    if (head > bed - 0.3) return hidden(); // water
    // And not on a shore texel either: z is the share of it under water. At
    // 64 m a texel that is partly lake draws its waterline somewhere inside
    // it, and a tree there stands in the drawn water.
    if (plane2.z > 0.04) return hidden();

    // Where a wood stands: the climate's woodland, on soil (not sand, bare
    // rock, snow or bog), in the forest mass and not in its clearings, thinner
    // on poor soil - the terms the CPU ecology multiplies.
    const SurfaceClimate climate = climateAt(p);
    const float total = max(dot(w0, float4(1, 1, 1, 1)) + w1.x + w1.y, 1e-4);
    const float soil = saturate((w0.x + w0.y) / total) *
                       (1.0 - saturate((w0.z + w0.w * 1.2 + w1.y + w1.x * 0.7) / total));
    const float canopy = saturate(climate.forest) * soil * farForestMass(p) *
                         (0.35 + 0.65 * saturate(climate.environment.y));
    // One tree per 8 m cell at full canopy, as the CPU scatter: a coarser cell
    // is occupied more often and its tree drawn larger for what it stands for.
    const float expected = canopy * 0.94 * (spacing * spacing) / 64.0;
    const float occupied = saturate(expected);
    if (farHash(cellId, 3.0 + ring) >= occupied) return hidden();
    const float grow = clamp(sqrt(expected / max(occupied, 1e-4)), 1.0, 1.8);

    const int species = farHash(cellId, 4.0 + ring) < saturate(climate.foliage.y + 0.18) ? 1 : 0;
    const float scale = (0.75 + 0.65 * farHash(cellId, 5.0 + ring)) * grow;
    const float2 size = kTreeSize[species] * scale;
    const float yaw = farHash(cellId, 6.0 + ring) * 6.2831853;

    // Standing card, turned to the eye about the vertical.
    float2 toEye = eye.xy - p;
    toEye = dot(toEye, toEye) > 1e-6 ? normalize(toEye) : float2(0, 1);
    const float2 right = float2(-toEye.y, toEye.x);
    // Too small to cover a pixel: not worth a quad.
    const float4 baseClip = project(float3(p, bed));
    const float4 topClip = project(float3(p, bed + size.y));
    const float pixels = abs(topClip.y / max(topClip.w, 1e-3) - baseClip.y / max(baseClip.w, 1e-3)) * viewport.y * 0.5;
    if (pixels < 0.35 || baseClip.w <= 0.0) return hidden();
    if (abs(baseClip.x) > baseClip.w * 1.3 + size.x || baseClip.y > baseClip.w * 1.3 + size.y) return hidden();

    // One triangle per tree, not two: the card is the unit square inside it
    // and the fragment stage discards the rest. Half the vertex runs.
    const float2 corners[3] = {float2(-1, 0), float2(3, 0), float2(-1, 2)};
    const float2 c = corners[vertex % 3];
    const float3 world = float3(p + right * (c.x * size.x * 0.5), bed - 0.4 + c.y * size.y);

    // The baked view facing the eye: views are k/8 of a turn from the yaw,
    // named by their screen-right direction as the placed impostors are.
    const float angle = atan2(toEye.y, toEye.x) - 1.5707963;
    const float turns = frac((angle - yaw) / 6.2831853);
    const int view = (int)floor(turns * 8.0 + 0.5) % 8;

    FarTreeOut o;
    o.position = project(world);
    o.uvLayer = float3(c.x * 0.5 + 0.5, 1.0 - c.y, (float)(species * 8 + view));
    o.world = world;
    o.coverage = presence;
    // A little variation per tree, as the placed ones carry.
    o.tint = float3(1, 1, 1) * (0.88 + 0.20 * farHash(cellId, 7.0 + ring));
    o.crown = yaw * 0.15915494;
    return o;
}

float4 FarTreesPS(FarTreeOut input) : SV_Target0
{
    const float4 texel = treeAtlas.Sample(treeSampler, input.uvLayer);
    if (any(input.uvLayer.xy < 0.0) || any(input.uvLayer.xy > 1.0)) discard;
    const float threshold = frac(52.9829189 * frac(dot(floor(input.position.xy),
                                                 float2(0.06711056, 0.00583715))));
    clip(texel.a * input.coverage - max(0.25, threshold));
    // A crown seen from afar: lit mostly from above, a little from the eye side.
    const float3 eyeDir = landscapeEye(input.world);
    const float3 normal = normalize(float3(0, 0, 0.75) + float3(eyeDir.xy, 0) * 0.45);
    const float shadow = proceduralShadow(input.world, normal);
    const float3 pigment = landscapePigment(texel.rgb * input.tint * landscapeCrownTint(input.crown, texel.rgb), 1.0);
    const float3 lit = pigment * landscapeDaylight(normal, 0.85, shadow);
    return float4(landscapeFinish(lit, input.world), sceneDepthAlpha(input.world));
}

