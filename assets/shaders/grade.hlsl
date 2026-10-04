// The picture's last word: a grade over the finished world, before the
// interface goes on (GradePass, Stage::Post).
//
// The engine's half (doc/plan_procedural_environment_2026-10-03.md, part H):
// the copy of the picture and its mip chain, edge smoothing, the colour
// lookups of the grading regions, and the strength the whole grade is mixed
// in by. The look itself is the game's: stylePicture() in
// assets/shaders/game/style_picture.hlsli, given the picture, the pass's
// dials and the scene's gradeStyle rows.
//
// Display-referred, like everything else this renderer draws (see
// landscape_look.hlsli): what comes in is what would have gone to the screen,
// and so is what goes out. The alpha is the opaque world's encoded view
// distance and is passed through untouched.
#include "world.hlsli"

cbuffer GradeDraw : register(b1, space3)
{
    float4 grade;       // x strength (0..1), y bloom, z vignette, w grain
    float4 gradeChain;  // x levels in the picture's mip chain, y FXAA, z lookup present, w lookup size
    float4 gradeSpare0; // x brightness, y contrast, z saturation (the settings'), w lookup blend
    float4 gradeSpare1;
};

Texture2D pictureTex : register(t0, space2);
SamplerState pictureSampler : register(s0, space2);
// The two lookups of the grading regions the camera is between, each a strip
// of `size` squares (GradePass::luts).
Texture2DArray lutTex : register(t1, space2);
SamplerState lutSampler : register(s1, space2);

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

// What the game's grade is handed, and hands back in `colour`.
struct StylePicture {
    float3 colour;      // in and out
    float3 unGraded;    // the picture before any grade, after edge smoothing
    float alpha;        // the world's encoded view distance; sky near 1
    float2 uv;
    float2 size;        // pixels
    float2 pixel;       // this pixel's integer position
    float chainLast;    // the coarsest level of the picture's chain
    float bloom, vignette, grain;              // the pass's dials
    float brightness, contrast, saturation;    // the settings' dials, 1 = as built
};

// The grading region's rows (Scene::gradeStyle), zero when the game has none.
float4 gradeRow(int i) { return envGradePS[i]; }
bool gradeRowsPresent() { return envSwitchesPS.w > 1.5; }

#include "game/style_picture.hlsli"

float3 gradeLookup(float3 c, float layer)
{
    const float size = max(gradeChain.w, 2.0);
    const float3 s = saturate(c) * (size - 1.0);
    const float blue = floor(s.b);
    const float t = s.b - blue;
    const float2 texel = float2(1.0 / (size * size), 1.0 / size);
    const float2 inSquare = (s.rg + 0.5) * texel * float2(1.0, 1.0);
    const float3 a = lutTex.SampleLevel(lutSampler, float3(inSquare + float2(blue / size, 0.0), layer), 0).rgb;
    const float3 b = lutTex.SampleLevel(lutSampler, float3(inSquare + float2(min(blue + 1.0, size - 1.0) / size, 0.0), layer), 0).rgb;
    return lerp(a, b, t);
}

float4 GradePS(GradeOut input) : SV_Target0
{
    const float2 size = max(viewportPS.xy, float2(1.0, 1.0));
    const float2 uv = input.position.xy / size;
    const float4 source = pictureTex.SampleLevel(pictureSampler, uv, 0.0);
    float3 c = gradeChain.y > 0.5 ? gradeFxaa(uv, 1.0 / size, source.rgb) : source.rgb;
    // Edges only (the grade itself turned off): the smoothed picture as it is.
    if (grade.x <= 0.0) return float4(c, source.a);

    StylePicture p;
    p.colour = c;
    p.unGraded = c;
    p.alpha = source.a;
    p.uv = uv;
    p.size = size;
    p.pixel = floor(input.position.xy);
    p.chainLast = max(gradeChain.x - 1.0, 0.0);
    p.bloom = grade.y; p.vignette = grade.z; p.grain = grade.w;
    p.brightness = gradeSpare0.x; p.contrast = gradeSpare0.y; p.saturation = gradeSpare0.z;
    stylePicture(p);
    c = p.colour;

    // The regions' lookups, blended as the camera crosses between them.
    if (gradeChain.z > 0.5)
        c = lerp(gradeLookup(c, 0.0), gradeLookup(c, 1.0), saturate(gradeSpare0.w));

    c = lerp(p.unGraded, saturate(c), saturate(grade.x));
    return float4(c, source.a);
}
