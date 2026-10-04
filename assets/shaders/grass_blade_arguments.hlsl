// The blades' indirect draw (foliage_pages.hlsl BladeVS): the same survivors
// as the grass cards' draw, with the clump mesh's index count.
ByteAddressBuffer arguments : register(t0, space0);
RWByteAddressBuffer blades : register(u0, space1);
cbuffer Input : register(b0, space2) { uint indexCount; uint3 unused; };
[numthreads(1, 1, 1)]
void BladeArgumentsCS() {
    blades.Store(0, indexCount);
    blades.Store(4, arguments.Load(4));
    blades.Store(8, 0);
    blades.Store(12, 0);
    blades.Store(16, 0);
}
