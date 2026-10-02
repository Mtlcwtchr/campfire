// What the editor has picked out of the world, as lines over it
// (game::HighlightPass): the water chosen on the Water tab.
#include "world.hlsli"

struct HighlightOut {
    float4 position : SV_Position;
    float along : TEXCOORD0;
};

HighlightOut HighlightVS(float3 p : TEXCOORD0)
{
    HighlightOut o;
    // A little over the surface it marks, so it never sits under the water.
    o.position = project(p + float3(0.0, 0.0, 3.0));
    o.along = p.x + p.y;
    return o;
}

float4 HighlightPS(HighlightOut input) : SV_Target0
{
    // Slowly marching dashes, so a selection reads as a selection and not as
    // a feature of the country.
    const float dash = frac(input.along / 240.0 - viewportPS.z * 0.25);
    const float3 colour = dash < 0.6 ? float3(0.35, 0.95, 1.0) : float3(1.0, 1.0, 1.0);
    return float4(colour, 1.0);
}

