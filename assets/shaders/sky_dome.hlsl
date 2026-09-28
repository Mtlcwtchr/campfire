// The sky: what the distance fog fades into, and where the clouds live.
//
// One triangle over the screen at the far plane, drawn after the opaque world
// with a LESS_EQUAL depth test and no depth write, so it lands only where
// nothing was drawn. The sky itself is the analytic atmosphere of
// landscape_look.hlsli (the same colour the fog uses along the same ray),
// with a high cirrus dome from the GoodSky pack (UE) and the volumetric
// cloud deck of cloud_field.hlsli marched in world space.
#include "landscape_look.hlsli"

#ifndef SKY_NO_TEXTURES
Texture2D cloudDome : register(t0, space2);
SamplerState cloudDomeSampler : register(s0, space2);
#endif

struct SkyOut { float4 position : SV_Position; float2 ndc : TEXCOORD0; };

SkyOut SkyVS(uint id : SV_VertexID)
{
    SkyOut o;
    const float2 p = float2(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0);
    o.position = float4(p, 1.0, 1.0);
    o.ndc = p;
    return o;
}

// The world direction through a pixel: the ray d with
// (row0 - x*row3).d = 0 and (row1 - y*row3).d = 0, in front of the eye.
float3 skyRay(float2 ndc)
{
    const float3 r0 = viewProjectionPS[0].xyz, r1 = viewProjectionPS[1].xyz, r3 = viewProjectionPS[3].xyz;
    float3 d = cross(r0 - ndc.x * r3, r1 - ndc.y * r3);
    if (dot(d, r3) < 0.0) d = -d;
    return normalize(d);
}

float3 sunDisk(float3 d)
{
    const float c = saturate(dot(d, landscapeSun()));
    const float overcast = parametersPS[2].z * parametersPS[0].x;
    const float3 sunlight = landscapeAtmosphere(landscapeSun()) * 0.4 + float3(0.9, 0.85, 0.75);
    return sunlight * (smoothstep(0.99985, 0.99995, c) * 1.4 + pow(c, 600.0) * 0.25) * (1.0 - overcast * 0.9);
}

float skyHash(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }

float3 skyColour(float3 d, float2 pixel, bool dome)
{
    float3 sky = landscapeSkyRay(d);
    float cover = 1.0; // how much of the sun disk shows through
#ifndef SKY_NO_TEXTURES
    if (dome && d.z > 0.0) {
        // Cirrus far above the deck, drifting slowly with the wind.
        const float2 uv = 0.5 + d.xy / (1.0 + d.z) * 0.5 + windPS.xy * viewportPS.z * 0.00002;
        const float cirrus = cloudDome.SampleLevel(cloudDomeSampler, uv, 0).r;
        const float amount = saturate(cirrus * 1.2) * smoothstep(0.02, 0.25, d.z) * 0.55;
        const float3 lit = lerp(landscapeSkyRay(float3(0, 0, 1)) + 0.25, float3(1.0, 0.97, 0.92),
                                0.5 + 0.5 * dot(d, landscapeSun()));
        sky = lerp(sky, lit, amount);
        cover *= 1.0 - amount;
    }
#endif
    const int steps = (int)cloudsPS.w;
    if (steps > 0 && d.z > 0.0) {
        const float sunScale = lookPS.w > 0.5 ? lookPS.x : 1.0;
        const float3 sunColour = (landscapeAtmosphere(landscapeSun()) * 0.3 + float3(0.85, 0.8, 0.7)) * sunScale;
        const float3 ambient = lerp(landscapeSkyRay(float3(0, 0, 1)), landscapeSkyRay(d), 0.5) * 0.9;
        const float4 cloud = cloudsAlong(cameraPS.xyz, d, 60000.0, steps, skyHash(pixel + frac(viewportPS.z)),
                                         landscapeSun(), sunColour, ambient, true);
        // Distant deck melts into the horizon like the world does.
        const float distance = max(0.0, (cloudLayer().base - cameraPS.z) / max(d.z, 0.02));
        const float fade = saturate(distance / 55000.0);
        sky = lerp(sky * cloud.a + cloud.rgb, sky, fade * 0.7);
        cover *= lerp(cloud.a, 1.0, fade * 0.7);
    }
    return sky * (lookPS.w > 0.5 ? lookPS.z : 1.0) + sunDisk(d) * cover;
}

float4 SkyPlainPS(SkyOut input) : SV_Target0
{
    return float4(skyColour(skyRay(input.ndc), input.position.xy, false), 1.0);
}

#ifndef SKY_NO_TEXTURES
float4 SkyPS(SkyOut input) : SV_Target0
{
    return float4(skyColour(skyRay(input.ndc), input.position.xy, skyHorizonPS.w > 0.5), 1.0);
}
#endif

