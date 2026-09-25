#ifndef RING_REVEAL_HLSLI
#define RING_REVEAL_HLSLI

// What a draw says about itself, at the fragment stage.
//
// This used to hold a radial reveal as well: an expanding frontier and an
// ordered dither, so ground that was ready was clipped away until the circle
// reached it. That is gone. A patch is drawn the moment it exists, and what
// is not there yet is covered by the coarser ground behind it rather than by
// a hole with a curtain over it.
//
// The block stays because the draw carries sixteen floats to both stages and
// the water reads its own from here.
cbuffer RingDraw : register(b1, space3)
{
    float4 ringState;       // x: morph, y: this is a ring mesh, z: retained snapshot
    float4 ringWindow;
    float4 ringReplacement;
    float4 ringReserved;
};

#endif
