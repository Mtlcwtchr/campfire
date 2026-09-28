// The frozen cull camera of the scene view, drawn as lines: four rays from the
// eye and the rectangle where they end. The corners come from the CPU in the
// per-draw block, the vertex id picks which one.
#include "world.hlsli"

cbuffer FrustumVertex : register(b1, space1)
{
    float4 points[4]; // eye.xyz + c0.x | c0.yz + c1.xy | c1.z + c2.xyz | c3.xyz + spare
};

struct FrustumOut { float4 position : SV_Position; float shade : TEXCOORD0; };

float3 frustumPoint(uint i)
{
    const float values[16] = {points[0].x, points[0].y, points[0].z, points[0].w,
                              points[1].x, points[1].y, points[1].z, points[1].w,
                              points[2].x, points[2].y, points[2].z, points[2].w,
                              points[3].x, points[3].y, points[3].z, points[3].w};
    return float3(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
}

FrustumOut FrustumVS(uint id : SV_VertexID)
{
    // Eight lines: eye to each corner, then the far rectangle.
    const uint ends[16] = {0, 1, 0, 2, 0, 3, 0, 4, 1, 2, 2, 3, 3, 4, 4, 1};
    FrustumOut o;
    o.position = project(frustumPoint(ends[id]));
    o.shade = id < 8 ? 0.75 : 1.0;
    return o;
}

float4 FrustumPS(FrustumOut input) : SV_Target0
{
    return float4(float3(1.0, 0.86, 0.2) * input.shade, 1.0);
}

