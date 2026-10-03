#ifndef SHADOW_FIELD_HLSLI
#define SHADOW_FIELD_HLSLI
#include "world.hlsli"
#include "cloud_field.hlsli"
Texture2DArray shadowField : register(SHADOW_TEXTURE_SLOT, space2);
SamplerState shadowSampler : register(SHADOW_SAMPLER_SLOT, space2);

float4 shadowFilterWeights(float f)
{
    // Exact overlap of a three-texel-wide box with four source texels. The
    // fractional end weights keep the blur continuous during sub-texel motion;
    // a plain 3x3/4x4 average of nearest samples would still move in steps.
    return float4(1.0-f,1.0,1.0,f)/3.0;
}

// No shadow from the relief itself. Each level holds the ground only as a
// 48x48 grid of the height field over the whole level (shadow_clipmap.cpp,
// the `w` channel): its chord strays from the drawn ground by metres on the
// finest level and by tens on the coarse ones, and every attempt to read it -
// on every level, then only the coarse ones with a bias of half a cell -
// shaded the ground in rectangles and in even bands along the grid's rows,
// the worst of it while a level was being recompiled. Slopes are still lit by
// their angle to the sun; what is missing is a ridge's shadow cast across the
// valley beside it, which wants the drawn surface in the tile (or a horizon
// map), not this grid.

float proceduralShadow(float3 world, float3 normal)
{
    // Clouds dim the sun before anything on the ground can.
    const float clouds = cloudShadow(world);
    if (shadowSunPS.w < 0.5) return clouds;
    const float3 sun = shadowSunPS.xyz;
    const float3 right = abs(sun.z) > 0.999 ? float3(1,0,0) : normalize(float3(-sun.y,sun.x,0));
    const float3 up = cross(sun,right);
    const float facing=dot(normal,sun);
    const float2 receiverSlope=abs(facing)>0.1 ?
        clamp(-float2(dot(normal,right),dot(normal,up))/facing,-4.0,4.0) : float2(0,0);
    float visibility = 0.0;
    float remaining = 1.0;
    // The wide 4x4 filter only where a pixel is smaller than its soft edge:
    // past a few tens of metres (and on the map) a pixel spans more than a
    // shadow texel of the finest level that covers it, and a bilinear 2x2
    // filter is the same picture for a quarter of the reads - in every pass
    // that lights anything, grass included, which is most of the frame.
    const bool perspective = dot(abs(viewProjectionPS[3].xyz), float3(1, 1, 1)) > 0.0;
    const bool wide = perspective && dot(world - cameraPS.xyz, world - cameraPS.xyz) < 70.0 * 70.0;
    // Filter visibility, never blocker depths: blending an empty texel's depth
    // with a tree would produce light leaks and erase transmitting crowns.
    // Only the finest covering level is read except in a transition strip.
    [loop] for (int level = 0; level < 4; ++level) {
        const float span = shadowClipPS[level].w;
        if (span <= 0) continue;
        const float3 local = world - shadowClipPS[level].xyz;
        const float2 uv = float2(dot(local,right),dot(local,up)) / span + 0.5;
        const float edge = min(min(uv.x,uv.y),min(1.0-uv.x,1.0-uv.y));
        if (edge <= 2.0/128.0) continue;
        const float depth = dot(local,sun);
        const float texel = span/128.0;
        const float bias = max(0.08,texel*(0.45+1.5*(1.0-saturate(dot(normal,sun)))));
        const float2 grid=uv*128.0-0.5;
        const float2 base=floor(grid);
        const float4 wx=shadowFilterWeights(frac(grid.x)),wy=shadowFilterWeights(frac(grid.y));
        float amount = 0;
        // Graphics setting: spread of the same 4x4 filter, 1 = one texel.
        const float soft = qualityPS.y > 0.0 ? clamp(qualityPS.y, 0.5, 3.0) : 1.0;
        [branch] if (!wide) {
            const float2 near = floor(grid);
            const float2 f = frac(grid);
            [unroll] for (int y = 0; y < 2; ++y) [unroll] for (int x = 0; x < 2; ++x) {
                const float2 tap = (near + float2(x, y) + 0.5) / 128.0;
                const float4 blocker = shadowField.SampleLevel(shadowSampler, float3(tap, level), 0);
                const float receiver = depth + dot(receiverSlope, (tap - uv) * span);
                const float opaque = receiver + bias < blocker.x ? 0.0 : 1.0;
                const float canopy = receiver + bias < blocker.y ? exp(-blocker.z) : 1.0;
                amount += opaque * canopy * (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
            }
        } else
        [unroll] for (int y = 0; y < 4; ++y) [unroll] for (int x = 0; x < 4; ++x) {
            const float2 offset=(float2(x-1,y-1)-0.5)*soft+0.5;
            const float2 tap=(base+offset+0.5)/128.0;
            const float4 blocker = shadowField.SampleLevel(shadowSampler,
                float3(tap,level),0);
            // A wider footprint on an inclined receiver must compare against
            // the receiver plane at THAT tap, not the centre pixel's depth.
            const float receiver=depth+dot(receiverSlope,(tap-uv)*span);
            const float opaque = receiver+bias < blocker.x ? 0.0 : 1.0;
            const float canopy = receiver+bias < blocker.y ? exp(-blocker.z) : 1.0;
            amount += opaque*canopy*wx[x]*wy[y];
        }
        const float weight = smoothstep(2.0/128.0,0.10,edge);
        visibility += remaining*amount*weight;
        remaining *= 1.0-weight;
        if (remaining < 0.001) return visibility * clouds;
    }
    return (visibility+remaining) * clouds;
}
#endif

