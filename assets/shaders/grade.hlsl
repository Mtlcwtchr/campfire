// The picture's last word: a grade over the finished world, before the
// interface goes on (GradePass, Stage::Post).
//
// Warm and a little old. Light the colour of a late afternoon; shadows that
// fall towards umber rather than towards black; blacks lifted and whites held
// back the way a print holds them; blues that have faded a step ahead of the
// reds and the earth; a soft golden bloom where the light is strongest, a
// vignette you notice only when it is gone, and the faintest grain over all
// of it. Fantasy by way of an old painted plate, not by way of a fairground:
// nothing here adds colour that was not in the picture, it only leans it.
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

float4 GradePS(GradeOut input) : SV_Target0
{
    const float2 size = max(viewportPS.xy, float2(1.0, 1.0));
    const float2 uv = input.position.xy / size;
    const float4 source = pictureTex.SampleLevel(pictureSampler, uv, 0.0);
    float3 c = source.rgb;

    // --- light that spills ----------------------------------------------
    //
    // Two widths out of the copy's own mip chain - a near halo about a
    // hundredth of the screen across and a broad one four times that - so a
    // sunlit face, a patch of sky, the glitter on water glows into the air
    // round it without leaving a ghost of its shape. Only what is already
    // bright spills, and it spills warm: the halo is the colour of the light,
    // not of the thing lit.
    const float last = max(gradeChain.x - 1.0, 0.0);
    const float3 haloNear = gradeBlur(uv, min(3.0, last), size);
    const float3 haloFar = gradeBlur(uv, min(5.0, last), size);
    const float3 halo = haloNear * 0.6 + haloFar * 0.4;
    const float spill = smoothstep(0.50, 0.92, gradeLuma(halo));
    const float3 bloom = halo * spill * float3(1.0, 0.80, 0.56) * grade.y;
    c = c + bloom * (1.0 - c);   // screened: it brightens, never clips

    // --- the light's colour -----------------------------------------------
    //
    // A late-afternoon white: highlights lean to gold, the shadows to a warm
    // brown with the least touch of plum in it. Split rather than one tint, so
    // the midtones - most of the ground - keep their own colour.
    float luma = gradeLuma(c);
    const float highlights = smoothstep(0.30, 0.85, luma);
    const float shadows = 1.0 - smoothstep(0.05, 0.45, luma);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(1.05, 1.0, 0.86), highlights);
    c += float3(0.014, 0.006, 0.009) * shadows;

    // --- a print's curve ---------------------------------------------------
    //
    // A little more contrast through the middle, and then the range pulled in
    // at both ends: blacks that never reach black, whites that never quite
    // reach white. The floor is warm, which is what makes the old look old.
    const float3 curved = c * c * (3.0 - 2.0 * c);
    c = lerp(c, curved, 0.20);
    const float3 floorColour = float3(0.040, 0.030, 0.029);
    const float3 ceilingColour = float3(0.975, 0.958, 0.912);
    c = floorColour + saturate(c) * (ceilingColour - floorColour);

    // --- faded blues ------------------------------------------------------
    //
    // Everything a step less saturated, and blue and cyan two steps: a sky
    // that has been in the sun, water that reads as water and not as dye.
    // Warm colours keep most of theirs, so a roof, a fire, a face stay alive.
    luma = gradeLuma(c);
    const float blue = saturate((c.b - max(c.r, c.g)) * 5.0);
    const float warm = saturate((c.r - c.b) * 3.0);
    const float saturation = lerp(0.90, 0.76, blue) + 0.07 * warm;
    c = lerp(float3(luma, luma, luma), c, saturation);

    // --- the edge of the plate ---------------------------------------------
    //
    // Round, not the screen's shape, and darkening towards a warm brown rather
    // than to grey. Starts well out from the middle: a frame, not a tunnel.
    const float aspect = size.x / size.y;
    const float2 centred = (uv - 0.5) * float2(aspect, 1.0);
    const float corner = length(float2(aspect, 1.0) * 0.5);
    const float edge = smoothstep(0.42, 1.12, length(centred) / corner);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(0.72, 0.66, 0.60), edge * grade.z);

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

    c = lerp(source.rgb, saturate(c), saturate(grade.x));
    return float4(c, source.a);
}

