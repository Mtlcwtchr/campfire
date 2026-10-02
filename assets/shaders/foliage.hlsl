// Grass: one quad per blade, standing on the ground, leaning in the wind.
//
// Nothing about a blade is computed on the CPU. The instance carries where it
// stands, how big it is, its colour and one random number; the wind is four
// numbers in the scene. Everything else - which way this blade leans, how far,
// how stiffly, when it flutters - is worked out here, per vertex, per frame.
#include "world.hlsli"
#include "ground.hlsli"
#include "noise.hlsli"
#include "wind_field.hlsli"
#include "ring_reveal.hlsli"
#include "landscape_look.hlsli"
#include "weather.hlsli"
#define SHADOW_TEXTURE_SLOT t1
#define SHADOW_SAMPLER_SLOT s1
#include "shadow_field.hlsli"

Texture2DArray cardsTex : register(t0, space2);
SamplerState cardsSampler : register(s0, space2);

struct FoliageVertexIn { float2 corner : TEXCOORD0; float2 uv : TEXCOORD1; };
struct FoliageInstanceIn {
    float3 position : TEXCOORD2;
    float scale : TEXCOORD3;
    float4 tint : TEXCOORD4;
    float phase : TEXCOORD5;
    float variant : TEXCOORD6;
    float4 climate : TEXCOORD7;
};
struct FoliageOut {
    float4 position : SV_Position;
    float3 uvLayer : TEXCOORD0;
    float4 tint : TEXCOORD1;
    float2 worldXY : TEXCOORD3;
    // How far this blade is leaning, and how hard the wind is over it, for the
    // shading. A field under wind is not uniformly green: the fronts read as
    // bands of light and shade travelling over it, because a leaning blade turns
    // its side to the light and a blade laid right over shows its pale
    // underside. Half of what makes wind visible is this rather than the
    // movement.
    float2 lean : TEXCOORD2;
    float4 blade : TEXCOORD4; // Upward canopy normal and height along the card.
    float3 worldPosition : TEXCOORD5;
    float4 weather : TEXCOORD6;
};

// The ground flora past the six imported grass views, in the order
// tools/make_foliage_cards.py writes them (cards.json): layer 6 onwards.
static const int kCardShortGrass = 6;
static const int kCardSeedGrass = 7;
static const int kCardFlowersWhite = 8;
static const int kCardFlowersYellow = 9;
static const int kCardFlowersPurple = 10;
static const int kCardFern = 11;
static const int kCardDryGrass = 12;
static const int kCardReeds = 13;
static const int kCardUndergrowth = 14;
static const int kCardFlowersRed = 15;

// Width and height of a card against the imported meadow grass at the same
// scale. The imported views stand knee to hip high rather than at a man's
// chest: a meadow seen on foot is a field the eye crosses, not a hedge in
// front of the lens. Widths stay at or under one - the GPU cull's bounds
// (grass_cluster_prepare.hlsl) assume the imported card's width.
float2 foliageCardShape(float variant)
{
    const int layer = int(variant + 0.5);
    if (layer < kCardShortGrass) return float2(1.0, 0.70);
    if (layer == kCardShortGrass) return float2(0.90, 0.46);
    if (layer == kCardSeedGrass) return float2(0.80, 0.78);
    if (layer == kCardFern) return float2(1.00, 0.52);
    if (layer == kCardDryGrass) return float2(0.90, 0.66);
    if (layer == kCardReeds) return float2(0.70, 1.10);
    if (layer == kCardUndergrowth) return float2(0.75, 0.36);
    return float2(0.72, 0.56);   // flowers
}

// What a card's texel is as a surface. The imported views are a neutral olive
// the climate's plant colour multiplies; the generated ones are drawn in a
// temperate meadow's own colours, so only their green is moved by the climate
// (relative to that meadow) and a petal, a seed head or a cattail keeps its own.
float3 foliageCardAlbedo(float3 texel, float3 tint, float variant)
{
    if (variant < kCardShortGrass - 0.5) return texel * tint;
    const float3 temperate = float3(0.40, 0.63, 0.19);
    const float leafy = saturate((texel.g - max(texel.r, texel.b)) * 14.0);
    return texel * lerp(float3(1.0, 1.0, 1.0), tint / temperate, leafy);
}

