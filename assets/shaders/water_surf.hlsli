#ifndef WATER_SURF_HLSLI
#define WATER_SURF_HLSLI
// The sea meeting its shore.
//
// One model, evaluated by the vertex stage (which moves the water: the wave
// rolling in and the sheet of swash running up the sand) and by the pixel stage
// (which decides where the water ends and draws the white on it), from the same
// inputs - so the white always sits on the wave that made it.
//
// Each wave, followed in from a few wavelengths out:
//
//   * it shoals: taller, peaked, its face steepening towards the beach, and it
//     travels at the shallow-water speed sqrt(g h), so the crests bunch up and
//     turn to follow the depth contours;
//   * where the water is about as shallow as the wave is tall it breaks and runs
//     on as a bore - a steep white face with a long back - whose height is a
//     fixed share of the depth, so it dies away to nothing at the waterline;
//   * at the waterline it becomes a thin sheet of swash that climbs the sand,
//     slowing as it goes, holds for a moment at its highest and drains back;
//     its leading edge is a churned lip of foam;
//   * the foam stays where the water put it - on the water behind the bore, and
//     stranded on the sand as the sheet drains - tearing open into lace and
//     bubbles and dissolving over a few seconds, and the sand stays dark and wet
//     for a while longer.
//
// Why it used to come out as sliced strips: the old model kept one clock per
// place and let the wave "wrap" at the same instant everywhere along it, with a
// wave height picked per wave from a hash. Wherever that wrap crossed water that
// was not flat, the surface and its white jumped - a seam, moving along the
// coast. Here every quantity is a continuous function of a continuous phase:
// the wave profile is periodic and smooth through its own trough, a wave's size
// is smooth noise of the phase (so each wave keeps its size all the way in),
// and the foam and the wet sand carry the wave before the current one so that
// nothing appears or vanishes at a wrap.
//
// Everything here is the SEA's: page water (rivers and lakes) never runs it,
// and the swash is only allowed onto ground that actually falls away to the sea
// within a few tens of metres (`reach`, worked out by the caller from the pages)
// - not onto every patch of low ground in the country, which is how the old
// swash flooded places no wave could reach.

static const float kWsGravity = 9.81;
static const float kWsNone = -1.0e4;
// The highest the swash can climb in the strongest wind, metres: callers only
// need to look for the sea's reach below this.
static const float kWsMaxRunup = 1.6;
// Share of a swash spent climbing; the rest it drains back.
static const float kWsSwashPeak = 0.38;

// How much of this water is the open sea, from the page's filtered river and
// lake shares. One smooth ramp, not a switch at their first non-zero texel -
// that was a line across every river mouth.
float wsSeaShare(float river, float lake) { return 1.0 - smoothstep(0.0, 0.35, river + lake); }

// How much of this water the sea sheet draws rather than page water: all of the
// sea, and a river's last stretch where it runs deep at the sea's own level.
// (Page water is not drawn at all on a square wholly half a metre under the sea
// with no head over a quarter of a metre, so the sheet has to own that water;
// shallow water at that level stays page water's, which follows a narrow
// channel far better from afar.) The two overlap over a band and page water
// fades out across it on top of the sheet, which shades the same water the
// same way - so a river mouth has no line in it.
float wsSheetShare(float river, float head, float depth)
{
    return max(1.0 - smoothstep(0.45, 0.60, river),
               (1.0 - smoothstep(0.20, 0.32, head)) * smoothstep(0.25, 0.50, depth));
}

// The still level the sheet stands at: the sea's, and up a river mouth the
// river's own head, so the sheet meets page water at the same height.
float wsSheetLevel(float head, float river) { return head * smoothstep(0.05, 0.30, river); }

struct WsCoastIn {
    float2 p;          // where, metres
    float bed;         // the ground's height, metres above sea level
    float level;       // the still water's level here (0 for the sea)
    float slope;       // how fast the ground rises towards the shore, smoothed
    float2 windDir;    // unit, the way the swell runs
    float sea;         // 0..1 how much of this water is the open sea's
    float reach;       // 0..1 whether the sea can run up this ground at all
    float clock;       // seconds
    float wind;        // 0..1
    float spacing;     // metres the surface is resolved at here
};

