// Traverse one mesh DAG root on the GPU. Each root owns a fixed output range
// equal to its local cluster count; unselected entries become invalid bucket
// sentinels, so the shared culler can still apply frustum and budget guards.

StructuredBuffer<uint> roots : register(t0, space0);
StructuredBuffer<uint> clusters : register(t1, space0);
StructuredBuffer<uint> families : register(t2, space0);
StructuredBuffer<uint> familyChildren : register(t3, space0);
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

static const uint kNoFamily = 0xffffffff;
float loadFloat(uint value) { return asfloat(value); }
float pixelsPerMetre(float depth) {
    return focal > 0 ? focal / max(depth, 1e-3) : halfWidth;
}
float3 rotateModel(float3 p, float yaw) {
    const float c = cos(yaw), s = sin(yaw);
    return float3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

void invalidate(uint output) {
    candidates[output * 8 + 0] = asuint(0.0);
    candidates[output * 8 + 1] = asuint(0.0);
    candidates[output * 8 + 2] = asuint(0.0);
    candidates[output * 8 + 3] = asuint(0.0);
    candidates[output * 8 + 4] = asuint(0.0);
    candidates[output * 8 + 5] = asuint(0.0);
    candidates[output * 8 + 6] = kNoFamily;
    candidates[output * 8 + 7] = 0;
}

[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    const uint root = id.x * 12;
    const float3 position = float3(loadFloat(roots[root + 0]),
                                   loadFloat(roots[root + 1]),
                                   loadFloat(roots[root + 2]));
    const float scale = loadFloat(roots[root + 3]);
    const float yaw = loadFloat(roots[root + 4]);
    const uint clusterFirst = roots[root + 5];
    const uint clusterCount = roots[root + 6];
    const uint bucketBase = roots[root + 7];
    const uint outputFirst = roots[root + 8];
    const uint payload = roots[root + 9];
    const uint familyFirst = roots[root + 10];

    for (uint i = 0; i < clusterCount; ++i) {
        const uint output = outputFirst + i;
        if (output < candidateCount) invalidate(output);
    }

    uint stack[256];
    uint stackSize = 0;
    for (uint i = 0; i < clusterCount; ++i) {
        const uint local = (clusterFirst + i) * 12;
        if (isinf(loadFloat(clusters[local + 5])) && stackSize < 256)
            stack[stackSize++] = i;
    }
    // Each family is expanded once. Every parent in a family's group pushes
    // the same children, so in a DAG k parents a level make k^depth paths: a
    // tree beside the eye, refined to its finest level, was hundreds of
    // thousands of steps for one thread - seconds of GPU, a command buffer
    // past its watchdog, and every frame after it black. Visiting a family
    // twice only wrote the same output slots again (each cluster's slot is
    // its own), so this selects exactly what it did. The step cap is a last
    // guard for meshes with more families than the mask holds.
    uint expanded[64];   // 2048 families
    for (uint w = 0; w < 64; ++w) expanded[w] = 0;
    uint steps = 0;
    while (stackSize > 0 && steps < 32768) {
        ++steps;
        const uint idLocal = stack[--stackSize];
        const uint local = (clusterFirst + idLocal) * 12;
        const float3 localCentre = float3(loadFloat(clusters[local + 0]),
                                          loadFloat(clusters[local + 1]),
                                          loadFloat(clusters[local + 2]));
        const float3 centre = position + scale * rotateModel(localCentre, yaw);
        const float depth = dot(rowW, float4(centre, 1));
        const float scalePixels = pixelsPerMetre(depth);
        const float error = loadFloat(clusters[local + 4]) * abs(scale) * scalePixels;
        if (error <= pixelError) {
            const uint output = outputFirst + idLocal;
            if (output >= candidateCount) continue;
            candidates[output * 8 + 0] = asuint(centre.x);
            candidates[output * 8 + 1] = asuint(centre.y);
            candidates[output * 8 + 2] = asuint(centre.z);
            candidates[output * 8 + 3] = asuint(loadFloat(clusters[local + 3]) * abs(scale));
            candidates[output * 8 + 4] = asuint(loadFloat(clusters[local + 4]) * abs(scale));
            candidates[output * 8 + 5] = asuint(loadFloat(clusters[local + 5]) * abs(scale));
            candidates[output * 8 + 6] = bucketBase + clusters[local + 6];
            candidates[output * 8 + 7] = payload;
            continue;
        }
        const uint family = clusters[local + 8];
        if (family == kNoFamily) continue;
        if (family < 2048u) {
            const uint bit = 1u << (family & 31u);
            if ((expanded[family >> 5] & bit) != 0u) continue;
            expanded[family >> 5] |= bit;
        }
        const uint range = (familyFirst + family) * 2;
        const uint childFirst = families[range + 0];
        const uint childCount = families[range + 1];
        for (uint child = 0; child < childCount; ++child)
            if (stackSize < 256)
                stack[stackSize++] = familyChildren[childFirst + child];
    }
}
