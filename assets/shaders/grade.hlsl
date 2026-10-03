// The picture's last word: a grade over the finished world, before the
// interface goes on (GradePass, Stage::Post).
//
// A storybook in late-afternoon light - the Witcher's Velen and Toussaint,
// Clair Obscur's painted plates: gold in the light and blue in the shade,
// colour that is rich without being dye, a cinema's contrast with a deep but
// living black, and the brightest things glowing warm into the air round
// them. The grade leans the light and the shade apart and fills the colour
// in; it does not invent hues the picture did not have.
//
// Display-referred, like everything else this renderer draws (see
// landscape_look.hlsli): what comes in is what would have gone to the screen,
// and so is what goes out. The alpha is the opaque world's encoded view
// distance and is passed through untouched.
#include "world.hlsli"

cbuffer GradeDraw : register(b1, space3)
{
    float4 grade;       // x strength (0..1), y bloom, z vignette, w grain
    float4 gradeChain;  // x levels in the picture's mip chain
    float4 gradeSpare0;
    float4 gradeSpare1;
};

Texture2D pictureTex : register(t0, space2);
SamplerState pictureSampler : register(s0, space2);

struct GradeOut {
    float4 position : SV_Position;
};

GradeOut GradeVS(uint id : SV_VertexID)
{
    // One triangle over the whole screen; the corners come out of the id.
    GradeOut output;
    output.position = float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
    return output;
}

float gradeLuma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// White noise that does not repeat on any scale a screen has.
float gradeHash(float2 p)
{
    p = frac(p * float2(0.1031, 0.1030));
    p += dot(p, p.yx + 33.33);
    return frac((p.x + p.y) * p.x);
}

// A level of the picture's chain, read as a small tent of four rather than one
// box: a single bilinear tap of a coarse level shows the level's own squares.
float3 gradeBlur(float2 uv, float level, float2 size)
{
    const float2 texel = exp2(level) / size;
    return (pictureTex.SampleLevel(pictureSampler, uv + texel * float2(-0.5, -0.5), level).rgb +
            pictureTex.SampleLevel(pictureSampler, uv + texel * float2( 0.5, -0.5), level).rgb +
            pictureTex.SampleLevel(pictureSampler, uv + texel * float2(-0.5,  0.5), level).rgb +
            pictureTex.SampleLevel(pictureSampler, uv + texel * float2( 0.5,  0.5), level).rgb) * 0.25;
}

// --- edges -------------------------------------------------------------------
//
// FXAA (Lottes' 3.11 "quality" variant): find the edge through this pixel from
// the luma of its neighbours, walk along it both ways to its ends, and take
// the one bilinear sample across it that the pixel's place on that edge calls
// for. One pass over the finished picture instead of four samples a pixel
// through every stage of the frame - MSAA cost the frame several times over.
float fxaaLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float fxaaAt(float2 uv) { return fxaaLuma(pictureTex.SampleLevel(pictureSampler, uv, 0.0).rgb); }

