// The noise the world is torn about with.
//
// Shared rather than copied, because the ground and the water tear their borders
// with it and two noises that disagree put a visible seam exactly where the two
// meet: the waterline is a border of the ground as well as the edge of the sea.
#ifndef NOISE_HLSLI
#define NOISE_HLSLI

// Value noise on the world, at whatever scale the caller divides by. Integer
// hash rather than the usual sin-of-a-dot: that one bands visibly at these
// sizes, and a band in the border between grass and dirt is exactly the
// artefact this is here to remove.
float hashAt(float2 cell)
{
    uint2 c = uint2(int2(floor(cell)) + 4096);
    uint h = c.x * 0x27d4eb2du + c.y * 0x9e3779b1u;
    h ^= h >> 15; h *= 0x85ebca6bu; h ^= h >> 13;
    return float(h & 0xffffu) / 65535.0;
}

float noiseAt(float2 at);
// The one field of islands (engine/biomes/patch_field.hpp - the same arithmetic):
// moss and water, flowers, shrub islands. 0..1; patchRegionAt: 0 small and
// ragged islands, 1 broad fields.
float patchRegionAt(float2 p)
{
    return smoothstep(0.42, 0.62, noiseAt(p / 760.0 + float2(9.1, -3.3)));
}
float patchFieldAt(float2 p)
{
    p.x += (noiseAt(p / 90.0 + float2(5.3, -1.1)) - 0.5) * 56.0;
    p.y += (noiseAt(p / 90.0 + float2(-7.7, 2.9)) - 0.5) * 56.0;
    const float small = noiseAt(p / 46.0 + float2(3.1, 7.7)) * 0.6 + noiseAt(p / 21.0 + float2(11.3, -4.2)) * 0.4;
    const float large = noiseAt(p / 190.0 + float2(1.7, 5.3)) * 0.65 + noiseAt(p / 80.0 + float2(-6.4, 2.2)) * 0.35;
    const float p01 = lerp(small, large, patchRegionAt(p));
    return saturate((p01 - 0.5) * 1.9 + 0.5);
}

float noiseAt(float2 at)
{
    float2 whole = floor(at);
    float2 part = at - whole;
    part = part * part * (3.0 - 2.0 * part);
    float a = hashAt(whole);
    float b = hashAt(whole + float2(1, 0));
    float c = hashAt(whole + float2(0, 1));
    float d = hashAt(whole + float2(1, 1));
    return lerp(lerp(a, b, part.x), lerp(c, d, part.x), part.y);
}

// Single-octave 2D gradient Perlin, not value noise or a directional smear.
// Eight unit gradients and a quintic fade keep lattice boundaries smooth.
float2 perlinGradient(float2 cell)
{
    const uint h = uint(hashAt(cell) * 65535.0 + 0.5) & 7u;
    const float x = (h & 1u) != 0u ? -1.0 : 1.0;
    const float y = (h & 2u) != 0u ? -1.0 : 1.0;
    if (h < 4u) return float2(x, y) * 0.70710678118;
    return h < 6u ? float2(x, 0.0) : float2(0.0, x);
}

float perlinAt(float2 at)
{
    const float2 cell = floor(at), f = at - cell;
    const float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const float a = dot(perlinGradient(cell), f);
    const float b = dot(perlinGradient(cell + float2(1, 0)), f - float2(1, 0));
    const float c = dot(perlinGradient(cell + float2(0, 1)), f - float2(0, 1));
    const float d = dot(perlinGradient(cell + float2(1, 1)), f - float2(1, 1));
    return saturate(0.5 + 0.70710678118 * lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y));
}

// Value noise that gives up rather than alias.
//
// `footprint` is how much of the noise's OWN cell one pixel covers - the same
// units `at` is in, so a caller working in metres divides both by the feature
// size. Resolved below a quarter cell, gone by about one, and what is left is
// the mean.
//
// This is the only honest answer at that size. A pattern finer than the pixel
// sampling it does not average to itself: it averages to whatever the sampling
// grid happens to land on, which moves with the camera and with every wobble
// of the surface underneath. That is the ripple - it is not a pattern on the
// ground, it is the sampling showing through, and it is worst exactly where
// the ground turns, because a turning surface sweeps its footprint across the
// noise fastest there.
//
// Noise belongs on the boundary of a transition, and a boundary the eye cannot
// resolve wants to be a clean line, not a seething one.
float filteredNoiseAt(float2 at, float footprint)
{
    const float resolved = 1.0 - smoothstep(0.25, 0.9, footprint);
    return lerp(0.5, noiseAt(at), resolved);
}

// The same value noise with its analytic gradient (per unit of `at`):
// x = value, yz = d value / d at. Filtered exactly as filteredNoiseAt.
float3 filteredNoiseGradAt(float2 at, float footprint)
{
    const float2 whole = floor(at), f = at - whole;
    const float2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    const float a = hashAt(whole), b = hashAt(whole + float2(1, 0));
    const float c = hashAt(whole + float2(0, 1)), d = hashAt(whole + float2(1, 1));
    const float k = a - b - c + d;
    const float value = lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
    const float2 gradient = du * float2(b - a + k * u.y, c - a + k * u.x);
    const float resolved = 1.0 - smoothstep(0.25, 0.9, footprint);
    return float3(lerp(0.5, value, resolved), gradient * resolved);
}

// Cheap, stationary fractal noise for material-scale variation. The weights
// are normalised so this can be used directly as a tint or a mask.
float fbmAt(float2 at)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    float total = 0.0;
    [unroll] for (int octave = 0; octave < 5; ++octave) {
        value += noiseAt(at * frequency) * amplitude;
        total += amplitude;
        frequency *= 2.03;
        amplitude *= 0.5;
    }
    return value / max(total, 1e-5);
}

// Bounded two-channel domain warp. Keeping the displacement small relative to
// the input scale prevents swimming and preserves continuity across pages.
float2 domainWarp(float2 at)
{
    const float2 a = float2(fbmAt(at + float2(17.3, 41.7)),
                            fbmAt(at + float2(-23.1, 8.4)));
    const float2 b = float2(fbmAt(at * 1.91 + float2(71.2, -13.8)),
                            fbmAt(at * 1.91 + float2(4.6, 59.1)));
    return (a * 0.70 + b * 0.30 - 0.5) * 0.85;
}

float macroNoise(float2 worldXY)
{
    const float2 at = worldXY / 96.0;
    return fbmAt(at + domainWarp(at) * 0.42);
}

#endif
