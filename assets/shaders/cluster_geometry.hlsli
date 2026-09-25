#ifndef CLUSTER_GEOMETRY_HLSLI
#define CLUSTER_GEOMETRY_HLSLI
// What the selection step of the virtual geometry layer works on, shared by the
// shaders that reset it and the one that runs it.
//
// The bindings are NOT here, on purpose. A binding index follows what an entry
// point actually uses, not what its file declares: a reset that touches only
// the draw arguments gets them at u0 even if the file also declares a visible
// list, and binding that list first then zeroes the wrong buffer with no error
// anywhere. So each shader states its own bindings and there is one entry point
// per file.

struct Cluster {
    float3 centre;      // world metres
    float radius;       // bounding sphere, world metres
    float error;        // geometric error of THIS cluster, world metres
    float parentError;  // of the coarser cluster this refines; infinity at a root
    uint bucket;        // which draw call it belongs to
    uint payload;       // what that draw needs to find it: instance index, offset
};

// One indexed draw, in the layout the card reads.
struct DrawArguments {
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

cbuffer Selection : register(b0, space2) {
    // Rows of the view-projection, so a sphere can be clipped without a matrix
    // type crossing the binding.
    float4 rowX;
    float4 rowY;
    float4 rowW;
    // How many clusters there are, how many fit in one bucket's slice of the
    // visible list, how many buckets, and the allowance in pixels.
    uint clusterCount;
    uint bucketCapacity;
    uint bucketCount;
    float pixelError;
    // Half the viewport in pixels and the focal term, so an error in metres
    // becomes an error in pixels the same way the CPU does it.
    float halfWidth;
    float halfHeight;
    float focal;
    float pad;
    uint hizEnabled;
    uint hizWidth;
    uint hizHeight;
    uint hizLevels;
    // Clip-space z row, kept after the legacy fields so reset/compact jobs
    // retain their existing uniform offsets.
    float4 rowZ;
};

// Metres to pixels at this depth. A perspective frame divides by depth; an
// orthographic one has a constant scale and says so by passing a focal term of
// zero with the scale in `halfWidth`.
float pixelsPerMetre(float depth) {
    return focal > 0 ? focal / max(depth, 1e-3) : halfWidth;
}

#endif
