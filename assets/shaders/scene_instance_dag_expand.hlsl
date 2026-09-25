// Traverse explicit instance-hierarchy adjacency. Individual and density nodes
// carry an invalid bucket; they are still traversed so a replacement parent can
// resolve to the correct valid merged/canopy node without CPU candidate packing.

StructuredBuffer<uint> nodes : register(t0, space0);
StructuredBuffer<uint> children : register(t1, space0);
StructuredBuffer<uint> roots : register(t2, space0);
RWStructuredBuffer<uint> candidates : register(u0, space1);

cbuffer Selection : register(b0, space2) {
    uint rootCount;
    uint candidateCount;
    uint pad0;
    uint pad1;
    float4 rowX;
    float4 rowY;
    float4 rowW;
    float pixelError;
    float halfWidth;
    float halfHeight;
    float focal;
    float pad2;
};

float loadFloat(uint value) { return asfloat(value); }
float pixelsPerMetre(float depth) {
    return focal > 0 ? focal / max(depth, 1e-3) : halfWidth;
}
void invalidate(uint output) {
    candidates[output * 8 + 0] = asuint(0.0);
    candidates[output * 8 + 1] = asuint(0.0);
    candidates[output * 8 + 2] = asuint(0.0);
    candidates[output * 8 + 3] = asuint(0.0);
    candidates[output * 8 + 4] = asuint(0.0);
    candidates[output * 8 + 5] = asuint(0.0);
    candidates[output * 8 + 6] = 0xffffffff;
    candidates[output * 8 + 7] = 0;
}

[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    const uint root = id.x * 4;
    const uint nodeFirst = roots[root + 0];
    const uint nodeCount = roots[root + 1];
    const uint outputFirst = roots[root + 2];
    for (uint i = 0; i < nodeCount; ++i)
        if (outputFirst + i < candidateCount) invalidate(outputFirst + i);

    uint stack[256];
    uint stackSize = 0;
    for (uint i = 0; i < nodeCount; ++i) {
        const uint node = (nodeFirst + i) * 12;
        if (isinf(loadFloat(nodes[node + 5])) && stackSize < 256) stack[stackSize++] = i;
    }
    while (stackSize > 0) {
        const uint localId = stack[--stackSize];
        const uint node = (nodeFirst + localId) * 12;
        const float3 centre = float3(loadFloat(nodes[node + 0]),
                                     loadFloat(nodes[node + 1]),
                                     loadFloat(nodes[node + 2]));
        const float depth = dot(rowW, float4(centre, 1));
        const float error = loadFloat(nodes[node + 4]) * pixelsPerMetre(depth);
        if (error <= pixelError) {
            const uint output = outputFirst + localId;
            if (output >= candidateCount || nodes[node + 6] == 0xffffffff) continue;
            candidates[output * 8 + 0] = nodes[node + 0];
            candidates[output * 8 + 1] = nodes[node + 1];
            candidates[output * 8 + 2] = nodes[node + 2];
            candidates[output * 8 + 3] = nodes[node + 3];
            candidates[output * 8 + 4] = nodes[node + 4];
            candidates[output * 8 + 5] = nodes[node + 5];
            candidates[output * 8 + 6] = nodes[node + 6];
            candidates[output * 8 + 7] = nodes[node + 11];
            continue;
        }
        const uint childFirst = nodes[node + 8];
        const uint childCount = nodes[node + 9];
        for (uint child = 0; child < childCount; ++child)
            if (stackSize < 256) stack[stackSize++] = children[childFirst + child];
    }
}
