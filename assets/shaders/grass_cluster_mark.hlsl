StructuredBuffer<uint> visible : register(t0, space0);
ByteAddressBuffer arguments : register(t1, space0);
RWStructuredBuffer<uint> ranks : register(u0, space1);
cbuffer Input : register(b0, space2) { uint rootOffset; uint rootCount; float2 unused; };
[numthreads(64, 1, 1)]
void MarkCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= min(arguments.Load(4), rootCount)) return;
    const uint source = visible[id.x];
    if (source < rootCount) ranks[source] = 1;
}

