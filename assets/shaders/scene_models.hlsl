// Shared geometry + frame-arena instances. Far objects use relightable baked views.
#include "world.hlsli"
#include "wind_field.hlsli"
#include "scene_model_motion.hlsli"
#include "landscape_look.hlsli"
Texture2DArray colourTex : register(t0, space2);
SamplerState colourSampler : register(s0, space2);
Texture2DArray normalTex : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
struct ModelIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float3 colour : TEXCOORD3;
    float2 layer : TEXCOORD4; // texture layer, how solid the mass is here
    float3 morph : TEXCOORD5; // position on the replacement cluster surface
    float3 morphNormal : TEXCOORD6;
    float4 originScale : TEXCOORD7;
    float4 motion : TEXCOORD8; // yaw, phase, tint, vegetation
    float4 shape : TEXCOORD9; // width, height, impostor layer, mesh weight
    // representation: 0 is model-local surface geometry (including authored
    // leaf quads), 1 is only the normalized four-vertex baked billboard.
    float4 lod : TEXCOORD10; // representation, coverage, next view, morph
};
struct ModelOut {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 uvLayer : TEXCOORD2; // uv, this view's layer, the next one's
    float4 colour : TEXCOORD3; // tint, and coverage in w
    nointerpolation float4 flags : TEXCOORD4;
    float4 extra : TEXCOORD5; // height fraction, yaw, tint, how far between views
    nointerpolation float2 material : TEXCOORD6;
};
float3 rotateModel(float3 p,float yaw) {
    float c=cos(yaw),s=sin(yaw);return float3(c*p.x-s*p.y,s*p.x+c*p.y,p.z);
}
ModelOut ModelVS(ModelIn v) {
    const bool card=v.lod.x>0.5;
    const float up=saturate(card?v.position.z:v.position.z/max(0.001,v.shape.y));
    float3 p=rotateModel(v.position,v.motion.x);
    if (card) {
        const float2 right=normalize(viewProjection[0].xy);
        p=float3(right*(v.position.x*v.shape.x),v.position.z*v.shape.y);
    } else {
        // Cluster-local vertices carry the exact surface point of the
        // replacement family. At the beginning of the interval the child is
        // unchanged; at the end it lies on the representation that replaces
        // it. Cards do not participate in this morph.
        p=lerp(p,rotateModel(v.morph,v.motion.x),saturate(v.lod.w));
    }
    p=p*v.originScale.w+v.originScale.xyz;
    const float gust=windGust(v.originScale.xy,wind.xy,viewport.z,wind.w,1.0);
    // Root exactly fixed; wind strength zero freezes trees, rocks never move.
    p.xy+=wind.xy*modelWindDisplacement(up,v.shape.y*v.originScale.w,wind.z,
                                      v.motion.w,v.motion.y,viewport.z,gust);
    ModelOut o;
    o.position=project(p);o.world=p;
    const float3 morphNormal=normalize(lerp(v.normal,v.morphNormal,saturate(v.lod.w)));
    o.normal=rotateModel(morphNormal,v.motion.x);
    o.uvLayer=float4(v.uv,card?v.shape.z:v.layer.x,card?v.lod.z:v.layer.x);
    o.material=o.uvLayer.zw;
    o.colour=float4(card?float3(1,1,1):v.colour,card?1.0:v.layer.y);
    o.flags=float4(v.lod.x,v.shape.w,v.lod.y,v.motion.w);
    o.extra=float4(up,v.motion.x,v.motion.z,card?v.lod.w:0.0);
    return o;
}
float4 ModelPS(ModelOut i,bool front : SV_IsFrontFace) : SV_Target0 {
    const bool card=i.flags.x>0.5;
    // An impostor stands between two of the eight baked views. Averaging them
    // ghosts one silhouette over the other; choosing one per pixel by a dither
    // does not, and over an alpha-tested card at that size it reads as a turn
    // rather than as a snap. Its own hash, so the choice does not correlate
    // with the mesh/card crossover's.
    const float turning=frac(19.1907*frac(dot(floor(i.position.xy),float2(0.0917,0.0413))));
    const float layer=turning<i.extra.w?i.material.y:i.material.x;
    const float3 uvLayer=float3(card?i.uvLayer.xy:frac(i.uvLayer.xy),layer);
    float4 texel=colourTex.SampleGrad(colourSampler,uvLayer,ddx(i.uvLayer.xy),ddy(i.uvLayer.xy));
    // Coverage is one everywhere except on a crown, where it says how solid the
    // leaf mass is. Cutting against it is what keeps the thin edge of a canopy
    // thin: a shell around foliage is a closed surface, and a closed surface
    // drawn opaque reads as a potato at exactly the distance it is used.
    clip(texel.a*i.colour.a-0.2);
    const float threshold=frac(52.9829189*frac(dot(floor(i.position.xy),float2(0.06711056,0.00583715))));
    // Complementary screen-door transition: never alpha blend unsorted trees.
    clip(card?threshold-i.flags.y:i.flags.y-threshold);
    const float coverage=frac(31.713*frac(dot(floor(i.position.xy),float2(0.0371,0.0817))));
    clip(i.flags.z-coverage);
    float3 n=normalize(i.normal);
    const float3 encoded=normalTex.SampleGrad(normalSampler,uvLayer,ddx(i.uvLayer.xy),ddy(i.uvLayer.xy)).xyz*2-1;
    if (card) n=normalize(rotateModel(encoded,i.extra.y));
    else {
        const float3 dx=ddx(i.world),dy=ddy(i.world);
        const float2 ux=ddx(i.uvLayer.xy),uy=ddy(i.uvLayer.xy);
        const float det=ux.x*uy.y-ux.y*uy.x;
        const float3 t=dx*uy.y-dy*ux.y,b=dy*ux.x-dx*uy.x;
        if (abs(det)>0.0000001 && dot(t,t)>0.0000001 && dot(b,b)>0.0000001)
            n=normalize(normalize(t)*sign(det)*encoded.x+normalize(b)*sign(det)*encoded.y+n*encoded.z);
        if (!front) n=-n;
    }
    const float3 pigment=landscapePigment(texel.rgb*i.colour.rgb*i.extra.z,i.flags.w);
    float3 lit=pigment*landscapeDaylight(n,lookRootOcclusion(i.extra.x));
    const float transmission=pow(saturate(dot(-landscapeSun(),landscapeEye(i.world))),3.0);
    lit+=pigment*float3(1.0,0.95,0.65)*transmission*i.flags.w*i.extra.x*0.18;
    return float4(landscapeFinish(lit,i.world),1);
}
