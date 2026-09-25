#ifndef SCENE_MODEL_MOTION_HLSLI
#define SCENE_MODEL_MOTION_HLSLI
#ifdef __cplusplus
#include <cmath>
namespace world::decor {
using std::sin;
#endif
// Shared with CPU regressions. Metres, not world-size-relative displacement.
inline float modelWindDisplacement(float up,float height,float strength,float vegetation,
                                   float phase,float seconds,float gust) {
    if (strength<=0 || vegetation<=0 || up<=0) return 0;
    if (up>1) up=1;
    if (strength>2) strength=2;
    const float sway=sin(seconds*1.2f+phase)+0.35f*sin(seconds*2.1f+phase*1.7f);
    return sway*(0.012f+0.008f*gust)*strength*vegetation*height*up*up;
}
#ifdef __cplusplus
} // namespace world::decor
#endif
#endif

