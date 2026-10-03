// Ground coordinates fine enough to texture with, anywhere in the world.
//
// A world coordinate is a float. 228 km from the origin a float steps in 1/64
// of a metre: eight pixels of ground at the feet. A scan with millimetre texels
// sampled at those steps is drawn as bands of one colour 1.6 cm wide - the
// ground reads as striped paint rather than a texture - and every screen
// derivative of the position (mip choice, the pixel's footprint, the slope of
// the drawn surface) is a different number in every pixel quad.
//
// So the same point is also kept as a frame origin, a multiple of
// kFrameMetres and therefore an exact float, plus the metres from it. The
// vertex stage subtracts the origin from each (exact) vertex; what the
// rasterizer interpolates is then small near the eye and exact to a fraction
// of a millimetre. Anything finer than a centimetre - texture coordinates,
// cells of small decals, derivatives - is computed from that offset; smooth
// fields and metre-scale noise keep reading the plain world coordinate.
#ifndef WORLD_FRAME_HLSLI
#define WORLD_FRAME_HLSLI

#include "noise.hlsli"

// Small, so the offsets near the eye are small and exact to micrometres: at a
// 4 km frame the offset itself held only quarter-millimetre steps, a third of
// a pixel at the feet, and screen derivatives of it were still noise. Large
// enough that an origin has few significant bits (frameTurns relies on it:
// fifteen bits of 64 m reach 2 000 km, the largest world).
static const float kFrameMetres = 64.0;

// The frame a vertex stage measures from: the origin at or below the eye.
float2 frameAnchorOf(float2 p)
{
    return floor(p * (1.0 / kFrameMetres)) * kFrameMetres;
}

// The same origin, recovered in the pixel stage from the two interpolated
// readings of one point. They differ by the origin to within the world
// coordinate's own error, so rounding to the frame grid returns it exactly -
// and a caller that passes the world coordinate as its own offset (probes)
// gets the origin nought, the world unchanged.
float2 frameAnchorFrom(float2 worldXY, float2 frameXY)
{
    return round((worldXY - frameXY) * (1.0 / kFrameMetres)) * kFrameMetres;
}

// x modulo m, as a value in about [0, m).
float frameWrap(float x, float m)
{
    return x - m * floor(x / m);
}

// (a * f) modulo m, for `a` a frame origin, without the product's rounding.
// f is cut into pieces of nine significant bits (the last shorter); an origin
// has at most fifteen, so each partial product is an exact float, and each is
// reduced on its own. A pattern laid at f turns per metre, repeating every m
// turns, starts the frame exactly where the world puts it. Rounded, the
// product of a 200 km origin is off by a hundredth of a turn - the whole
// ground would jump by that every time the eye crossed into the next frame.
//
// The three reduced pieces are summed as fixed point, not as floats: under
// fast math the Metal compiler factored a*high + a*middle + a*low back into
// one rounded a*f, and the ground jumped by two texels at every frame. An
// integer conversion is something it cannot see through.
float frameTurns(float a, float f, float m)
{
    const float high = asfloat(asuint(f) & 0xFFFF8000u);
    const float rest = f - high;
    const float middle = asfloat(asuint(rest) & 0xFFFF8000u);
    const float low = rest - middle;
    const float unit = 1048576.0;   // a millionth of a turn: far below a texel
    const int sum = int(frameWrap(a * high, m) * unit) +
                    int(frameWrap(a * middle, m) * unit) +
                    int(frameWrap(a * low, m) * unit);
    return frameWrap(float(sum) * (1.0 / unit), m);
}

float2 frameTurns(float2 a, float f, float m)
{
    return float2(frameTurns(a.x, f, m), frameTurns(a.y, f, m));
}

// Cells of `metres` laid on the world, read from the frame: the whole cell
// (the integer floor(world / metres) names) and where in it the point lies,
// both exact. Hashes of `whole` are the hashes the world coordinate gave, so
// nothing placed by cell moves; only the position inside the cell is sharp.
void frameCells(float2 anchor, float2 local, float metres,
                out float2 whole, out float2 within)
{
    const float inverse = 1.0 / metres;
    const float2 start = frameTurns(anchor, inverse, 1.0);
    // anchor / metres minus its exact fraction is a whole number of cells to
    // well inside a float's error: rounding recovers it.
    const float2 first = round(anchor * inverse - start);
    const float2 inside = start + local * inverse;
    const float2 step = floor(inside);
    whole = first + step;
    within = inside - step;
}

// The same along one direction (a unit vector): cells of `metres` across the
// world's lines perpendicular to it, for patterns laid on a turned axis.
void frameCellsAlong(float2 anchor, float2 local, float2 direction, float metres,
                     out float whole, out float within)
{
    const float2 per = direction / metres;
    const float start = frameWrap(frameTurns(anchor.x, per.x, 1.0) +
                                  frameTurns(anchor.y, per.y, 1.0), 1.0);
    const float first = round(dot(anchor, per) - start);
    const float inside = start + dot(local, per);
    const float step = floor(inside);
    whole = first + step;
    within = inside - step;
}

// noiseAt(world / metres + shift), with the position inside its lattice cell
// exact. `shift` is a whole number of cells (a salt), as callers use it.
float frameNoiseAt(float2 anchor, float2 local, float metres, float2 shift)
{
    float2 whole, part;
    frameCells(anchor, local, metres, whole, part);
    whole += shift;
    part = part * part * (3.0 - 2.0 * part);
    const float a = hashAt(whole);
    const float b = hashAt(whole + float2(1, 0));
    const float c = hashAt(whole + float2(0, 1));
    const float d = hashAt(whole + float2(1, 1));
    return lerp(lerp(a, b, part.x), lerp(c, d, part.x), part.y);
}

float frameNoiseAt(float2 anchor, float2 local, float metres)
{
    return frameNoiseAt(anchor, local, metres, float2(0.0, 0.0));
}

#endif

