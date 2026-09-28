cbuffer View : register(b0, space1) { row_major float4x4 viewProjection; };
struct MeshOut { float4 position : SV_Position; float3 world : TEXCOORD0; };
MeshOut meshVs(float3 position : TEXCOORD0) {
    MeshOut result;
    result.position = mul(viewProjection, float4(position, 1));
    result.world = position;
    return result;
}
float4 meshPs(MeshOut input) : SV_Target0 {
    float3 n = normalize(cross(ddx(input.world), ddy(input.world)));
    if (n.z < 0) n = -n;
    float height = saturate(input.world.z / 3500.0);
    float3 colour = lerp(float3(0.20, 0.38, 0.25), float3(0.68, 0.64, 0.63), height);
    float light = 0.45 + 0.55 * saturate(dot(n, normalize(float3(-0.4, -0.6, 0.8))));
    return float4(colour * light, 1);
}
float4 wirePs(MeshOut input) : SV_Target0 { return float4(0.48, 0.77, 0.63, 1); }

Texture2D<float4> uiTexture : register(t0, space2);
SamplerState uiSampler : register(s0, space2);
struct OverlayOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };
OverlayOut overlayVs(uint id : SV_VertexID) {
    OverlayOut result;
    result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return result;
}
float4 overlayPs(OverlayOut input) : SV_Target0 {
    // SDL's transparent software target contains premultiplied colour. The
    // engine's standard alpha pipeline expects straight colour at its input.
    float4 p = uiTexture.Sample(uiSampler, input.uv);
    return float4(p.rgb / max(p.a, 1e-6), p.a);
}
