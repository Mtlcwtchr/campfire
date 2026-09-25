// The water: the ground's own triangles, lifted to the level the water stands
// at. How deep it is at a vertex is already in the vertex, so there is no second
// mesh and nothing to keep in step.
//
// What makes water look like water is not its colour, it is that every part of
// it faces a slightly different way and each of those ways catches the light
// differently. So the surface is a shape here rather than a wash: a long swell
// worked out from the wind, two painted normal maps scrolling past each other
// on top of it, and the light of this world reflected off the sum of the three.
// The colour underneath all that is two numbers.
#include "ground.hlsli"
#include "climate_field.hlsli"
#include "noise.hlsli"
#include "ring_reveal.hlsli"
#include "shore_motion.hlsli"
#include "water_motion.hlsli"
#include "landscape_look.hlsli"
#include "weather.hlsli"
#include "water_body.hlsli"
#include "ice_surface.hlsli"

// The surface, as four layers of one array (tools/bake_water.py and
// tools/bake_foam_residue.py):
//   0  ripple  the fine grain the wind drags across the top
//   1  swell   the broad cells the whole surface has
//   2  foam    the white net that gathers at the shore, in the alpha channel
//   3  residue independent PHX porous/bubble mask left after water retreats
// RGB is a normal, alpha is height - except on the foam layer, where alpha is
// the mask. No light is baked into any of them: water moves, so the light has to
// be worked out where the ripple is.
Texture2DArray waterTex : register(t0, space2);
SamplerState waterSampler : register(s0, space2);

struct WaterIn {
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
    // The last field of the layout the water shares with the terrain.
    //
    // It used to be five: foliage, desert cover, exposure and travel, the six
    // environment numbers and the four geography ones - declared here even
    // though the water read only three of them, because the compiler hands a
    // SPIR-V location out per field in declaration order and does not read the
    // number off the TEXCOORD. Skipping the ones it had no use for slid
    // environment and geography down onto foliage and desert cover, and the
    // water spent every frame reading a vegetation weight as its air
    // temperature. A thermal of nought is -21 C, which is why every river on
    // the map was under ice in midsummer.
    //
    // The three it does read are climate, and climate is one field over the
    // world now rather than eleven floats a corner, so it reads them where the
    // vertex stands - and there is nothing left to declare and not use.
    float2 relief : TEXCOORD11;   // wind exposure, openness
};
struct WaterOut {
    float4 position : SV_Position;
    float2 worldXY : TEXCOORD0;
    float depth : TEXCOORD1;
    float cover : TEXCOORD2;
    float lift : TEXCOORD3;
    // Undisplaced water head; a sloping bed does not make a lake a waterfall.
    float baseLevel : TEXCOORD4;
    float worldHeight : TEXCOORD5;
    float ice : TEXCOORD6;
    float4 motion : TEXCOORD7;
    nointerpolation uint explicitSurface : TEXCOORD8;
};

// The long swell, and the slope it has here.
//
// Two crossing waves travelling with the wind, with wavelengths in the hundreds
// of metres - and that is not a stylistic choice. The mesh under this is sampled
// every four metres up close and every sixty-four out wide; a wave shorter than
// a handful of vertices is not a wave on the surface, it is aliasing that
// changes shape every time the level does. The short ripples are the normal
// maps' business, and a normal map is per pixel.
//
// Worked out in both stages from the same function: the vertex stage lifts the
// surface by it and the pixel stage lights the slope of it, and if the two
// disagreed the light would sit beside the wave instead of on it. The wind and
// the clock are handed in because the two stages read them out of different
// constant buffers.
// A coordinate turned about the origin. What it is for: one painted pattern
// read at two sizes still lines its two readings up, because they are the same
// pattern square to the same axes. Turned by an angle that is not a right
// angle, the two cannot agree anywhere, and a slope read from a turned map has
// to be turned back the same way or the light on it points across the ripple.
float2 spun(float2 v, float angle)
{
    const float c = cos(angle), s = sin(angle);
    return float2(c * v.x - s * v.y, s * v.x + c * v.y);
}

