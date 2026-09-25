#pragma once
// The climate of the whole world, as three pictures the vertex stage samples.
//
// This used to ride on the vertex: eleven numbers a corner, fifty-two bytes,
// carried through every level of every tile. It is a field whose samples are
// sixty-four metres apart and which never changes after the map is raised, so
// on a four-metre mesh those fifty-two bytes were the same answer written out
// two hundred and fifty-six times over. Here it is one texel, read where the
// vertex stands.
//
//   plane 0: steppe, boreal, temperate, tropical
//   plane 1: desert, temperature, fertility, moisture
//   plane 2: wind x, wind y, drainage, forest density
//
// Wind is stored about a half, because it blows west and south too; the span
// has to match world::ClimateField::kSignedSpan.

#include "world.hlsli"

Texture2D climatePlane0 : register(t0, space0);
Texture2D climatePlane1 : register(t1, space0);
Texture2D climatePlane2 : register(t2, space0);
SamplerState climateSampler : register(s0, space0);
SamplerState climateSampler1 : register(s1, space0);
SamplerState climateSampler2 : register(s2, space0);

// parameters[20] is where the world tells the card how to find itself in these
// pictures: world metres times xy, plus zw, is the texture coordinate.
#define CLIMATE_TRANSFORM parameters[20]

static const float kClimateSignedSpan = 4.0;

struct SurfaceClimate {
    float4 foliage;      // steppe, boreal, temperate, tropical
    float desert;
    float4 environment;  // temperature, fertility, moisture, spare
    float4 geography;    // wind x, wind y, spare (curvature), drainage
    float forest;
};

SurfaceClimate climateAt(float2 worldXY)
{
    const float2 uv = worldXY * CLIMATE_TRANSFORM.xy + CLIMATE_TRANSFORM.zw;
    const float4 a = climatePlane0.SampleLevel(climateSampler, uv, 0);
    const float4 b = climatePlane1.SampleLevel(climateSampler1, uv, 0);
    const float4 c = climatePlane2.SampleLevel(climateSampler2, uv, 0);

    SurfaceClimate result;
    result.foliage = a;
    result.desert = b.x;
    // The fourth was a constant one on the vertex and is a constant one here.
    result.environment = float4(b.y, b.z, b.w, 1.0);
    result.geography = float4((c.x - 0.5) * kClimateSignedSpan,
                           (c.y - 0.5) * kClimateSignedSpan, 0.0, c.z);
    result.forest = c.w;
    return result;
}
