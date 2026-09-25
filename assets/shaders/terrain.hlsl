// The ground: six materials mixed by weight, lit by the vertex normal.
#include "ground.hlsli"
#include "noise.hlsli"
#include "ring_reveal.hlsli"
#include "foliage_field.hlsli"
#include "wind_field.hlsli"
#include "relief.hlsli"
#include "terrain_erosion.hlsli"
#include "sand_motion.hlsli"
#include "landscape_look.hlsli"
#include "inspection.hlsli"
#include "weather.hlsli"
#include "climate_field.hlsli"

// The six materials as layers of one array, so a pixel can pick the two it is
// made of instead of sampling all six and throwing four away.
Texture2DArray groundTex : register(t0, space2);
SamplerState groundSampler : register(s0, space2);
// Page detail textures occupy t1..t9 in the page variant, so the material
// channels live after them. The mesh variant binds three contiguous samplers.
#ifdef TERRAIN_PAGE_MATERIALS
Texture2DArray groundNormalTex : register(t10, space2);
SamplerState groundNormalSampler : register(s10, space2);
Texture2DArray groundPropertiesTex : register(t11, space2);
SamplerState groundPropertiesSampler : register(s11, space2);
#else
Texture2DArray groundNormalTex : register(t1, space2);
SamplerState groundNormalSampler : register(s1, space2);
Texture2DArray groundPropertiesTex : register(t2, space2);
SamplerState groundPropertiesSampler : register(s2, space2);
#endif
#include "terrain_material.hlsli"

#ifndef TERRAIN_MATERIAL_PROBE
#ifdef TERRAIN_PAGE_MATERIALS
Texture2D coverClimate0 : register(t12, space2);
Texture2D coverClimate1 : register(t13, space2);
Texture2D coverClimate2 : register(t14, space2);
SamplerState coverSampler0 : register(s12, space2);
SamplerState coverSampler1 : register(s13, space2);
SamplerState coverSampler2 : register(s14, space2);
#else
Texture2D coverClimate0 : register(t3, space2);
Texture2D coverClimate1 : register(t4, space2);
Texture2D coverClimate2 : register(t5, space2);
SamplerState coverSampler0 : register(s3, space2);
SamplerState coverSampler1 : register(s4, space2);
SamplerState coverSampler2 : register(s5, space2);
#endif
#endif

struct TerrainIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 weights0 : TEXCOORD2;
    float2 weights1 : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float waterHeight : TEXCOORD5;
    float4 waterMotion : TEXCOORD6;
    float waterCover : TEXCOORD7;
    float morphHeight : TEXCOORD8;
    float3 morphNormal : TEXCOORD9;
    float2 morphUv : TEXCOORD10;
    // What is left of the eleven climate channels this vertex used to carry:
    // the two numbers that are not climate at all. Exposure is this point
    // against its upwind skyline and openness is it against the ground around
    // it - both properties of the height at this level, so both belong to the
    // corner. The weather itself is one field over the world and is read from
    // climate_field.hlsli where the vertex stands.
    float2 relief : TEXCOORD11;   // wind exposure, openness
};
struct TerrainOut {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float4 weights0 : TEXCOORD1;
    float2 weights1 : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float2 worldXY : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float worldHeight : TEXCOORD6;
    float4 foliage : TEXCOORD7;
    float desertCover : TEXCOORD8;
    float windExposure : TEXCOORD9;
    float4 environment : TEXCOORD10;
    float4 geography : TEXCOORD11;
    float travelCost : TEXCOORD12;
    float4 weather : TEXCOORD13; // snow metres, wetness, ice, enabled
    float3 triangleBary : TEXCOORD14;
    float4 stageDiagnostic : TEXCOORD15;
};

TerrainOut TerrainVS(TerrainIn input)
{
    TerrainOut output;
    output.triangleBary = 0.0;
    output.stageDiagnostic = 0.0;
    const float3 where = morphed(input.position, input.morphHeight);
    output.position = project(where);
    output.normal = morphedNormal(input.normal, input.morphNormal, input.position.xy);
    output.weights0 = input.weights0;
    output.weights1 = input.weights1;
    output.uv = morphedUv(input.uv, input.morphUv);
    output.worldXY = where.xy;
    output.waterDepth = waterMorphed(input.position.xy, input.waterHeight, input.morphUv) - where.z;
    output.worldHeight = where.z;
    const SurfaceClimate place = climateAt(input.position.xy);
    output.foliage = place.foliage;
    output.desertCover = place.desert;
    output.windExposure = input.relief.x;
    output.travelCost = 1.0;
    output.environment = place.environment;
    output.geography = float4(place.geography.x, place.geography.y, input.relief.y, place.geography.w);
    const WeatherVertex climate=weatherVertex(input.position,place.environment.x,place.environment.z,place.geography.w);
    output.weather=climate.surface;
    if (parameters[0].x>0.5) {
        const float baseline=18.0+(place.environment.x-0.65)*60.0-input.position.z*0.004;
        output.environment.x=(lerp(climate.air.x,baseline,table[6].z)+30.0)/80.0;
        output.environment.z=lerp(saturate(place.environment.z*0.55+climate.surface.y*0.65),place.environment.z,table[6].z);
        // Foundation reuses geography.x for immutable mud potential, not wind.
        if ((int)extra.w!=5)
            output.geography.xy*=lerp(climate.air.w/max(wind.z,0.001),1.0,table[6].z);
    }
    return output;
}

