// This game's surface style: the body of the engine's styleSurface() hook
// (environment_style.hlsli), after fantasy_80s_art_asset_style_spec §3, §8:
//
//   baseColor -> palette shaping -> shadow tint -> highlight tint
//             -> macro variation -> print response
//
// Rows (content/config/style/palettes.json, "rows" in this order). Every row
// is a change from the unstyled colour, so zeros are "as before":
//   0 shadow_tint      rgb tint the darks lean to, a = strength
//   1 highlight_tint   rgb tint the lights lean to, a = strength
//   2 shaping          x saturation change, y value change, z vegetation
//                      saturation change, w rock warm/cool split
//   3 macro            x amplitude, y metres across, z warmth amplitude, w print grain
//   4 print            x posterize amount, y levels, z, w spare
//   5-7 spare
#ifndef GAME_STYLE_SURFACE_HLSLI
#define GAME_STYLE_SURFACE_HLSLI

#include "../noise.hlsli"

void styleSurface(inout StyleSurface s)
{
    float3 c = s.colour;
    const float4 shadowTint = styleRow(0), highlightTint = styleRow(1);
    const float4 shaping = styleRow(2), macro = styleRow(3), print = styleRow(4);

    // Palette shaping: broad saturation and value, more or less for leaves.
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float saturation = 1.0 + shaping.x + shaping.z * s.vegetation;
    c = max(lerp(lum.xxx, c, saturation), 0.0) * (1.0 + shaping.y);

    // Stone is never a uniform grey: warm faces, cool shadow planes.
    if (shaping.w != 0.0) {
        const float rock = s.kind == kStyleRock ? 1.0 : s.rockShare;
        const float warm = (lum - 0.25) * 2.0;
        c *= 1.0 + float3(0.06, 0.0, -0.06) * warm * shaping.w * rock;
    }

    // Shadow and highlight tints, split by luminance before light: the darks
    // of the albedo lean cool, the lights warm.
    lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float darks = 1.0 - smoothstep(0.08, 0.35, lum);
    const float lights = smoothstep(0.35, 0.75, lum);
    c = lerp(c, c * shadowTint.rgb * 1.6, darks * shadowTint.a);
    c = lerp(c, c * highlightTint.rgb + highlightTint.rgb * 0.03, lights * highlightTint.a);

    // Macro variation: broad patches of value and warmth, never per pixel.
    if (macro.x != 0.0 || macro.z != 0.0) {
        const float metres = max(macro.y, 4.0);
        const float n = noiseAt(s.worldPos.xy / metres) * 2.0 - 1.0;
        const float m = noiseAt(s.worldPos.xy / (metres * 0.37) + 17.3) * 2.0 - 1.0;
        c *= 1.0 + n * macro.x;
        c *= 1.0 + float3(1.0, 0.0, -1.0) * m * macro.z;
    }

    // Print response: a light, partial quantisation and a fixed grain in the
    // world, so it holds still as the camera moves.
    if (print.x > 0.0) {
        const float levels = max(print.y, 4.0);
        c = lerp(c, round(c * levels) / levels, print.x);
    }
    if (macro.w > 0.0) c *= 1.0 + (hashAt(floor(s.worldPos.xy * 8.0)) - 0.5) * macro.w;

    s.colour = max(c, 0.0);
}

#endif
