#ifndef ENVIRONMENT_FIELDS_HLSLI
#define ENVIRONMENT_FIELDS_HLSLI
// The same scalar readouts in C++ tests and the GPU, not two balance models.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
namespace world::environment_logic {
using std::log2;
inline float envClamp(float x) { return std::clamp(x,0.0f,1.0f); }
#else
float envClamp(float x) { return saturate(x); }
#endif
float envSmooth(float a, float b, float x) {
    float t=envClamp((x-a)/(b-a)); return t*t*(3.0f-2.0f*t);
}
float soilReadout(float potential, float delta) { return envClamp(potential+delta); }
float moistureReadout(float local, float rainfall, float drainage) {
    // Rain is a weather multiplier, drainage remains a terrain coefficient.
    return envClamp(local*rainfall + (1.0f-drainage)*envClamp(rainfall-1.0f)*0.25f);
}
float travelReadout(float cost) {
    // Cost comes from HeightField::travelAt + navmesh::travelCost, including
    // sharp banks, fords and swimming. The shader only chooses a palette.
    if (cost>30.0f) return 1.0f;
    return 0.9f*log2(cost>1.0f ? cost : 1.0f)/log2(30.0f);
}
float foundationReadout(float slope, float depth, float moisture, float drainage) {
    if (depth>0.0f) return 0.0f;
    // Terrain screening ONLY: no invented load-bearing soil strength.
    return (1.0f-envSmooth(0.05f,0.5f,slope))*(0.25f+0.75f*drainage)*
           (1.0f-0.85f*envSmooth(0.6f,1.0f,moisture));
}
float soilFoundationReadout(float slope, float depth, float moisture, float drainage,
                            float bearingStrength, float mudPotential) {
    // Relative substrate indices, NOT engineering capacity or building permission.
    // Wetness is current weather; the immutable substrate is never rewritten.
    return foundationReadout(slope,depth,0.0f,envClamp(drainage))*envClamp(bearingStrength)*
           (1.0f-0.85f*envSmooth(0.6f,1.0f,moisture)*envClamp(mudPotential));
}
#ifdef __cplusplus
} // namespace world::environment_logic
#endif
#endif

