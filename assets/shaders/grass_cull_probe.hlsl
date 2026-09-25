// Offscreen regression of compute-written instance data + an indirect draw.
struct Input {
    float2 corner : TEXCOORD0;
    float3 root : TEXCOORD1;
};
float4 ProbeVS(Input input) : SV_Position {
    return float4(input.root.xy * 0.01 + input.corner * 0.18, 0.5, 1);
}
float4 ProbePS() : SV_Target0 { return float4(0, 1, 0, 1); }

