// Shared art direction for the existing display-referred UNORM renderer.
// This is NOT an HDR/sRGB conversion: normal maps and UI keep their contracts.
#ifndef LANDSCAPE_LOOK_HLSLI
#define LANDSCAPE_LOOK_HLSLI
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace world::look {
using std::exp2;
inline float lookSaturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
#else
#include "world.hlsli"
#include "cloud_field.hlsli"
inline float lookSaturate(float x) { return saturate(x); }
#endif

// Identity through the midtones, then a smooth shoulder instead of RGB clipping.
inline float lookHighlight(float peak)
{
    if (peak <= 0.78) return peak > 0.0 ? peak : 0.0;
    const float excess = peak - 0.78;
    return 0.78 + excess / (1.0 + excess / 0.22);
}

inline float lookHaze(float distance, float elevation)
{
    const float d = distance > 0.0 ? distance : 0.0;
    const float h = elevation > 0.0 ? elevation : 0.0;
    const float density = 0.70 + 0.30 * exp2(-h / 600.0);
    // Half of it by sixteen kilometres, not eight: the middle distance keeps
    // its colour and only the far ridges go into the blue.
    const float amount = (1.0 - exp2(-d * density / 16000.0)) * 0.60;
    return amount < 0.38 ? amount : 0.38;
}

inline float lookWaterDepth(float depth)
{
    return 1.0 - exp2(-(depth > 0.0 ? depth : 0.0) / 3.5);
}

// Distance fog: nothing before `start`, fully opaque at `end` (the draw
// distance), smooth in between so the edge of what is drawn is never a line.
inline float lookFog(float distance, float start, float end)
{
    const float span = end - start > 1.0 ? end - start : 1.0;
    const float t = lookSaturate((distance - start) / span);
    return t * t * (3.0 - 2.0 * t);
}

inline float lookWetness(float depth, float upright)
{
    const float wet = lookSaturate((depth + 0.9) / 0.9);
    const float flat = lookSaturate((upright - 0.35) / 0.55);
    return wet * wet * (3.0 - 2.0 * wet) * flat;
}

inline float lookRootOcclusion(float up)
{
    const float t = lookSaturate(up / 0.75);
    return 0.64 + 0.36 * t * t * (3.0 - 2.0 * t);
}

#ifndef __cplusplus
float3 landscapeSun()
{
    return dot(shadowSunPS.xyz,shadowSunPS.xyz)>0.5 ? shadowSunPS.xyz : normalize(float3(-0.55, -0.55, 0.63));
}

bool landscapePerspective()
{
    return dot(abs(viewProjectionPS[3].xyz), float3(1.0, 1.0, 1.0)) > 0.0;
}

float3 landscapeEye(float3 worldPosition)
{
    if (landscapePerspective()) return normalize(cameraPS.xyz - worldPosition);
    const float3 axis = viewProjectionPS[2].xyz;
    return normalize(axis.z < 0.0 ? -axis : axis);
}

float3 landscapePigment(float3 colour, float vegetation)
{
    const float grey = dot(colour, float3(0.2126, 0.7152, 0.0722));
    // Keep biome palettes distinct: temper yellow-green, don't turn it all grey.
    return lerp(float3(grey, grey, grey), colour, 0.94 - 0.12 * saturate(vegetation));
}

// One tree's leaves against its neighbours'. A wood whose every crown is the
// same green reads as a carpet from any distance; a painted one is a mosaic -
// cool deep greens, warm yellow-greens, and here and there a crown already
// turning. `random` is the tree's own number (its yaw, as a fraction of a
// turn), so the placed tree, its impostor and the far card agree. Only green
// texels move: bark and dead wood keep their colour. Brightness is kept.
float3 landscapeCrownTint(float random, float3 texel)
{
    const float r = frac(random * 7.31);
    float3 tint = lerp(float3(0.88, 0.99, 0.98), float3(1.06, 1.03, 0.86), smoothstep(0.05, 0.90, r));
    tint = lerp(tint, float3(1.24, 1.00, 0.60), smoothstep(0.935, 0.965, r));
    const float leafy = saturate((texel.g - max(texel.r, texel.b)) * 8.0);
    return lerp(float3(1.0, 1.0, 1.0), tint, leafy);
}

float3 landscapeDaylight(float3 normal, float occlusion, float visibility)
{
    const float cloud=parametersPS[2].z*parametersPS[0].x;
    // Graphics settings scale the two lights; unset (w = 0) keeps the look.
    const float sunScale = lookPS.w > 0.5 ? lookPS.x : 1.0;
    const float skyScale = lookPS.w > 0.5 ? lookPS.y : 1.0;
    const float skyView = saturate(normal.z * 0.5 + 0.5);
    // A warm sun and a sky only a little cooler: the late-afternoon storybook
    // light of a painted world (the Witcher's Velen, Clair Obscur's plates).
    // The shade is cool air over warm ground, not blue paint - a sky light
    // any bluer turned every slope facing away from the sun to slate.
    const float3 ambient = lerp(float3(0.205, 0.215, 0.235), float3(0.300, 0.330, 0.385), skyView);
    const float3 bounce = float3(0.095, 0.066, 0.034) * (1.0 - saturate(normal.z));
    // Occlusion removes sky light, not the sunlight falling on an exposed lip.
    // The sun is warm, not amber: (1.17, 1.00, 0.78) on top of the grade's own
    // gold turned every sunlit meadow orange; daylight in Velen is a warm
    // white, and the gold belongs to the evening.
    return (ambient * (1.0+cloud*0.12) + bounce) * saturate(occlusion) * skyScale +
            lerp(float3(1.12, 1.00, 0.85),float3(0.85,0.91,1.0),cloud) *
            (0.92 * saturate(dot(normal, landscapeSun())) * (1.0-cloud*0.78) * visibility) * sunScale;
}

float3 landscapeDaylight(float3 normal, float occlusion)
{
    return landscapeDaylight(normal,occlusion,1.0);
}

// The sky itself: single-scattering Rayleigh + Mie along the view ray, with
// the sunlight reddened by its own path through the air. Display-referred
// (this renderer is UNORM), so the result is exposed with 1 - exp(-x), and
// the thin zenith path gets a little more so it reads as blue, not grey.
// Tuned in .cache/graphics-20260927/atmosphere.py.
float3 landscapeAtmosphere(float3 d)
{
    const float3 sun = landscapeSun();
    const float3 betaR = float3(0.0058, 0.0135, 0.0331);
    const float3 betaM = float3(0.004, 0.004, 0.004);
    const float up = max(d.z, 0.0);
    const float depthR = 8.0 / (up + 0.1), depthM = 1.2 / (up + 0.1);
    const float sunUp = max(sun.z, 0.0);
    const float3 sunlight = exp(-(betaR * (8.0 / (sunUp + 0.1)) + betaM * (1.2 / (sunUp + 0.1))));
    const float mu = dot(d, sun);
    const float phaseR = 0.75 * (1.0 + mu * mu);
    const float g = 0.76;
    const float phaseM = (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * mu, 1e-3), 1.5) * 0.08;
    const float3 total = betaR * depthR + betaM * depthM;
    const float3 scatter = (betaR * depthR * phaseR + betaM * depthM * phaseM) / max(total, 1e-4) * (1.0 - exp(-total));
    const float3 sky = sunlight * scatter * 3.2 + float3(0.012, 0.016, 0.026);
    return 1.0 - exp(-sky * 1.05 * (1.0 + 0.9 * up));
}

