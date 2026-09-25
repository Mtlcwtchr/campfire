// Stable within-block rank. Bit 0 retains membership; upper bits store the
// inclusive prefix. Workgroup shared memory only, no subgroup-size assumptions.
RWStructuredBuffer<uint> ranks : register(u0, space1);
RWStructuredBuffer<uint> blocks : register(u1, space1);
cbuffer Input : register(b0, space2) { uint rootOffset; uint rootCount; float2 unused; };
groupshared uint prefix[256];
[numthreads(256, 1, 1)]
void ScanCS(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint member = id.x < rootCount ? ranks[id.x] : 0;
    prefix[lane] = member;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1; step < 256; step <<= 1) {
        const uint add = lane >= step ? prefix[lane - step] : 0;
        GroupMemoryBarrierWithGroupSync();
        prefix[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    if (id.x < rootCount) ranks[id.x] = (prefix[lane] << 1) | member;
    if (lane == 255) blocks[group.x] = prefix[lane];
}

