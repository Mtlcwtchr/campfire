// Sprites: one quad, drawn once per card, expanded by the shader.
//
// The mesh is four corners and nothing else. Everything that makes one card
// different from the next - where it stands, how big it is, which way it faces,
// which picture it shows, what colour it is tinted - arrives per instance, so a
// crowd of any size is one draw call.
#include "world.hlsli"
#include "landscape_look.hlsli"

// A page each, rather than a packed atlas with texture coordinates chased into
// it. Same idea, done by the hardware: no packer, no bleeding between
// neighbours at a mip level, and a card can still take a piece of its own page
// through the uv rectangle below.
Texture2DArray pagesTex : register(t0, space2);
SamplerState pagesSampler : register(s0, space2);

struct SpriteIn {
    float2 corner : TEXCOORD0;    // -1..1 across, 0..1 up
    float2 uv : TEXCOORD1;
    float3 position : TEXCOORD2;  // world metres
    float2 size : TEXCOORD3;      // metres across, metres tall
    float rotation : TEXCOORD4;
    float4 tint : TEXCOORD5;
    float4 piece : TEXCOORD6;     // u0, v0, u1, v1
    float page : TEXCOORD7;
    float mode : TEXCOORD8;       // 0 upright, 1 laid on the ground
};

struct SpriteOut {
    float4 position : SV_Position;
    float3 uv : TEXCOORD0;        // page in z
    float4 tint : TEXCOORD1;
    float3 worldPosition : TEXCOORD2;
};

SpriteOut SpriteVS(SpriteIn input)
{
    SpriteOut output;
    if (input.mode > 0.5)
    {
        // Laid on the ground: real geometry in the world's own plane, turned
        // about the up axis. Correct under any projection, which is why a road
        // or a shadow is drawn this way rather than as a card - if the view is
        // ever tipped, this is what still lies on the hillside.
        const float s = sin(input.rotation), c = cos(input.rotation);
        const float2 flat = float2(input.corner.x * input.size.x * 0.5,
                                   (input.corner.y - 0.5) * input.size.y);
        const float3 at = input.position +
                          float3(flat.x * c - flat.y * s, flat.x * s + flat.y * c, 0.0);
        output.position = project(at);
        output.worldPosition = at;
    }
    else
    {
        // Upright: a card anchored at a point on the ground, keeping its size on
        // screen. Expanded after the projection rather than before it, which is
        // what makes it face the eye without needing to know where the eye is -
        // and it is the honest description of the art, which is drawn from one
        // side and does not turn.
        //
        // Divided by w so that the day the matrix gains perspective a card in
        // the distance shrinks like everything else.
        float4 anchor = project(input.position);
        const float2 across = float2(input.corner.x * input.size.x * 0.5,
                                     input.corner.y * input.size.y);
        const float2 pixels = across * camera.w;
        anchor.xy += float2(pixels.x * 2.0 / viewport.x, pixels.y * 2.0 / viewport.y) *
                     max(anchor.w, 0.0001);
        output.position = anchor;
        output.worldPosition = input.position;
    }
    output.uv = float3(lerp(input.piece.xy, input.piece.zw, input.uv), input.page);
    output.tint = input.tint;
    return output;
}

float4 SpritePS(SpriteOut input) : SV_Target0
{
    float4 c = pagesTex.Sample(pagesSampler, input.uv) * input.tint;
    // Cut out rather than blended, for the pipeline that writes depth: a crowd
    // is not sorted, and a blended card that writes depth hides the one behind
    // it whichever order they happen to arrive in.
    clip(c.a - 0.35);
    return float4(landscapeFinish(c.rgb, input.worldPosition), 1.0);
}

// The same, blended and not writing depth. For the few things that want a soft
// edge and are few enough to sort.
float4 SpriteSoftPS(SpriteOut input) : SV_Target0
{
    float4 c = pagesTex.Sample(pagesSampler, input.uv) * input.tint;
    clip(c.a - 0.004);
    c.rgb = landscapeFinish(c.rgb, input.worldPosition);
    return c;
}
