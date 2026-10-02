// Choosing what to draw, on the card.
//
// This is the selection step of the virtual geometry layer: the frame hands it
// every cluster that could be drawn, and it writes the ones that survive plus
// the arguments of the draws that will read them. Nothing comes back to the CPU.
//
// The criterion is a cut, not a threshold. A cluster is drawn when its own
// projected error is small enough AND its parent's is not - so exactly one
// level of a hierarchy is chosen along every path, with no gaps and no overlap.
// Set a parent error of infinity and the same shader selects from a flat list,
// which is what a scatter of instances is; give it a real parent and it cuts a
// DAG. One rule, so a flat list and a hierarchy cannot disagree about what
// "enough detail" means.
//
// Bindings follow SDL's compute layout: read-only storage buffers at t, space0;
// read-write at u, space1; uniforms at b, space2.

#include "cluster_geometry.hlsli"

StructuredBuffer<Cluster> clusters : register(t0, space0);
StructuredBuffer<float> hiz : register(t1, space0);
StructuredBuffer<uint> pageTable : register(t2, space0);

RWStructuredBuffer<uint> visible : register(u0, space1);
RWStructuredBuffer<DrawArguments> arguments : register(u1, space1);
RWStructuredBuffer<uint> pageFeedback : register(u2, space1);

bool outsidePlane(float4 plane, Cluster cluster) {
    return dot(plane, float4(cluster.centre, 1)) < -cluster.radius * length(plane.xyz);
}

bool occludedByHiZ(Cluster cluster) {
    if (hizEnabled == 0 || hizLevels == 0) return false;
    const float4 clip = float4(cluster.centre, 1);
    const float clipW = dot(rowW, clip);
    const float clipZ = dot(rowZ, clip);
    if (clipW <= 0 || length(rowZ.xyz) < 1e-6) return false;
    const float2 ndc = float2(dot(rowX, clip), dot(rowY, clip)) / clipW;
    const float2 radiusNdc = float2(length(rowX.xyz), length(rowY.xyz)) * cluster.radius / clipW;
    const float2 uv0 = ndc * 0.5 + 0.5;
    const float2 uvRadius = radiusNdc * 0.5;
    const float2 minUv = saturate(uv0 - uvRadius);
    const float2 maxUv = saturate(uv0 + uvRadius);
    const float extentPixels = max((maxUv.x - minUv.x) * hizWidth,
                                   (maxUv.y - minUv.y) * hizHeight);
    const uint mip = min(hizLevels - 1, (uint)max(0.0, ceil(log2(max(1.0, extentPixels)))));
    const uint mipWidth = max(1, hizWidth >> mip);
    const uint mipHeight = max(1, hizHeight >> mip);
    uint mipBase = 0;
    for (uint level = 0; level < mip; ++level)
        mipBase += max(1, hizWidth >> level) * max(1, hizHeight >> level);
    const uint2 a = min(uint2(minUv * float2(mipWidth, mipHeight)),
                        uint2(mipWidth - 1, mipHeight - 1));
    const uint2 b = min(uint2(maxUv * float2(mipWidth, mipHeight)),
                        uint2(mipWidth - 1, mipHeight - 1));
    // Reversed depth: the farthest of the four is the smallest, and the
    // cluster's nearest point the largest.
    const float z = min(min(hiz[mipBase + a.y * mipWidth + a.x],
                            hiz[mipBase + a.y * mipWidth + b.x]),
                        min(hiz[mipBase + b.y * mipWidth + a.x],
                            hiz[mipBase + b.y * mipWidth + b.x]));
    const float nearest = (clipZ + cluster.radius * length(rowZ.xyz)) / clipW;
    return nearest < z - 1e-4;
}

[numthreads(64, 1, 1)]
void CullCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= clusterCount) return;
    const Cluster cluster = clusters[id.x];

    const float depth = dot(rowW, float4(cluster.centre, 1));
    // Behind the camera, entirely. The radius is included so a cluster whose
    // centre is behind the eye but which still crosses the screen survives.
    if (outsidePlane(rowW, cluster)) return;
    // Clip inequalities are w +/- x/y >= 0, not |x| <= w + radius*|rowX|.
    // The latter misses the depth component of each side plane's normal and
    // can discard a sphere whose centre is outside but geometry is on screen.
    if (outsidePlane(rowW + rowX, cluster) || outsidePlane(rowW - rowX, cluster) ||
        outsidePlane(rowW + rowY, cluster) || outsidePlane(rowW - rowY, cluster)) return;
    if (occludedByHiZ(cluster)) return;

    if (cluster.bucket >= bucketCount) return;
    const uint pageState = pageTable[cluster.bucket];
    if (pageState == 0) {
        pageFeedback[cluster.bucket] = 1;
        return;
    }

    // The cut. Its own error has to fit in the allowance and its parent's has
    // to fail it, which is what picks exactly one level along every path.
    const float scale = pixelsPerMetre(depth);
    if (pageState != 2) {
        const bool fine = cluster.error * scale <= pixelError;
        const bool coarserWouldNot = cluster.parentError * scale > pixelError;
        if (!fine || !coarserWouldNot) return;
    }
    uint slot = 0;
    InterlockedAdd(arguments[cluster.bucket].instanceCount, 1, slot);
    // A bucket that overflows drops the extra rather than writing past its
    // slice. The count is corrected so the draw never reads what was not
    // written; an overflowing frame is a budget to raise, not memory to
    // corrupt, and it shows up as a bucket pinned at its capacity.
    if (slot >= bucketCapacity) {
        InterlockedMin(arguments[cluster.bucket].instanceCount, bucketCapacity);
        return;
    }
    visible[cluster.bucket * bucketCapacity + slot] = cluster.payload;
}
