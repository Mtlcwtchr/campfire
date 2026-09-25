// Turn the per-bucket visibility counts into a dense instance prefix.
//
// Cluster culling keeps a fixed slice per bucket so atomics can write without
// a second pass. The draw, however, only needs the survivors. This one-thread
// prefix pass rewrites firstInstance to the dense output offset, allowing the
// gather buffer to be sized to the submitted cluster budget instead of
// bucketCount * bucketCapacity.

RWStructuredBuffer<uint> arguments : register(u0, space1);

cbuffer Selection : register(b0, space2) {
    float4 rowX;
    float4 rowY;
    float4 rowW;
    uint clusterCount;
    uint bucketCapacity;
    uint bucketCount;
    float pixelError;
    float halfWidth;
    float halfHeight;
    float focal;
    float pad;
};

[numthreads(1, 1, 1)]
void CompactCS(uint3 id : SV_DispatchThreadID) {
    if (id.x != 0) return;
    uint first = 0;
    for (uint bucket = 0; bucket < bucketCount; ++bucket) {
        const uint argument = bucket * 5;
        const uint count = min(arguments[argument + 1], bucketCapacity);
        arguments[argument + 4] = first;
        first += count;
    }
}
