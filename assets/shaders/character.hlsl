// A skinned character (CharacterPass): skinned on the CPU, posed and turned
// there too, so a vertex arrives as an offset from the character's origin in
// world axes. The origin itself arrives already projected (worked out in
// double on the CPU): two million metres from the world's corner a float
// world position is six centimetres coarse, and a body whose every vertex is
// rounded on its own is a crumpled one.
//
// Lit like the scene's models (landscape_look.hlsli): the same daylight, the
// same shadow field, the same haze - plus a specular lobe from the material's
// roughness and metal, because armour without a highlight is cloth.
#include "world.hlsli"
#include "landscape_look.hlsli"
#define SHADOW_TEXTURE_SLOT t3
#define SHADOW_SAMPLER_SLOT s3
#include "shadow_field.hlsli"

Texture2DArray albedoTex : register(t0, space2);
SamplerState albedoSampler : register(s0, space2);
Texture2DArray normalTex : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
Texture2DArray surfaceTex : register(t2, space2);
SamplerState surfaceSampler : register(s2, space2);

// The same sixteen floats to both stages (DrawItem::own).
cbuffer CharacterDraw : register(b1, space1)
{
    float4 originClip;    // the origin, projected
    float4 originWorld;   // the origin, world metres; w: fade (dither), 1 solid
    float4 material;      // x layer, y cut-out, z: 1 for the impostor card
    float4 card;          // impostor: right.xy, width, height
};
cbuffer CharacterDrawPS : register(b1, space3)
{
    float4 originClipPS;
    float4 originWorldPS;
    float4 materialPS;
    float4 cardPS;
};

struct CharacterIn {
    float2 uv : TEXCOORD0;
    float3 offset : TEXCOORD1;   // skinned, world axes, from the origin
    float3 normal : TEXCOORD2;
};
struct CharacterOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
};

CharacterOut CharacterVS(CharacterIn input)
{
    CharacterOut o;
    o.position = originClip + mul(viewProjection, float4(input.offset, 0.0));
    o.world = originWorld.xyz + input.offset;
    o.normal = input.normal;
    o.uv = input.uv;
    return o;
}

float4 CharacterPS(CharacterOut i, bool front : SV_IsFrontFace) : SV_Target0
{
    const float3 uvLayer = float3(i.uv, materialPS.x);
    float4 texel = albedoTex.Sample(albedoSampler, uvLayer);
    if (materialPS.y > 0.5) clip(texel.a - 0.45);
    // Dissolve in and out (first person, the card crossover): never blended.
    const float threshold = frac(52.9829189 * frac(dot(floor(i.position.xy), float2(0.06711056, 0.00583715))));
    clip(originWorldPS.w - threshold);

    // Tangent frame from the screen derivatives (as scene_models.hlsl): the
    // packer keeps no tangents for a skinned mesh, and these follow the pose.
    float3 n = normalize(i.normal);
    if (!front) n = -n;
    const float3 encoded = normalTex.Sample(normalSampler, uvLayer).xyz * 2.0 - 1.0;
    {
        const float3 dx = ddx(i.world), dy = ddy(i.world);
        const float2 ux = ddx(i.uv), uy = ddy(i.uv);
        const float det = ux.x * uy.y - ux.y * uy.x;
        const float3 t = dx * uy.y - dy * ux.y, b = dy * ux.x - dx * uy.x;
        if (abs(det) > 1e-9 && dot(t, t) > 1e-12 && dot(b, b) > 1e-12)
            n = normalize(normalize(t) * sign(det) * encoded.x + normalize(b) * sign(det) * encoded.y + n * encoded.z);
    }
    const float4 surface = surfaceTex.Sample(surfaceSampler, uvLayer);
    const float roughness = clamp(surface.r, 0.08, 1.0), metal = surface.g;

    const float3 pigment = landscapePigment(texel.rgb, 0.0);
    const float shadow = proceduralShadow(i.world, n);
    const float3 eye = landscapeEye(i.world);
    // Metal keeps some diffuse here: this look has no environment map for it
    // to reflect, and a plate that only showed a highlight would be black
    // everywhere the sun is not mirrored in it. Its colour goes into the
    // highlight and into a sky term instead.
    float3 lit = pigment * (1.0 - metal * 0.35) * landscapeDaylight(n, 1.0, shadow);
    const float3 h = normalize(landscapeSun() + eye);
    const float gloss = 2.0 / max(roughness * roughness * roughness * roughness, 0.002) - 2.0;
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), saturate(pigment * 1.7 + 0.10), metal);
    const float lobe = pow(saturate(dot(n, h)), gloss) * (gloss + 8.0) / 25.0;
    lit += f0 * min(lobe, 6.0) * saturate(dot(n, landscapeSun())) * shadow * 0.9;
    // The sky in the metal: brighter on plates that face up, a rim at grazing angles.
    const float fresnel = pow(1.0 - saturate(dot(n, eye)), 4.0);
    lit += f0 * metal * (0.18 + 0.22 * saturate(n.z * 0.5 + 0.5)) * (1.0 - roughness * 0.6);
    lit += float3(0.30, 0.33, 0.38) * fresnel * 0.18 * (1.0 - roughness * 0.7);
    return float4(landscapeFinish(lit, i.world), sceneDepthAlpha(i.world));
}

// The impostor: one card, facing the eye about the vertical, the baked view
// nearest to where the eye is (CharacterPass picks it).
struct CardIn {
    float2 corner : TEXCOORD0;   // x -1..1, y 0..1
    float2 uv : TEXCOORD1;
};
struct CardOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float2 uv : TEXCOORD1;
};
CardOut CharacterCardVS(CardIn input)
{
    CardOut o;
    const float3 offset = float3(card.xy * (input.corner.x * card.z * 0.5), input.corner.y * card.w);
    o.position = originClip + mul(viewProjection, float4(offset, 0.0));
    o.world = originWorld.xyz + offset;
    o.uv = input.uv;
    return o;
}
float4 CharacterCardPS(CardOut i) : SV_Target0
{
    const float3 uvLayer = float3(i.uv, materialPS.x);
    const float4 texel = albedoTex.Sample(albedoSampler, uvLayer);
    clip(texel.a - 0.3);
    const float threshold = frac(52.9829189 * frac(dot(floor(i.position.xy), float2(0.06711056, 0.00583715))));
    clip(originWorldPS.w - threshold);
    // The card's own normals are of the baked frame, not of this turn of it;
    // far away, lit as an upright body facing the eye is enough.
    const float3 n = normalize(landscapeEye(i.world) * 0.7 + float3(0, 0, 0.5));
    const float shadow = proceduralShadow(i.world, n);
    const float3 surface = normalTex.Sample(normalSampler, uvLayer).xyz + surfaceTex.Sample(surfaceSampler, float3(0.5, 0.5, 0)).xyz * 0.0;
    const float3 lit = landscapePigment(texel.rgb, 0.0) * landscapeDaylight(n, 0.9 + 0.1 * surface.z, shadow);
    return float4(landscapeFinish(lit, i.world), sceneDepthAlpha(i.world));
}