float3 gradeFxaa(float2 uv, float2 rcp, float3 rgbM)
{
    const float lumaM = fxaaLuma(rgbM);
    const float lumaN = fxaaAt(uv + float2(0.0, -rcp.y));
    const float lumaS = fxaaAt(uv + float2(0.0, rcp.y));
    const float lumaE = fxaaAt(uv + float2(rcp.x, 0.0));
    const float lumaW = fxaaAt(uv + float2(-rcp.x, 0.0));
    const float rangeMax = max(lumaM, max(max(lumaN, lumaS), max(lumaE, lumaW)));
    const float rangeMin = min(lumaM, min(min(lumaN, lumaS), min(lumaE, lumaW)));
    const float range = rangeMax - rangeMin;
    if (range < max(0.0312, rangeMax * 0.125)) return rgbM;
    const float lumaNW = fxaaAt(uv + float2(-rcp.x, -rcp.y));
    const float lumaNE = fxaaAt(uv + float2(rcp.x, -rcp.y));
    const float lumaSW = fxaaAt(uv + float2(-rcp.x, rcp.y));
    const float lumaSE = fxaaAt(uv + float2(rcp.x, rcp.y));
    const float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    const float edgeHorz1 = -2.0 * lumaM + lumaNS;
    const float edgeVert1 = -2.0 * lumaM + lumaWE;
    const float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    const float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    const float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    const float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    const float edgeHorz = abs(-2.0 * lumaW + lumaNWSW) + abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    const float edgeVert = abs(-2.0 * lumaS + lumaSWSE) + abs(edgeVert1) * 2.0 + abs(edgeVert2);
    const bool horzSpan = edgeHorz >= edgeVert;
    float lengthSign = horzSpan ? rcp.y : rcp.x;
    const float lumaN2 = horzSpan ? lumaN : lumaW;
    const float lumaS2 = horzSpan ? lumaS : lumaE;
    const float gradientN = lumaN2 - lumaM, gradientS = lumaS2 - lumaM;
    const bool pairN = abs(gradientN) >= abs(gradientS);
    const float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    const float subpixA = (lumaNS + lumaWE) * 2.0 + lumaNWSW + lumaNESE;
    const float subpixC = saturate(abs(subpixA * (1.0 / 12.0) - lumaM) / range);
    float2 posB = uv;
    if (horzSpan) posB.y += lengthSign * 0.5; else posB.x += lengthSign * 0.5;
    const float2 offNP = horzSpan ? float2(rcp.x, 0.0) : float2(0.0, rcp.y);
    float2 posN = posB - offNP, posP = posB + offNP;
    const float lumaNN = (pairN ? lumaN2 : lumaS2) + lumaM;
    const float lumaMM = lumaM - lumaNN * 0.5;
    const float gradientScaled = gradient * 0.25;
    float lumaEndN = fxaaAt(posN) - lumaNN * 0.5;
    float lumaEndP = fxaaAt(posP) - lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    // Wider strides further out, as the reference's quality presets do.
    static const float kStride[10] = { 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0, 12.0 };
    [loop] for (int i = 0; i < 10 && !(doneN && doneP); ++i) {
        if (!doneN) { posN -= offNP * kStride[i]; lumaEndN = fxaaAt(posN) - lumaNN * 0.5; }
        if (!doneP) { posP += offNP * kStride[i]; lumaEndP = fxaaAt(posP) - lumaNN * 0.5; }
        doneN = doneN || abs(lumaEndN) >= gradientScaled;
        doneP = doneP || abs(lumaEndP) >= gradientScaled;
    }
    const float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    const float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    const bool lumaMLTZero = lumaMM < 0.0;
    const bool goodSpan = dstN < dstP ? ((lumaEndN < 0.0) != lumaMLTZero) : ((lumaEndP < 0.0) != lumaMLTZero);
    const float pixelOffset = 0.5 - min(dstN, dstP) / (dstP + dstN);
    const float subpixF = (-2.0 * subpixC + 3.0) * subpixC * subpixC;
    // 0.6 of the reference's sub-pixel term: foliage stays crisp, stair steps go.
    const float offset = max(goodSpan ? pixelOffset : 0.0, subpixF * subpixF * 0.6);
    float2 posM = uv;
    if (horzSpan) posM.y += offset * lengthSign; else posM.x += offset * lengthSign;
    return pictureTex.SampleLevel(pictureSampler, posM, 0.0).rgb;
}

