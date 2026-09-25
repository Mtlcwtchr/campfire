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
    const float amount = (1.0 - exp2(-d * density / 8500.0)) * 0.55;
    return amount < 0.32 ? amount : 0.32;
}

inline float lookWaterDepth(float depth)
{
    return 1.0 - exp2(-(depth > 0.0 ? depth : 0.0) / 3.5);
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
    return normalize(float3(-0.55, -0.55, 0.63));
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

float3 landscapeDaylight(float3 normal, float occlusion)
{
    const float cloud=parametersPS[2].z*parametersPS[0].x;
    const float skyView = saturate(normal.z * 0.5 + 0.5);
    const float3 ambient = lerp(float3(0.25, 0.29, 0.33), float3(0.36, 0.40, 0.44), skyView);
    const float3 bounce = float3(0.055, 0.042, 0.026) * (1.0 - saturate(normal.z));
    // Occlusion removes sky light, not the sunlight falling on an exposed lip.
    return (ambient * (1.0+cloud*0.12) + bounce) * saturate(occlusion) +
            lerp(float3(1.09, 1.02, 0.88),float3(0.85,0.91,1.0),cloud) *
            (0.78 * saturate(dot(normal, landscapeSun())) * (1.0-cloud*0.78));
}

float3 landscapeSky(float up)
{
    return lerp(lerp(float3(0.68, 0.71, 0.70), float3(0.43, 0.55, 0.66), saturate(up)),
                float3(0.49,0.54,0.59),parametersPS[2].z*parametersPS[0].x);
}

float3 landscapeFinish(float3 colour, float3 worldPosition)
{
    colour = max(colour, 0.0);
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
    return lerp(colour, float3(0.59, 0.65, 0.69), haze);
}
#else
} // namespace world::look
#endif
#endif

