// Compact selected roots into an ordinary vertex/instance buffer. Rendering
// needs no vertex-stage storage bindings and consumes the culler's indirect count.
ByteAddressBuffer roots : register(t0, space0);
StructuredBuffer<uint> ranks : register(t1, space0);
StructuredBuffer<uint> blocks : register(t2, space0);
RWByteAddressBuffer compacted : register(u0, space1);
cbuffer Input : register(b0, space2) {
    uint rootOffset;
    uint rootCount;
    float unused0;
    float unused1;
};
[numthreads(64, 1, 1)]
void GatherCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    const uint rank = ranks[id.x];
    if ((rank & 1u) == 0) return;
    const uint destination = blocks[id.x / 256] + (rank >> 1) - 1;
    const uint at = rootOffset + id.x * 32;
    compacted.Store4(destination * 32, roots.Load4(at));
    compacted.Store4(destination * 32 + 16, roots.Load4(at + 16));
}

