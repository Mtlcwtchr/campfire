// The sketch of a coast, extruded (game::SketchPass, generation/world_sketch.hpp).
//
// The person's mark, not the world: a flat top where the paint is over the
// shore value and a wall where it crosses it, drawn in the colours of a map
// laid on a table rather than of any real ground - so nobody takes it for
// something that has been worked out.
#include "world.hlsli"

cbuffer SketchPixel : register(b1, space3)
{
    float4 sketch;   // shore value, spare
};

static const float kSketchTop = 180.0;   // metres over the sea the slab stands

struct SketchOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float value : TEXCOORD1;
    float top : TEXCOORD2;
};

SketchOut SketchVS(float4 data : TEXCOORD0)
{
    SketchOut o;
    // A wall's foot goes a little under the water so the sea meets it.
    const float height = data.z > 0.5 ? kSketchTop : -4.0;
    o.world = float3(data.xy, height);
    o.position = project(o.world);
    o.value = data.w;
    o.top = data.z;
    return o;
}

float4 SketchPS(SketchOut input) : SV_Target0
{
    const float shore = sketch.x > 0.0 ? sketch.x : 512.0;
    const float3 dx = ddx(input.world), dy = ddy(input.world);
    float3 normal = normalize(cross(dx, dy));
    if (normal.z < 0.0) normal = -normal;
    const bool wall = normal.z < 0.5;
    float3 colour;
    if (!wall) {
        // The top: only where the sketch is land.
        clip(input.value - shore);
        // Parchment, with the paint's own contours pencilled on it: how firmly
        // the land was painted reads as lines, not as heights nobody chose.
        colour = float3(0.80, 0.72, 0.52);
        const float band = input.value / 96.0;
        const float toLine = abs(frac(band + 0.5) - 0.5);
        const float pencil = 1.0 - smoothstep(0.0, fwidth(band) * 1.2, toLine);
        colour = lerp(colour, float3(0.58, 0.48, 0.32), pencil * 0.35);
        // The coast itself inked along the edge of the top.
        const float edge = 1.0 - smoothstep(0.0, fwidth(input.value) * 2.5, input.value - shore);
        colour = lerp(colour, float3(0.24, 0.18, 0.12), edge);
    } else {
        colour = float3(0.46, 0.37, 0.26);
    }
    const float3 light = normalize(float3(-0.45, -0.35, 0.82));
    const float lit = 0.55 + 0.45 * saturate(dot(normal, light));
    // The scene's exposure, as the ground gets it, so the sketch sits in the
    // picture rather than glowing out of it at dusk.
    const float exposure = lookPS.w > 0.5 ? lookPS.z : 1.0;
    return float4(saturate(colour * lit * exposure), 1.0);
}

