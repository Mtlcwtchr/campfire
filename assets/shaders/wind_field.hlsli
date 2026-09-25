// One wind, in the world, sampled wherever anything needs to know about it.
//
// Shared for the same reason the tearing noise is: the grass a hundred metres
// off is drawn as blades, and the grass a hundred and one metres off is drawn as
// a colour on the ground, and if the two disagree about where the gust is there
// is a line across the meadow at the changeover. Both call in here.
//
// Everything is a function of world position and the clock. Nothing is stored,
// nothing is advanced per frame, and no blade is animated on the CPU: what the
// vertex stage gets is where it stands, how big it is and one random number, and
// it works the rest out. That is what makes a million blades affordable.
#ifndef WIND_FIELD_HLSLI
#define WIND_FIELD_HLSLI
#include "noise.hlsli"

// Lulls and squalls: how much of the wind reaches this piece of ground at all.
//
// Big slow patches drifting downwind, a hundred metres across and a smaller
// octave inside them. Around one; below is a lull, above a squall. Without this
// every blade on the map is always doing something, which reads as a hum rather
// than as weather - the thing that makes a field look windy is that parts of it
// are momentarily *not*.
//
// `resolve` is how much of the small octave this caller can actually see: zero
// for a distant clump standing in for twenty blades, or for a pixel of ground
// wider than the octave, both of which would otherwise boil.
float windGust(float2 worldXY, float2 direction, float time, float gustiness, float resolve)
{
    const float2 drift = worldXY - direction * (time * 3.4);
    const float broad = noiseAt(drift / 104.0);
    const float close = noiseAt(drift / 33.0 + float2(17.3, -5.1));
    const float patch = (broad * 0.70 + close * 0.30 * resolve) / (0.70 + 0.30 * resolve);
    return 1.0 + gustiness * (patch - 0.5) * 1.75;
}

// A front: the line of bending grass that crosses a field, dark going in and
// pale coming back out. Half of what the eye reads as wind is this and not the
// movement of any one blade.
//
// Distance measured along the wind, less time - a travelling wave and nothing
// else. The catch is that a travelling plane wave is *exactly* a set of straight
// parallel bands, and over open ground that is what it looks like: corrugated
// iron sliding sideways. So the along-wind coordinate is displaced by a
// stationary field before the sine sees it. The front still travels, at the same
// speed, in the same direction; it is no longer a ruler. Two scales of warp,
// because a front that meanders on one scale only reads as a drawn curve.
//
// Returned in -1..1. Callers that want a gust rather than a swell should shape
// it - see windShape.
float windFront(float2 worldXY, float2 direction, float time, float resolve)
{
    const float along = dot(worldXY, direction);
    const float across = dot(worldXY, float2(-direction.y, direction.x));
    // Metres of along-wind displacement of the front line. Against a thirty metre
    // wavelength this is nearly half a wavelength of meander, which is what it
    // takes to stop the eye finding the grid - and as much as it can be. Warp
    // steeper than the wavelength folds the wave back through itself, and the
    // fold is not a soft artefact: it pinches the front into a bright thread and
    // draws it across the meadow like a caustic. The pair below stays clear of
    // it - measured, the along-wind derivative bottoms out at +0.42.
    const float warp = (noiseAt(worldXY / 40.0) - 0.5) * 13.5 +
                       (noiseAt(worldXY / 13.5 + float2(31.7, 12.3)) - 0.5) * 3.2 * resolve;
    float wave = sin((along + warp) * 0.205 - time * 1.30 - across * 0.012);
    wave += resolve * 0.42 * sin((along + warp * 1.7) * 0.56 - time * 2.35 + across * 0.11);
    return wave / (1.0 + resolve * 0.42);
}

// A front lays the grass over quickly and lets it back slowly. Squaring about
// zero keeps the sign and spends most of the cycle near nothing, which is the
// difference between a gust and a swell.
float windShape(float front)
{
    return front * abs(front);
}

// What the wind does to anything rooted in it, from the two above: a steady
// downwind lean for as long as the gust lasts, and the front riding on top of
// that. Nought is standing up; one is laid right over.
//
// The steady part is the half that a sum of waves cannot give you. A wave leans
// the grass upwind exactly as often as down, and a meadow that spends half its
// time bending into the wind is a meadow of seaweed - the direction the wind is
// blowing becomes unreadable, because nothing in the picture is displaced by it
// on average. It is only a quarter of the swing, and it is the quarter that says
// which way.
//
// Both the blades and the ground colour that stands in for them at distance come
// through here, so the near field and the far field cannot disagree about what
// the wind is doing to the same piece of grass.
float windLean(float gust, float front)
{
    return saturate(gust) * (0.28 + windShape(front) * 0.72);
}

#endif
