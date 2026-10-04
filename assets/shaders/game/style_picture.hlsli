// This game's grade: the body of the engine's stylePicture() hook (grade.hlsl).
//
// A storybook in late-afternoon light - the Witcher's Velen and Toussaint,
// Clair Obscur's painted plates: gold in the light and blue in the shade,
// colour that is rich without being dye, a cinema's contrast with a deep but
// living black, and the brightest things glowing warm into the air round
// them. The grade leans the light and the shade apart and fills the colour
// in; it does not invent hues the picture did not have.
//
// The grading region's rows (content/config/style/grades.json, "rows" in this
// order) move it from there; every row is a change, so zeros are this look:
//   0 highlight   rgb added to the gold the lights lean to, a = how much
//   1 shadow      rgb added to the blue the shades lean to, a = how much
//   2 bloom_tint  rgb the spill leans to, a = its share (and extra spill)
//   3 curve       x contrast, y saturation, z vignette, w grain (each a change)
#ifndef GAME_STYLE_PICTURE_HLSLI
#define GAME_STYLE_PICTURE_HLSLI

void stylePicture(inout StylePicture p)
{
    const bool regional = gradeRowsPresent();
    const float4 highlight = regional ? gradeRow(0) : 0.0;
    const float4 shadow = regional ? gradeRow(1) : 0.0;
    const float4 bloomTint = regional ? gradeRow(2) : 0.0;
    const float4 curve = regional ? gradeRow(3) : 0.0;
    float3 c = p.colour;

    // --- light that spills ----------------------------------------------
    //
    // Two widths out of the copy's own mip chain - a near halo about a
    // hundredth of the screen across and a broad one four times that - so a
    // sunlit face, the sky by the sun, the glitter on water glows into the
    // air round it. Only what is already bright spills, and it spills gold.
    const float last = p.chainLast;
    const float3 haloNear = gradeBlur(p.uv, min(3.0, last), p.size);
    const float3 haloFar = gradeBlur(p.uv, min(5.0, last), p.size);
    const float3 halo = haloNear * 0.55 + haloFar * 0.45;
    const float spill = smoothstep(0.48, 0.90, gradeLuma(halo));
    const float3 bloom = halo * spill * lerp(float3(1.0, 0.84, 0.60), bloomTint.rgb, saturate(bloomTint.a)) * p.bloom * (1.0 + bloomTint.a);
    c = c + bloom * (1.0 - c);   // screened: it brightens, never clips
    c *= p.brightness;          // the settings' brightness

    // --- gold in the light, blue in the shade -------------------------------
    //
    // Split, so the midtones - most of the ground - keep their own colour:
    // the highlights lean to the sun's gold, the shadows to the sky's blue,
    // which is what makes a shadow read as cool air rather than as dirt.
    // Not the sky itself (its alpha is the far end of the depth encoding):
    // gold over a bright blue sky turned it mint.
    float luma = gradeLuma(c);
    const float skyPixel = smoothstep(0.985, 0.999, p.alpha);
    const float highlights = smoothstep(0.35, 0.90, luma) * (1.0 - skyPixel);
    const float shadows = 1.0 - smoothstep(0.04, 0.42, luma);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(1.05, 1.01, 0.91) + highlight.rgb * highlight.a, highlights);
    c = lerp(c, c * (float3(0.96, 0.99, 1.05) + shadow.rgb * shadow.a) + float3(0.000, 0.003, 0.008), shadows * 0.5);

    // --- a cinema's curve -----------------------------------------------------
    //
    // Contrast through the middle about a pivot a little under mid-grey, a
    // soft toe into a deep black that is not crushed, and a shoulder that
    // rolls the whites off instead of clipping them.
    const float pivot = 0.42;
    float3 graded = pivot + (c - pivot) * (1.16 * p.contrast * (1.0 + curve.x));
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
    c = lerp(float3(luma, luma, luma), c, vibrance * p.saturation * (1.0 + curve.y));
    // Greens kept green: a hair warmer in the light, never pushed to olive.
    const float green = saturate((c.g - max(c.r, c.b)) * 6.0);
    c += float3(0.010, 0.006, -0.008) * green;

    // --- the edge of the frame ----------------------------------------------
    //
    // Round, darkening towards a deep warm brown, and starting well out from
    // the middle: a frame, not a tunnel.
    const float aspect = p.size.x / p.size.y;
    const float2 centred = (p.uv - 0.5) * float2(aspect, 1.0);
    const float corner = length(float2(aspect, 1.0) * 0.5);
    const float edge = smoothstep(0.45, 1.15, length(centred) / corner);
    c *= lerp(float3(1.0, 1.0, 1.0), float3(0.70, 0.64, 0.60), edge * p.vignette * (1.0 + curve.z));

    // --- grain --------------------------------------------------------------
    //
    // Triangular, so it has no bias, changing twenty-four times a second as
    // film would, and strongest in the shadows where film shows it. It also
    // dithers away any banding the eight-bit target would put in a sky.
    const float2 pixel = p.pixel;
    const float tick = floor(frac(viewportPS.z / 64.0) * 64.0 * 24.0);
    const float2 seed = pixel + float2(tick * 7.13, tick * 3.71);
    const float noise = gradeHash(seed) + gradeHash(seed.yx + 17.0) - 1.0;
    c += noise * p.grain * (1.0 + curve.w) * (0.45 + 0.55 * (1.0 - luma));

    p.colour = c;
}

#endif
