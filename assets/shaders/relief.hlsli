// Cuts, where the ground breaks.
//
// Not a texture of cuts over the whole world. That was the first attempt and it
// was wrong: a joint network over every rock face, bedding over every cliff and
// a creep noise over every slope came out as shredded ground, and once
// everything is cut nothing reads as cut. What makes an edge look eroded is how
// much of it is *not* - a few grooves biting into the break of slope, clean
// ground in between, and nothing whatever on the flat.
//
// So there are exactly two things here, and both are one mask:
//
//   the cut    a groove, shaded and lit as a groove and not painted on
//   the edge   the same mask pushing the stone/soil border about, so the line
//              along the top of a slope is fingered instead of being a contour
//
// The second is most of what is actually visible from an RTS camera. Ground is
// sampled every four metres, so a cliff is one or two triangles wide in plan and
// there is no room on the face for anything to be seen; the border along the top
// of it is metres long and right under the eye. Cutting the border is what the
// terrain alpha masks in a landscape tool are for, and it is what this is for.
#ifndef RELIEF_HLSLI
#define RELIEF_HLSLI
#include "noise.hlsli"

// Noise smeared along the fall line: taps up and down the slope, averaged. That
// is a line integral of the noise along the flow, and it comes out as streaks
// pointing downhill. Water runs downhill, so the cuts it leaves do.
//
// Smeared, not stretched, and the distinction is the whole reason this function
// exists rather than a divide. Stretching needs a coordinate that runs along the
// contour, and the cheap way to get one - `dot(worldXY, contour)`, which was
// tried - measures from the world origin. Round a conical hill that dot is
// `dot(centre, contour)`, so the rib spacing came out proportional to how far
// the hill sat from (0, 0), and this world is four hundred kilometres wide: the
// same slope had ribs a metre apart near the middle of the map and finer than a
// pixel out at the edge, where they aliased into a shimmer. Smearing only ever
// looks at ground within a few metres of here, and so does not care where here
// is.
float reliefStreak(float2 worldXY, float2 downhill, float metres, float smear, int reach)
{
    float total = 0.0;
    for (int t = -reach; t <= reach; ++t)
        total += noiseAt((worldXY + downhill * (smear * float(t))) / metres);
    return total / float(2 * reach + 1);
}

// The same, resolved against the pixel. Smearing averages ALONG the fall line
// and so does nothing about the frequency across it, which is the direction a
// rib has its edges in - five taps down the slope leave a sub-metre pattern
// just as unsampled across the contour as one tap would. `pixel` is metres of
// ground to a pixel; the comparison is against this streak's own size.
float reliefStreak(float2 worldXY, float2 downhill, float metres, float smear, int reach,
                   float pixel)
{
    float total = 0.0;
    for (int t = -reach; t <= reach; ++t)
        total += filteredNoiseAt((worldXY + downhill * (smear * float(t))) / metres,
                                 pixel / metres);
    return total / float(2 * reach + 1);
}

// Curvature: how fast the normal turns as you walk across this pixel.
//
// The surface divergence of the normal is twice the mean curvature, in units of
// one over metres. Positive over a lip, an outcrop, the top of a bank - ground
// that is being taken away. Negative in a hollow, which is where what was taken
// away ends up, and which therefore wants no cuts at all. It costs two
// derivatives and asks nothing new of the vertex.
//
// It is piecewise constant over a triangle, though, because the normal the
// rasteriser hands over is linear in one: four-metre triangles, and a clean
// four-metre straight edge in the mask would be worse than having no mask. So
// the caller tears the threshold about with noise, the way the material borders
// in the ground shader already do. A blocky gate on a fine pattern reads as the
// pattern coming and going; a blocky gate with a clean edge reads as triangles.
float reliefCurvature(float3 normal, float3 worldPos)
{
    const float3 dpx = ddx(worldPos), dpy = ddy(worldPos);
    const float3 dnx = ddx(normal), dny = ddy(normal);
    const float span = dot(dpx, dpx) + dot(dpy, dpy);
    return span > 1e-9 ? (dot(dnx, dpx) + dot(dny, dpy)) / span : 0.0;
}

