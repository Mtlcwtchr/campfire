// Compact the GPU-selected merged/canopy hierarchy nodes into the ordinary
// scene-model instance stream. The node payload is an offset into the frame's
// InstanceArena, so this pass never has to read back the selected cut.

struct ModelInstance {
    float3 position;
    float scale;
    float yaw;
    float phase;
    float tint;
    float vegetation;
    float width;
    float height;
    float layer;
    float mesh;
    float mode;
    float coverage;
    float layerNext;
    float viewBlend;
};

StructuredBuffer<uint> visible : register(t0, space0);
// DrawArguments is a 20-byte indirect record; raw uint addressing keeps the
// stride identical on Metal, Vulkan and D3D12.
StructuredBuffer<uint> arguments : register(t1, space0);
StructuredBuffer<float4> source : register(t2, space0);
RWStructuredBuffer<float4> compacted : register(u0, space1);

cbuffer Gather : register(b0, space2) {
    uint bucketCapacity;
    uint bucketCount;
    float pad0;
    float pad1;
};

ModelInstance loadSource(uint byteOffset) {
    const uint base = byteOffset / 16;
    ModelInstance value;
    const float4 row0 = source[base + 0];
    const float4 row1 = source[base + 1];
    const float4 row2 = source[base + 2];
    const float4 row3 = source[base + 3];
    value.position = row0.xyz;
    value.scale = row0.w;
    value.yaw = row1.x;
    value.phase = row1.y;
    value.tint = row1.z;
    value.vegetation = row1.w;
    value.width = row2.x;
    value.height = row2.y;
    value.layer = row2.z;
    value.mesh = row2.w;
    value.mode = row3.x;
    value.coverage = row3.y;
    value.layerNext = row3.z;
    value.viewBlend = row3.w;
    return value;
}

[numthreads(64, 1, 1)]
void GatherCS(uint3 id : SV_DispatchThreadID) {
    const uint slot = id.x;
    const uint total = bucketCapacity * bucketCount;
    if (slot >= total) return;
    const uint bucket = slot / bucketCapacity;
    const uint within = slot - bucket * bucketCapacity;
    const uint argument = bucket * 5;
    const uint count = arguments[argument + 1];
    if (within >= count) return;
    const uint output = (arguments[argument + 4] + within) * 4;
    const ModelInstance value = loadSource(visible[slot]);
    compacted[output + 0] = float4(value.position,value.scale);
    compacted[output + 1] = float4(value.yaw,value.phase,value.tint,value.vegetation);
    compacted[output + 2] = float4(value.width,value.height,value.layer,value.mesh);
    compacted[output + 3] = float4(value.mode,value.coverage,value.layerNext,value.viewBlend);
}