// How a material meets its neighbour comes from the scene now, not from here.
//
//   table[i].x   turns of this material's texture in one turn of the reference
//   table[i].y   how wide its border with a neighbour is, in weight
//   table[i].z   how far noise tears that border about
//   table[i].w   how big the tears are, in metres
//
// It was six triples of constants in this file, which meant that trying a
// different number meant a rebuild - and picking these by argument rather than
// by looking is how ground ends up looking argued about. They are content now
// (content/config/ground.json) and the explorer can move them while it runs.

// Smoothly cross-fade the two strongest materials near their shared border.
// Noise lets each material intrude a little into its neighbour at the border.
// Keep biome interiors crisp and reuse the same mix at both texture scales.
struct Ground {
    int top, under;
    float mix;          // nought for the second material, one for the first
    float3 colour;      // blended near read
    float4 properties;
    float3 normal;
};

// One scatter of stones: a grid of cells, each holding at most one stone,
// jittered inside its cell so the grid leaves no mark.
//
// Requirement 19 wants stones placed for geological reasons and explicitly not
// by uniform random scatter, so the density is never a constant here - every
// caller passes it in from a field that says why a stone would be there: the
// foot of a cliff, a river bed, an exposed nose, rocky ground. An empty cell
// costs two noise reads and returns.
//
// Returns the coverage mask and the height of the dome, so a stone can shade
// as a stone rather than as a stain: x lights the albedo, y bends the normal.
float2 stoneScatter(float2 worldXY, float metres, float density, float salt, float pixel)
{
    const float2 cell = worldXY / metres;
    const float2 id = floor(cell);
    // hashAt, not noiseAt. Smooth noise is correlated between neighbouring
    // cells by construction - that is what makes it smooth - so picking cells
    // with it seats the stones in drifts and rows, and the first attempt drew
    // exactly that: gravel in diagonal stripes. A hash gives each cell an
    // independent number, which is what a scatter needs.
    const float pick = hashAt(id + salt);
    const float present = (1.0 - smoothstep(density - 0.04, density + 0.04, pick)) *
                          saturate(density / 0.04);
    const float shape = hashAt(id + salt + 13.7);
    // Off the middle of its cell, or the grid is the pattern.
    const float2 centre = float2(0.26 + shape * 0.48,
                                 0.26 + frac(pick * 7.31) * 0.48);
    // A wide spread of sizes, weighted small. All one size is what made the
    // first version read as bubbles rather than stones - a bed of identical
    // circles is a texture of circles, whatever it is coloured like.
    const float grade = frac(shape * 5.17);
    const float radius = 0.05 + grade * grade * 0.26;
    const float2 off = frac(cell) - centre;
    // And not round. A stone seen from above is a blob: squashed along some
    // axis of its own and dented on one side. Two cheap distortions - an
    // anisotropic stretch and a lobe - cost four multiplies and remove the
    // thing the eye actually picks up, which is that every outline is a circle.
    const float turn = frac(pick * 3.77) * 6.2832;
    const float2 axis = float2(cos(turn), sin(turn));
    const float2 local = float2(dot(off, axis), dot(off, float2(-axis.y, axis.x)));
    const float squash = 0.72 + frac(shape * 2.13) * 0.55;
    const float2 shaped = float2(local.x / squash, local.y * squash);
    const float lobe = 1.0 + 0.22 * cos(turn * 2.0 + atan2(shaped.y, shaped.x) * 3.0);
    const float away = length(shaped) / (radius * lobe);
    const float footprint = pixel / max(metres * radius, 1e-4);
    const float resolved = 1.0 - smoothstep(0.35, 1.25, footprint);
    const float aa = min(footprint, 0.5);
    const float mask = (1.0 - smoothstep(0.62 - aa, 1.0 + aa, away)) * present * resolved;
    // A stone sits in the ground, not on it: the dome is shallow.
    return float2(mask, saturate(1.0 - away * away) * mask * metres * radius * 0.16);
}

// What kind of edge one material makes against another.
//
// Requirement 10 asks for classes of transition mask - organic soft, organic
// sharp, broken erosion, patchy cellular, narrow ragged - rather than one mask
// everywhere. The reason is not variety for its own sake: two materials meet
// the way the process that put them there leaves them. Grass creeping into
// bare soil is a soft, wide, mottled edge because it is a living front. Rock
// coming up through anything is a hard narrow one, because it is a break, and
// it is torn at a fine scale because it is a fracture. Marsh against dry
// ground is a ragged fringe at the scale of tussocks. Snow is drifted and
// therefore soft and broad, whatever it lies on.
//
// One material order per pair, so the same boundary is the same class from
// both sides. Returns: how wide the transition is against the profile's own
// width, how fine the tear is against its noise scale, and how much of the
// answer the tear is allowed to be.
//
//   Grass 0, Dirt 1, Sand 2, Rock 3, Marsh 4, Snow 5
float3 groundEdgeStyle(int a, int b)
{
    const int low = min(a, b), high = max(a, b);
    // Snow drifts over everything: broad, soft, coarse.
    if (high == 5) return float3(1.55, 2.2, 0.55);
    // Marsh against dry ground: a narrow fringe torn at the scale of tussocks.
    if (high == 4) return float3(0.55, 0.42, 1.35);
    // Rock through anything: a break, so narrow and finely fractured. Against
    // sand it is the classic pack-into-the-gaps pair, which wants the tear
    // finer still.
    if (high == 3) return low == 2 ? float3(0.50, 0.30, 1.45)
                                   : float3(0.62, 0.38, 1.30);
    // Sand against grass or soil: a drifting front, sharper than a living one
    // but not a break.
    if (high == 2) return float3(0.85, 0.80, 1.05);
    // Grass into bare soil: the soft wide mottle of a living front.
    return float3(1.60, 1.30, 0.70);
}

