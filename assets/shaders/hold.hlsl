// The last picture of a world that was replaced, held over the new one until
// its ground is in (HoldPass, Stage::Surface). A copy, drawn as it was; the
// alpha - the opaque world's encoded view distance - goes with it, so the
// grade and the fog that read it after this see the old picture whole.
#include "world.hlsli"

Texture2D heldTex : register(t0, space2);
SamplerState heldSampler : register(s0, space2);

struct HoldOut {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

HoldOut HoldVS(uint id : SV_VertexID)
{
    // One triangle over the whole screen.
    HoldOut output;
    const float2 corner = float2(id == 2 ? 2.0 : 0.0, id == 1 ? 2.0 : 0.0);
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(corner.x, 1.0 - corner.y);
    return output;
}

float4 HoldPS(HoldOut input) : SV_Target0
{
    return heldTex.SampleLevel(heldSampler, input.uv, 0.0);
}
