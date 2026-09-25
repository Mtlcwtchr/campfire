// At the fixed 131072-root budget there are at most 512 blocks. One 256-thread
// group scans pairs and replaces block totals with exclusive global offsets.
RWStructuredBuffer<uint> blocks : register(u0, space1);
cbuffer Input : register(b0, space2) { uint rootOffset; uint rootCount; float2 unused; };
groupshared uint prefix[256];
[numthreads(256, 1, 1)]
void PrefixCS(uint lane : SV_GroupIndex) {
    const uint count = (rootCount + 255) / 256;
    const uint first = lane * 2;
    const uint a = first < count ? blocks[first] : 0;
    const uint b = first + 1 < count ? blocks[first + 1] : 0;
    prefix[lane] = a + b;
    GroupMemoryBarrierWithGroupSync();
    for (uint step = 1; step < 256; step <<= 1) {
        const uint add = lane >= step ? prefix[lane - step] : 0;
        GroupMemoryBarrierWithGroupSync();
        prefix[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint base = prefix[lane] - a - b;
    if (first < count) blocks[first] = base;
    if (first + 1 < count) blocks[first + 1] = base + a;
}