// Filter along the local fall line, never rotate/stretch absolute world
// coordinates by a direction that varies per pixel. That makes a curved cliff
// lip sweep through distant noise cells and turn material borders into bands.
float groundEdgeNoise(float2 worldXY, float2 downhill, float metres, float alongness)
{
    if (alongness <= 0.001) return noiseAt(worldXY / metres);
    return reliefStreak(worldXY, downhill, metres, metres * alongness * 1.6, 1);
}

// The same, told what a pixel covers. For the octave whose cell can be smaller
// than one.
float groundEdgeNoise(float2 worldXY, float2 downhill, float metres, float alongness,
                      float pixel)
{
    if (alongness <= 0.001) return filteredNoiseAt(worldXY / metres, pixel / metres);
    return reliefStreak(worldXY, downhill, metres, metres * alongness * 1.6, 1, pixel);
}

Ground groundHere(float4 weights0, float2 weights1, float3 worldPos, float3 normal)
{
    const float total = dot(max(weights0, 0.0), float4(1, 1, 1, 1)) +
                        dot(max(weights1, 0.0), float2(1, 1));
    weights0 = max(weights0, 0.0) / max(total, 1e-5);
    weights1 = max(weights1, 0.0) / max(total, 1e-5);
    if (total < 1e-5) weights0.x = 1.0;
    const float2 worldXY = worldPos.xy;
    const float2 downhill = normalize(normal.xy + float2(1e-5, 1e-5));
    // Warp measured in metres, not texture turns. Never scale it with zoom.
    const float3 p = worldPos + float3(domainWarp(worldXY / 47.0) * 0.35, 0.0);
    const float3 dx = ddx(p), dy = ddy(p);
    float w[6] = {weights0.x, weights0.y, weights0.z, weights0.w, weights1.x, weights1.y};
    Ground here;
    here.top = 0;
    here.under = 1;
    float best = -1, next = -1;
    for (int i = 0; i < 6; ++i) {
        if (w[i] > best) { next = best; here.under = here.top; best = w[i]; here.top = i; }
        else if (w[i] > next) { next = w[i]; here.under = i; }
    }

    float4 profileA = tablePS[here.top], profileB = tablePS[here.under];
    // A small minimum keeps narrow rock/sand borders visibly soft as well.
    const float3 edgeStyle = groundEdgeStyle(here.top, here.under);
    float width = min(0.90, max(0.20, 0.5 * (profileA.y + profileB.y)) * edgeStyle.x);
    const MaterialSample top = sampleGroundMaterial(here.top, p, normal, dx, dy);
    [branch] if (next <= 0.0) {
        // No neighbour, even when the editor asks for an extra-wide border.
        here.mix = 1.0;
        here.colour = top.colour;
        here.properties = top.properties;
        here.normal = top.normal;
        return here;
    }
    const MaterialSample under = sampleGroundMaterial(here.under, p, normal, dx, dy);

    const float gap = best - next;
    const float border = (1.0 - smoothstep(width, width * 2.0, gap)) *
                         smoothstep(0.0, 0.10, next);
    const float metres = max(0.25, min(profileA.w, profileB.w) * edgeStyle.y);
    // Stretched down the fall line where the edge is an erosion break.
    //
    // Requirement 10 asks for directional masks as well as organic ones, and
    // this is where the direction comes from: what strips soil off rock is
    // water running down, so the bare patches are long the way it ran and
    // short across it. An isotropic mask on the same boundary reads as a
    // sponge. Only for the fractured classes - a living grass front has no
    // direction, and neither does a drift of snow.
    const float alongness = saturate((1.35 - edgeStyle.y) * 1.6);
    // Metre-scale border structure is independent of the texture's mip and
    // camera footprint: the BROAD octave carries the shape of the boundary, and
    // that shape must not change with zoom or the border crawls as you move in.
    //
    // The fine octave is a different claim. `metres` bottoms out at a quarter
    // of a metre, so on a narrow rock-through-soil border this is a 0.25 m
    // pattern - under a pixel at anything but the closest zoom, and material
    // borders sit on exactly the slopes and cliff tops where the ripple was
    // reported. Fading it to its own mean is not a zoom-dependent tear: the
    // boundary stays where the broad octave puts it, and only detail that was
    // never resolvable stops being asked for.
    const float footprint = max(length(dx.xy), length(dy.xy));
    const float fine = groundEdgeNoise(worldXY, downhill, metres, alongness,
                                       footprint) * 2.0 - 1.0;
    const float broad = groundEdgeNoise(worldXY, downhill, metres * 4.0, alongness) * 2.0 - 1.0;
    // The tear is capped against the transition's own width, or it would swing
    // the boundary further than the boundary is wide and read as two materials
    // interleaved. But the cap has to scale with how torn the class is meant to
    // be: the narrow classes - marsh against dry ground, rock through soil - are
    // narrow *and* ragged, and capping their tear at the same fraction of a
    // much smaller width left them narrow and straight. A straight material
    // boundary follows the mesh, so marsh met grass along a visible line of
    // triangle edges.
    const float amount = min(max(profileA.z, profileB.z) * edgeStyle.z,
                             width * 0.95 * max(1.0, edgeStyle.z));
    // A fixed material order makes the noise change sign when top/under swap.
    // Otherwise the same boundary would jump to a different colour at equal weights.
    const float orientation = here.top < here.under ? 1.0 : -1.0;
    const float intrusion = (broad * 0.65 + fine * 0.35) * amount * border * orientation;

    here.mix = materialBorderMix(gap, width, intrusion,
                                 top.borderHeight - under.borderHeight, border);
    here.colour = lerp(under.colour, top.colour, here.mix);
    here.properties = lerp(under.properties, top.properties, here.mix);
    here.normal = normalize(lerp(under.normal, top.normal, here.mix));
    return here;
}

