// What the two ground shaders share: the morph across a change of level.
#ifndef GROUND_HLSLI
#define GROUND_HLSLI

#include "world.hlsli"

// What one patch of ground says to the vertex stage, at its own slot.
//
// Its own file rather than a corner of world.hlsli because only the two shaders
// that draw the ground are pushed it - a constant buffer nobody fills is not
// zeros, it is whatever was there last - and because the ground and the water
// have to morph together or the water tears away from its own bed.
// The block is sixteen floats wide because that is what the draw pushes; the
// fragment stage reads the same sixteen as RingDraw. Only the first is read
// here.
cbuffer PatchVertex : register(b1, space1)
{
    float4 morphing;   // how far this patch has walked towards the level above it
    float4 morphWindow;
    float4 morphReplacement;
    float4 morphSettings;
};

// How far this point has walked towards the level above it.
//
// A ring mesh has none to walk. Its vertices are polar, so no vertex of one
// level stands where a vertex of the level above stands, and there is nothing
// to walk towards - which is why buildRingMesh gives every vertex itself as its
// own coarse target. The regional wave that used to be faded in over the top of
// that projected the previous frame's triangles on the CPU, and it has gone.
//
// A real morph needs the levels to share a lattice. That arrives with the
// clipmap, not before.
float terrainMorph(float2 worldXY)
{
    return morphing.y < 0.5 ? morphing.x : 0.0;
}

float waterMorphed(float2 worldXY, float level, float2 source)
{
    return morphing.y > 0.5 ? lerp(level, source.x, terrainMorph(worldXY)) : level;
}

// The ground between two levels of detail.
//
// A patch is built at one level and carries, in every vertex, where the level
// above it puts that same point. As the zoom crosses towards that level the
// vertices walk there, so that at the moment the coarser mesh takes over, this
// one is already drawing the coarser mesh exactly - and the swap, which used to
// be the whole view changing shape at once, cannot be seen at all.
//
// Half the vertices do not move: they are the coarse mesh's own vertices, at
// the same world coordinate. It is the ones in between that come up out of the
// hollows and down off the crests, which is precisely the detail the coarser
// level does not have.
float3 morphed(float3 position, float coarseHeight)
{
    return float3(position.xy, lerp(position.z, coarseHeight, terrainMorph(position.xy)));
}

float3 morphedNormal(float3 normal, float3 coarseNormal, float2 worldXY)
{
    return normalize(lerp(normal, coarseNormal, terrainMorph(worldXY)));
}

// And the material with it.
//
// A turn of the ground is scaled with the level, so the same hillside is laid
// out at two sizes by two levels, and measured against the picture that is very
// nearly all of what a change of level looks like - the shape hardly moves at
// all. Walking the coordinate across turns the jump into a slow zoom of the
// ground itself, which nothing in the eye is looking for.
float2 morphedUv(float2 uv, float2 coarseUv)
{
    return morphing.y > 0.5 ? uv : lerp(uv, coarseUv, morphing.x);
}

#endif
