// Compact the source model instances selected by cluster_cull into the vertex
// stream consumed by scene_models.hlsl. The culler owns one fixed visible slice
// per cluster, while the indirect argument's firstInstance points at the
// prefix-sized output slice for that cluster. No GPU -> CPU readback is needed.

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
// DrawArguments is a 20-byte indirect record. Read it as raw uints so the
// storage-buffer stride cannot be rounded differently by a backend: +1 is the
// instance count and +4 is firstInstance.
StructuredBuffer<uint> arguments : register(t1, space0);
// Use a float4 structured view rather than ByteAddressBuffer. Scene instance
// payloads are 64-byte aligned, so byteOffset/16 addresses the same fields;
// the structured view also keeps Metal's storage-buffer binding identical to
// the other gather inputs.
StructuredBuffer<float4> clusterMorph : register(t2, space0);
StructuredBuffer<float4> source : register(t3, space0);
RWStructuredBuffer<float4> compacted : register(u0, space1);

cbuffer Gather : register(b0, space2) {
    uint bucketCapacity;
    uint bucketCount;
    float pixelError;
    float halfWidth;
    float focal;
    float pad0;
    float4 rowW;
};

float3 rotateModel(float3 p,float yaw) {
    float c=cos(yaw),s=sin(yaw);return float3(c*p.x-s*p.y,s*p.x+c*p.y,p.z);
}

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
    ModelInstance value=loadSource(visible[slot]);
    const float4 local=clusterMorph[bucket*2];
    const float3 centre=value.position+value.scale*rotateModel(local.xyz,value.yaw);
    const float depth=dot(rowW,float4(centre,1));
    const float pixelsPerMetre=focal>0 ? focal/max(depth,1e-3) : halfWidth;
    const float own=local.w*value.scale*pixelsPerMetre;
    const float parent=clusterMorph[bucket*2+1].x*value.scale*pixelsPerMetre;
    value.viewBlend=parent>own ? saturate((pixelError-own)/max(parent-own,1e-5)) : 0;
    const uint output = (arguments[argument + 4] + within) * 4;
    compacted[output + 0] = float4(value.position,value.scale);
    compacted[output + 1] = float4(value.yaw,value.phase,value.tint,value.vegetation);
    compacted[output + 2] = float4(value.width,value.height,value.layer,value.mesh);
    compacted[output + 3] = float4(value.mode,value.coverage,value.layerNext,value.viewBlend);
}
