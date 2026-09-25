// A probe, not a material: the smallest thing that can prove an indirect draw
// did what its arguments said.
//
// Every number a draw argument carries has to be visible in the picture, so
// nothing here comes from a constant buffer and nothing is computed. A corner
// comes from the vertex stream, where the quad sits comes from the instance
// stream, and the colour says which instance drew it - so a wrong vertex
// offset, a wrong first index and a wrong first instance each show as a
// different wrong picture rather than as the same blank one.
struct ProbeOut {
    float4 position : SV_Position;
    float4 tint : TEXCOORD0;
};

ProbeOut ProbeVS(float2 corner : TEXCOORD0, float2 place : TEXCOORD1, float4 tint : TEXCOORD2)
{
    ProbeOut output;
    output.position = float4(corner + place, 0.0, 1.0);
    output.tint = tint;
    return output;
}

float4 ProbePS(ProbeOut input) : SV_Target0
{
    return input.tint;
}