float swellAt(float2 p, float2 dir, float clock, out float2 slope)
{
    const float2 across = float2(-dir.y, dir.x);
    const float along = dot(p, dir);
    const float side = dot(p, across);
    // Three components at wavelengths that are not multiples of each other, and
    // each with its phase wandering slowly over the map. Both of those are here
    // for the same reason: a sum of plain sines is periodic, and a periodic
    // surface seen from above is corduroy - evenly spaced parallel stripes with
    // a whitecap on every one of them. With the phases wandering, a crest holds
    // its line for a few hundred metres and then loses it, which is what swell
    // does.
    //
    // Wavelengths in the hundreds of metres on purpose. The mesh under this is
    // sampled every four metres up close and every sixty-four out wide; a wave
    // shorter than a handful of vertices is not a wave on the surface, it is
    // aliasing that changes shape every time the level changes. The short
    // ripples are the normal maps' business, and a normal map is per pixel.
    const float k1 = 6.2831853 / 213.0;         // radians to the metre
    const float k2 = 6.2831853 / 88.0;
    const float k3 = 6.2831853 / 151.0;
    const float wander1 = (noiseAt(p / 540.0) - 0.5) * 5.0;
    const float wander2 = (noiseAt(p / 260.0 + 19.3) - 0.5) * 4.0;
    const float a = along * k1 - clock * 0.55 + wander1;
    const float b = along * k2 * 0.86 + side * k2 * 0.42 - clock * 0.92 + wander2;
    const float c = along * k3 * 0.30 - side * k3 * 0.95 - clock * 0.41 - wander1;
    // The slope of it, for the light. The wandering is treated as standing
    // still here - its own gradient is a fifth of a wave over five hundred
    // metres and adding it would cost another two noise reads to make no
    // difference anybody can see.
    slope = dir * (cos(a) * k1 * 0.50 + cos(b) * k2 * 0.86 * 0.28 + cos(c) * k3 * 0.30 * 0.22) +
            across * (cos(b) * k2 * 0.42 * 0.28 - cos(c) * k3 * 0.95 * 0.22);
    return sin(a) * 0.50 + sin(b) * 0.28 + sin(c) * 0.22;
}

float waveComponent(float2 p, float2 direction, float wavelength, float clock,
                    float offset, float spacing, out float2 slope)
{
    const float k = 6.2831853 / wavelength;
    const float2 across = float2(-direction.y, direction.x);
    const float bend = dot(p, across) * k * 0.17 + offset;
    const float phase = dot(p, direction) * k - clock * sqrt(9.81 * k) +
                        offset + sin(bend) * 0.65;
    const float resolved = waterWaveResolution(wavelength, spacing);
    slope = waterWaveDerivative(phase) * resolved *
            (direction * k + across * (cos(bend) * k * 0.17 * 0.65));
    return waterWaveShape(phase) * resolved;
}

float waveField(float2 p, float2 direction, float clock, float spacing, out float2 slope)
{
    float2 a, b, c, d;
    const float longWave = waveComponent(p, direction, 213.0, clock, 0.4, spacing, a);
    const float crossing = waveComponent(p, spun(direction, 0.73), 88.0, clock, 2.1, spacing, b);
    const float shortWave = waveComponent(p, spun(direction, -0.41), 57.0, clock, 4.3, spacing, c);
    const float chop = waveComponent(p, spun(direction, 1.17), 32.0, clock, 1.7, spacing, d);
    slope = a * 0.65 + b * 0.38 + c * 0.22 + d * 0.12;
    return longWave * 0.65 + crossing * 0.38 + shortWave * 0.22 + chop * 0.12;
}

float3 flowingNormal(float2 uv, float2 travel, float clock)
{
    const float phase = frac(clock * 0.125);
    const float2 dx = ddx(uv), dy = ddy(uv);
    const float3 first = waterTex.SampleGrad(waterSampler,
            float3(uv - travel * (phase - 0.5), 0), dx, dy).xyz;
    const float3 second = waterTex.SampleGrad(waterSampler,
            float3(uv - travel * (frac(phase + 0.5) - 0.5), 0), dx, dy).xyz;
    return lerp(first, second, waterFlowWeight(phase)) * 2.0 - 1.0;
}

