#ifndef WATER_SCENE_HLSLI
#define WATER_SCENE_HLSLI
// Water that sees the world around it.
//
// What made the old water read as a painted sheet was not its colour: it could
// not see anything. It had no idea what lay under it, so it blended a tint over
// the bed with one alpha; and no idea what stood around it, so it reflected a
// formula for the sky. Real water, seen from a hillside, is almost entirely
// those two things - the bed through it near the shore, the far bank and the
// sky in it further out, and which of the two you see is decided by the angle
// (Fresnel). Its own colour is only what is left in between.
//
// So the water stage runs after a copy of the finished opaque world is taken
// (StageInfo::grabColour). That copy's alpha carries each pixel's view
// distance (sceneDepthAlpha in landscape_look.hlsli). With it this shader:
//
//   * refracts: reads the bed through the rippled surface, bent by the
//     ripples and by how deep it is, never pulling in anything that stands in
//     front of the water;
//   * absorbs: attenuates that bed per colour channel along the real light
//     path (sun down to the bed, bed up to the eye, both refracted), so red
//     goes first, shallow water is clear, deep water is blue, a lake is
//     greener and a silty river browner, and water seen at a glancing angle
//     is more opaque than water seen from above;
//   * scatters: adds the water's own colour for the part of the column the
//     bed no longer shows through;
//   * reflects: marches the mirrored ray through the screen against that
//     distance, so the far shore, hills and trees appear upside down in it,
//     with the sky where the ray leaves the picture;
//   * lights: Schlick fresnel (F0 = 0.02), a GGX sun glint whose roughness
//     follows the wind and the pixel footprint, light through the back of a
//     crest, and caustics dancing on the shallow bed.
//
// The sea is the same medium, rougher: taller swell, more chop, whitecaps,
// and a surf zone where the waves shoal, bunch up, break into white water and
// run up the beach.

Texture2D sceneGrab : register(t10, space2);
SamplerState sceneGrabSampler : register(s10, space2);

#include "water_surf.hlsli"

// Set by WaterSheetPS for the pixel it is shading: the wave state it already
// worked out (to find the waterline) and the slope of the breaking wave's own
// surface, so the lighting sees the face the vertex stage built.
static WsSurfState gSheetSurf;
static float2 gSheetSlope = float2(0.0, 0.0);
static bool gSheetHasSurf = false;
static float gSheetCalm = 1.0;   // what is left of the open-sea swell here