FoliageOut FoliageVS(FoliageVertexIn vertex, FoliageInstanceIn instance)
{
    if (instance.tint.a <= 0.0) {
        FoliageOut hidden = (FoliageOut)0;
        hidden.position = float4(0, 0, 0, 1);
        return hidden; // all corners coincide: no pixel work for rejected roots
    }
    // Across the screen rather than across the world, so a blade always faces
    // the camera edge-on and never turns into a line. This is the card's width
    // only - which way it *leans* is a question about the world, below.
    const float2 right = normalize(viewProjection[0].xy);
    const float t = viewport.z;
    const float2 direction = wind.xy;
    const float2 sideways = float2(-direction.y, direction.x);

    // How much of the fine detail this instance can carry. Past LOD 0 one card
    // stands in for a clump of twenty blades, and a clump that flutters is a
    // boiling far field: the small, fast movements are exactly the ones that
    // have to go as the thing they are drawn on grows.
    const float crisp = 1.0 - smoothstep(1.3, 3.0, instance.scale);

    // Three numbers of its own, out of the one random it was given.
    //
    // Blades that share a stiffness and a beat move as a sheet, and a sheet is
    // what the eye picks out first. The instance carries one random - the sway
    // phase - so the other two are cut out of it: sixteen bits of randomness
    // spread over three uses is still sixteen bits, and there are not sixty
    // thousand blades within sight of each other.
    const float own = instance.phase;
    // Slack blades lie right over in a gust; wiry ones barely move. Nothing
    // else in the picture says as plainly that these are individual plants.
    const float stiffness = 0.70 + 0.80 * frac(own * 7.31);
    const float beat = frac(own * 3.77) * 6.2831853;

    // A. Base sway. Never still, never in step, and independent of the weather -
    // grass moves in a dead calm. Two beats so it does not tick.
    const float sway = sin(own + t * 1.15) * 0.10 + sin(beat + t * 0.61) * 0.06;

    // B. The gust field: where the wind is, and the front crossing this ground.
    const float gust = windGust(instance.position.xy, direction, t, wind.w, crisp);
    const float front = windFront(instance.position.xy, direction, t, crisp);

    // What the blade does about it. Stiffness divides, so the same front lays
    // one blade flat and only nods its neighbour. The strength of the wind
    // scales what the wind does and not the sway: at wind nought the meadow
    // should go quiet, not freeze solid.
    const float bend = (windLean(gust, front) * wind.z + sway) / stiffness;
    // A little across the wind as well, or the whole field is one hinge.
    const float drift = sin(beat * 1.7 + t * 0.83) * 0.12 * crisp * wind.z;

    // C. Fine flutter: the tip only, fast, small, and only when there is wind in
    // the first place. This is what separates grass from cardboard, and it is
    // also the first thing that has to stop existing at distance.
    const float shiver = sin(t * 8.7 + beat * 2.3) * 0.075 * crisp * saturate(gust - 0.55);

    // Height-based bending. The foot is in the ground and the tip carries the
    // whole lever, so the profile is the square of the height up the card, not
    // the height: a blade that leans in a straight line from the root is a
    // windscreen wiper. The flutter is squared again on top of that - it is a
    // leaf moving, not a stem.
    const float up = vertex.corner.y;
    const float profile = up * up;
    const float tip = bend * profile + shiver * profile * up;

    // Mid-distance instances are wider clumps, never giant grass stalks.
    const float2 shape = foliageCardShape(instance.variant);
    const float height = min(instance.scale, 1.3) * 1.9 * shape.y;
    float3 p = instance.position;
#ifndef FOLIAGE_PAGES
    if (morphing.y > 0.5) p = morphed(p, instance.climate.w);
#endif
    const float lay = tip * 0.42;
    p.xy += right * (vertex.corner.x * instance.scale * shape.x) +
            direction * (lay * height) +
            sideways * (drift * profile * height * 0.16);
    // And a bent blade is a shorter blade - without this the grass stretches as
    // it leans, which is the one thing that gives a bending card away as a card.
    // The blade keeps its length, so how much height is left is what the lean
    // did not spend: a right angle of it and the tip is on the ground.
    p.z += up * height * sqrt(saturate(1.0 - lay * lay));

    FoliageOut output;
    const WeatherVertex climate=weatherVertex(instance.position,instance.climate.x,instance.climate.y,instance.climate.z);
    output.weather=climate.surface;
    output.weather.w=climate.air.x;
    output.position = project(p);
    output.uvLayer = float3(vertex.uv, instance.variant);
    output.tint = instance.tint;
    output.tint.rgb *= lerp(float3(0.95, 0.98, 0.91), float3(1.05, 1.01, 0.96), frac(own * 0.91));
    const bool perspective = dot(abs(viewProjection[3].xyz), float3(1.0, 1.0, 1.0)) > 0.0;
    // The screen-size fade is a decision: in the scene view it belongs to the
    // frozen cull camera, so flying away does not thin or fill the meadow.
    const bool frozen = cullState.y > 0.5;
    const float4x4 decider = frozen ? cullViewProjection : viewProjection;
    const float depthW = frozen ? mul(cullViewProjection, float4(p, 1.0)).w : output.position.w;
    const float pixelsPerMetre = perspective ?
        length(decider[0].xyz) * viewport.x * 0.5 / max(0.5, depthW) : camera.w;
    output.tint.a *= smoothstep(0.7, 3.0, instance.scale * pixelsPerMetre);
    // Grass reach (GraphicsSettings::foliageDistance, fog.w): the far tier
    // stops there, so it fades over the last fifth instead of ending in a line.
    if (perspective && fog.w > 0.0) output.tint.a *= 1.0 - smoothstep(0.8 * fog.w, fog.w, depthW);
    output.lean = float2(bend, gust);
    output.worldXY = instance.position.xy;
    output.blade = float4(normalize(float3(-direction * (bend * up * 0.30), 1.0)), up);
    output.worldPosition = p;
    return output;
}

