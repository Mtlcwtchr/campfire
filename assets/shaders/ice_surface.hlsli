// Stationary world-space ice: frosted plates and filtered Voronoi cracks.
#ifndef ICE_SURFACE_HLSLI
#define ICE_SURFACE_HLSLI
#include "noise.hlsli"

float iceCracks(float2 world, float cellMetres, float pixelMetres)
{
    const float visible = 1.0-smoothstep(cellMetres*0.08,cellMetres*0.35,pixelMetres);
    if (visible <= 0.0) return 0.0;
    const float2 p=world/cellMetres;
    const float2 base=floor(p);
    float first=100.0, second=100.0;
    [unroll] for (int y=-1;y<=1;++y) {
        [unroll] for (int x=-1;x<=1;++x) {
            const float2 cell=base+float2(x,y);
            const float2 seed=cell+0.2+0.6*float2(hashAt(cell),hashAt(cell+float2(173,419)));
            const float distance=length(seed-p);
            if (distance<first) { second=first; first=distance; }
            else second=min(second,distance);
        }
    }
    const float aa=max(0.001,pixelMetres/cellMetres);
    // F2-F1 is a continuous approximate distance to plate boundaries.
    return (1.0-smoothstep(0.012,0.012+aa*1.5,second-first))*visible;
}

float3 iceSurfaceColour(float2 p, float pixelMetres)
{
    const float frost=0.48+(noiseAt(p/36.0)-0.5)*0.55*
        (1.0-smoothstep(4.0,18.0,pixelMetres))+
        (noiseAt(p/3.0)-0.5)*0.15*(1.0-smoothstep(0.3,1.5,pixelMetres));
    float3 colour=lerp(float3(0.29,0.48,0.55),float3(0.80,0.87,0.88),saturate(frost));
    const float cracks=max(iceCracks(p,19.0,pixelMetres),iceCracks(p+71.0,4.0,pixelMetres)*0.42);
    return lerp(colour,float3(0.13,0.29,0.35),cracks*0.72);
}
#endif