struct Relief {
    float cut;      // nought on untouched ground, one at the bottom of a groove
    float edge;     // push the stone/soil border out of line, about -0.3 .. 0.3
};

// What has been cut into this piece of ground. `worldPos` is where it is, x, y
// and height together, because the curvature is measured in all three; `detail`
// is how much of the fine work a pixel can still resolve, nought to one; and
// `pixel` is the ground a pixel covers, in metres, which is what decides how
// much of each pattern below is still a pattern rather than a sampling artefact.
//
// `detail` alone was not enough, and the reason is arithmetic. It closes over a
// pixel of 0.28 to 0.80 metres, but the fine streak here is 0.62 metres across:
// at the near end of that band it is already down to two pixels, and a value
// noise at two pixels a cell is not a soft version of itself, it is a seethe.
// So the gate has to be per pattern, against that pattern's own size, and
// `detail` stays as what it is - a statement about whether this kind of detail
// is wanted at all, not about whether it can be sampled.
Relief reliefAt(float3 worldPos, float3 normal, float detail, float pixel)
{
    // Downhill, horizontally. The height gradient is -normal.xy / normal.z, so
    // the way down the slope is simply the way the normal leans. Degenerate on
    // the level, where the mask below is nought and none of this is used.
    const float2 downhill = normalize(normal.xy + float2(1e-5, 1e-5));

    // Two sizes: the scratches, and the broader lobes they sit in. Five taps on
    // the fine one and three on the coarse - the fine one needs the length,
    // because a noise smeared four times its own cell is a streak and smeared
    // once is just noise.
    //
    // Both are sub-metre, and that is a correction. They were three and nine
    // metres, chosen for a camera that does not exist: the zoom runs from 0.9 to
    // 96 pixels a metre and sits at 34, so a pixel of ground is eight
    // centimetres across and a three-metre rib is a hundred and nine pixels
    // wide. That is not a cut, it is a soft wide patch of shade, and it is why
    // none of this could be seen. A cut wants to be twenty pixels.
    const float ribs = reliefStreak(worldPos.xy, downhill, 2.1, 2.1, 1, pixel) * 0.55 +
                       reliefStreak(worldPos.xy + float2(7.1, -3.3), downhill, 0.62, 0.58, 2,
                                    pixel) * 0.45;

    // Steep enough for anything to happen at all: twenty degrees before it
    // starts, full by forty-five. There is no erosion on the level, and a groove
    // on flat ground is not relief, it is a mark on the picture.
    const float steep = smoothstep(0.06, 0.30, 1.0 - normal.z);

    // And convex: the corner, not the whole face. This is the part that was
    // missing, and it is the part that decides whether this reads as erosion or
    // as a dirty texture. Steepness alone lets the cuts loose over every slope
    // in the world, which is a lot of slope; curvature says *break of slope*,
    // and a break of slope is the only place water concentrates enough to cut
    // anything. Threshold torn by its own noise, per reliefCurvature.
    // And convex, in the band the ground actually curves in. Measured over a
    // synthetic four-metre heightfield of hills and a plateau, the per-triangle
    // curvature on steep ground runs p50 0.000, p90 0.022, p99 0.051 one over
    // metres - so the band this had at 0.012 to 0.090 opened for one per cent of
    // steep ground, which is to say never, which is the other reason none of
    // this could be seen. Four to thirty thousandths opens fully on a seventh of
    // steep ground and partly on much more, and it is scale invariant: the same
    // numbers at 34 pixels a metre and at 10.
    //
    // The tear is three quarters of the band, on purpose, and a metre across.
    // The curvature underneath it comes in four-metre blocks - the normal the
    // rasteriser hands over is linear in a triangle, so its derivative is
    // constant in one - and at eight centimetres to the pixel a block is a
    // hundred and thirty pixels wide. A tear finer than the block is what makes
    // the block edge ragged instead of straight.
    //
    // And filtered, because a tear finer than the PIXEL does not make the edge
    // ragged - it makes it boil. The block edge is then straight again at that
    // distance, which is right: a four-metre step seen from far enough that a
    // metre is sub-pixel is a step nobody can see either.
    const float tear = filteredNoiseAt(worldPos.xy / 1.2 + float2(21.7, -8.9), pixel / 1.2) - 0.5;
    const float convex = smoothstep(0.004, 0.030,
                                    reliefCurvature(normal, worldPos) + tear * 0.020);
    // Not nothing away from a corner: a sheer face still weathers, it just does
    // not get gullied. And the weighting is deliberately not lopsided, because
    // the curvature underneath is blocky and the more of the answer it decides
    // the more the blocks show - a third to two thirds, so a corner gets three
    // times a face and not five.
    const float mask = steep * (0.35 + 0.65 * convex) * detail;

    Relief result;
    // A cut where the streaks run low, and a good stretch of nothing where they
    // do not. The band is deliberately off to one side of the noise's middle: at
    // the middle, half the ground is groove, which is the mistake this whole
    // file was rewritten to stop making. A third of the mask cut and two thirds
    // left alone - measured. Half and it is corduroy; a tenth and there is
    // nothing to see.
    //
    // The mask shifts the threshold rather than scaling the answer, and that is
    // not a detail. Scaling puts the mask's own shape into the output, and the
    // mask's shape has four-metre steps in it wherever the curvature changes
    // cell: a groove crossing one was chopped off square, and a straight dark
    // dash a few pixels long reads as a scratch drawn with a ruler - which is
    // what it was. Shifted, the edge of every cut is carried by the streak field
    // instead, so a step in the mask makes a cut ragged rather than ending it.
    //
    // The gate on the end is what keeps level ground clean, since a shift alone
    // still lets an unusually low streak through. It closes below a mask of a
    // quarter, which is steepness deciding and not curvature - the blocky part
    // of the mask lives above a quarter and is multiplied by one - so the gate
    // brings no steps of its own.
    result.cut = smoothstep(0.60, 0.34, ribs + (1.0 - mask) * 0.34) *
                 smoothstep(0.0, 0.25, mask);
    result.edge = (ribs - 0.5) * mask;
    return result;
}

