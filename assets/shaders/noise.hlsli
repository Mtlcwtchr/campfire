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