float2 wsScreenUv(float3 world, out float w)
{
    const float4 clip = mul(viewProjectionPS, float4(world, 1.0));
    w = clip.w;
    const float safeW = abs(clip.w) > 1e-6 ? clip.w : 1e-6;
    const float2 ndc = clip.xy / safeW;
    return float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

float4 wsGrab(float2 uv) { return sceneGrab.SampleLevel(sceneGrabSampler, uv, 0); }

// The column's optics. Extinction per metre, and the colour an infinitely
// deep column of it has in daylight. Mixed through a matrix: the body flags
// come from a scalar select, and DXC folds a vector scaled by one into an
// OpSelect its own SPIR-V validator rejects.
struct WsBody {
    float3 extinction;
    float3 deep;
};
WsBody wsBody(float river, float lake, float ocean)
{
    const float3 w = float3(ocean, lake, river) / max(ocean + lake + river, 1e-4);
    const float3x3 extinction = float3x3(float3(0.40, 0.095, 0.080),   // sea: clear, blue-green
                                         float3(0.55, 0.150, 0.200),   // lake: organics, a green cast
                                         float3(0.80, 0.420, 0.550));  // river: silt
    const float3x3 deep = float3x3(float3(0.030, 0.118, 0.150),
                                   float3(0.030, 0.092, 0.082),
                                   float3(0.088, 0.102, 0.072));
    WsBody b;
    b.extinction = mul(w, extinction);
    b.deep = mul(w, deep);
    return b;
}

// landscapeFinish(c) == scale * highlight(c * exposure) + offset, exactly: the
// haze, the cloud deck and the fog it applies are all affine in the colour.
// Worked out once here instead of calling it twice (the cloud march in it is
// the dearest thing in the pixel), so the part of the water that is the
// picture behind it can skip the haze it already carries.
struct WsFinish {
    float3 scale;
    float3 offset;
    float exposure;
};
WsFinish wsFinishTerms(float3 worldPosition)
{
    WsFinish f;
    f.exposure = lookPS.w > 0.5 ? lookPS.z : 1.0;
    const bool perspective = landscapePerspective();
    const float span = viewportPS.y * 2.0 / max(cameraPS.w, 0.01);
    const float behindFocus = -dot(worldPosition - cameraPS.xyz, landscapeEye(worldPosition));
    const float distance = perspective ? length(worldPosition - cameraPS.xyz) :
                           max(0.0, span * 0.65 + behindFocus);
    const float weatherFog = parametersPS[0].x * parametersPS[2].y;
    const float haze = saturate(lookHaze(distance, worldPosition.z) +
                                (1.0 - exp2(-distance / 1800.0)) * weatherFog * 0.28);
    const float3 ray = normalize(worldPosition - cameraPS.xyz);
    const float3 hazeColour = perspective ? landscapeSkyRay(ray) : float3(0.59, 0.65, 0.69);
    f.scale = (1.0 - haze).xxx;
    f.offset = hazeColour * haze;
    [branch] if (perspective && cloudsPS.w >= 0.5 && distance >= 1.0) {
        const float jitter = frac(sin(dot(worldPosition.xy, float2(12.9898, 78.233))) * 43758.5453);
        const float sunScale = lookPS.w > 0.5 ? lookPS.x : 1.0;
        const float4 cloud = cloudsAlong(cameraPS.xyz, ray, distance, 6, jitter, landscapeSun(),
            float3(1.05, 0.98, 0.88) * sunScale, landscapeSky(0.6) * 0.9, false);
        f.scale *= cloud.a;
        f.offset = f.offset * cloud.a + cloud.rgb * f.exposure;
    }
    const float fog = landscapeFogAmount(worldPosition);
    [branch] if (fog > 0.0) {
        f.scale *= 1.0 - fog;
        f.offset = lerp(f.offset, landscapeSkyRay(ray), fog);
    }
    return f;
}
float3 wsHighlight(float3 c)
{
    c = max(c, 0.0);
    const float peak = max(c.x, max(c.y, c.z));
    return peak > 0.0001 ? c * (lookHighlight(peak) / peak) : c;
}

// The mirrored ray, marched through the screen. rgb: what it found, a: how
// sure (0 = it left the picture, use the sky).
float4 wsTraceReflection(float3 origin, float3 dir, float originDistance, float jitter)
{
    if (!landscapePerspective() || dir.z < 0.003) return 0;
    float step = max(0.3, originDistance * 0.008);
    float t = step * (0.5 + jitter);
    float previous = 0.0;
    [loop] for (int i = 0; i < 22; ++i) {
        const float3 q = origin + dir * t;
        float w;
        const float2 uv = wsScreenUv(q, w);
        if (w <= 0.0 || any(uv < 0.0) || any(uv > 1.0)) return 0;
        const float rayDistance = length(q - cameraPS.xyz);
        const float sceneDistance = sceneDepthFromAlpha(wsGrab(uv).a);
        // One quantisation step of the encoded distance is ~5 %: anything
        // closer than that is the surface the ray is leaving, not a hit.
        // Passing far behind something nearer (a trunk in the foreground) is
        // not a hit on it either; the march goes on.
        if (rayDistance > sceneDistance * 1.05 &&
            rayDistance - sceneDistance < max(6.0, sceneDistance * 0.25) + (t - previous)) {
            float a = previous, b = t;
            [unroll] for (int k = 0; k < 4; ++k) {
                const float m = 0.5 * (a + b);
                float wm;
                const float3 qm = origin + dir * m;
                const float2 um = wsScreenUv(qm, wm);
                if (length(qm - cameraPS.xyz) > sceneDepthFromAlpha(wsGrab(um).a) * 1.05) b = m;
                else a = m;
            }
            float wh;
            const float2 hit = wsScreenUv(origin + dir * b, wh);
            const float2 edge = min(hit, 1.0 - hit);
            const float fade = smoothstep(0.0, 0.06, min(edge.x, edge.y)) *
                               (1.0 - smoothstep(16.0, 22.0, float(i)));
            return float4(wsGrab(hit).rgb, fade);
        }
        previous = t;
        t += step;
        step *= 1.28;
    }
    return 0;
}

// The bed through the rippled surface. The bend grows with depth (a deeper bed
// swims more) and is worked out in world metres, so it has the right size at
// every distance.
float3 wsRefraction(float3 position, float2 straight, float waterDistance, float3 surface, float depth)
{
    const float bend = min(depth, 3.0) * 0.55;
    float w;
    const float2 uv = wsScreenUv(position + float3(surface.xy * bend, 0.0), w);
    float4 seen = wsGrab(saturate(uv));
    // Never refract something standing in front of the water into it.
    if (sceneDepthFromAlpha(seen.a) < waterDistance * 0.97) seen = wsGrab(straight);
    return seen.rgb;
}

// Light focused by the ripples onto the bed: two drifting height fields, and
// the web where they agree. Cheap, and the pattern is the right one.
float wsCaustics(float2 p, float clock)
{
    const float a = waterTex.SampleLevel(waterSampler, float3(p / 3.3 + float2(clock * 0.031, clock * 0.017), 0), 0).a;
    const float b = waterTex.SampleLevel(waterSampler, float3(spun(p, 1.3) / 2.6 - float2(clock * 0.023, -clock * 0.029), 0), 0).a;
    return pow(saturate(1.0 - abs(a - b) * 3.2), 6.0);
}

// Interleaved gradient noise: decorrelates the march start per pixel so its
// steps do not band.
float wsJitter(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// The surf zone: swell shoaling onto a beach. A wave's travel time over ground
// of depth h goes as the integral of 1/sqrt(g h), so crests are laid out on
// sqrt(depth) - they follow the depth contours (parallel to the shore, bent
// round every headland and bar), bunch up as the water shallows, and arrive
// at the waterline on the same clock the swash on the sand runs to. The
// along-shore noise staggers them so a coast is not one straight line of surf.
struct WsSurf {
    float height;   // of the breaking crest, 0..1
    float perDepth; // d(height)/d(depth), in crest heights per metre of depth
    float breaking; // white water: the crest where it is tumbling
    float zone;     // how far inside the surf zone this is
};
WsSurf wsSurf(float2 p, float depth, float waveTime, float blowing, float clock)
{
    WsSurf s;
    const float d = max(depth, 0.0);
    // The last few metres of water, not the whole shelf: breakers are a strip
    // along the beach, and a nine-metre zone drew them as bands across bays.
    s.zone = smoothstep(4.5, 1.6, d) * smoothstep(-0.05, 0.25, depth);
    // And never a continuous line: swell comes in sets, peaks and closes out
    // in sections. Patches tens of metres long, drifting along the coast.
    const float sets = smoothstep(0.38, 0.70, noiseAt(p / 55.0 + float2(clock * 0.021, -clock * 0.013))) *
                       (0.45 + 0.55 * smoothstep(0.25, 0.75, noiseAt(p / 17.0 + 3.3)));
    s.zone *= sets;
    const float stagger = (noiseAt(p / 70.0) - 0.5) * 0.9 + (noiseAt(p / 23.0 + 5.1) - 0.5) * 0.25;
    const float k = 1.35;
    const float phase = frac(sqrt(d) * k + waveTime + stagger);
    // Steep front towards the shore (phase just below 1 wraps to the crest),
    // long gentle back: a breaker, not a sine.
    const float front = smoothstep(0.80, 1.0, phase);
    const float back = 1.0 - smoothstep(0.0, 0.55, phase);
    s.height = max(front, back) * s.zone;
    // d(phase)/d(depth) = k / (2 sqrt d). Shoreward the back rises gently
    // and the front drops steeply: the face looks at the beach.
    const float dPhase = k * 0.5 / sqrt(max(d, 0.05));
    const float dShape = front > back ? 1.0 / 0.20 : (phase < 0.55 ? -1.0 / 0.55 : 0.0);
    s.perDepth = dShape * dPhase * s.zone;
    // They break where the water is about as shallow as they are tall, and
    // tumble down their own back for a while after.
    const float shallowEnough = smoothstep(4.5, 1.2, d);
    // White on the crest itself and the face falling from it, and a trail
    // down its back that thins out behind.
    s.breaking = saturate(max(front, back * back * back) * 1.1) * shallowEnough * s.zone *
                 (0.55 + 0.45 * blowing);
    return s;
}

// Inverse of smoothstep on [0, 1]: when did a smoothstep ramp pass x?
float wsInverseSmooth(float x)
{
    return 0.5 - sin(asin(1.0 - 2.0 * saturate(x)) / 3.0);
}

// Whitecaps as things, not as a threshold on a wave function (which drew
// them along the crests as white strokes). Every ~26 m cell of open sea may
// hold one: born at its own moment, bright for a breath, then a lace that
// tears open, drifts downwind and is gone after a few seconds - and more of
// the cells light the harder it blows.
float wsWhitecaps(float2 p, float2 downwind, float clock, float wind, float metresPerPixel)
{
    const float size = 26.0;
    const float2 q = p / size - downwind * (clock * 0.07);
    const float2 base = floor(q - 0.5);
    const float netTurn = max(5.0, 20.0 * metresPerPixel);
    const float net = waterTex.Sample(waterSampler, float3(spun(p, 0.9) / netTurn + downwind * clock * 0.02, 2)).w * 0.6 +
                      waterTex.Sample(waterSampler, float3(p / (netTurn * 0.43) + 0.31, 3)).w * 0.4;
    float cap = 0.0;
    [unroll] for (int j = 0; j < 2; ++j) {
        [unroll] for (int i = 0; i < 2; ++i) {
            const float2 cell = base + float2(i, j);
            const float h1 = hashAt(cell + 11.7), h2 = hashAt(cell + 37.1);
            const float h3 = hashAt(cell + 73.9), h4 = hashAt(cell + 5.3);
            const float period = 6.0 + 7.0 * h3;
            const float seconds = frac(clock / period + h4) * period;
            // How many cells cap at all rises with the wind.
            const float chance = saturate(wind * 0.75 - 0.08);
            const float lit = step(h1 * 0.6 + h2 * 0.4, chance);
            const float2 metres = (q - (cell + 0.2 + 0.6 * float2(h1, h2))) * size;
            const float radius = lerp(1.2, 4.5, saturate(seconds / 3.5)) * (0.6 + 0.8 * h3);
            const float shape = 1.0 - smoothstep(radius * 0.35, radius, length(metres * float2(1.0, 1.0 + h4)));
            const float strength = smoothstep(0.0, 0.35, seconds) * exp(-seconds / 2.2);
            const float threshold = lerp(0.20, 0.80, saturate(seconds / 4.5));
            cap = max(cap, lit * shape * strength * smoothstep(threshold, threshold + 0.18, net));
        }
    }
    return cap;
}

// The shore: waves that break, roll in, run up the sand, and drain back.
//
// depth > 0 is water over the bed, depth < 0 height of the sand above sea
// level (the swash apron). Time is carried in "seconds since the crest passed
// here": crests travel on sqrt(depth) (shallow-water speed), so they follow
// the depth contours round every point and bar, bunch up as the water
// shallows, and reach the waterline on the same clock the swash runs to.
struct WsShore {
    float foam;       // white
    float film;       // the thin sheet of swash on the sand, 0..1
    float wetSand;    // how recently the sand was under it
    float surfZone;   // where the crests are breaking
};
WsShore wsShore(float2 p, float depth, float slope, float2 shoreward, float waveTime,
                float clock, float wind, float metresPerPixel)
{
    WsShore o = (WsShore)0;
    const float period = lerp(9.0, 7.0, saturate(wind));
    const float height = lerp(0.45, 1.5, saturate(wind));     // breaker height, metres
    const float breakDepth = 1.25 * height;
    const float d = max(depth, 0.0);
    // Swell comes in sets and breaks in sections, never one line along a coast.
    const float stagger = (noiseAt(p / 70.0) - 0.5) * 0.9 + (noiseAt(p / 23.0 + 5.1) - 0.5) * 0.25;
    const float sets = smoothstep(0.30, 0.65, noiseAt(p / 55.0 + float2(clock * 0.021, -clock * 0.013)));
    const float age = frac(waveTime + sqrt(d) * 1.35 + stagger) * period;
    const float shoreAge = frac(waveTime + stagger) * period;   // the same crest at the waterline
    // The foam's own pattern, carried shoreward by the bore and back by the
    // backwash, so it moves with the water instead of sliding under it.
    const float turn = max(7.0, 36.0 * metresPerPixel);
    const float carried = (1.0 - exp(-age / 1.5)) * 5.0 - smoothstep(period * 0.45, period, age) * 3.0;
    const float2 uv = (p - shoreward * carried) / turn;
    const float net = waterTex.Sample(waterSampler, float3(uv, 2)).w * 0.6 +
                      waterTex.Sample(waterSampler, float3(spun(uv * 2.1, 1.3) + 0.37, 3)).w * 0.4;

    // Breaking: from about 1.25 wave heights of depth to the beach. The
    // rolling front is dense; behind it the foam it left thins, tears open
    // and settles over a few seconds.
    o.surfZone = smoothstep(breakDepth * 1.6, breakDepth * 0.9, d) * smoothstep(-0.05, 0.15, depth);
    const float broken = o.surfZone * (0.35 + 0.65 * sets);
    const float front = exp(-age / 0.45) * smoothstep(0.10, 0.50, net + 0.30);
    const float settling = saturate(age / (period * 0.8));
    const float trail = exp(-age / 3.0) * smoothstep(lerp(0.30, 0.80, settling), 1.0, net + 0.15);
    float foam = broken * max(front, trail * 0.8);

    // Against a steep bank the wave does not run up, it hits: a burst of
    // white at the waterline as each crest arrives, falling back.
    const float steep = smoothstep(0.18, 0.55, slope);
    foam = max(foam, steep * exp(-shoreAge / 0.8) * (1.0 - smoothstep(0.0, 1.2, abs(depth))) *
                     smoothstep(0.15, 0.55, net + 0.20) * (0.5 + 0.5 * sets));

    // The swash. The sheet climbs to its run-up height (less on a steep bank),
    // holds a moment and drains back; its leading edge is a lip of foam.
    const float elevation = -depth;
    const float phase = shoreAge / period;
    const float reach = height * lerp(0.55, 0.25, steep) * (0.6 + 0.4 * sets);
    const float rise = smoothstep(0.02, 0.24, phase);
    const float fall = 1.0 - smoothstep(0.42, 0.95, phase);
    const float edge = reach * rise * fall;
    const float onSand = step(0.0, elevation);
    o.film = onSand * smoothstep(edge + 0.02, edge - 0.03, elevation);
    const float lipWidth = 0.04 + 0.02 * saturate(metresPerPixel);
    const float lip = onSand * (1.0 - smoothstep(0.0, lipWidth, abs(elevation - edge))) *
                      (phase < 0.42 ? 1.0 : 0.45) * step(0.005, edge) * smoothstep(0.15, 0.55, net + 0.25);
    foam = max(foam, lip);
    // Lace riding on the sheet itself.
    foam = max(foam, o.film * smoothstep(0.50, 0.90, net) * 0.6);
    // And what it leaves: once the water has drained past this height, the
    // foam on the sand settles and drains away in place over a second or two,
    // and the sand stays dark for a while longer.
    if (elevation > 0.0 && elevation < reach) {
        const float passed = 0.42 + 0.53 * wsInverseSmooth(1.0 - elevation / max(reach, 1e-3));
        const float dry = (phase - passed) * period;
        if (dry > 0.0) {
            const float lace = exp(-dry / 1.4) * smoothstep(lerp(0.35, 0.85, saturate(dry / 2.5)), 1.0, net + 0.10);
            foam = max(foam, lace * 0.85);
            o.wetSand = saturate(1.0 - dry / 5.0);
        } else {
            o.wetSand = 1.0;
        }
    }
    o.foam = saturate(foam);
    return o;
}

float4 WaterScenePS(WaterOut input)
{
    const float river = saturate(input.motion.z), lake = saturate(input.motion.w);
    // How much of this is sea, continuously: the river and lake flags are
    // filtered fields, and a switch at their first non-zero texel was a line
    // across every river mouth - river on one side, sea on the other.
    const float ocean = 1.0 - smoothstep(0.0, 0.35, river + lake);
    const float2 downstream = input.motion.xy / max(length(input.motion.xy), 0.0001);
    // The water's own slope (for falls), before any discard.
    const float2 headDx = ddx(input.worldXY), headDy = ddy(input.worldXY);
    const float headDet = headDx.x * headDy.y - headDx.y * headDy.x;
    const float inverseHeadDet = abs(headDet) > 1e-7 ? 1.0 / headDet : 0.0;
    const float2 waterGradient = float2(ddx(input.baseLevel) * headDy.y - ddy(input.baseLevel) * headDx.y,
        headDx.x * ddy(input.baseLevel) - headDy.x * ddx(input.baseLevel)) * inverseHeadDet;
    const float waterSlope = length(waterGradient);
    const float3 fall = float3(wxWaterfall(waterSlope, input.cover) * river,
                               -waterGradient / max(waterSlope, 0.0001));
    const float ice = saturate(input.ice) * (1.0 - fall.x) * smoothstep(0.02, 0.30, input.depth);
    const float depthDx = ddx(input.depth), depthDy = ddy(input.depth);
    const float perPixel = length(float2(depthDx, depthDy));
    // Which way the bed rises, from the same derivatives: towards the shore.
    const float2 depthGradient = float2(depthDx * headDy.y - depthDy * headDx.y,
                                        headDx.x * depthDy - headDy.x * depthDx) * inverseHeadDet;
    if (extraPS.w > 0.5) discard;

    // --- where the water ends (unchanged: see WaterPS) --------------------
    const float2 p = input.worldXY;
    const float ragged = noiseAt(p / 7.4) * 0.62 + noiseAt(p / 2.3 + 13.1) * 0.38 - 0.5;
    const float sampled = max(length(headDx), length(headDy));
    // Depth is trusted to draw the waterline once the sampling is fine enough
    // for it to mean something - and always for a lake, which is never
    // narrower than its own samples: judged by coverage from afar, a lake ran
    // a texel past its shore at its own level and hung over the ground below.
    const float trusted = max(1.0 - smoothstep(1.5, 6.0, sampled), saturate(input.motion.w * 2.0));
    const float inland = 1.0 - ocean;
    if (input.explicitSurface != 0) {
        clip(input.cover - 0.02);
        clip(input.depth - 0.0001);
    } else {
        clip(input.cover - lerp(0.02, -1.0, trusted * inland));
        clip(lerp(1.0, input.depth + ragged * 0.22, trusted * inland));
    }

    const float3 position = float3(p, input.worldHeight);
    const float2 direction = lerp(windPS.xy, downstream, river);
    const float2 dir = direction / max(length(direction), 0.0001);
    const float2 across = float2(-dir.y, dir.x);
    const float clock = viewportPS.z;
    const float blowing = 0.45 + 0.55 * windPS.z;
    // The sea is never still: a floor under its roughness whatever the wind.
    const float rough = lerp(blowing, 0.75 + 0.45 * windPS.z, ocean);
    const float metresPerPixel = landscapePerspective() ? max(0.0001, sampled) :
                                 2.0 / max(cameraPS.w, 1e-4);
    const float2 shoreward = dot(depthGradient, depthGradient) > 1e-8 ?
                             -normalize(depthGradient) : dir;
    // The swell's clock at the shore: the swash on the sand and the breakers
    // coming in run on the same one (about 7-9 s a wave).
    const float waveTime = clock / lerp(9.0, 7.0, saturate(windPS.z)) + noiseAt(p / 180.0) * 0.35;

    // --- the surface ------------------------------------------------------
    //
    // Geometric swell (the same field the vertex stage lifted the surface by),
    // then ripples at three fixed WORLD sizes: centimetre chop close up, metre
    // wavelets, and the ten-metre texture that is all a distant bay shows. Each
    // is faded as it drops below a few pixels, so near water is busy and far
    // water goes calm and mirror-like, which is what the eye expects.
    float2 slope, capSlope;
    const float swell = swellAt(p, dir, clock, capSlope);
    waveField(p, dir, clock, max(4.0, ringState.w), slope);
    slope *= waterWaveRoom(input.depth, input.cover) * waterWaveAmplitude(windPS.z) *
             (1.0 - fall.x) * (1.0 - ice) * ocean * (1.0 + 0.6 * ocean) * gSheetCalm;
    const float2 drift = float2(noiseAt(p / 620.0) - 0.5, noiseAt(p / 710.0 + 31.7) - 0.5);
    const float2 q = p + drift * 9.0;
    const float2 current = ocean * (dir * (0.65 + 0.45 * blowing) + drift * 0.9 +
                           across * (sin(dot(p, across) / 83.0 - clock * 0.23) * 0.24)) +
                           river * downstream * (0.65 + min(waterSlope, 0.5) * 1.5) +
                           lake * windPS.xy * 0.12;
    const float fineSeen = 1.0 - smoothstep(0.05, 0.35, metresPerPixel);
    const float midSeen = 1.0 - smoothstep(0.25, 2.5, metresPerPixel);
    const float broadSeen = 1.0 - smoothstep(3.0, 30.0, metresPerPixel);
    float2 lying = -slope;
    [branch] if (fineSeen > 0.0) {
        const float3 fineN = flowingNormal(q / 2.4, current * 8.0 / 2.4, clock);
        lying += fineN.xy / max(fineN.z, 0.25) * 0.20 * rough * fineSeen * (1.0 + 0.5 * ocean);
    }
    [branch] if (midSeen > 0.0) {
        const float3 midN = flowingNormal(spun(q, 2.1) / 9.5, spun(current, 2.1) * 8.0 / 9.5, clock + 3.7);
        lying += spun(midN.xy / max(midN.z, 0.25), -2.1) * 0.24 * rough * midSeen * (1.0 + 0.7 * ocean);
    }
    const float3 broadN = waterTex.Sample(waterSampler,
            float3(spun(q - dir * (clock * 0.62) + across * (clock * 0.30), 0.8) / 41.0, 1)).xyz * 2.0 - 1.0;
    lying += spun(broadN.xy / max(broadN.z, 0.25), -0.8) * 0.12 * (1.0 - river) * broadSeen *
             (1.0 + 0.8 * ocean);
    // The breaking wave's own face (the sheet's surface, as built).
    WsSurfState surf = (WsSurfState)0;
    [branch] if (ocean > 0.001) {
        if (gSheetHasSurf) surf = gSheetSurf;
        else surf = wsSurfAt(p, -input.depth, length(depthGradient), clock, windPS.z);
        lying -= gSheetSlope * ocean;
    }
    lying *= (1.0 - ice) * (ocean + (0.42 * river + 0.20 * lake) * (1.0 - ocean));
    const float3 surface = normalize(float3(lying, 1.0));

    // --- what the eye sees of it -----------------------------------------
    const float3 eye = landscapeEye(position);
    const float3 sun = landscapeSun();
    const float3 up = float3(0.0, 0.0, 1.0);
    const float waterDistance = length(position - cameraPS.xyz);
    float straightW;
    const float2 straight = wsScreenUv(position, straightW);
    const float sunshine = 1.0 - parametersPS[0].x * parametersPS[2].z * 0.85;
    const float3 sunColour = float3(1.05, 1.0, 0.90) * (lookPS.w > 0.5 ? lookPS.x : 1.0) * sunshine;

    const float nv = saturate(dot(surface, eye));
    const float fresnel = 0.02 + 0.98 * pow(1.0 - nv, 5.0);

    // Reflection: a calmer normal than the lit one, because reflections on
    // water are coherent - long wobbling streaks, not confetti. Not traced at
    // all where the surface reflects too little of anything to matter.
    const float3 mirrorNormal = normalize(float3(surface.xy * 0.6, 1.0));
    float3 mirrored = reflect(-eye, mirrorNormal);
    mirrored.z = max(mirrored.z, 0.01);
    mirrored = normalize(mirrored);
    float4 found = 0;
    [branch] if (fresnel > 0.035)
        found = wsTraceReflection(position + up * 0.05, mirrored, waterDistance, wsJitter(input.position.xy));
    const float3 sky = landscapeSkyRay(mirrored);

    // Refraction and the column. Snell's law for both paths (1.33).
    const float wet = max(input.depth, 0.0);
    const float3 bedColour = wsRefraction(position, straight, waterDistance, surface, wet);
    const float cosIn = max(abs(eye.z), 0.02);
    const float cosView = sqrt(1.0 - (1.0 - cosIn * cosIn) / 1.7689);
    const float cosSun = max(sqrt(1.0 - (1.0 - sun.z * sun.z) / 1.7689), 0.2);
    WsBody body = wsBody(river, lake, ocean);
    // Surf stirs the sand up: the breaking zone is milky and sandy-green.
    body.deep = lerp(body.deep, float3(0.13, 0.17, 0.15), surf.broken * ocean * 0.45);
    body.extinction = lerp(body.extinction, float3(0.9, 0.7, 0.75), surf.broken * ocean * 0.35);
    const float3 viewThrough = exp(-body.extinction * wet / cosView);
    const float3 bedThrough = viewThrough * exp(-body.extinction * wet / cosSun);
    float caustic = 0.0;
    [branch] if (wet < 8.0 && midSeen > 0.0)
        caustic = wsCaustics(p, clock) * exp(-wet * 0.35) * smoothstep(0.03, 0.5, wet) *
                  saturate(sun.z * 2.0) * sunshine * (1.0 - ice) * midSeen * (1.0 - surf.broken);
    const float3 daylight = landscapeDaylight(up, 1.0);

    // Light through the back of a crest, looking towards the sun.
    const float towardSun = pow(saturate(dot(-eye.xy, sun.xy) * 0.5 + 0.5), 4.0);
    const float crest = saturate(input.lift * 1.2) * (1.0 - nv * 0.5) * ocean;

    // The sun's glint: GGX, roughened by wind and by the footprint (what the
    // ripples inside one pixel would have averaged to).
    const float roughness = clamp(0.045 + 0.06 * windPS.z + 0.04 * ocean +
                                  0.22 * smoothstep(0.3, 15.0, metresPerPixel), 0.04, 0.45);
    const float glint = min(waterSunHighlight(surface, eye, sun, roughness), 30.0);

    // What this shader lights itself ("own") and what it took from the
    // picture ("taken", which already carries its own haze).
    float3 own = body.deep * daylight * (1.0 - viewThrough) * (1.0 - fresnel) +
                 body.deep * sunColour * towardSun * crest * 6.0 +
                 sky * fresnel * (1.0 - found.a) +
                 sunColour * glint;
    float3 taken = bedColour * (1.0 + caustic * 1.6) * bedThrough * (1.0 - fresnel) +
                   found.rgb * fresnel * found.a;
    float takenShare = (1.0 - fresnel) * dot(bedThrough, float3(0.30, 0.50, 0.20)) + fresnel * found.a;
    const float3 foamLight = saturate(daylight * 0.85 + 0.15);
    const float icePixelMetres = max(length(headDx), length(headDy));
    const WsFinish finish = wsFinishTerms(position);

    // --- what lies on it -------------------------------------------------
    //
    // One path for every water. The inland terms (white water where a river
    // falls) are weighted by what is river, the sea's (swash, breakers,
    // whitecaps) by what is sea, so a river mouth blends from one to the other.
    float white = 0.0;
    {
        const float2 flowUv = (p - downstream * clock * 0.85) / max(14.0, 48.0 * metresPerPixel);
        const float authoredFoam = waterTex.Sample(waterSampler, float3(flowUv, 2)).w;
        white = fall.x * smoothstep(0.35, 0.85, authoredFoam) * 0.55 * (1.0 - ice) * (1.0 - ocean);
    }
    float film = 0.0, wetSand = 0.0, support = 1.0;
    [branch] if (ocean > 0.001) {
        // Only the sand the swash can reach: the apron's own extent.
        support = smoothstep(0.02, 0.22, input.cover) * smoothstep(-0.80, -0.05, input.depth);
        const float bedSlope = min(length(depthGradient), 2.0);
        // The foam's pattern, carried with the water it floats on.
        const float turn = max(7.0, 36.0 * metresPerPixel);
        const float2 foamUv = (p - shoreward * surf.carried) / turn;
        const float net = waterTex.Sample(waterSampler, float3(foamUv, 2)).w * 0.6 +
                          waterTex.Sample(waterSampler, float3(spun(foamUv * 2.1, 1.3) + 0.37, 3)).w * 0.4;
        // Fresh foam is nearly solid; as it ages it tears into lace and the
        // holes grow until nothing is left - settling, not switching off.
        const float thinning = saturate(surf.foamAge / 5.0);
        const float lace = smoothstep(lerp(0.12, 0.78, thinning), lerp(0.40, 0.98, thinning), net);
        float shoreFoam = max(surf.front * smoothstep(0.05, 0.45, net + 0.35), surf.foam * lace);
        // Against a steep bank the wave does not run up, it hits: a burst of
        // white at the waterline as each one arrives.
        shoreFoam = max(shoreFoam, smoothstep(0.18, 0.55, bedSlope) * surf.impact * smoothstep(0.1, 0.5, net + 0.2));
        const float caps = wsWhitecaps(p, dir, clock, windPS.z, metresPerPixel) *
                           (1.0 - surf.broken) * smoothstep(2.0, 6.0, input.depth);
        white = lerp(white, saturate(shoreFoam + caps * 0.8), ocean);
        film = surf.film * ocean;
        wetSand = (surf.film > 0.5 ? 1.0 : surf.wetAge >= 0.0 ? saturate(1.0 - surf.wetAge / 5.0) : 0.0) *
                  (1.0 - white) * ocean;
    }
    const float3 foamColour = float3(0.92, 0.94, 0.93) * foamLight;
    // Never quite opaque: thin foam shows the water it floats on.
    own = lerp(own, foamColour, white * 0.88);
    float keep = 1.0 - white * 0.88;
    taken *= 1.0 - wetSand * 0.35;
    [branch] if (ice > 0.001) {
        const float3 iceColour = iceSurfaceColour(p, icePixelMetres) * daylight;
        own = lerp(own, iceColour, ice);
        keep *= 1.0 - ice;
    }

    // Coverage. Inland: an antialiased waterline (at the line the column is
    // nothing and the result is the bed itself, so no ramp has to hide it).
    // Sea: the same, plus the swash film carried up the beach.
    const float edge = clamp(perPixel * 1.5, 0.02, 0.40);
    const float inlandAlpha = wbSmooth(0.0, edge, input.depth) * wbSmooth(0.0, 0.20, input.cover);
    const float wobble = (surface.x + surface.y) * 0.20;
    const float edgeSoft = max(0.06, perPixel * 1.5);
    // The sheet of swash on the sand is thin: mostly the wet sand through it,
    // and the sky in it.
    float seaAlpha = saturate((input.depth + wobble) / edgeSoft);
    seaAlpha = max(seaAlpha, film * 0.45);
    float alpha = lerp(inlandAlpha, seaAlpha, ocean);
    alpha = max(alpha, white * 0.85);
    alpha = max(alpha, wetSand * 0.30);
    alpha = lerp(alpha, wbIceAlpha(input.depth, input.cover, perPixel), ice);
    const float3 finished = finish.scale * wsHighlight(own * finish.exposure) +
                            finish.offset * (1.0 - takenShare * keep) + taken * keep;
    return float4(finished, saturate(alpha) * lerp(1.0, support, ocean));
}
#endif

