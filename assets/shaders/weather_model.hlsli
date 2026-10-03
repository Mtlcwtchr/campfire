#ifndef WEATHER_MODEL_HLSLI
#define WEATHER_MODEL_HLSLI
// Shared CPU/GPU model. Units: Celsius, precipitation 0..1, snow metres.
// No renderer time, random mutable stream, chunk IDs or camera coordinates.
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace world::weather {
using uint = std::uint32_t;
using std::floor;
using std::abs;
inline float wxClamp(float x) { return std::clamp(x, 0.0f, 1.0f); }
#else
float wxClamp(float x) { return saturate(x); }
#endif
inline float wxMix(float a, float b, float t) { return a + (b-a)*t; }
inline float wxSmooth(float a, float b, float x) {
    float t = wxClamp((x-a)/(b-a)); return t*t*(3.0f-2.0f*t);
}
inline float wxHash(int x, int y, uint seed) {
    uint h = uint(x)*374761393u + uint(y)*668265263u + seed*1442695041u;
    h = (h ^ (h >> 13))*1274126177u;
    return float((h ^ (h >> 16)) & 65535u)/65535.0f;
}
inline float wxRegion(float x, float y, uint seed) {
    x /= 12000.0f; y /= 12000.0f;
    int ix = int(floor(x)), iy = int(floor(y));
    float u = wxSmooth(0.0f,1.0f,x-float(ix));
    float v = wxSmooth(0.0f,1.0f,y-float(iy));
    return wxMix(wxMix(wxHash(ix,iy,seed),wxHash(ix+1,iy,seed),u),
                 wxMix(wxHash(ix,iy+1,seed),wxHash(ix+1,iy+1,seed),u),v);
}
struct WeatherAir { float temperature, precipitation, cloud, wind; };
struct WeatherSurface { float snow, wet, ice; };
inline WeatherSurface wxEmpty() {
    WeatherSurface s; s.snow=0.0f; s.wet=0.0f; s.ice=0.0f; return s;
}
// seasonC is the content-defined reference climate. Index 0.65 is its reference.
// Warm regions have weaker winters. Elevation contributes a lapse correction.
inline WeatherAir wxAir(float thermal, float moisture, float height, float x, float y,
                        float seasonC, float pressure, uint key, float wind, int preset) {
    float regional = wxRegion(x,y,key);
    float continental = wxMix(1.35f,0.35f,wxSmooth(0.55f,0.95f,thermal));
    WeatherAir a;
    a.temperature = 18.0f + (thermal-0.65f)*60.0f + (seasonC-18.0f)*continental
                    - height*0.004f + (regional-0.5f)*7.0f;
    float front = wxClamp(pressure + (regional-0.5f)*0.85f + (moisture-0.5f)*0.45f);
    a.cloud = wxSmooth(0.18f,0.85f,front);
    a.precipitation = wxSmooth(0.52f,0.94f,front);
    a.wind = wind*(0.65f+regional*0.7f);
    // Debug forcing still respects local temperature: rain becomes snow in frost.
    if (preset==1) { a.cloud=0.08f; a.precipitation=0.0f; }
    if (preset==2) { a.cloud=0.92f; a.precipitation=0.75f; }
    if (preset==3) { a.cloud=1.0f; a.precipitation=1.0f; a.wind=2.0f; }
    if (preset==4) { a.cloud=0.02f; a.precipitation=0.0f; a.temperature+=6.0f; }
    // Fog: a still, damp, bright-grey day. The renderer closes the view in (world_renderer.cpp).
    if (preset==5) { a.cloud=0.6f; a.precipitation=0.0f; a.wind*=0.25f; }
    return a;
}
inline WeatherSurface wxAdvance(WeatherSurface s, WeatherAir a, float drainage) {
    float frozen = 1.0f-wxSmooth(-1.0f,2.0f,a.temperature);
    float melt = wxClamp(a.temperature/12.0f)*0.12f;
    float thaw = s.snow < melt ? s.snow : melt;
    // Bounded 16-day reconstruction, with retention rather than permanent snow.
    s.snow = s.snow*0.94f + a.precipitation*frozen*0.09f - melt;
    s.snow = s.snow>0.0f ? s.snow : 0.0f;
    s.wet = wxClamp(s.wet*(0.86f-0.38f*wxClamp(drainage)) +
                    a.precipitation*(1.0f-frozen)*0.65f + thaw*3.0f -
                    wxClamp((a.temperature-8.0f)/30.0f)*0.12f);
    s.ice = wxClamp(s.ice*0.96f + wxClamp(-a.temperature/12.0f)*0.20f -
                    wxClamp(a.temperature/8.0f)*0.35f);
    return s;
}
inline float wxSnowMask(float snow, float upright, float depth, float breakup) {
    return wxSmooth(0.005f,0.16f,snow)*wxSmooth(0.35f,0.85f,upright)*
           (1.0f-wxSmooth(-0.10f,0.02f,depth))*wxSmooth(0.0f,0.20f,snow-breakup*0.07f);
}
// HeightField's ocean is exactly level 0. Inland water merges into the sea
// at its mouth; fade across the last half metre instead of a hard ice seam.
// `flowing` is how much of this water is a river rather than a pond, nought to
// one. Moving water carries its heat down away from the surface and breaks the
// skin as fast as it forms: a pond ices over in the first still frost, a river
// needs a real winter behind it, and a fast one does not freeze at all. None of
// that was in the model - freezing was a function of the accumulated cold and
// nothing else - so every brook on the map iced over at the same instant the
// pond beside it did, which is why the rivers were under ice.
inline float wxInlandIce(float accumulated, float surfaceLevel, float cover, float flowing) {
    const float moving = wxClamp(flowing);
    // Still water starts to take at a twelfth of a winter's cold and is closed
    // by four fifths of it. A river needs most of a winter to start and more
    // than the model can accumulate to close, so the fastest water keeps an
    // open channel however hard the frost is - which is what rivers do.
    const float begins = wxMix(0.08f, 0.60f, moving);
    const float closed = wxMix(0.80f, 1.90f, moving);
    return wxSmooth(begins,closed,accumulated)*wxSmooth(0.05f,0.5f,surfaceLevel)*
           wxSmooth(0.02f,0.30f,cover);
}
inline float wxWaterfall(float surfaceSlope, float cover) {
    // Ignore partial shoreline triangles whose dry corner has no water head.
    return wxSmooth(0.20f,0.65f,surfaceSlope)*wxSmooth(0.65f,0.95f,cover);
}
#ifdef __cplusplus
} // namespace world::weather
#endif
#endif
