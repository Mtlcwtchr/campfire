// A picture drawn on the processor, put on the screen as one quad.
//
// No geometry: the corners come out of the vertex id, and where the quad sits is
// four numbers pushed for this draw alone - left, top, width and height, in clip
// space.
// The scene, for the size of the window - and for a second reason worth writing
// down. A constant buffer declared at b1 in a shader that has nothing at b0 is
// compacted down to b0 by the compiler, and the push to slot one then goes
// nowhere: the menu came out drawn from uninitialised numbers, which is to say
// not drawn at all. Reading the window size from b0 keeps both slots real.
#include "world.hlsli"

cbuffer MenuVertex : register(b1, space1)
{
    float4 place;   // left, top, width, height, in screen pixels
};

Texture2D pictureTex : register(t0, space2);
SamplerState pictureSampler : register(s0, space2);

struct MenuOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

MenuOut MenuVS(uint id : SV_VertexID)
{
    // Two triangles, as six corners of the unit square.
    const uint2 corners[6] = {uint2(0, 0), uint2(1, 0), uint2(0, 1),
                              uint2(1, 0), uint2(1, 1), uint2(0, 1)};
    const float2 corner = float2(corners[id]);
    MenuOut output;
    const float2 pixel = place.xy + corner * place.zw;
    output.position = float4(pixel.x / viewport.x * 2.0 - 1.0, 1.0 - pixel.y / viewport.y * 2.0,
                             0.0, 1.0);
    output.uv = corner;
    return output;
}

float4 MenuPS(MenuOut input) : SV_Target0
{
    float4 c = pictureTex.Sample(pictureSampler, input.uv);
    // Drawn into a surface that was cleared to nothing, so what was never
    // written is never shown.
    if (c.a <= 0.004) discard;
    return c;
}