// The sky along a ray, dimmed to grey by the weather's overcast. What the
// fog fades into and what the sky pass draws: they are the same colour.
float3 landscapeSkyRay(float3 d)
{
    const float overcast = parametersPS[2].z * parametersPS[0].x;
    const float3 clear = landscapeAtmosphere(d);
    const float grey = dot(clear, float3(0.3, 0.5, 0.2));
    return lerp(clear, float3(grey, grey, grey) * float3(0.86, 0.9, 0.95) + 0.06, saturate(overcast));
}

float3 landscapeSky(float up)
{
    // Azimuth-free callers (reflections): the sky side-on to the sun.
    const float3 sun = landscapeSun();
    float2 side = float2(-sun.y, sun.x);
    side = dot(side, side) > 1e-6 ? normalize(side) : float2(1, 0);
    const float z = clamp(up, -1.0, 1.0);
    return landscapeSkyRay(float3(side * sqrt(max(0.0, 1.0 - z * z)), z));
}

// How much of the fog covers a point, and what it is: the sky seen along the
// same ray, so at the draw distance a surface is exactly the sky behind it.
float landscapeFogAmount(float3 worldPosition)
{
    if (fogPS.z < 0.5 || !landscapePerspective()) return 0.0;
    return lookFog(length(worldPosition - cameraPS.xyz), fogPS.y, fogPS.x);
}

float3 landscapeFog(float3 colour, float3 worldPosition)
{
    const float amount = landscapeFogAmount(worldPosition);
    if (!(amount > 0.0)) return colour;
    const float3 ray = normalize(worldPosition - cameraPS.xyz);
    return lerp(colour, landscapeSkyRay(ray), amount);
}

