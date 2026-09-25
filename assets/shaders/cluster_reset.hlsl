// Emptying the draw arguments at the start of a frame.
//
// Only the instance count. Everything else about a draw - which indices, which
// vertex offset - was decided when the geometry was built and has to survive
// every frame that resets its count.
//
// Its own file with its own single binding, because a binding index follows
// what an entry point uses: sharing a file with the cull would put the
// arguments at u1 for one entry point and u0 for the other.
#include "cluster_geometry.hlsli"

RWStructuredBuffer<DrawArguments> arguments : register(u0, space1);
RWStructuredBuffer<uint> pageFeedback : register(u1, space1);

[numthreads(64, 1, 1)]
void ResetCS(uint3 id : SV_DispatchThreadID) {
    if (id.x < bucketCount) {
        arguments[id.x].instanceCount = 0;
        pageFeedback[id.x] = 0;
    }
}