float4 GradePS(GradeOut input) : SV_Target0
{
    const float2 size = max(viewportPS.xy, float2(1.0, 1.0));
    const float2 uv = input.position.xy / size;
    const float4 source = pictureTex.SampleLevel(pictureSampler, uv, 0.0);
    float3 c = gradeChain.y > 0.5 ? gradeFxaa(uv, 1.0 / size, source.rgb) : source.rgb;
    // Edges only (the grade itself turned off): the smoothed picture as it is.
    if (grade.x <= 0.0) return float4(c, source.a);
    const float3 unGraded = c;

    // --- light that spills ----------------------------------------------
    //
    // Two widths out of the copy's own mip chain - a near halo about a
    // hundredth of the screen across and a broad one four times that - so a
    // sunlit face, the sky by the sun, the glitter on water glows into the
    // air round it. Only what is already bright spills, and it spills gold.
    const float last = max(gradeChain.x - 1.0, 0.0);
    const float3 haloNear = gradeBlur(uv, min(3.0, last), size);
    const float3 haloFar = gradeBlur(uv, min(5.0, last), size);
    const float3 halo = haloNear * 0.55 + haloFar * 0.45;
    const float spill = smoothstep(0.48, 0.90, gradeLuma(halo));
    const float3 bloom = halo * spill * float3(1.0, 0.84, 0.60) * grade.y;
    c = c + bloom * (1.0 - c);   // screened: it brightens, never clips
    c *= gradeSpare0.x;          // the settings' brightness

    // --- gold in the light, blue in the shade -------------------------------
    //
    // Split, so the midtones - most of the ground - keep their own colour:
    // the highlights lean to the sun's gold, the shadows to the sky's blue,
    // which is what makes a shadow read as cool air rather than as dirt.
    // Not the sky itself (its alpha is the far end of the depth encoding):
    // gold over a bright blue sky turned it mint.
    float luma = gradeLuma(c);
    const float skyPixel = smoothstep(0.985, 0.999, source.a);
    const float highlights = smoothstep(0.35, 0.90, luma) * (1.0 - skyPixel);
    const float shadows = 1.0 - smoothstep(0.04, 0.42, luma);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(1.05, 1.01, 0.91), highlights);
    c = lerp(c, c * float3(0.96, 0.99, 1.05) + float3(0.000, 0.003, 0.008), shadows * 0.5);

    // --- a cinema's curve -----------------------------------------------------
    //
    // Contrast through the middle about a pivot a little under mid-grey, a
    // soft toe into a deep black that is not crushed, and a shoulder that
    // rolls the whites off instead of clipping them.
    const float pivot = 0.42;
    float3 graded = pivot + (c - pivot) * (1.16 * gradeSpare0.y);
    graded = max(graded, 0.0);
    graded = graded * graded * (3.0 - 2.0 * saturate(graded));   // toe and shoulder
    c = lerp(c, saturate(graded), 0.55);
    const float3 floorColour = float3(0.012, 0.013, 0.020);
    const float3 ceilingColour = float3(0.990, 0.975, 0.945);
    c = floorColour + saturate(c) * (ceilingColour - floorColour);

    // --- colour, filled in ------------------------------------------------
    //
    // Vibrance rather than saturation: what is dull gains most and what is
    // already strong gains little, so a meadow comes alive and a red roof does
    // not turn to dye. Greens lean a touch to the warm side - the late sun on
    // grass - and the sky keeps its blue.
    luma = gradeLuma(c);
    const float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
    const float vibrance = 1.0 + 0.30 * (1.0 - saturate(chroma * 2.5));
    c = lerp(float3(luma, luma, luma), c, vibrance * gradeSpare0.z);
    // Greens kept green: a hair warmer in the light, never pushed to olive.
    const float green = saturate((c.g - max(c.r, c.b)) * 6.0);
    c += float3(0.010, 0.006, -0.008) * green;

    // --- the edge of the frame ----------------------------------------------
    //
    // Round, darkening towards a deep warm brown, and starting well out from
    // the middle: a frame, not a tunnel.
    const float aspect = size.x / size.y;
    const float2 centred = (uv - 0.5) * float2(aspect, 1.0);
    const float corner = length(float2(aspect, 1.0) * 0.5);
    const float edge = smoothstep(0.45, 1.15, length(centred) / corner);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(0.70, 0.64, 0.60), edge * grade.z);

    // --- grain --------------------------------------------------------------
    //
    // Triangular, so it has no bias, changing twenty-four times a second as
    // film would, and strongest in the shadows where film shows it. It also
    // dithers away any banding the eight-bit target would put in a sky.
    const float2 pixel = floor(input.position.xy);
    const float tick = floor(frac(viewportPS.z / 64.0) * 64.0 * 24.0);
    const float2 seed = pixel + float2(tick * 7.13, tick * 3.71);
    const float noise = gradeHash(seed) + gradeHash(seed.yx + 17.0) - 1.0;
    c += noise * grade.w * (0.45 + 0.55 * (1.0 - luma));

    c = lerp(unGraded, saturate(c), saturate(grade.x));
    return float4(c, source.a);
}