// The cloud deck between the eye and a surface: mountain tops standing in
// cloud, the world seen from above the deck. Free when the stretch never
// enters the slab, which is every pixel of a view from under the clouds.
// Where along the ray the cloud march starts, as a pattern fixed to the
// ground (4 m cells, small numbers): a sine hash of absolute world metres is
// noise in 32-bit floats a few kilometres out, and it changed with every
// morph and every move - the cloud deck shimmered over the ground.
float landscapeCloudJitter(float2 xy)
{
    const float2 cell = fmod(floor(xy / 4.0), 4096.0);
    return frac(52.9829189 * frac(dot(cell, float2(0.06711056, 0.00583715))));
}

float3 landscapeClouded(float3 colour, float3 worldPosition)
{
    if (!landscapePerspective() || cloudsPS.w < 0.5) return colour;
    const float3 toSurface = worldPosition - cameraPS.xyz;
    const float far = length(toSurface);
    if (far < 1.0) return colour;
    const float3 d = toSurface / far;
    const float jitter = landscapeCloudJitter(worldPosition.xy);
    const float sunScale = lookPS.w > 0.5 ? lookPS.x : 1.0;
    const float4 cloud = cloudsAlong(cameraPS.xyz, d, far, 6, jitter, landscapeSun(),
        float3(1.05, 0.98, 0.88) * sunScale, landscapeSky(0.6) * 0.9, false);
    return colour * cloud.a + cloud.rgb * (lookPS.w > 0.5 ? lookPS.z : 1.0);
}

// The view distance of an opaque surface, written into the colour target's
// alpha (nothing blends the opaque world, so the channel is free). Water reads
// it from the copy taken before it is drawn: it is the depth its screen-space
// reflections march against and what keeps refraction from pulling in things
// in front of the water. Log-encoded, 0.5 m to 60 km in 8 bits: every step is
// about 4.7 % of the distance, which is the tolerance a reflection hit needs.
// The sky writes 1: as far as anything can be. An orthographic view has no eye
// distance to write, so its surfaces write 0 - "something opaque is here" is
// still what the open-sea sheet needs to know.
static const float kSceneDepthNear = 0.5;
// To four thousand kilometres: the whole world from as high as a camera goes.
// It was sixty, and from any higher every distance read as sixty - a water
// surface two hundred kilometres away found everything "in front of" it, and
// looked through itself at the sky.
static const float kSceneDepthLog = 22.93;   // log2(4000000 / 0.5)
float sceneDepthAlpha(float3 worldPosition)
{
    if (!landscapePerspective()) return 0.0;
    const float d = length(worldPosition - cameraPS.xyz);
    return saturate(log2(max(d, kSceneDepthNear) / kSceneDepthNear) / kSceneDepthLog);
}
float sceneDepthFromAlpha(float a)
{
    return kSceneDepthNear * exp2(a * kSceneDepthLog);
}

float3 landscapeFinish(float3 colour, float3 worldPosition)
{
    colour = max(colour, 0.0) * (lookPS.w > 0.5 ? lookPS.z : 1.0);
    const float peak = max(colour.x, max(colour.y, colour.z));
    if (peak > 0.0001) colour *= lookHighlight(peak) / peak; // Preserve hue in highlights.
    // An orthographic camera has no physical eye distance. Use its visible span
    // and signed depth relative to focus, never distance to the world origin.
    const float span = viewportPS.y * 2.0 / max(cameraPS.w, 0.01);
    const float behindFocus = -dot(worldPosition - cameraPS.xyz, landscapeEye(worldPosition));
    const float distance = landscapePerspective() ? length(worldPosition - cameraPS.xyz) :
                           max(0.0, span * 0.65 + behindFocus);
    const float weatherFog=parametersPS[0].x*parametersPS[2].y;
    const float haze = saturate(lookHaze(distance, worldPosition.z)+
                               (1.0-exp2(-distance/1800.0))*weatherFog*0.28);
    // Aerial perspective fades toward the sky seen along the same ray - the
    // same colour the distance fog and the sky pass use - not a fixed pale
    // blue. Under an overcast sky that pale band was far brighter than the
    // grey horizon above it, and drew a hard line where the ground ends.
    const float3 hazeColour = landscapePerspective()
        ? landscapeSkyRay(normalize(worldPosition - cameraPS.xyz)) : float3(0.59, 0.65, 0.69);
    float3 hazed = lerp(colour, hazeColour, haze);
    return landscapeFog(landscapeClouded(hazed, worldPosition), worldPosition);
}
#else
} // namespace world::look
#endif
#endif