float4 FoliagePS(FoliageOut input) : SV_Target0
{
    float4 texel = cardsTex.Sample(cardsSampler, input.uvLayer);
    // The mip chain averages coverage with the empty card round it, and an
    // alpha-tested clump read from a small level loses its blades: lift the
    // coverage by the level it was read at, so a far clump stays a clump.
    const float2 texels = input.uvLayer.xy * 256.0;
    const float level = 0.5 * log2(max(max(dot(ddx(texels), ddx(texels)), dot(ddy(texels), ddy(texels))), 1.0));
    texel.a = saturate(texel.a * (1.0 + level * 0.30));
    clip(texel.a - 0.08);
    // Screen-door coverage can write depth; blended unsorted cards cannot.
    const float threshold = frac(52.9829189 * frac(dot(floor(input.position.xy),
                                                 float2(0.06711056, 0.00583715))));
    clip(texel.a * input.tint.a * extraPS.z - max(0.001, threshold));
    const float n = noiseAt(input.worldXY / 19.0 + float2(4.1, -2.7));
    const float reveal = ringState.y > 0.5 ? 1.0 : saturate(ringState.x);
    clip(n + reveal * 1.15 - 0.72);
    // Leaning one way catches the light, the other way turns away from it; and a
    // blade laid right over shows its pale underside whichever way it went,
    // which is what turns a front into a shiver of light crossing the field
    // rather than movement the eye has to look for. The lull term is the other
    // half of it: ground the wind has left alone sits a shade darker.
    const float bend = input.lean.x, gust = input.lean.y;
    // Both offsets are the field's own mean, so wind shades the meadow without
    // lifting or dropping its overall colour - and so the blades match the
    // ground colour that stands in for them a step further out.
    const float light = 1.0 + (bend - 0.23) * 0.20 + (abs(bend) - 0.28) * 0.14 +
                        (gust - 1.0) * 0.05;
    const float up = saturate(input.blade.w);
    clip(up+0.08-input.weather.x*0.65);
    const float3 normal = normalize(input.blade.xyz);
    const float root = lookRootOcclusion(up);
    float3 pigment = landscapePigment(foliageCardAlbedo(texel.rgb, input.tint.rgb, input.uvLayer.z), 1.0);
    pigment=weatherVegetation(pigment,parametersPS[0].z,input.weather.y,input.weather.w);
    pigment=lerp(pigment,float3(0.76,0.80,0.82),
                 wxSmooth(0.01,0.25,input.weather.x)*wxSmooth(0.45,1.0,up)*0.8);
    const float shadow = proceduralShadow(input.worldPosition,normal);
    float3 lit = pigment * landscapeDaylight(normal, root, shadow) * light;
    // Broad transmission, not a shiny rim: thin leaf tips let warm light through.
    const float backlight = pow(saturate(dot(-landscapeSun(), landscapeEye(input.worldPosition)) * 0.5 + 0.5), 3.0);
    lit += pigment * float3(1.06, 1.0, 0.70) * (backlight * up * 0.16) * shadow;
    lit *= lerp(0.80, 1.0, smoothstep(0.0, 0.65, up));
    return float4(landscapeFinish(lit, input.worldPosition), sceneDepthAlpha(input.worldPosition));
}