// Turn a cut into a normal, with no tangent frame to hang it on.
//
// The ground has no texture coordinate up here that belongs to the relief - the
// material's UV is the material's, at whatever scale content gave it - so the
// frame comes from the screen instead: how the world moves across one pixel, and
// how the depth moves with it, is exactly the gradient wanted, already expressed
// in the surface's own plane. (Mikkelsen's construction for bump mapping an
// unparametrised surface.)
//
// Clamped, because the cut is a smoothstep of a noise and its edge can be as
// steep as the noise is. Unclamped, the pixel on the lip of a groove turns its
// normal further than the groove is deep and lights up like a chip of glass.
//
// Must be called in control flow the whole quad agrees on - it takes screen
// derivatives, and a lane that skipped the work has nothing to differentiate.
float3 reliefNormal(float3 normal, float3 worldPos, float depth, float scale)
{
    const float3 dpx = ddx(worldPos), dpy = ddy(worldPos);
    const float dhx = ddx(depth), dhy = ddy(depth);
    const float3 r1 = cross(dpy, normal), r2 = cross(normal, dpx);
    const float det = dot(dpx, r1);
    if (abs(det) < 1e-9) return normal;
    float3 gradient = (r1 * dhx + r2 * dhy) / det;
    const float steep = length(gradient);
    if (steep > 2.5) gradient *= 2.5 / steep;
    return normalize(normal - gradient * scale);
}

#endif