// Ground that is standing in for a level not yet cut. Same in every way but the
// depth it writes.
TerrainOut TerrainBackdropVS(TerrainIn input)
{
    TerrainOut output = TerrainVS(input);
    output.position = projectBehind(morphed(input.position, input.morphHeight));
    return output;
}

float4 TerrainPS(TerrainOut input) : SV_Target0
{
#ifndef TERRAIN_PAGE_MATERIALS
    if (extraPS.w > 0.5 && ringReserved.x > 0.5) {
        float stripe=step(0.5,frac((input.position.x+input.position.y)/16.0));
        return float4(lerp(float3(0.19,0.19,0.23),float3(0.31,0.31,0.35),stripe),1.0);
    }
#endif
    const float3 worldPos = float3(input.worldXY, input.worldHeight);

    // The shore, as a band of ground rather than a line where the water stops.
    //
    // Requirement 8 asks for deep water, shallow water, wet shoreline, a sand or
    // mud band, dry bank and a vegetation fringe, and what was here was two of
    // them: water, and then grass with a dark wet rim a few centimetres wide.
    // Read from the height above the water rather than from a distance to it -
    // the same thing on any bank worth the name, and it is already carried on
    // the vertex, where a distance is not. Capillary rise works this way round
    // too: what makes a beach is how far the ground is above the water table,
    // not how far it is from the edge in plan.
    //
    // The width of the band is itself noisy in world space, so the sand does not
    // arrive at a contour line. Two octaves, stationary, the same trick the
    // water's own edge uses - a tiling mask would put the shore on a grid.
    const float aboveWater = -input.waterDepth;
    // How much ground one pixel covers. Wanted this early because everything
    // below that tears a boundary with noise has to be told what it may still
    // resolve; a tear finer than this is not a tear, it is a shimmer.
    const float pixel = max(length(ddx(input.worldXY)), length(ddy(input.worldXY)));
    const float shoreBreak = fbmAt(input.worldXY / 19.0 +
                                   domainWarp(input.worldXY / 19.0) * 0.35) * 0.62 +
                             filteredNoiseAt(input.worldXY / 5.7 + 31.4, pixel / 5.7) * 0.38;
    const float bankWidth = 1.1 + shoreBreak * 2.6;
    const float shoreBand = 1.0 - smoothstep(-0.4, bankWidth, aboveWater);
    // Not up a cliff and not on a wall: a beach is flat ground at the water.
    const float shoreFlat = smoothstep(0.55, 0.88, normalize(input.normal).z);
    // Curvature of the ground over eight metres, carried on the vertex. What a
    // break of slope is, and - requirement 6 - what decides where material
    // collects and where it is stripped.
    const float bend = extraPS.w > 0.5 ? 0.0 : input.geography.z;
    // The tear on that curvature, and the one place in this shader where an
    // unfiltered noise was doing real damage.
    //
    // Its job is to keep the hollow/nose boundary off the mesh: the ramps below
    // are saturate(bend * 16), so they turn over a curvature band of about a
    // sixteenth, and this moves the boundary by a seventh of that band. That is
    // a boundary tear and it is correct.
    //
    // What was wrong is that it was a 1.6 m pattern asked for at every zoom,
    // with no gate of any kind - the relief below at least fades out with
    // `detail`, this did not. Past about a metre to the pixel neighbouring
    // pixels were sampling uncorrelated cells of it, so the hollow/nose
    // boundary - which is to say every break of slope, every rise, every cliff
    // top in the view - crawled. That is the ripple: not a pattern on the
    // slopes, the sampling of a pattern too fine to sample.
    const float bendTear = (filteredNoiseAt(input.worldXY / 1.6 + float2(52.3, 17.9),
                                            pixel / 1.6) - 0.5) * 0.018;
    const float steepness = 1.0 - normalize(input.normal).z;

    // Erosion and deposition, requirement 6: "in concave areas strengthen the
    // accumulation of moisture, soil and vegetation; on convex and exposed
    // areas expose rock more often".
    //
    // Both directions off the one number, and it is the right number: a hollow
    // is where water slows and drops what it was carrying, and a nose is where
    // it speeds up and takes the soil with it. Weights rather than a repaint,
    // so everything downstream - the material blend, the meadow, what a walker
    // feels - agrees about it. Modest: this is meant to read as the ground
    // having a history, not as green stripes down every gully.
    const float hollow = saturate(-(bend + bendTear) * 16.0);
    const float nose = saturate((bend + bendTear) * 16.0);
    float4 shoreWeights0 = input.weights0;
    float2 shoreWeights1 = input.weights1;
    [branch] if (shoreBand > 0.002) {
        const float sandy = shoreBand * shoreFlat;
        // Under the water it is mud, above it sand: the bed of a channel is not
        // a beach, and drawing beach sand under two metres of water is what made
        // the bottom read as bright as the bank.
        const float drowned = smoothstep(0.0, 0.9, input.waterDepth);
        shoreWeights0.z = saturate(shoreWeights0.z + sandy * (1.0 - drowned) * 0.90);
        shoreWeights1.x = saturate(shoreWeights1.x + sandy * drowned * 0.75);
        shoreWeights0.x *= 1.0 - sandy * 0.92;   // grass gives way at the water
        shoreWeights0.y *= 1.0 - sandy * 0.45;   // and so, less, does bare soil
    }
    // Applied after the shore band, which is a stronger statement about the
    // same ground: a beach is a beach whether or not it sits in a hollow.
    shoreWeights0.x = saturate(shoreWeights0.x * (1.0 + hollow * 0.45) *
                                                 (1.0 - nose * 0.30));   // grass
    shoreWeights0.y = saturate(shoreWeights0.y * (1.0 + hollow * 0.30));  // soil collects
    shoreWeights0.w = saturate(shoreWeights0.w * (1.0 - hollow * 0.35) +
                               nose * steepness * 0.35);                  // rock comes through
    const float3 slopeNormal = normalize(input.normal);
    Ground ground = groundHere(shoreWeights0, shoreWeights1, worldPos, slopeNormal);
    float3 colour = ground.colour;
    // Gentle world-anchored variation, never a magnified second albedo sample.
    colour *= 1.0 + (macroNoise(input.worldXY) - 0.5) * 0.06;
    colour *= lerp(float3(1.01, 1.0, 0.99), float3(0.99, 1.0, 1.01),
                   smoothstep(100.0, 1400.0, input.worldHeight));
    const float grassCover = materialCoverage(0, ground.top, ground.under, ground.mix);
    const float rockCover = materialCoverage(3, ground.top, ground.under, ground.mix);

    const float3 sun = landscapeSun();
    float3 normal = normalize(input.normal);

    // Cuts where the ground breaks: grooves down the fall line, and the same
    // mask fingering the stone/soil border along the top of a slope.
    //
    // How much of it survives is decided by the size of a pixel on the ground
    // and nothing else - not by the LOD of the mesh, which says how finely the
    // *shape* is carried and not how close the eye is to it. In practice that is
    // the top half of the zoom.
    //
    // Derivatives can differ across a curved surface; derivative operations
    // below run outside per-pixel branches, even when their effect is faded.
    // Sub-metre work, so it lives on the near half of a zoom that runs from 0.9
    // to 96 pixels a metre. `pixel` is the world footprint of one screen pixel:
    // for this projection that is 2.83 / pixelsPerMetre, so eight centimetres at
    // the default zoom of 34 and three metres at the far end. Full detail above
    // about ten pixels a metre, gone below three and a half - by which point the
    // finest streak is under a pixel and would fizz rather than fade.
    const float detail = 1.0 - smoothstep(0.28, 0.80, pixel);
    // Keep derivatives outside divergent detail/material branches.
    Relief relief = reliefAt(worldPos, normal, detail, pixel);
    relief.cut *= rockCover;

    // Cliff geometry may change with LOD, but it must not select a new albedo.
    // The old normal-based mask repainted grass and soil as rock on coarse
    // faces because the coarse normal is different.
    // The cuts decide where the border runs and not only what is drawn on it.
    // Stone reaches up a rib, soil hangs down between them, and the top of a
    // slope becomes a fingered line instead of a contour. That is what a terrain
    // alpha mask in a landscape tool is for, and on a heightfield sampled every
    // four metres it is the part of this big enough to see from an RTS camera:
    // the face is one triangle wide in plan, the border along it is metres long
    // and right under the eye.
    // A cliff is a structure, not a material on a steep slope.
    //
    // Requirement 5 names the parts: top, lip, face, foot, talus. What tells
    // them apart is not steepness - a face and the ground above it are equally
    // steep where they meet - but the *break* of slope, which is curvature.
    // Convex is ground turning over, concave is ground landing.
    //
    // Taken from the vertex (geography.z), not from the interpolated normal.
    // The screen-space curvature is constant inside a triangle, so a break of
    // slope comes out as a stripe one triangle wide - no use for telling a lip
    // from a face. The carried one is the vertex against the eight around it
    // two steps out, and it interpolates into a band of ground.
    //
    // Bands from the measured distribution: on steep ground |curvature| runs
    // p50 0.014 and p90 0.053 on seed 11, so five to forty-five thousandths
    // opens on most of a real break and on almost nothing else. Torn with a
    // metre-scale noise so the eight-metre lattice underneath does not draw
    // its own edges - the same trick the material borders use.
    // The lip: where a field stops being a field and starts being a drop. Bare
    // and broken, because it is what the weather reaches first.
    const float cliffLip = smoothstep(0.10, 0.34, steepness) *
                           smoothstep(0.005, 0.045, bend + bendTear);
    // The foot: the concave landing. Not on the face, which is straight, and
    // not out on the flat, where nothing has fallen.
    const float cliffFoot = smoothstep(0.05, 0.20, steepness) *
                            (1.0 - smoothstep(0.26, 0.50, steepness)) *
                            smoothstep(0.005, 0.045, -bend + bendTear);

    // table.x is relative to the 14-metre reference turn. Three projections
    // avoid the infinitely stretched top-down UVs of a near-vertical face.
    // `colour` is the material blend resolved from stable world-space weights.
    float3 groundColour = colour;
    const float sandSupport = sandDriftSupport(input.desertCover, input.weights0.z,
            input.weights0.w, input.weights0.x, input.weights1.y, input.weights1.x,
            input.waterDepth, normal.z);
    // Loose sand has its own micro relief, not the soil's metre-deep cuts.
    relief.cut *= 1.0 - sandSupport;

    float3 shadingNormal = ground.normal;
    // The geometric normal is left alone: the triplanar blend and the meadow both
    // ask which way this ground faces, and neither wants an answer that changes
    // every three metres. Only the light sees the cut surface.
    shadingNormal = reliefNormal(shadingNormal, worldPos, relief.cut, 0.10);
#ifdef TERRAIN_PAGE_MATERIALS
    // Final-stage erosion evidence comes from the source, not triangle slope.
    // Keep the virtual channels out of water, snow, sand and diagnostic views.
    ErosionRelief erosion = erosionRelief(input.worldXY, normal, pixel, input.stageDiagnostic.y);
    const float erosionSupport = smoothstep(0.0,3.0,aboveWater) *
        (1.0-sandSupport) * (1.0-saturate(input.weights1.y)) *
        (extraPS.w < 0.5 && ringState.y < 3.0 ? 1.0 : 0.0);
    shadingNormal = reliefNormal(shadingNormal,worldPos,erosion.depth*erosionSupport,0.65);
    groundColour *= 1.0-erosion.cut*erosionSupport*0.12;
#endif

    // Normal maps already contain the scan's relief. Differentiating the
    // displacement as well doubled it, and screen XY was not a world gradient.
    // Ground cover is persistent at ALL distances, never a distant-only repaint.
    // The material itself is never replaced by a triplanar rock projection.
    // This keeps a cliff's identity identical to the flat ground at every LOD.

    // The lip, and the talus under it - requirement 5's sequence, flat grass ->
    // broken rocky lip -> cliff face -> scree -> soil. Both read off the same
    // curvature that separated the zones, so they arrive wherever a break of
    // slope does and nowhere else; nothing is placed.
    // Lip and scree remain available as masks for small stone detail below,
    // but they do not repaint the substrate with a second material.
    float sandCover = 0.0;
    float sandRoughness = 0.86;
    [branch] if (sandSupport > 0.001 && pixel < 1.8 && dot(windPS.xy, windPS.xy) > 1e-6) {
        const float2 direction = normalize(windPS.xy);
        const float2 across = float2(-direction.y, direction.x);
        const float alongMetres = dot(input.worldXY, direction);
        const float acrossMetres = dot(input.worldXY, across);
        // A stationary patch mask breaks up the response without sliding the
        // substrate. Terrain horizons suppress transport behind ridges/banks.
        const float patch = smoothstep(0.20, 0.70, noiseAt(input.worldXY / 7.0));
        const float exposure = saturate(input.windExposure);
        sandCover = sandSupport * (0.40 + 0.60 * patch) *
                    (1.0 - smoothstep(1.2, 1.8, pixel));
        const SandRipple ripple = sandRipple(alongMetres, acrossMetres, viewportPS.z,
                                               windPS.z * exposure, pixel);
        const float2 slopeXY = direction * ripple.alongSlope + across * ripple.acrossSlope;
        const float3 gradient = float3(slopeXY, 0.0);
        const float3 tangentGradient = gradient - normal * dot(gradient, normal);
        shadingNormal = normalize(shadingNormal - tangentGradient * sandCover);
        sandRoughness = ripple.roughness;
        const float drift = sandDriftOpacity(alongMetres, acrossMetres,
                viewportPS.z, windPS.z, windPS.w, pixel);
        groundColour *= 1.0 + drift * sandCover * exposure * 0.08;
        sandRoughness = lerp(sandRoughness, 0.96, drift * exposure);
    }
    // Restrained scree on EXPOSED rock only. Sand is not evidence of a river,
    // and a small residual rock weight is not permission to cover a meadow.
    const float exposed = stoneSupport(rockCover, grassCover,
        materialCoverage(2, ground.top, ground.under, ground.mix),
        materialCoverage(5, ground.top, ground.under, ground.mix),
        materialCoverage(4, ground.top, ground.under, ground.mix));
    const float patch = smoothstep(0.45, 0.72, noiseAt(input.worldXY / 13.0 + 37.1));
    const float washed = shoreBand * shoreFlat * smoothstep(0.0, 0.8, input.waterDepth);
    const float2 scree = stoneScatter(input.worldXY, 0.85,
        exposed * patch * (cliffFoot * 0.18 + cliffLip * 0.06), 3.1, pixel);
    const float2 gravel = stoneScatter(input.worldXY, 1.7,
        exposed * patch * washed * 0.12, 21.7, pixel);
    // No extra rock-colour overlay or fake multi-metre boulders. The substrate
    // already contains stones; just let occasional fragments catch the light.
    shadingNormal = reliefNormal(shadingNormal, worldPos, scree.y + gravel.y, 0.35);

    // Ground clutter - requirement 11, "remove the feeling of a clean texture".
    //
    // The stones above are one class of it; these are the rest. Same rule as
    // the stones and for the same reason: nothing is scattered uniformly, every
    // density comes from a field that says why the thing would be lying there.
    // None of it is an entity - it is a few reads of noise and a tint, which is
    // what the requirement asks for explicitly.
    //
    // Under the same `detail` gate: clutter smaller than a pixel is not clutter,
    // it is a dirty texture, which is the thing this is meant to cure.
    float clutterHeight = 0.0;
    [branch] if (detail > 0.001) {
        const float grassy = grassCover;
        // Litter under vegetation: leaves, twigs, the debris of things growing
        // and dropping. Where the meadow is, patchy rather than even, and it
        // takes the ground's own colour down rather than adding a new one -
        // leaf litter is the same palette as what shed it.
        const float2 litter = stoneScatter(input.worldXY, 0.55,
                grassy * 0.20 * saturate(input.foliage.z + input.foliage.w + 0.3), 91.3, pixel);
        groundColour *= 1.0 - litter.x * 0.06;
        // Tussocks: grass grows in clumps on damp ground, not as a lawn. A
        // lighter, drier crown on a small dome, which is what a tussock is from
        // above.
        const float2 tussock = stoneScatter(input.worldXY, 0.9,
                grassy * (0.10 + 0.15 * hollow), 57.7, pixel);
        groundColour *= 1.0 + tussock.x * 0.04;
        // Mud: standing water leaves it, so it goes where the ground is damp and
        // flat - the floor of a hollow, the back of a bank - and nowhere on a
        // slope, because mud does not stay on one.
        const float2 mud = stoneScatter(input.worldXY, 2.2,
                hollow * (1.0 - smoothstep(0.06, 0.22, steepness)) * 0.20, 33.1, pixel);
        groundColour *= 1.0 - mud.x * 0.06;
        // The strand line: shells and driftwood, only on a shore, and only just
        // above the water where the last tide left them.
        const float strand = shoreBand * (1.0 - smoothstep(0.0, 0.9, input.waterDepth)) *
                             saturate(shoreWeights0.z * 1.5);
        const float2 flotsam = stoneScatter(input.worldXY, 1.4, strand * 0.12, 77.5, pixel);
        groundColour *= 1.0 + flotsam.x * 0.04;
        // All of it sits on the ground rather than in it.
        clutterHeight = (litter.y * 0.4 + tussock.y + flotsam.y * 0.6) * detail;
    }
    shadingNormal = reliefNormal(shadingNormal, worldPos, clutterHeight, 0.20);

    float4 communityWeights = input.foliage;
    float moisture = input.environment.z, forestField = 0.0;
#ifndef TERRAIN_MATERIAL_PROBE
    // Vertex-interpolated climate loses forest boundaries on coarse triangles.
    const float2 coverUv = input.worldXY*parametersPS[20].xy+parametersPS[20].zw;
    communityWeights = coverClimate0.SampleLevel(coverSampler0,coverUv,0);
    moisture = coverClimate1.SampleLevel(coverSampler1,coverUv,0).w;
    forestField = coverClimate2.SampleLevel(coverSampler2,coverUv,0).w;
#endif
    float hierarchyDensity = 0.0;
    [unroll] for (int region = 0; region < 8; ++region) {
        const float4 density = vegetationDensityPS[region];
        if (density.z > 0.0 && density.w > 0.0) {
            const float distanceToRegion = distance(input.worldXY, density.xy);
            hierarchyDensity = max(hierarchyDensity,
                density.w * (1.0 - smoothstep(density.z * 0.68, density.z,
                                               distanceToRegion)));
        }
    }
    const float coverField = foliageField(input.worldXY.x,input.worldXY.y,32.0);
    VegetationCover vegetation = vegetationCover(
        materialCoverage(0,ground.top,ground.under,ground.mix),
        materialCoverage(1,ground.top,ground.under,ground.mix),
        materialCoverage(2,ground.top,ground.under,ground.mix),rockCover,
        materialCoverage(4,ground.top,ground.under,ground.mix),
        materialCoverage(5,ground.top,ground.under,ground.mix),
        communityWeights.x,communityWeights.y,communityWeights.z,communityWeights.w,
        moisture,input.worldHeight,input.waterDepth,normal.z,forestField,coverField);
    // Very-far instance nodes do not submit triangles. They increase the
    // terrain's integrated canopy/ground-density signal inside their spatial
    // regions, so the last representation remains continuous with the object
    // hierarchy instead of dropping vegetation at the mesh cutoff.
    vegetation.canopy = max(vegetation.canopy, hierarchyDensity * 0.72);
    vegetation.grass = max(vegetation.grass, hierarchyDensity * 0.42);
    const float luminance = dot(groundColour,float3(0.2126,0.7152,0.0722));
    groundColour = float3(
        vegetationGroundChannel(groundColour.r,vegetation.red,luminance,vegetation.ground,vegetation.canopy),
        vegetationGroundChannel(groundColour.g,vegetation.green,luminance,vegetation.ground,vegetation.canopy),
        vegetationGroundChannel(groundColour.b,vegetation.blue,luminance,vegetation.ground,vegetation.canopy));
    const float livingCover = max(grassCover,vegetation.ground);
    groundColour = landscapePigment(groundColour, livingCover);
    const float weatherWet=input.weather.y*input.weather.w;
    groundColour=lerp(groundColour,weatherVegetation(groundColour,parametersPS[0].z,weatherWet,
                     input.environment.x*80.0-30.0),livingCover);
    const float snow=wxSnowMask(input.weather.x,normal.z,input.waterDepth,
                               noiseAt(input.worldXY/11.0))*input.weather.w;
    const float3 worldDx = ddx(worldPos), worldDy = ddy(worldPos);
    float4 surfaceProperties = ground.properties;
    [branch] if (snow > 0.001) {
        const MaterialSample snowSurface = sampleGroundMaterial(5, worldPos, normal, worldDx, worldDy);
        groundColour = lerp(groundColour, snowSurface.colour, snow);
        surfaceProperties = lerp(surfaceProperties, snowSurface.properties, snow);
    }
    shadingNormal=normalize(lerp(shadingNormal,normal,snow*0.8));
    sandCover*=1.0-snow;
    // And a hollow holds its water after a nose has shed it - the third of the
    // three things requirement 6 says a concavity accumulates, after soil and
    // vegetation. Bounded: this darkens a dell, it does not paint a puddle.
    const float wetness = max(max(lookWetness(input.waterDepth, normal.z), weatherWet),
                              hollow * 0.35) *
                          (1.0-max(input.weights1.y,snow));
    groundColour *= 1.0 - wetness * 0.18;

    // Stylized roughness microvariation. The current renderer is deliberately
    // diffuse-first, so this modulates the small broad grain lobe below rather
    // than pretending to be a full metallic/BRDF path.
    const float roughNoise = filteredMaterialNoise(input.worldXY / 0.7, pixel / 0.7);
    const float materialRoughness = clamp(surfaceProperties.g + (roughNoise - 0.5) * 0.06 -
                                          wetness * 0.18, 0.38, 0.98);
    // AO removes ambient light only. Do not multiply photographed albedo by
    // another black cavity layer or fade AO away with camera distance.
    const float skyVisibility = lerp(1.0, surfaceProperties.r, 0.38) *
                                (1.0 - saturate(relief.cut) * 0.20);
    float3 lit = groundColour * landscapeDaylight(shadingNormal, skyVisibility);
    // A weak, broad dry-grain lobe: roughness really affects lighting, without
    // wet-looking glitter. Perspective uses the eye-to-surface direction.
    const float3 eye = landscapeEye(float3(input.worldXY, input.worldHeight));
    const float roughness = lerp(materialRoughness, sandRoughness, sandCover);
    const float lobe = pow(saturate(dot(shadingNormal, normalize(sun + eye))),
                           lerp(48.0, 5.0, roughness));
    const float sunVisibility = saturate(dot(shadingNormal, sun)) *
        (1.0 - saturate(parametersPS[2].z * parametersPS[0].x) * 0.78);
    // Quiet dielectric highlight for every material, not just animated sand.
    lit += float3(1.0, 0.97, 0.90) * (0.045 * (1.0 - roughness) * lobe * sunVisibility);
    float3 finished = landscapeFinish(lit, float3(input.worldXY, input.worldHeight));
    // The map goes on last, over finished ground rather than instead of it.
    //
    // A flat wash would take the hills out along with the material, and the
    // reading would float over a shape that is no longer there. So the layer is
    // modulated by what it covers: the terrain's own light comes back up through
    // the colour, a slope still reads as a slope, and a coast is still a coast.
    // The ramp keeps its meaning because the modulation is a scale and not a
    // second hue - equal readings stay equal in colour, lighter or darker only
    // by how the ground beneath them is lit.
    [branch] if (extraPS.w > 0.5) {
        // The water pass is suppressed under a map rather than washing it out,
        // so the sea would otherwise arrive as bare seabed. Sink it first: a
        // coastline is half of what makes any of these readable.
        finished = lerp(finished, float3(0.05, 0.09, 0.15),
                        smoothstep(0.0, 2.5, input.waterDepth) * 0.72);
        const float4 map = inspectionColour(input.environment, input.geography, normal,
                input.waterDepth, input.worldHeight, input.worldXY, input.travelCost);
        // Bounded either side: dark ground must not swallow the reading, and
        // snow must not push the ramp past white, which would clip one channel
        // before the others and quietly change the colour's meaning.
        const float ground = dot(finished, float3(0.30, 0.59, 0.11));
        const float3 layer = saturate(map.rgb * lerp(0.68, 1.12, saturate(ground)));
        finished = lerp(finished, layer, map.a);
    }
    return float4(finished, 1.0);
}
