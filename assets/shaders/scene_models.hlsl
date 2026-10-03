// Shared geometry + frame-arena instances. Far objects use relightable baked views.
#include "world.hlsli"
#include "wind_field.hlsli"
#include "scene_model_motion.hlsli"
#include "landscape_look.hlsli"
#include "impostor_depth.hlsli"
#include "hemisphere_impostor.hlsli"
#include "environment_detail.hlsli"
#define SHADOW_TEXTURE_SLOT t2
#define SHADOW_SAMPLER_SLOT s2
#include "shadow_field.hlsli"
Texture2DArray colourTex : register(t0, space2);
SamplerState colourSampler : register(s0, space2);
Texture2DArray normalTex : register(t1, space2);
SamplerState normalSampler : register(s1, space2);
#ifndef IMPOSTOR_DEPTH_TEXTURE_SLOT
#define IMPOSTOR_DEPTH_TEXTURE_SLOT t3
#define IMPOSTOR_DEPTH_SAMPLER_SLOT s3
#endif
Texture2DArray depthTex : register(IMPOSTOR_DEPTH_TEXTURE_SLOT, space2);
SamplerState depthSampler : register(IMPOSTOR_DEPTH_SAMPLER_SLOT, space2);
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
    // leaf quads), 1 legacy billboard, 2 side-depth, 3 hemisphere-depth, 4 cluster proxy.
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
    nointerpolation float3 frame : TEXCOORD7; // width, height, world scale
    float3 plane : TEXCOORD8; // animated billboard's unscaled, unrotated plane
    nointerpolation float3 localEye : TEXCOORD9;
};
float3 rotateModel(float3 p,float yaw) {
    float c=cos(yaw),s=sin(yaw);return float3(c*p.x-s*p.y,s*p.x+c*p.y,p.z);
}
ModelOut ModelVS(ModelIn v) {
    const bool card=v.lod.x>0.5;
    const bool hemisphere=v.lod.x>2.5;
    const float side=length(v.shape.xy);
    float up=saturate(card?v.position.z:v.position.z/max(0.001,v.shape.y));
    float3 p=rotateModel(v.position,v.motion.x);
    if (hemisphere) {
        const float3 right=normalize(viewProjection[0].xyz),above=normalize(viewProjection[1].xyz);
        p=right*(v.position.x*side)+above*((v.position.z-0.5)*side)+float3(0,0,v.shape.y*0.5);
        up=v.lod.x>3.5?0.65:saturate(p.z/max(0.001,v.shape.y));
    } else if (card) {
        const float2 right=normalize(viewProjection[0].xy);
        p=float3(right*(v.position.x*v.shape.x),v.position.z*v.shape.y);
    } else {
        // Cluster-local vertices carry the exact surface point of the
        // replacement family. At the beginning of the interval the child is
        // unchanged; at the end it lies on the representation that replaces
        // it. Cards do not participate in this morph.
        p=lerp(p,rotateModel(v.morph,v.motion.x),saturate(v.lod.w));
    }
    const float3 plane=rotateModel(p-float3(0,0,hemisphere?v.shape.y*0.5:0),-v.motion.x);
    const float3 centre=v.originScale.xyz+float3(0,0,v.shape.y*v.originScale.w*0.5);
    float3 eye=cross(viewProjection[0].xyz,viewProjection[1].xyz);
    eye=eye.z<0?-eye:eye;
    if (dot(abs(viewProjection[3].xyz),float3(1,1,1))>0) eye=camera.xyz-centre;
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
    o.frame=float3(hemisphere?float2(side,side):v.shape.xy,v.originScale.w);o.plane=plane;
    o.localEye=rotateModel(normalize(eye),-v.motion.x);
    return o;
}
bool reprojectModelDepth(ModelOut i,float layer,out float2 uv,out float3 world) {
    const bool hemisphere=i.flags.x>2.5;
    float3 worldRay=landscapeEye(i.world);
    if (hemisphere && !landscapePerspective()) {
        worldRay=cross(viewProjectionPS[0].xyz,viewProjectionPS[1].xyz);
        worldRay=normalize(worldRay.z<0?-worldRay:worldRay);
    }
    const float3 ray=rotateModel(worldRay,-i.extra.y);
    float2 projected=i.uvLayer.xy;
    float3 hit=i.plane;
    bool valid=true;
    const float hemisphereIndex=layer-i.material.x;
    if (hemisphere) valid=intersectHemisphereDepth(i.plane,ray,0,i.frame.x,hemisphereIndex,hit,projected);
    const float2 fallback=projected;
    [unroll] for (int step=0;step<3;++step) {
        const float4 packed=depthTex.SampleLevel(depthSampler,float3(projected,layer),0);
        float2 next;
        const float depth=decodeImpostorDepth(packed.rg,i.frame.x);
        const float view=round(packed.b*255.0);
        const bool intersects=hemisphere?
            intersectHemisphereDepth(i.plane,ray,depth,i.frame.x,view,hit,next):
            intersectImpostorDepth(i.plane,ray,depth,i.frame.xy,view,hit,next);
        valid=valid && packed.a>=0.2 && intersects;
        projected=next;
        valid=valid && all(projected>=0.0) && all(projected<=1.0);
    }
    const float4 coverage=depthTex.SampleLevel(depthSampler,float3(projected,layer),0);
    valid=valid && coverage.a>=0.2;
    uv=valid?projected:fallback;
    world=i.world+(valid?rotateModel((hit-i.plane)*i.frame.z,i.extra.y):float3(0,0,0));
    return valid;
}
float4 shadeModel(ModelOut i,bool front,bool depthAware,out float resultDepth) {
    resultDepth=i.position.z;
    const bool card=i.flags.x>0.5;
    // An impostor stands between neighbouring baked views. Averaging them
    // ghosts one silhouette over the other; choosing one per pixel by a dither
    // does not, and over an alpha-tested card at that size it reads as a turn
    // rather than as a snap. Its own hash, so the choice does not correlate
    // with the mesh/card crossover's.
    const float turning=frac(19.1907*frac(dot(floor(i.position.xy),float2(0.0917,0.0413))));
    const float layer=i.flags.x>2.5?i.material.x+hemisphereView(i.localEye,turning):
        (turning<i.extra.w?i.material.y:i.material.x);
    float2 uv=card?i.uvLayer.xy:frac(i.uvLayer.xy);
    const float2 gx=ddx(i.uvLayer.xy),gy=ddy(i.uvLayer.xy);
    if (depthAware && i.flags.x>1.5) {
        float3 world;
        if (reprojectModelDepth(i,layer,uv,world)) {
            i.world=world;
            const float4 clipPosition=mul(viewProjectionPS,float4(i.world,1));
            resultDepth=saturate(clipPosition.z/max(clipPosition.w,1e-6));
        }
    }
    const float3 uvLayer=float3(uv,layer);
    float4 texel=colourTex.SampleGrad(colourSampler,uvLayer,gx,gy);
    // Coverage is one everywhere except on a crown, where it says how solid the
    // leaf mass is. Cutting against it is what keeps the thin edge of a canopy
    // thin: a shell around foliage is a closed surface, and a closed surface
    // drawn opaque reads as a potato at exactly the distance it is used.
    clip(texel.a*i.colour.a-0.2);
    const float threshold=frac(52.9829189*frac(dot(floor(i.position.xy),float2(0.06711056,0.00583715))));
    // Complementary screen-door transition: never alpha blend unsorted trees.
    clip(card?threshold-i.flags.y:i.flags.y-threshold);
    const float coverage=frac(31.713*frac(dot(floor(i.position.xy),float2(0.0371,0.0817))));
    // Positive: present where the hash is below it. Negative: the complement
    // of a coarser representation fading in over this one (present where the
    // hash is at or above |w|), so the two dissolve into each other pixel for
    // pixel with no overlap and no hole.
    clip(i.flags.z>=0.0?i.flags.z-coverage:coverage+i.flags.z);
    float3 n=normalize(i.normal);
    const float3 encoded=normalTex.SampleGrad(normalSampler,uvLayer,gx,gy).xyz*2-1;
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
    // The negative material-response range is stable rock habitat; vegetation
    // remains 0..1. Depth impostors use their reconstructed world point/normal
    // here too, so moss does not jump when a rock changes representation.
    const float habitat=saturate(-i.flags.w);
    [branch] if (habitat>0.01) {
        const float shade=environmentMossShelter(n.x,n.y,n.z);
        const float patch=environmentNoise((i.world.xy+i.world.z*float2(0.37,0.19))/1.1)*0.65+
                          environmentNoise(i.world.xy/0.23)*0.35;
        const float cover=environmentMossSurface(habitat,n.z,shade,patch);
        const float2 mossUV=(i.world.xy+i.world.z*float2(0.37,0.23))/2.0;
        const float2 ux=ddx(mossUV),uy=ddy(mossUV);
        const float3 dx=ddx(i.world),dy=ddy(i.world);
        [branch] if (cover>0.01) {
            uint w,h,layers,levels;
            colourTex.GetDimensions(0,w,h,layers,levels);
            const float4 moss=colourTex.SampleGrad(colourSampler,float3(mossUV,layers-1),ux,uy);
            const float maskedCover=cover*moss.a;
            texel.rgb=lerp(texel.rgb,moss.rgb,maskedCover*0.80);
            const float3 mossNormal=normalTex.SampleGrad(normalSampler,float3(mossUV,layers-1),ux,uy).xyz*2-1;
            const float det=ux.x*uy.y-ux.y*uy.x;
            const float3 t=dx*uy.y-dy*ux.y,b=dy*ux.x-dx*uy.x;
            if (abs(det)>0.0000001 && dot(t,t)>0.0000001 && dot(b,b)>0.0000001)
                n=normalize(lerp(n,normalize(t)*sign(det)*mossNormal.x+
                    normalize(b)*sign(det)*mossNormal.y+n*mossNormal.z,maskedCover*0.35));
        }
    }
    const float vegetation=saturate(i.flags.w);
    const float3 crown=lerp(float3(1,1,1),landscapeCrownTint(i.extra.y*0.15915494,texel.rgb),vegetation);
    const float3 pigment=landscapePigment(texel.rgb*i.colour.rgb*i.extra.z*crown,vegetation);
    const float shadow=proceduralShadow(i.world,n);
    float3 lit=pigment*landscapeDaylight(n,lookRootOcclusion(i.extra.x),shadow);
    const float transmission=pow(saturate(dot(-landscapeSun(),landscapeEye(i.world))),3.0);
    lit+=pigment*float3(1.0,0.95,0.65)*transmission*vegetation*i.extra.x*0.18*shadow;
    return float4(landscapeFinish(lit,i.world),sceneDepthAlpha(i.world));
}
float4 ModelPS(ModelOut i,bool front : SV_IsFrontFace) : SV_Target0 {
    float unused;
    return shadeModel(i,front,false,unused);
}
struct ModelDepthOut { float4 colour : SV_Target0; float depth : SV_Depth; };
ModelDepthOut ModelDepthPS(ModelOut i,bool front : SV_IsFrontFace) {
    ModelDepthOut result;
    result.colour=shadeModel(i,front,true,result.depth);
    return result;
}