struct WsCoast {
    float wave;        // the incoming wave's height above the still level, metres
    float swashTop;    // the swash sheet's surface above the still level, or kWsNone
    float swept;       // how high up the beach this and the last wave reach
    float calm;        // share of the open-sea swell left here
    float broken;      // 0..1 inside the zone where the waves break
    float roller;      // 0..1 white water on the face of a breaking wave
    float trail;       // 0..1 foam left floating behind it
    float trailAge;    // seconds since that foam was made
    float carried;     // metres it has drifted shoreward since
    float film;        // 0..1 under a swash sheet on ground that is otherwise dry
    float lip;         // 0..1 the churned leading edge of the swash
    float sheetFoam;   // 0..1 foam riding on the sheet behind the lip
    float residue;     // 0..1 foam stranded on the sand as the water drains
    float residueAge;  // seconds since the water left it
    float wetSand;     // 0..1 how recently the sand was under water
};

float wsPeriod(float wind) { return lerp(9.5, 7.0, saturate(wind)); }
// Breaker height in metres: a swell is always running, the wind adds to it.
float wsBreakerHeight(float wind) { return lerp(0.32, 1.05, saturate(wind)); }

// When each wave reaches the waterline, place to place along a coast: a slow
// wander, so a beach is never struck all along its length at once.
float wsStagger(float2 p)
{
    return (noiseAt(p / 83.0) - 0.5) * 0.70 + (noiseAt(p / 29.0 + 5.1) - 0.5) * 0.16;
}

// How big a wave is, as a smooth function of which wave it is (its phase) and
// of where along the coast: sets of bigger ones every so often, lulls between.
float wsWaveSize(float wave, float2 p)
{
    const float one = noiseAt(float2(wave * 0.47, 3.7) + p / 340.0);
    const float sets = noiseAt(float2(wave * 0.13, 17.3) + p / 900.0);
    return (0.55 + 0.60 * one) * (0.72 + 0.45 * sets);
}

// One wave passing a point: 0 at the trough, 1 at the crest, over u in [0, 1)
// periods since the trough. The face takes `rise` of the period and the back
// the rest, as (1 - v)^b (1 + b v), which is level at the crest and at the next
// trough - so the profile is smooth through its own wrap, and a phase that
// wraps somewhere never draws a seam there.
float wsProfile(float u, float rise, float back)
{
    if (u < rise) {
        const float x = u / rise;
        return x * x * (3.0 - 2.0 * x);
    }
    const float v = saturate((u - rise) / (1.0 - rise));
    return pow(max(1.0 - v, 1.0e-6), back) * (1.0 + back * v);
}
// Its mean over a period, so the wave moves water about without raising the sea.
float wsProfileMean(float rise, float back) { return rise * 0.5 + 2.0 * (1.0 - rise) / (back + 2.0); }

// The swash's leading edge, as a share of its run-up, over q in [0, 1] of its
// duration: decelerating up the slope, a moment at the top, draining back.
float wsSwashEnvelope(float q)
{
    if (q <= 0.0 || q >= 1.0) return 0.0;
    if (q < kWsSwashPeak) {
        const float a = 1.0 - q / kWsSwashPeak;
        return 1.0 - a * a;
    }
    return 1.0 - pow((q - kWsSwashPeak) / (1.0 - kWsSwashPeak), 1.6);
}
// When the edge passed a share h of the run-up on the way up, and on the way down.
float wsSwashCame(float h) { return kWsSwashPeak * (1.0 - sqrt(saturate(1.0 - h))); }
float wsSwashWent(float h) { return kWsSwashPeak + (1.0 - kWsSwashPeak) * pow(saturate(1.0 - h), 1.0 / 1.6); }

struct WsSwash {
    float height;     // how far up it climbs, metres above the still level
    float duration;   // share of a period it takes, up and back
};
// One wave's swash. `wave` names the wave (it is held for the whole swash, so
// the edge's path and the times read back off it agree).
WsSwash wsSwashOf(float wave, float2 p, float slope, float breaker, float reach, float period)
{
    WsSwash w;
    const float s = clamp(slope, 0.008, 0.5);
    // Tongues and cusps along the beach, different for every wave.
    const float cusps = 0.78 + 0.44 * noiseAt(p / 13.0 + float2(wave * 0.31, wave * 0.17));
    // A steep beach throws the water relatively higher (it surges rather than
    // spills) but not as far; a flat one never lets it run more than ~22 m.
    const float h = breaker * wsWaveSize(wave, p) * lerp(0.45, 0.80, smoothstep(0.03, 0.15, s)) *
                    cusps * saturate(reach);
    w.height = min(h, s * 22.0);
    // Ballistic up and down a plane of this slope, held to most of a period.
    w.duration = clamp(2.0 * sqrt(2.0 * kWsGravity * max(w.height, 0.01)) / (kWsGravity * s * period),
                       0.55, 0.88);
    return w;
}

