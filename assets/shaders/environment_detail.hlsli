#ifndef ENVIRONMENT_DETAIL_HLSLI
#define ENVIRONMENT_DETAIL_HLSLI
#ifdef __cplusplus
#include <algorithm>
namespace world::decor {
inline float environmentUnit(float x) { return std::clamp(x,0.0f,1.0f); }
#else
float environmentUnit(float x) { return saturate(x); }
float environmentHash(float2 cell) {
    cell=fmod(cell,4096.0);
    return frac(52.9829189*frac(dot(cell,float2(0.06711056,0.00583715))));
}
float environmentNoise(float2 p) {
    const float2 cell=floor(p),f=frac(p),u=f*f*(3.0-2.0*f);
    return lerp(lerp(environmentHash(cell),environmentHash(cell+float2(1,0)),u.x),
                lerp(environmentHash(cell+float2(0,1)),environmentHash(cell+float2(1,1)),u.x),u.y);
}
#endif
// Habitat is permanent ecology. Rain may wet the surface but cannot grow or
// remove moss each frame. Up-facing ledges and sheltered sides carry it;
// undersides, dry exposed walls and snow do not.
inline float environmentMossHabitat(float moisture,float canopy,float disturbance,float snow) {
    return environmentUnit((moisture-0.32f)*2.0f)*(0.35f+0.65f*environmentUnit(canopy))*
           (1.0f-environmentUnit(disturbance))*(1.0f-environmentUnit(snow*2.0f));
}
inline float environmentMossShelter(float nx,float ny,float nz) {
    // Persistent aspect toward the exposed side of the landscape. Using the
    // moving sun here would make the moss grow and disappear during a day.
    return 1.0f-environmentUnit(nx*0.350456f-ny*0.821068f+nz*0.450586f);
}
inline float environmentMossSurface(float habitat,float upright,float shade,float patch) {
    const float ledge=environmentUnit((upright+0.12f)*1.25f);
    const float shelter=0.25f+0.75f*environmentUnit(shade);
    return environmentUnit(habitat*ledge*shelter*environmentUnit((patch-0.25f)*2.0f));
}
#ifdef __cplusplus
} // namespace world::decor
#endif
#endif
