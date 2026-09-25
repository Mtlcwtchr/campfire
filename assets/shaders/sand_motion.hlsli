// Ground-hugging windblown sand, not moving dunes or a volumetric dust storm.
// Reuse the scalar world-noise implementation so C++ tests run the real shader.
#ifndef SAND_MOTION_HLSLI
#define SAND_MOTION_HLSLI
#include "foliage_field.hlsli"
#ifdef __cplusplus
namespace world::sand {
using std::sin;
using std::cos;
using foliage::foliageNoise;
using foliage::smoothstep;
using foliage::saturate;
#endif

inline float sandDriftSupport(float desert, float sand, float rock, float grass,
                              float snow, float marsh, float depth, float upright)
{
    const float dry = 1.0 - smoothstep(-0.30, -0.02, depth);
    return saturate(desert) * smoothstep(0.18, 0.65, sand) *
           (1.0 - smoothstep(0.15, 0.65, rock)) *
           (1.0 - smoothstep(0.15, 0.60, grass)) *
           (1.0 - smoothstep(0.02, 0.25, snow)) *
           (1.0 - smoothstep(0.05, 0.35, marsh)) *
           smoothstep(0.55, 0.90, upright) * dry;
}

inline float sandDriftSpeed(float windStrength)
{
    // Translation never multiplies time by a varying gust: that makes patterns
    // accelerate backwards as a gust subsides. Gusts affect coverage instead.
    return 0.09 * smoothstep(0.12, 0.85, windStrength);
}

inline float sandRippleSpeed(float windStrength)
{
    return 0.012 * smoothstep(0.12, 0.85, windStrength);
}

inline float sandRidgeSpeed(float windStrength)
{
    return 0.003 * smoothstep(0.12, 0.85, windStrength);
}

struct SandRipple {
    float height; // Virtual bump profile for derivative tests; NEVER vertex displacement.
    float alongSlope;
    float acrossSlope;
    float roughness;
};

inline SandRipple sandRipple(float along, float across, float clock, float windStrength,
                              float footprint)
{
    const float broad = 1.0 - smoothstep(0.40, 1.80, footprint);
    // Conservative fine cutoff also covers compression by the parent warp.
    const float fine = 1.0 - smoothstep(0.10, 0.32, footprint);
    const float broadK = 6.2831853 / 4.8;
    const float fineK = 6.2831853 / 1.1;
    const float warp = 0.45 * sin(across * 0.21) + 0.16 * sin(across * 0.63);
    const float warpDy = 0.0945 * cos(across * 0.21) + 0.1008 * cos(across * 0.63);
    const float phase = broadK * (along + warp - clock * sandRidgeSpeed(windStrength));
    const float crest = sin(phase), face = cos(phase);
    const float phaseDx = broadK, phaseDy = broadK * warpDy;

    // The fine ripples share the large crest's curved coordinates, then bend
    // around its shoulders. This is not two independent stripe textures added.
    const float nested = fineK * (along + warp + 0.26 * crest + 0.09 * sin(across * 1.1) -
                                  clock * sandRippleSpeed(windStrength));
    const float nestedDx = fineK * (1.0 + 0.26 * face * phaseDx);
    const float nestedDy = fineK * (warpDy + 0.26 * face * phaseDy + 0.099 * cos(across * 1.1));
    const float envelope = 0.65 + 0.35 * crest;
    const float small = sin(nested), smallSlope = cos(nested);
    SandRipple result;
    result.height = 0.14 * crest * broad + 0.022 * envelope * small * fine;
    // Full chain rule, including the amplitude envelope: normals follow the
    // same continuous profile on the large ridge and in the quieter trough.
    result.alongSlope = 0.14 * face * phaseDx * broad + 0.022 * fine *
            (0.35 * face * phaseDx * small + envelope * smallSlope * nestedDx);
    result.acrossSlope = 0.14 * face * phaseDy * broad + 0.022 * fine *
            (0.35 * face * phaseDy * small + envelope * smallSlope * nestedDy);
    result.roughness = 0.86 + 0.035 * crest * broad + 0.065 * envelope * small * fine;
    return result;
}

inline float sandDriftOpacity(float along, float across, float clock, float windStrength,
                              float gustiness, float footprint)
{
    const float drive = smoothstep(0.12, 0.85, windStrength);
    const float fine = 1.0 - smoothstep(0.07, 0.24, footprint);
    const float medium = 1.0 - smoothstep(0.30, 1.20, footprint);
    const float x = along - clock * sandDriftSpeed(windStrength);
    const float ribbon = smoothstep(0.30, 0.68, foliageNoise(x / 2.4, across / 0.8 + 13.1));
    const float wisps = smoothstep(0.38, 0.72, foliageNoise(x / 0.9 - 7.9, across / 0.5));
    const float weather = smoothstep(0.26, 0.72,
            foliageNoise(along / 19.0 - clock * 0.025, across / 11.0 + 27.3));
    const float gust = 1.0 - saturate(gustiness) * (0.55 - weather * 0.55);
    // Gaps remain still. No broad moving haze, and no detail past Nyquist.
    return 0.30 * drive * gust * (wisps * fine * 0.25 + ribbon * medium * 0.75);
}

#ifdef __cplusplus
} // namespace world::sand
#endif
#endif

