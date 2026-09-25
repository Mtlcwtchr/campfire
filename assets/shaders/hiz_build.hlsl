// Max-depth pyramid. The depth target uses the normal [0,1] depth convention:
// a larger value is farther, so a max reduction is conservative for occlusion.

Texture2D<float> depthSource : register(t0, space0);
// This entry point binds only one read buffer. Keeping it at t0 is important
// on Metal: register t1 can alias the u0 storage slot after shadercross strips
// resources that are unused by the other entry point in this file.
StructuredBuffer<float> source : register(t0, space0);
RWStructuredBuffer<float> target : register(u0, space1);

cbuffer Pyramid : register(b0, space2) {
    uint width;
    uint height;
    uint sourceWidth;
    uint sourceHeight;
    uint sourceBase;
    uint targetBase;
};

[numthreads(8, 8, 1)]
void BuildMip0CS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    target[targetBase + id.y * width + id.x] = depthSource.Load(int3(id.xy, 0));
}

[numthreads(8, 8, 1)]
void BuildMipCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const uint2 base = id.xy * 2;
    const uint2 extent = uint2(max(1, sourceWidth), max(1, sourceHeight));
    const uint2 a = min(base, extent - 1);
    const uint2 b = min(base + uint2(1, 0), extent - 1);
    const uint2 c = min(base + uint2(0, 1), extent - 1);
    const uint2 d = min(base + uint2(1, 1), extent - 1);
    target[targetBase + id.y * width + id.x] =
        max(max(source[sourceBase + a.y * sourceWidth + a.x],
                source[sourceBase + b.y * sourceWidth + b.x]),
            max(source[sourceBase + c.y * sourceWidth + c.x],
                source[sourceBase + d.y * sourceWidth + d.x]));
}

[numthreads(8, 8, 1)]
void CopyMipCS(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    target[targetBase + id.y * width + id.x] = source[id.y * width + id.x];
}