WaterOut WaterVS(WaterIn input)
{
    WaterOut output;
    output.explicitSurface = 0;
    // Ocean swash has a dry apron. Inland water stays at its hydraulic head:
    // lifting it above dry terrain produced water-shaped hills at the bank.
    const float3 bed = morphed(input.position, input.morphHeight);
    const float head = waterMorphed(input.position.xy, input.waterHeight, input.morphUv);
    const float cover = morphing.y > 0.5 ?
        lerp(input.waterCover, input.morphUv.y, terrainMorph(input.position.xy)) : input.waterCover;
    float level = head;
    output.motion=input.waterMotion;
    const float ocean=wbOcean(input.waterMotion.z,input.waterMotion.w);
    const float depth = head - bed.z;
    output.baseLevel = head;
    const SurfaceClimate place = climateAt(bed.xy);
    const WeatherVertex climate=weatherVertex(float3(bed.xy,input.waterHeight),
        place.environment.x,place.environment.z,place.geography.w);
    // Hide only the visual layer, including its wave suppression. Weather and
    // hydraulic heads stay unchanged for an apples-to-apples comparison.
    // How much of this is running water rather than standing. The river share
    // comes off the mesh with the rest of the water's motion, and the reach's
    // own fall steepens it: a millpond and a mountain stream are both "river"
    // by that flag and only one of them stays open in a frost.
    const float flowing=saturate(input.waterMotion.z)*
                        (0.55+0.45*saturate(length(input.waterMotion.xy)));
    output.ice=wxInlandIce(climate.surface.z,input.waterHeight,input.waterCover,flowing)*
               (1.0-ocean)*(1.0-saturate(parameters[1].w));

    // The swell, faded out as the water shallows. It has to be nought at the
    // waterline or the sea lifts off its own beach: a wave in a puddle is the
    // puddle, and the last metre of water is where the shore is drawn.
    float2 slope;
    const float swell = waveField(bed.xy, wind.xy, viewport.z, max(4.0, morphing.w), slope);
    // Thin flowing sheets have no room for large waves; flat deep water does,
    // regardless of how steep the submerged terrain happens to be.
    const float room = waterWaveRoom(depth, cover);
    output.lift = swell * room * waterWaveAmplitude(wind.z) * (1.0-output.ice)*
                  wbWaveScale(input.waterMotion.z,input.waterMotion.w);
    level += output.lift;
    level = lerp(level,max(level,bed.z+0.025),ocean);
    output.position = project(float3(bed.xy, level));
    output.worldHeight = level;
    output.worldXY = bed.xy;
    // Keep the sign: negative is the run-up area on dry land. The GPU vertex's
    // old clamped waterDepth would collapse that whole strip onto the shoreline.
    output.depth = depth;
    output.cover = cover;
    return output;
}

