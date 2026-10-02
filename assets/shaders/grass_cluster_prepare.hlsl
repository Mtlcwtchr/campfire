// Bounds for the existing 32-byte PageGrassRoot layout. No terrain resampling:
// all four stored heights bound every source/parent/stage morph combination.
ByteAddressBuffer roots : register(t0, space0);
RWByteAddressBuffer clusters : register(u0, space1);
RWStructuredBuffer<uint> ranks : register(u1, space1);
cbuffer Input : register(b0, space2) {
    uint rootOffset;
    uint rootCount;
    float windReach;
    float unused;
};
[numthreads(64, 1, 1)]
void PrepareCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    ranks[id.x] = 0;
    const uint at = rootOffset + id.x * 32;
    const float4 a = asfloat(roots.Load4(at));       // position, parent height
    const float4 b = asfloat(roots.Load4(at + 16));  // prior heights, upright, run
    const float low = min(min(a.z, a.w), min(b.x, b.y));
    const float high = max(max(a.z, a.w), max(b.x, b.y));
    const uint code = (uint)(b.w + 0.5);
    const float cell = (float)(1u << ((code >> 6) & 15u));
    // foliageCommunity.height <= 1.15; random scale <= .85; coarse width caps
    // at cell 64. FoliageVS caps height at 1.3*1.9. windReach is the analytic
    // displacement bound of FoliageVS, not a bound on this frame's gust alone.
    // A fine root's turf cards stand up to 0.8 m off it (world::kTurfReach).
    const float width = 0.85 * 1.15 * (cell > 2.5 ? min(cell, 64.0) * 0.32 : 1.0) + (cell > 2.5 ? 0.0 : 0.8);
    const float halfHeight = (high - low + 2.47) * 0.5;
    const float3 centre = float3(a.xy, (high + low + 2.47) * 0.5 - 0.025);
    const float radius = length(float2(width + windReach, halfHeight)) + 0.01;
    clusters.Store4(id.x * 32, asuint(float4(centre, radius)));
    clusters.Store4(id.x * 32 + 16, uint4(0, 0x7f800000u, 0, id.x));
}

