#ifndef WEATHER_HLSLI
#define WEATHER_HLSLI
#include "world.hlsli"
#include "weather_model.hlsli"
struct WeatherVertex { float4 surface; float4 air; };
// Vertex-only: history is evaluated per mesh vertex, never per terrain pixel.
WeatherVertex weatherVertex(float3 p, float thermal, float moisture, float drainage) {
    WeatherVertex result;
    result.surface=0.0; result.air=0.0;
    if (parameters[0].x<0.5) return result;
    WeatherSurface left=wxEmpty(), right=wxEmpty();
    float4 before=0.0, after=0.0;
    [loop] for (int i=0;i<17;++i) {
        float4 f=parameters[3+i];
        WeatherAir a=wxAir(thermal,moisture,p.z,p.x,p.y,f.x,f.y,(uint)f.z,f.w,(int)parameters[0].w);
        if (i<16) left=wxAdvance(left,a,drainage);
        if (i>0) right=wxAdvance(right,a,drainage);
        if (i==15) before=float4(a.temperature,a.precipitation,a.cloud,a.wind);
        after=float4(a.temperature,a.precipitation,a.cloud,a.wind);
    }
    result.surface=float4(lerp(float3(left.snow,left.wet,left.ice),
                               float3(right.snow,right.wet,right.ice),parameters[0].y),1.0);
    result.air=lerp(before,after,parameters[0].y);
    return result;
}
float3 weatherVegetation(float3 pigment, float seasonal, float wet, float temperature) {
    float autumn=wxSmooth(1.6,2.5,seasonal)*(1.0-wxSmooth(3.4,4.0,seasonal));
    autumn*=1.0-wxSmooth(18.0,28.0,temperature);
    float dry=parametersPS[0].w==4.0 ? 0.65 : 0.0;
    float amount=max(autumn*0.60,dry)*(parametersPS[0].x);
    float luminance=dot(pigment,float3(0.30,0.59,0.11));
    return lerp(pigment,luminance*float3(1.35,0.94,0.48),amount)*(1.0-wet*0.10);
}

// Small rain rings decorate the existing water normal, only close enough to
// resolve them. One bounded cell candidate, no geometry or texture fetches.
float2 rainSurfaceSlope(float2 xy,float clock,float rain,float pixel) {
    if (rain<=0.01 || pixel>=0.045) return 0.0;
    const float cellMetres=1.2;
    const int2 cell=int2(floor(xy/cellMetres));
    const float seed=wxHash(cell.x,cell.y,9371u);
    if (seed>rain*0.72) return 0.0;
    const float cycle=clock*1.4+seed*17.3;
    const uint episode=uint(floor(cycle));
    const float shape=wxHash(cell.x,cell.y,7237u+episode*53u);
    const float2 centre=(0.30+frac(shape*float2(17.73,71.91))*0.40)*cellMetres;
    const float2 delta=xy-float2(cell)*cellMetres-centre;
    const float distance=length(delta);
    if (distance>0.32 || distance<0.002) return 0.0;
    const float age=frac(cycle);
    const float radius=0.02+age*0.25;
    const float width=max(0.014,pixel*1.2);
    const float ring=1.0-smoothstep(width,width*2.8,abs(distance-radius));
    const float slope=sin((distance-radius)/width*2.5)*ring*
                      (1.0-age)*(1.0-age)*smoothstep(0.0,0.10,age)*
                      (1.0-smoothstep(0.018,0.045,pixel))*0.045*rain;
    return delta/distance*slope;
}
#endif