// What a wave that has been and gone left on the sand at height e.
void wsStranded(WsSwash w, float t, float e, float up, float period,
                inout float residue, inout float age, inout float wet)
{
    if (e >= w.height || w.height <= 0.0) return;
    const float h = e / w.height;
    if (t <= wsSwashCame(h) * w.duration) return;          // it has not got here yet
    const float dry = (t - wsSwashWent(h) * w.duration) * period;
    if (dry <= 0.0) return;                                   // it is still here
    // Most of it along the line where the water turned back - a crisp line,
    // as a swash mark is, but a quarter of a metre across the sand rather
    // than a pixel.
    const float edge = smoothstep(w.height, w.height - max(0.25 * up, 0.006), e);
    const float made = lerp(0.45, 1.0, smoothstep(0.55, 0.97, h)) * edge;
    const float left = made * exp(-dry / 2.6);
    if (left > residue) { residue = left; age = dry; }
    wet = max(wet, exp(-dry / 6.5) * edge);
}

WsCoast wsCoastAt(WsCoastIn i)
{
    WsCoast c = (WsCoast)0;
    c.swashTop = kWsNone;
    c.calm = 1.0;
    c.trailAge = 1.0e3;
    c.residueAge = 1.0e3;
    const float sea = saturate(i.sea);
    [branch] if (sea <= 0.001) return c;
    const float period = wsPeriod(i.wind);
    const float breaker = wsBreakerHeight(i.wind) * sea;
    const float depth = i.level - i.bed;
    const float d = max(depth, 0.0);
    const float e = -depth;                  // on the sand: its height above the water
    // When the waves reach the waterline here, in periods. A slight lean along
    // the swell's own direction, so they peel along a beach instead of landing
    // square to it.
    const float arrive = i.clock / period + wsStagger(i.p) - dot(i.p, i.windDir) * (1.0 / 350.0);

    // --- the wave coming in ----------------------------------------------
    //
    // Seconds for a crest to cross from here to the waterline over a beach of
    // this slope, at sqrt(g h): 2 sqrt(d) / (s sqrt(g)). The slope is held to
    // a sensible range and is the smoothed one, so it bends the crests rather
    // than breaking them up.
    const float travel = 2.0 * sqrt(d) / (clamp(i.slope, 0.02, 0.12) * sqrt(kWsGravity));
    // Only the last few wavelengths are this wave; further out it hands over
    // to the open sea's own swell.
    const float band = 1.0 - smoothstep(1.8 * period, 3.2 * period, travel);
    c.calm = lerp(1.0, smoothstep(1.0 * period, 2.6 * period, travel), sea);
    [branch] if (band > 0.0 && depth > 0.0) {
        const float phase = arrive + travel / period;
        const float u = frac(phase);
        const float size = wsWaveSize(phase, i.p);
        const float height = breaker * size;
        const float breakDepth = max(height / 0.78, 0.05);
        const float broken = smoothstep(breakDepth * 1.25, breakDepth * 0.80, d);
        const float shoaling = smoothstep(breakDepth * 4.0, breakDepth * 1.1, d);
        const float wavelength = period * sqrt(kWsGravity * max(d, 0.15));
        // A swell is nearly a sine; a shoaling wave peaks and leans forward; a
        // bore is a step with a long back. Never sharper than the surface can
        // carry: a face narrower than the mesh would alias.
        float rise = lerp(lerp(0.45, 0.24, shoaling), 0.08, broken);
        rise = clamp(max(rise, 1.6 * i.spacing / wavelength), 0.02, 0.5);
        const float back = lerp(lerp(2.0, 3.2, shoaling), 1.6, broken);
        const float resolved = 1.0 - smoothstep(0.12 * wavelength, 0.30 * wavelength, i.spacing);
        // Taller as it shoals (Green's law), then a fixed share of the depth
        // once broken - which is what takes it down to nothing at the waterline.
        const float shoaled = height * pow(breakDepth / max(d, breakDepth), 0.25);
        const float amplitude = lerp(shoaled, min(0.6 * d, height), broken) * band;
        const float profile = wsProfile(u, rise, back);
        c.wave = amplitude * resolved * (profile - wsProfileMean(rise, back));
        c.broken = broken * band;

        // White water on the face and the crest of a breaking wave, brightest
        // where it first goes over and white all the way in.
        const float white = c.broken * smoothstep(0.0, 0.08, d);
        const float face = smoothstep(0.0, rise, u) *
                           (1.0 - smoothstep(rise, rise + 0.07 + 0.10 * broken, u));
        const float vigour = (0.7 + 0.3 * smoothstep(breakDepth * 0.35, breakDepth * 0.9, d)) *
                             saturate(0.45 + 0.6 * size);
        c.roller = max(white * face * vigour,
                       band * shoaling * (1.0 - broken) * pow(profile, 6.0) * 0.35 * saturate(size));
        // And the foam it leaves floating behind it: this wave's, and what is
        // left of the last one's, so nothing switches off when the next arrives.
        const float lasts = 3.4;
        const float ageNow = max(u - rise, 0.0) * period;
        const float ageBefore = (u + 1.0 - rise) * period;
        const float now = smoothstep(rise * 0.5, rise + 0.03, u) * exp(-ageNow / lasts);
        const float before = exp(-ageBefore / lasts);
        c.trail = white * vigour * max(now, before);
        c.trailAge = now >= before ? ageNow : ageBefore;
        // Dragged in with the bore for a second or two, then pulled back.
        c.carried = 2.6 * (1.0 - exp(-c.trailAge / 1.1)) -
                    1.4 * smoothstep(0.35 * period, 1.1 * period, c.trailAge);
    }

    // --- the swash -------------------------------------------------------
    [branch] if (e > -0.6 && e < kWsMaxRunup + 0.2) {
        const float n = floor(arrive);
        const float since = arrive - n;      // share of a period since this wave arrived
        const float up = clamp(i.slope, 0.008, 0.5);
        const WsSwash now = wsSwashOf(n + 0.5, i.p, i.slope, breaker, i.reach, period);
        const WsSwash before = wsSwashOf(n - 0.5, i.p, i.slope, breaker, i.reach, period);
        c.swept = max(now.height, before.height);
        const float q = since / now.duration;
        const float top = now.height * wsSwashEnvelope(q);
        const float onSand = smoothstep(-0.04, 0.02, e);
        if (top > 0.0 && e < top) {
            // Under the sheet: thin at its edge, never more than a hand or two
            // deep behind it, lying on the sand rather than levelling it.
            c.swashTop = min(top, e + 0.05 + 0.3 * now.height);
            c.film = onSand;
            c.wetSand = onSand;
            const float behind = (top - e) / up;          // metres behind the edge
            const float climbing = 1.0 - 0.65 * smoothstep(kWsSwashPeak, kWsSwashPeak + 0.35, q);
            const float lipWidth = 0.5 + 1.2 * saturate(now.height / 0.8);
            c.lip = (1.0 - smoothstep(0.0, lipWidth, behind)) * climbing * smoothstep(0.0, 0.03, top);
            c.sheetFoam = lerp(0.75, 0.2, saturate(behind / max(now.height / up, 0.5))) *
                          (1.0 - 0.6 * smoothstep(kWsSwashPeak, 1.0, q)) * onSand;
        } else if (e > 0.0) {
            // Sand the sea has lately been over: this wave's (once it has
            // drained past) and the one before's, so the foam it stranded
            // dissolves in its own time instead of vanishing when the next
            // wave arrives.
            float residue = 0.0, age = 1.0e3, wet = 0.0;
            wsStranded(now, since, e, up, period, residue, age, wet);
            wsStranded(before, since + 1.0, e, up, period, residue, age, wet);
            c.residue = residue;
            c.residueAge = age;
            c.wetSand = wet;
        }
        // Against a steep bank the wave does not run up, it strikes: white at
        // the waterline as each one arrives, falling back.
        const float steep = smoothstep(0.18, 0.55, i.slope);
        c.lip = max(c.lip, steep * exp(-since * period / 0.9) *
                           (1.0 - smoothstep(0.0, 0.8, abs(e))) * saturate(breaker * 1.5));
    }
    return c;
}

// The water's surface above the still level: the wave, or the swash over it.
float wsCoastSurface(WsCoast c) { return max(c.wave, c.swashTop); }
#endif

