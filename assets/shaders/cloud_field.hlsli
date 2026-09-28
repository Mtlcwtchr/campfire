// The cloud layer: a real volume in world space, shared by everything that
// sees it. The sky pass marches it where nothing was drawn, landscapeFinish
// marches the stretch between the eye and every surface that crosses it
// (mountain tops in cloud, the world from above the cloud deck), and the
// ground's sunlight is attenuated through it. One density function, so the
// sky, the fog of cloud on a ridge and the shadows on the plain all agree.
//
// It is driven by the weather: overcast raises the coverage and deepens the
// deck, rain lowers the base, thickens and darkens it, wind carries it and
// the shapes evolve slowly. The settings panel's coverage/density/altitude
// are a bias on top of what the weather says, not a replacement for it.
#ifndef CLOUD_FIELD_HLSLI
#define CLOUD_FIELD_HLSLI
#include "world.hlsli"
#include "noise.hlsli"

struct CloudLayer {
    float base, thickness, coverage, density, rain;
    float2 drift;
    float evolve;
    bool enabled;
};

CloudLayer cloudLayer()
{
    CloudLayer layer;
    layer.enabled = cloudsPS.w > 0.5;
    const bool weather = parametersPS[0].x > 0.5;
    const float overcast = weather ? saturate(parametersPS[2].z) : 0.0;
    const float rain = weather ? saturate(parametersPS[2].y) : 0.0;
    const float bias = saturate(cloudsPS.x);
    // Without a weather model the setting is the coverage; with one, the
    // weather decides and the setting shifts it.
    layer.coverage = weather ? saturate(overcast * 0.85 + rain * 0.25 + (bias - 0.45)) : bias;
    layer.rain = rain;
    layer.base = max(cloudsPS.z, 200.0) * (1.0 - 0.4 * rain);
    layer.thickness = 1300.0 * (1.0 + 0.9 * rain + 0.5 * overcast);
    layer.density = max(cloudsPS.y, 0.0) * (1.0 + 1.6 * rain);
    const float windSpeed = 4.0 + 14.0 * max(windPS.z, 0.0);
    layer.drift = windPS.xy * windSpeed * viewportPS.z;
    layer.evolve = viewportPS.z * 0.0035;
    return layer;
}

// Large shapes, 0..1: where the deck is at all.
float cloudCoverageAt(float2 xy, CloudLayer layer)
{
    const float2 q = (xy + layer.drift) / 9000.0 + layer.evolve * float2(0.7, -0.4);
    const float n = noiseAt(q) * 0.55 + noiseAt(q * 2.03 + 17.1) * 0.30 + noiseAt(q * 4.07 + 3.7 - layer.evolve) * 0.15;
    return saturate((n - (1.0 - layer.coverage)) / max(layer.coverage * 0.6, 0.05));
}

// Density at a point: coverage, a height profile (flat bases, rounded tops
// that rise with coverage) and detail eroding the edges. The detail is two
// value-noise slices offset with height, a cheap stand-in for 3D noise.
float cloudDensityAt(float3 p, CloudLayer layer)
{
    const float h = (p.z - layer.base) / layer.thickness;
    if (h <= 0.0 || h >= 1.0) return 0.0;
    const float coverage = cloudCoverageAt(p.xy, layer);
    if (coverage <= 0.0) return 0.0;
    const float top = 0.45 + 0.5 * layer.coverage;
    const float profile = smoothstep(0.0, 0.12, h) * (1.0 - smoothstep(top * 0.6, top, h));
    const float body = coverage * profile;
    if (body <= 0.0) return 0.0;
    const float2 moved = p.xy + layer.drift * 1.6;
    const float detail = noiseAt(moved / 900.0 + h * float2(2.3, 1.7) + layer.evolve * 3.0) * 0.6 +
                         noiseAt(moved / 310.0 + h * float2(-3.1, 2.9)) * 0.4;
    return saturate(body * 1.5 - detail * (1.0 - body) * 0.9) * layer.density;
}

// Where the ray [0, far] is inside the slab; false when it never is.
bool cloudSpan(float3 eye, float3 d, float far, CloudLayer layer, out float enter, out float leave)
{
    const float low = layer.base, high = layer.base + layer.thickness;
    enter = 0.0; leave = far;
    if (abs(d.z) < 1e-4) {
        if (eye.z <= low || eye.z >= high) return false;
    } else {
        const float a = (low - eye.z) / d.z, b = (high - eye.z) / d.z;
        enter = max(0.0, min(a, b)); leave = min(far, max(a, b));
    }
    leave = min(leave, 60000.0);
    return leave > enter;
}

// Light through the cloud towards the sun: a few taps, Beer's law.
float cloudLightAt(float3 p, float3 sun, CloudLayer layer)
{
    const float toward = cloudDensityAt(p + sun * 280.0, layer) * 1.6;
    // Beer's law plus a softer lobe standing in for multiple scattering: a
    // thick deck is dark grey underneath, never black.
    return max(exp(-toward * 2.2), 0.3 * exp(-toward * 0.35));
}

// March the slab along a ray. rgb = light scattered towards the eye, a =
// transmittance of whatever lies behind. `lit` adds the sun taps (the sky);
// surfaces use the cheaper height-based light.
float4 cloudsAlong(float3 eye, float3 d, float far, int steps, float jitter, float3 sun,
                   float3 sunColour, float3 ambient, bool lit)
{
    const CloudLayer layer = cloudLayer();
    float enter, leave;
    if (!layer.enabled || steps <= 0 || !cloudSpan(eye, d, far, layer, enter, leave)) return float4(0, 0, 0, 1);
    const float step = (leave - enter) / steps;
    float t = enter + step * jitter;
    float transmittance = 1.0;
    float3 light = 0.0;
    const float darken = 1.0 - 0.35 * layer.rain;
    [loop] for (int i = 0; i < steps; ++i) {
        const float3 p = eye + d * t;
        const float density = cloudDensityAt(p, layer);
        if (density > 0.003) {
            const float h = saturate((p.z - layer.base) / layer.thickness);
            const float shade = lit ? cloudLightAt(p, sun, layer) : lerp(0.35, 1.0, h);
            const float absorbed = 1.0 - exp(-density * step * 0.0022);
            light += transmittance * absorbed * darken *
                     (sunColour * shade * 0.85 + ambient * lerp(0.6, 1.0, h) * 0.95);
            transmittance *= 1.0 - absorbed;
            if (transmittance < 0.02) break;
        }
        t += step;
    }
    return float4(light, transmittance);
}

// Sunlight left after the cloud deck, for the ground's shadows.
float cloudShadow(float3 world)
{
    const CloudLayer layer = cloudLayer();
    if (!layer.enabled) return 1.0;
    const float3 sun = dot(shadowSunPS.xyz, shadowSunPS.xyz) > 0.5 ? shadowSunPS.xyz : normalize(float3(-0.55, -0.55, 0.63));
    if (sun.z < 0.05) return 1.0;
    float enter, leave;
    if (!cloudSpan(world, sun, 1e9, layer, enter, leave)) return 1.0;
    // One read of the coverage field where the sun ray crosses the middle of
    // the deck, with the deck's optical depth: every lit pixel of the world
    // pays for this, so it is not a march.
    const float3 middle = world + sun * ((enter + leave) * 0.5);
    const float coverage = cloudCoverageAt(middle.xy, layer);
    const float optical = coverage * layer.density * (leave - enter) * 0.0022 * 0.45;
    return lerp(1.0, exp(-optical), 0.85);
}
#endif

