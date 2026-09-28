#ifndef WATER_SURF_HLSLI
#define WATER_SURF_HLSLI
// The sea meeting the beach: one wave at a time, and everything about it -
// the crest, its white, the run up the sand, the foam it leaves - timed off
// the same clock at the same place, in the vertex stage (which lifts the sea
// sheet) and the pixel stage (which draws the white) alike.
//
// Each wave: out past the last few tens of metres it is ordinary swell. Over
// that last strip it breaks and its crest runs in at a bore's speed (about
// 5 m/s), white on top, reaching the waterline; then it climbs the sand as a
// thin sheet, holds a moment and drains back. The foam it made stays where it
// was made - on the water behind the crest and on the sand behind the sheet -
// tearing into lace and fading over a few seconds while the water moves on.
//
// Only one crest is ever in the strip (it crosses it in under half a period),
// and distances are measured from the shore itself (depth over bed slope), not
// from depth contours, so nothing lines up into bands. Along the coast each
// wave arrives a little earlier or later and each has its own size, so the
// beach never does one thing all at once.

float wsInverseSmoothstep01(float x)
{
    return 0.5 - sin(asin(1.0 - 2.0 * saturate(x)) / 3.0);
}

struct WsSurfState {
    float surface;   // water surface height above sea level, metres
    float front;     // the dense white on a breaking crest or the swash lip, 0..1
    float foam;      // the foam left behind, before its lace pattern, 0..1
    float foamAge;   // seconds since that foam was made (how far it has thinned)
    float carried;   // metres the foam has been carried shoreward (negative: back)
    float broken;    // how far into the breaking strip this is, 0..1
    float film;      // on the sand: under the swash sheet
    float wetAge;    // on the sand: seconds since the water left it, -1 while wet or never
    float impact;    // at the waterline, just after a wave arrived (for steep banks)
    float calm;      // how much of the open-sea swell is left here (none at the beach)
};

static const float kSurfSpeed = 5.0;         // metres a second, a bore in shallow water

// One wave's contribution at seconds `t` into its cycle.
void wsSurfWave(inout WsSurfState s, float t, float size, float d, float shore,
                float period, float height, bool current)
{
    const float arrival = 0.45 * period;      // reaches the still waterline
    const float runup = 0.45 * period;        // up the sand and back
    const float strip = kSurfSpeed * arrival; // the breaking strip's width
    const float since = t - arrival;          // seconds since it reached the waterline
    const float x = since / runup;
    const float reach = size * height * 0.6;
    const float edge = (x > 0.0 && x < 1.0) ?
        reach * smoothstep(0.0, 0.30, x) * (1.0 - smoothstep(0.38, 1.0, x)) : 0.0;
    if (d > 0.0) {
        const float near = 1.0 - smoothstep(strip * 0.6, strip, shore);
        const float r = t - (arrival - min(shore, strip) / kSurfSpeed);   // since the crest passed
        if (current) {
            const float amp = size * height * 0.5 * near * smoothstep(0.0, 0.4, d);
            const float shape = r < 0.0 ? exp(-(r / 0.6) * (r / 0.6)) : exp(-r / 1.4);
            s.surface = max(amp * shape, edge * exp(-shore / 3.0));
            s.front = near * exp(-(r - 0.15) * (r - 0.15) / 0.12) * (0.6 + 0.4 * size);
            s.impact = since > 0.0 ? exp(-since / 0.8) * exp(-shore / 2.0) : 0.0;
            s.broken = near;
        }
        if (r > 0.0) {
            const float left = near * exp(-r / 4.0) * (0.5 + 0.5 * size);
            if (left > s.foam) {
                s.foam = left;
                s.foamAge = r;
                // The foam is dragged a little way with the crest, then stops;
                // the backwash pulls it back seaward.
                s.carried = kSurfSpeed * 0.6 * (1.0 - exp(-r / 0.7)) -
                            2.0 * smoothstep(0.4, 1.0, x) * exp(-shore / 6.0);
            }
        }
    } else {
        const float e = -d;                   // the sand's height above sea level
        if (current) {
            s.surface = edge;
            s.film = e < edge ? 1.0 : 0.0;
            s.front = (1.0 - smoothstep(0.0, 0.05, abs(e - edge))) * (x < 0.38 ? 1.0 : 0.4) * step(0.01, edge);
            s.impact = since > 0.0 ? exp(-since / 0.8) * (1.0 - smoothstep(0.0, 1.2, e)) : 0.0;
        }
        if (since > 0.0 && e < reach) {
            // When the climbing sheet reached this height, and when it left it.
            const float came = runup * 0.30 * wsInverseSmoothstep01(e / max(reach, 1e-3));
            const float went = runup * (0.38 + 0.62 * wsInverseSmoothstep01(1.0 - e / max(reach, 1e-3)));
            if (since > came) {
                const float wet = since < went ? 1.0 : 0.0;
                const float dry = max(since - went, 0.0);
                const float left = (wet > 0.5 ? 0.8 : 0.85 * exp(-dry / 1.6)) * (0.5 + 0.5 * size);
                if (left > s.foam) {
                    s.foam = left;
                    s.foamAge = since - came;
                    s.carried = 3.0 * (1.0 - exp(-(since - came) / 1.0)) - 3.0 * smoothstep(0.38, 1.0, x);
                }
                if (wet < 0.5) s.wetAge = s.wetAge < 0.0 ? dry : min(s.wetAge, dry);
            }
        }
    }
}

WsSurfState wsSurfAt(float2 p, float bed, float slope, float clock, float wind)
{
    WsSurfState s = (WsSurfState)0;
    s.wetAge = -1.0;
    const float w = saturate(wind);
    const float period = lerp(9.0, 7.0, w);
    const float height = lerp(0.35, 1.0, w);  // breaker height at the beach, metres
    const float d = -bed;
    // Metres from the shore, from the depth and how fast the bed falls away.
    const float shore = d > 0.0 ? min(d / max(slope, 0.02), 200.0) : 0.0;
    s.calm = smoothstep(8.0, 45.0, shore);
    const float stagger = (noiseAt(p / 90.0) - 0.5) * 0.45 + (noiseAt(p / 31.0 + 5.1) - 0.5) * 0.12;
    const float cycle = clock / period + stagger;
    const float index = floor(cycle);
    const float t = frac(cycle) * period;
    const float size = 0.5 + 0.5 * saturate(hashAt(float2(index, 3.7)) * 0.7 +
                                            noiseAt(p / 140.0 + index * 0.37) * 0.6);
    const float sizeBefore = 0.5 + 0.5 * saturate(hashAt(float2(index - 1.0, 3.7)) * 0.7 +
                                                  noiseAt(p / 140.0 + (index - 1.0) * 0.37) * 0.6);
    // The wave before this one: only its foam, still settling.
    wsSurfWave(s, t + period, sizeBefore, d, shore, period, height, false);
    wsSurfWave(s, t, size, d, shore, period, height, true);
    return s;
}
#endif

