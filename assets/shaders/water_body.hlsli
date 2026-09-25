#ifndef WATER_BODY_HLSLI
#define WATER_BODY_HLSLI
#ifdef __cplusplus
#include <algorithm>
namespace world::water_body {
inline float wbClamp(float x) { return std::clamp(x,0.0f,1.0f); }
#else
float wbClamp(float x) { return saturate(x); }
#endif
inline float wbSmooth(float a,float b,float x) {
    float t=wbClamp((x-a)/(b-a)); return t*t*(3.0f-2.0f*t);
}
// A filtered inland flag is evidence of a body, not a share of ocean.
// Dry texels carry zero flags: 10% lake + 90% dry must never become 90% surf.
inline float wbOcean(float river,float lake) { return river+lake>0.0001f ? 0.0f : 1.0f; }
inline float wbWaveScale(float river,float lake) {
    return wbOcean(river,lake); // inland ripples live in normals, not water level
}
inline float wbNormalScale(float river,float lake) {
    return wbOcean(river,lake)+0.42f*wbClamp(river)+0.20f*wbClamp(lake);
}
// How opaque inland water is: a ramp up from nothing at the waterline, and a
// deeper body reads more solid than a shallow one.
//
// The ramp is as wide as a pixel so the edge does not stair-step, and that is
// the whole of what the pixel term is for - but it had no ceiling, and
// `pixelDepth` is the change in depth across a pixel, which at a strategic zoom
// on a sloping bed is metres. A brook half a metre deep was then entirely
// inside its own edge ramp and came out at about two per cent opacity: the
// river was still there, still wet, still the right shape, and simply not
// drawn. Add the pool-and-bar variation along a reach and what showed was a
// dashed line - which is what it looked like, and it is not the geometry.
//
// So the ramp is capped at three quarters of a metre. Below that it is an
// antialiased edge; above it, water shallower than the cap would be invisible,
// and a shallow river is still a river. Half a metre now reads at about a
// third opacity instead of a fiftieth.
inline float wbInlandAlpha(float depth,float cover,float pixelDepth) {
    float edge=pixelDepth*1.2f;
    if (edge<0.08f) edge=0.08f;
    if (edge>0.75f) edge=0.75f;
    return wbSmooth(0.0f,edge,depth)*wbSmooth(0.0f,0.20f,cover)*
           (0.42f+0.50f*wbClamp(depth/2.0f));
}
inline float wbIceAlpha(float depth,float cover,float pixelDepth) {
    float edge=pixelDepth*1.2f;
    if (edge<0.08f) edge=0.08f;
    return 0.96f*wbSmooth(0.0f,edge,depth)*wbSmooth(0.0f,0.20f,cover);
}
#ifdef __cplusplus
}
#endif
#endif
