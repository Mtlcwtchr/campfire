#ifndef HEMISPHERE_IMPOSTOR_HLSLI
#define HEMISPHERE_IMPOSTOR_HLSLI
// Must match engine/render/hemisphere_impostor.hpp and tools/hemisphere_impostor.py.
void hemisphereBasis(float view,out float3 right,out float3 up,out float3 eye) {
    const float ring=view<8?0:view<16?1:view<20?2:3;
    const float first=ring==0?0:ring==1?8:ring==2?16:20;
    const float count=ring<2?8:ring==2?4:1;
    const float elevation=(ring==0?0:ring==1?45:ring==2?70:90)*0.0174532925199433;
    const float angle=(view-first)*6.283185307179586/count;
    const float c=cos(angle),s=sin(angle),ce=cos(elevation),se=sin(elevation);
    right=float3(c,s,0);up=float3(s*se,-c*se,ce);eye=float3(-s*ce,c*ce,se);
}
void hemisphereBand(float ring,float turn,out float2 views,out float2 weights) {
    const float first=ring==0?0:ring==1?8:ring==2?16:20;
    const float count=ring<2?8:ring==2?4:1;
    const float at=turn*count,index=min(floor(at),count-1),fraction=frac(at);
    views=first+float2(index,fmod(index+1,count));
    weights=float2(1-fraction,fraction);
}
bool hemisphereSelection(float3 direction,out float4 views,out float4 weights) {
    views=0;weights=0;
    if (dot(direction,direction)<1e-12 || direction.z<0) return false;
    direction=normalize(direction);
    if (dot(direction.xy,direction.xy)<1e-12) {
        views=20;weights=float4(1,0,0,0);return true;
    }
    const float elevation=asin(saturate(direction.z))*57.29577951308232;
    const float low=elevation<45?0:elevation<70?1:2;
    const float bottom=low==0?0:low==1?45:70,top=low==0?45:low==1?70:90;
    const float blend=saturate((elevation-bottom)/(top-bottom));
    // The pole's basis is fixed; the weight of the azimuth ring tends to zero.
    const float turn=frac((atan2(direction.y,direction.x)-1.5707963267948966)/6.283185307179586);
    hemisphereBand(low,turn,views.xy,weights.xy);
    hemisphereBand(low+1,turn,views.zw,weights.zw);
    weights*=float4(1-blend,1-blend,blend,blend);
    return true;
}
float hemisphereView(float3 direction,float dither) {
    float4 views,weights;
    hemisphereSelection(direction,views,weights);
    if (dither<weights.x) return views.x;
    if (dither<weights.x+weights.y) return views.y;
    return dither<weights.x+weights.y+weights.z?views.z:views.w;
}
float2 hemisphereUV(float3 position,float side,float view) {
    float3 right,up,eye;hemisphereBasis(view,right,up,eye);
    return float2(dot(position,right)/side+0.5,0.5-dot(position,up)/side);
}
bool intersectHemisphereDepth(float3 plane,float3 towardEye,float depth,float side,float view,
                             out float3 hit,out float2 uv) {
    float3 right,up,eye;hemisphereBasis(view,right,up,eye);
    const float denominator=dot(towardEye,eye);
    hit=plane;uv=0;
    if (denominator<=0.2 || side<=0 || view<0 || view>=21) return false;
    hit+=towardEye*((depth-dot(plane,eye))/denominator);
    uv=hemisphereUV(hit,side,view);
    return true;
}
#endif
