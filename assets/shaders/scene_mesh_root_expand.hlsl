// Expand one placed mesh root into world-space GeometryCluster candidates.
// The output is the exact raw 8-word record consumed as Cluster by cluster_cull.

StructuredBuffer<uint> roots : register(t0, space0);
StructuredBuffer<uint> localClusters : register(t1, space0);
RWStructuredBuffer<uint> candidates : register(u0, space1);

cbuffer Expand : register(b0, space2) {
    uint rootCount;
    uint candidateCount;
    uint pad0;
    uint pad1;
};

float loadFloat(uint value) { return asfloat(value); }
float3 rotateModel(float3 p, float yaw) {
    const float c = cos(yaw), s = sin(yaw);
    return float3(c * p.x - s * p.y, s * p.x + c * p.y, p.z);
}

[numthreads(64, 1, 1)]
void ExpandCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= rootCount) return;
    const uint rootBase = id.x * 12;
    const float3 position = float3(loadFloat(roots[rootBase + 0]),
                                   loadFloat(roots[rootBase + 1]),
                                   loadFloat(roots[rootBase + 2]));
    const float scale = loadFloat(roots[rootBase + 3]);
    const float yaw = loadFloat(roots[rootBase + 4]);
    const uint clusterFirst = roots[rootBase + 5];
    const uint clusterCount = roots[rootBase + 6];
    const uint bucketBase = roots[rootBase + 7];
    const uint outputFirst = roots[rootBase + 8];
    const uint payload = roots[rootBase + 9];
    const float radiusScale = abs(scale);

    for (uint i = 0; i < clusterCount; ++i) {
        const uint output = outputFirst + i;
        if (output >= candidateCount) continue;
        const uint local = (clusterFirst + i) * 12;
        const float3 centre = position + scale * rotateModel(
            float3(loadFloat(localClusters[local + 0]),
                   loadFloat(localClusters[local + 1]),
                   loadFloat(localClusters[local + 2])), yaw);
        candidates[output * 8 + 0] = asuint(centre.x);
        candidates[output * 8 + 1] = asuint(centre.y);
        candidates[output * 8 + 2] = asuint(centre.z);
        candidates[output * 8 + 3] = asuint(loadFloat(localClusters[local + 3]) * radiusScale);
        candidates[output * 8 + 4] = asuint(loadFloat(localClusters[local + 4]) * radiusScale);
        const float parent = loadFloat(localClusters[local + 5]);
        candidates[output * 8 + 5] = asuint(isinf(parent) ? parent : parent * radiusScale);
        candidates[output * 8 + 6] = bucketBase + localClusters[local + 6];
        candidates[output * 8 + 7] = payload;
    }
}