float4 WaterPS(WaterOut input) : SV_Target0
{
    const float river=saturate(input.motion.z), lake=saturate(input.motion.w);
    const float ocean=wbOcean(river,lake);
    const float2 downstream=input.motion.xy/max(length(input.motion.xy),0.0001);
    // Derive the WATER slope before discards, without animated wave heights.
    const float2 headDx=ddx(input.worldXY), headDy=ddy(input.worldXY);
    const float headDet=headDx.x*headDy.y-headDx.y*headDy.x;
    const float inverseHeadDet=abs(headDet)>1e-7 ? 1.0/headDet : 0.0;
    const float2 waterGradient=float2(ddx(input.baseLevel)*headDy.y-ddy(input.baseLevel)*headDx.y,
        headDx.x*ddy(input.baseLevel)-headDy.x*ddx(input.baseLevel))*inverseHeadDet;
    const float waterSlope=length(waterGradient);
    const float3 fall=float3(wxWaterfall(waterSlope,input.cover)*river,
                            -waterGradient/max(waterSlope,0.0001));
    const float ice=saturate(input.ice)*(1.0-fall.x)*smoothstep(0.02,0.30,input.depth);
    if (extraPS.w > 0.5) discard;
    // How much of the ground behind this fragment is actually under water. At
    // four metres to the sample it is one or nought and nothing changes; at
    // sixty-four it is the difference between a stream drawn as a stream and a
    // valley drawn as a lake.
    // Where the water ends, decided by the water rather than by the mesh.
    //
    // Three things were wrong with taking it from `cover` alone. Cover is a
    // per-vertex share of a footprint, so the line it draws runs along triangle
    // edges - a lake came out as a polygon with mitred corners. It says nothing
    // about where the bed actually crosses the surface, so the sheet ran on past
    // the waterline and its rim hung over the bank. And breaking it up with
    // noise, which fixed the first of those, tore holes right through the sheet
    // wherever cover was low - which at a strategic zoom is the whole of a
    // river, so the drainage came out as a chain of black gaps.
    //
    // The geometry already knows the answer. Both the surface and the bed are
    // planes across a triangle, so `depth` is exact at every pixel between the
    // corners, and the line where it reaches nought is the true waterline
    // whatever the mesh is sampled at. Clip on that and the edge follows the
    // ground instead of the triangulation - no polygon lakes, no overhanging
    // rim, and nothing to perforate.
    //
    // The noise stays, but only as a wobble of the waterline within a fraction
    // of a metre of depth, so it ruffles the edge without ever cutting inland
    // water in two. It is stationary in the world: the same bay is the same
    // shape from every camera and at every zoom.
    const float2 edgeAt = input.worldXY;
    const float ragged = noiseAt(edgeAt / 7.4) * 0.62 +
                         noiseAt(edgeAt / 2.3 + 13.1) * 0.38 - 0.5;

    // Cover still has the last word where the channel is narrower than the
    // square that samples it - a brook eight metres across seen at sixty-four
    // metres to the sample has no vertex inside it, and depth there is a
    // statement about the vertex, not about the brook. So the geometric test
    // is faded in as the sampling gets fine enough to mean something.
    const float sampled = max(length(ddx(input.worldXY)), length(ddy(input.worldXY)));
    const float trusted = 1.0 - smoothstep(1.5, 6.0, sampled);
    const float waterline = input.depth + ragged * 0.22;
    // Where the sampling is fine enough for depth to mean something, the shape
    // of the water is the shape of the waterline and nothing else. Cover is a
    // per-vertex number, so leaving it in charge here is what puts the staircase
    // on the edge of every lake: the outline steps from one vertex to the next
    // however exact the depth between them is. It stays in charge only at the
    // far end, where a channel narrower than its own sample has no vertex
    // inside it and depth is a statement about the vertex rather than the brook.
    const float inland = 1.0 - ocean;
    if (input.explicitSurface != 0) {
        clip(input.cover - 0.02); // skirts remain invisible
        clip(input.depth - 0.0001); // no screen-space fallback may widen the floodplain
    } else {
        clip(input.cover - lerp(0.02, -1.0, trusted * inland));
        clip(lerp(1.0, waterline, trusted * inland));
    }
    // And where the bed comes up through a surface that is standing still.
    //
    // A lake is level - that is what still water is, and tilting it to follow
    // the ground underneath would be a worse lie than the flat panel. What was
    // wrong was where the flat panel was allowed to end: at a vertex, so an
    // island or a hummock inside the basin was drowned or exposed a whole
    // sample at a time and the shoreline was a staircase of them.
    //
    // Both the surface and the bed are planes across a triangle, so their
    // difference is exact at every pixel between the corners: clipping on it
    // puts the waterline exactly where the two actually cross, whatever the
    // mesh happens to be sampled at, and that is the crisp edge rather than a
    // ramp fading over a sample.
    //
    // Only where the footprint says the whole of it is wet. Below that the
    // channel is narrower than the gap between two samples - a brook at a
    // coarse level - and a vertex sitting on dry ground beside it is expected
    // rather than contradictory; clipping there is how a river turns into a
    // dashed line. And never on the sea, whose apron is deliberately dry: the
    // swash runs up the beach above its own head.
    // (The waterline clip above has replaced the cover-gated one that used to
    // sit here: it only opened at cover 0.85 and so almost never fired.)

    const float2 p = input.worldXY;
    const float2 direction=lerp(windPS.xy,downstream,river);
    const float2 dir=direction/max(length(direction),0.0001);
    const float2 across = float2(-dir.y, dir.x);
    const float clock = viewportPS.z;
    const float blowing = 0.45 + 0.55 * windPS.z;

    // --- how the surface lies -------------------------------------------
    //
    // Four slopes added rather than four directions averaged, and the
    // difference matters: averaging two normals flattens both, adding two
    // slopes piles them up, which is what two sets of ripples on one surface
    // actually do. They scroll different ways at different speeds, which is
    // what keeps the pattern from reading as one photograph sliding across.
    float2 slope, capSlope;
    // Retain the approved irregular whitecap envelope; geometric waves and
    // their exact analytic slopes are a separate, mesh-filtered field.
    const float swell = swellAt(p, dir, clock, capSlope);
    waveField(p, dir, clock, max(4.0, ringState.w), slope);
    slope *= waterWaveRoom(input.depth, input.cover) * waterWaveAmplitude(windPS.z) *
             (1.0 - fall.x)*(1.0-ice)*wbWaveScale(river,lake);
    // How big a turn of the pattern is, in metres - and it is not a constant,
    // because the camera goes from three pixels to the metre down to half a
    // pixel. Held at ninety-odd pixels a turn: below about forty the ripples
    // alias into a diagonal hatch, and a normal map that aliases does not go
    // grey the way a colour map does - it throws white sparks, because a
    // specular highlight off a wrong direction is as bright as one off a right
    // one. That is what "the sea is made of fabric" was.
    //
    // Not quantised to doublings. Sliding the size as the zoom slides is a slow
    // breathing of the pattern while the wheel is turning and nothing at all
    // when it stops; quantising trades that for a pop at each step.
    // Two metres to the pixels-per-tile, not one: a tile is two metres across
    // on screen at a pixel to the tile, which the projection says (a metre of
    // world moves pixelsPerTile/2 pixels) and nothing else here would.
    const float metresPerPixel = landscapePerspective() ? max(0.0001, sampled) :
                                2.0 / max(cameraPS.w, 1e-4);
    const float rippleTurn = max(27.0, 95.0 * metresPerPixel);
    const float swellTurn = max(74.0, 240.0 * metresPerPixel);
    // Pushed about by a noise far longer than either turn, because a turn held
    // at a size in pixels is a turn the eye can find: out wide it is ninety
    // pixels across, and ninety pixels is small enough to see the same water
    // twice on one screen. The push is a fifth of a turn over some hundreds of
    // metres, which the eye reads as current rather than as a wobble.
    const float2 drift = float2(noiseAt(p / 620.0) - 0.5, noiseAt(p / 710.0 + 31.7) - 0.5);
    const float2 q = p + drift * rippleTurn * 0.85;
    // The scroll stays in metres a second whatever the turn is: the water has a
    // speed, and a speed measured in turns of a texture would change with the
    // zoom.
    const float2 fineUv = (q + dir * (clock * 0.58*ocean)) / rippleTurn;
    const float2 chopUv = spun(q - dir * (clock * 0.31), 2.1) / (rippleTurn * 0.43);
    const float2 broadUv = spun(q - dir * (clock * 0.62) + across * (clock * 0.30), 0.8) /
                           swellTurn;
    // Slowly varying local current bends the normal pattern rather than merely
    // sliding a flat photograph. Travel is in metres per eight-second cycle.
    const float2 current = ocean*(dir * (0.65 + 0.45 * blowing) + drift * 0.9 +
                           across * (sin(dot(p, across) / 83.0 - clock * 0.23) * 0.24))+
                           river*downstream*(0.65+min(waterSlope,0.5)*1.5)+lake*windPS.xy*0.12;
    const float3 fineN = flowingNormal(fineUv, current * 8.0 / rippleTurn, clock);
    const float3 chopN = waterTex.Sample(waterSampler, float3(chopUv, 0)).xyz * 2.0 - 1.0;
    const float3 broadN = waterTex.Sample(waterSampler, float3(broadUv, 1)).xyz * 2.0 - 1.0;
    // The long swell leads and the maps decorate it. The other way round - and
    // it was the other way round - the broad map's cells read as quilting: big
    // soft pillows all over the sea, at one size, going nowhere.
    float2 lying = -slope;
    lying += fineN.xy / max(fineN.z, 0.25) * 0.26 * blowing;
    lying += spun(chopN.xy / max(chopN.z, 0.25), -2.1) * 0.15 * blowing*ocean;
    lying += spun(broadN.xy / max(broadN.z, 0.25), -0.8) * 0.14*(1.0-river);
    lying *= (1.0-ice)*wbNormalScale(river,lake);
    const float3 surface = normalize(float3(lying, 1.0));

    // --- and where the eye is -------------------------------------------
    //
    // Out of the projection rather than written down: the third row of it is
    // the axis depth is measured along, which for a camera with no vanishing
    // point is exactly the direction it looks from. Written down as a constant
    // it would be wrong the day the view tips - and the glint on water is the
    // one thing in the picture that says which way the view is from.
    const float3 eye = landscapeEye(float3(input.worldXY, input.worldHeight));
    const float3 sun = landscapeSun();
    const float3 halfway = normalize(sun + eye);
    const float facing = saturate(dot(surface, sun));
    const float sparkle = pow(saturate(dot(surface, halfway)),
                              lerp(88.0, 36.0, smoothstep(1.0, 8.0, metresPerPixel)));
    // How much of the sky this bit of surface is showing the eye.
    //
    // This is what water is, seen from above: almost none of what reaches the
    // eye off a lake is light that went into it and came back out - it is the
    // sky, and how much of the sky depends on how far the surface has tilted
    // away from the eye. Looking nearly straight down, that is nearly nothing
    // for flat water and a lot for the side of every ripple, which is why open
    // water reads as dark with bright streaks rather than as an evenly lit
    // fabric. Lighting it with the sun instead - a diffuse term over the whole
    // surface - is exactly what made it read as wool.
    const float fresnel = pow(1.0 - saturate(dot(surface, eye)), 5.0);

    // --- how far down the bed is -----------------------------------------
    //
    // Keep the bed-visibility ramp several pixels wide. Swash/foam width uses
    // the horizontal apron below, not this depth scale.
    const float perPixel = length(float2(ddx(input.depth), ddy(input.depth)));
    const float soft = max(1.30, perPixel * 4.0);

    // --- the colour of it -------------------------------------------------
    const float deep = lookWaterDepth(input.depth);
    // Shallow water has to read as water.
    //
    // This was (0.19, 0.30, 0.26) - a grey-green within a few per cent of the
    // grass beside it. The depth ramp is exponential over three and a half
    // metres, so a brook half a metre deep sits at nine per cent of the way to
    // the deep colour and is drawn almost entirely in this one: the river was
    // the right shape, fully covered, fully opaque, and the same colour as the
    // bank. Along a reach the pools run a little deeper than the bars, and that
    // small difference was the only thing separating river from field - which
    // is what read as a dashed line at a strategic zoom.
    //
    // Turned towards the water it is standing in rather than the ground it is
    // crossing. Still muted, still the same family as the deep colour, and no
    // brighter - only unmistakably not grass.
    const float3 shallowColour = float3(0.17, 0.33, 0.36);
    const float3 deepColour = float3(0.055, 0.14, 0.18);
    float3 colour = lerp(shallowColour, deepColour, deep);
    // A restrained transmitted-light tint follows actual raised crests, not
    // a second texture pretending to be geometry.
    colour += float3(0.015, 0.055, 0.045) * saturate(input.lift) * deep;
    // The sky the surface is reflecting, strongest where it faces away from the
    // eye - which is the whole of why water is lighter at a glancing angle.
    const float3 sky = landscapeSky(reflect(-eye, surface).z);
    // Scaled by how deep it is, because that is the other half of the same
    // fact: what comes back off deep water is the sky, and what comes back off
    // a foot of water over sand is the sand. A river reflecting as hard as the
    // open sea reads as poured concrete.
    colour = lerp(colour, sky, saturate(0.04 + 0.70 * fresnel) * (0.32 + 0.68 * deep));
    const float sunshine=1.0-parametersPS[0].x*parametersPS[2].z*0.85;
    colour += float3(1.04, 1.0, 0.90) * (sparkle * blowing * 0.23 * sunshine);
    // Barely at all: water has next to no diffuse of its own, and this is here
    // only so the light in the picture agrees about which way the sun is.
    colour *= 0.94 + 0.10 * facing;

    // Inland shorelines are depth intersections, not a repeating ocean swash
    // mask. Reuse the authored foam texture only on actual rapid river reaches.
    float4 inlandResult=0;
    const float3 clearWaterColour=colour;
    const float icePixelMetres=max(length(ddx(p)),length(ddy(p)));
    [branch] if (ocean<1.0) {
        const float2 flowUv=(p-downstream*clock*0.85)/max(14.0,48.0*metresPerPixel);
        const float authoredFoam=waterTex.Sample(waterSampler,float3(flowUv,2)).w;
        const float whiteWater=fall.x*smoothstep(0.35,0.85,authoredFoam)*0.45*(1.0-ice);
        colour=lerp(colour,float3(0.83,0.87,0.86),whiteWater);
        [branch] if (ice>0.001) {
            const float3 iceColour=iceSurfaceColour(p,icePixelMetres);
            colour=lerp(colour,iceColour*landscapeDaylight(float3(0,0,1),1.0),ice);
        }
        float alpha=wbInlandAlpha(input.depth,input.cover,perPixel);
        alpha=lerp(alpha,wbIceAlpha(input.depth,input.cover,perPixel),ice);
        inlandResult=float4(landscapeFinish(colour,float3(p,input.worldHeight)),alpha);
        if (ocean<=0.0) return inlandResult;
        colour=clearWaterColour;
    }

    // --- the shore --------------------------------------------------------
    //
    // Cover is sampled over the enlarged mesh apron: half at the bank, zero
    // outside, one offshore. Use it for horizontal swash width. Taking min()
    // with depth squeezed that whole apron back into a sub-metre strip.
    // Depth remains the safety limit below, not a second shore coordinate.
    const float fromBank = input.cover * 2.0 - 1.0;
    // Only the low beach is allowed to get wet. Fade out well before either
    // the apron geometry ends or the bank rises into a cliff.
    const float support = smoothstep(0.02, 0.22, input.cover) *
                          smoothstep(-0.80, -0.05, input.depth);

    // Bend the actual swash front, not just its texture. Large lobes and smaller
    // tongues share one world-fixed shape for water, the lip and its residue.
    // Leave open water and the physical apron limits alone.
    const float pixelMetres = max(length(ddx(p)), length(ddy(p)));
    const float broadShore = (noiseAt(p / 96.0) * 2.0 - 1.0) *
                            (1.0 - smoothstep(48.0, 192.0, pixelMetres));
    const float shoreTongues = (noiseAt(p / 32.0 + 19.7) * 2.0 - 1.0) *
                              (1.0 - smoothstep(16.0, 64.0, pixelMetres));
    const float rawNotches = noiseAt(p / 7.0 + float2(43.1, 9.7));
    const float shoreNotches = (smoothstep(0.25, 0.75, rawNotches) * 2.0 - 1.0) *
                              (1.0 - smoothstep(3.5, 14.0, pixelMetres));
    const float shapedBank = surfWarpedBank(fromBank, broadShore, shoreTongues, shoreNotches);

    // Coherent wave groups, staggered along the coast rather than one global
    // sine flashing the entire shoreline. About 7--9 seconds per wave.
    const float waveTime = clock / lerp(9.0, 7.0, saturate(windPS.z)) +
                           noiseAt(p / 180.0) * 0.35;
    const float phase = frac(waveTime);
    const float runup = surfRunup(phase);
    const float strength = surfStrength(phase);
    const float residue = surfResidue(phase);
    const float waterFront = surfDryFront(phase, blowing) + 0.12;
    const float front = surfFoamFront(phase, blowing);

    // Recover the direction towards the bank from the signed-depth gradient.
    // UVs follow the advancing water, stop during the hold, then flow back.
    const float2 px = ddx(p), py = ddy(p);
    const float determinant = px.x * py.y - px.y * py.x;
    const float safeDet = abs(determinant) > 1e-6 ? determinant : 1e-6;
    float2 gradient = float2(ddx(input.depth) * py.y - ddy(input.depth) * px.y,
                             px.x * ddy(input.depth) - py.x * ddx(input.depth)) / safeDet;
    const float2 shoreward = dot(gradient, gradient) > 1e-6 ? -normalize(gradient) : dir;
    const float foamTurn = max(9.0, 42.0 * metresPerPixel);
    const float2 foamUv = (p - shoreward * (runup * 4.0)) / foamTurn;
    const float mask = waterTex.Sample(waterSampler, float3(foamUv, 2)).w;
    const float cells = waterTex.Sample(waterSampler,
            float3(spun(foamUv * 2.3, 1.1) + across * (clock * 0.018), 2)).w;
    const float foamMask = smoothstep(0.12, 0.82, mask * 0.7 + cells * 0.3);
    const float shoreFade = smoothstep(waterFront - 0.12,
                                       waterFront + lerp(0.26, 0.10, foamMask), shapedBank);
    const float frontWidth = max(0.045, fwidth(shapedBank) * 1.5);
    const float lip = 1.0 - smoothstep(frontWidth, frontWidth + 0.13,
                                     abs(shapedBank - front + (foamMask - 0.5) * 0.06));
    const float wake = smoothstep(front - frontWidth, front + frontWidth, shapedBank) *
                       (1.0 - smoothstep(front + 0.10, front + 0.50, shapedBank));
    // A deposit starts only after THIS pixel is clear of the water alpha ramp.
    // Its age is measured from that passage, not from a global wave phase.
    const float leftBehind = surfResidualFoam(waveTime, shapedBank, blowing);
    const float2 residueUv = spun(p, 0.67) / max(6.0, 31.0 * metresPerPixel);
    const float residueMask = waterTex.Sample(waterSampler, float3(residueUv, 3)).w;
    // Dense white film with porous detail, not only the sparse brightest cells.
    // Keep the pattern fixed: drain its alpha in place, without a second
    // threshold-driven erosion front travelling through the deposit.
    const float bubbles = smoothstep(0.015, 0.36, residueMask);
    const float movingFoam = strength * (lip * 0.85 + wake * 0.40) *
                             (0.25 + 0.75 * foamMask) * shoreFade;
    // Connect the fresh residue to the lip across the water alpha ramp. Blend
    // their independent patterns here, without double-brightening the overlap.
    const float contactPattern = lerp(bubbles, foamMask,
            smoothstep(waterFront - 0.155, front, shapedBank));
    const float contactFoam = surfContactFoam(phase, shapedBank, blowing) *
                              (0.55 + contactPattern * 0.45) * 0.98;
    const float residualFoam = leftBehind * (0.55 + bubbles * 0.45) * 0.98;
    const float contactJoin = surfContactJoin(phase, shapedBank, blowing);
    const float foam = saturate(surfBlendResidualFoam(contactJoin,
            max(contactFoam, movingFoam * contactJoin), residualFoam));
    // Whitecaps out in the open, where the swell is steep enough to break.
    //
    // How steep is enough wanders about over some hundreds of metres, and that
    // is not a decoration: the swell is a sum of sines, so a fixed threshold
    // breaks on every crest at once and the sea comes out as evenly spaced
    // diagonal stripes - corduroy, the same artefact the old ripples had. With
    // the threshold wandering, caps come in patches with clear water between
    // them, which is what wind on water does.
    const float breaking = 0.70 + (noiseAt(p / 240.0) - 0.5) * 0.85;
    const float caps = saturate((swell - breaking) * 3.0) * windPS.z * saturate(mask * 1.4) *
                       smoothstep(0.15, 0.8, fromBank);
    const float white = saturate(foam + caps * 0.40);
    colour = lerp(colour, float3(0.90, 0.925, 0.90), white);

    // --- white water ------------------------------------------------------
    //
    // Where the bed falls away under it, water stops being a surface. It is air
    // with water in it: white, opaque, and no kind of mirror - which is why the
    // sky term above has to be painted over here rather than turned down. A
    // waterfall that reflects the sky is a sheet of glass laid down a cliff.
    //
    // Strands drawn out five to one down the fall and scrolling down it at seven
    // metres a second. Down the fall rather than downwind: this is the only
    // water in the world whose direction is set by the ground and not by the
    // weather. Held to the pixel, because a strand thinner than a pixel does not
    // fade, it crawls - and falling water is the last place to put a crawl.
    float sheet = 0.0;
    [branch] if (fall.x > 0.004) {
        const float2 downhill = fall.yz;
        const float2 sideways = float2(-downhill.y, downhill.x);
        const float turn = max(0.55, 4.5 * metresPerPixel);
        const float run = dot(p, downhill) - clock * 7.0;
        const float over = dot(p, sideways);
        const float strands = noiseAt(float2(over / turn, run / (turn * 5.0))) * 0.62 +
                              noiseAt(float2(over / (turn * 0.34) + 11.3,
                                             run / (turn * 1.7) - 4.1)) * 0.38;
        // Not the whole face: a fall has clear water between its strands, and
        // that is most of what makes it read as falling rather than as paint.
        sheet = fall.x * smoothstep(0.30, 0.70, strands);
        colour = lerp(colour, float3(0.95, 0.975, 0.98), sheet * 0.92);
    }

    // --- and how much of the bed shows through ----------------------------
    //
    // The ripples wobble the depth the water is judged by, which is not
    // refraction - there is no scene texture to bend - but it is what
    // refraction looks like from up here: the bed swims a little under moving
    // water instead of lying still under a flat pane of it.
    const float wobble = (surface.x + surface.y) * 0.20;
    float alpha = min(saturate((input.depth + wobble) / soft), saturate(input.cover * 1.3)) *
                  (0.72 + 0.26 * deep);
    alpha = max(alpha, runup * 0.22 * (1.0 - smoothstep(0.0, 0.5, shapedBank)));
    alpha *= shoreFade;
    // Foam is not a tint on whatever is underneath, it is stuff floating on the
    // water: where there is foam, the water is that colour and not the sand's.
    alpha = max(alpha, white * 0.92);
    // Aerated water hides its bed completely.
    alpha = max(alpha, sheet * 0.95);
    // The last hand's breadth: a dark wet film on the sand, because a beach the
    // sea has just been over is darker than the beach above it.
    const float wet = residue * (1.0 - smoothstep(-0.08, 0.30, shapedBank)) * (1.0 - white);
    colour = lerp(colour, float3(0.20, 0.22, 0.21), wet * 0.55);
    alpha = max(alpha, wet * 0.34);
    // Every component vanishes inside the geometry margin, including the
    // lingering film after the water itself has retreated.
    [branch] if (ice>0.001) {
        const float3 iceColour=iceSurfaceColour(p,icePixelMetres);
        colour=lerp(colour,iceColour*landscapeDaylight(float3(0,0,1),1.0),ice);
    }
    alpha=lerp(alpha,wbIceAlpha(input.depth,input.cover,perPixel),ice);
    const float4 oceanResult=float4(landscapeFinish(colour, float3(input.worldXY, input.worldHeight)),
                                   saturate(alpha) * support);
    return lerp(inlandResult,oceanResult,smoothstep(0.0,1.0,ocean));
}
