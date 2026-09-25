#include "world.hlsli"
#include "weather_model.hlsli"
struct PrecipitationOut { float4 position : SV_Position; };
PrecipitationOut PrecipitationVS(uint id : SV_VertexID) {
    PrecipitationOut o;
    o.position=float4(id==2?3.0:-1.0,id==1?3.0:-1.0,0,1);
    return o;
}
float4 PrecipitationPS(PrecipitationOut input) : SV_Target0 {
    const float snow=1.0-wxSmooth(-1.0,2.0,parametersPS[2].x);
    const float intensity=parametersPS[2].y*parametersPS[0].x;
    const float time=viewportPS.z;
    const float sideways=dot(normalize(viewProjectionPS[0].xy),windPS.xy);
    const float2 direction=normalize(float2(sideways*0.4*windPS.z,1.0));
    float alpha=0;
    [unroll] for (int layer=0;layer<2;++layer) {
        const float size=layer==0?56.0:93.0;
        float2 p=input.position.xy;
        p+=float2(dot(viewProjectionPS[0].xy,cameraPS.xy)*viewportPS.x*0.5,
                   -dot(viewProjectionPS[1].xy,cameraPS.xy)*viewportPS.y*0.5);
        p-=direction*time*lerp(470.0,46.0,snow)*(1.0+float(layer)*0.4);
        float2 cell=floor(p/size), q=frac(p/size);
        // A particle may straddle a cell: sample its neighbours as well.
        // Jitter BOTH coordinates; a fixed y made every layer a row of dashes.
        [unroll] for (int y=-1;y<=1;++y) [unroll] for (int x=-1;x<=1;++x) {
            const float2 offset=float2(x,y);
            const int2 source=(int2)cell+int2(x,y);
            const float rnd=wxHash(source.x,source.y,773u+uint(layer));
            const float ry=wxHash(source.x,source.y,1193u+uint(layer));
            const float density=wxHash(source.x,source.y,1973u+uint(layer));
            float2 d=q-offset-float2(rnd,ry);
            d.x+=sin(time*0.7+rnd*17.0)*snow*0.07;
            const float across=abs(dot(d,float2(direction.y,-direction.x)));
            const float along=abs(dot(d,direction));
            const float width=lerp(0.007,0.027,snow);
            const float length=lerp(0.23,0.029,snow);
            const float mark=(1.0-smoothstep(width,width+1.0/size,across))*
                             (1.0-smoothstep(length,length+1.0/size,along));
            alpha+=mark*wxSmooth(density-0.1,density+0.1,intensity)*lerp(0.24,0.78,snow);
        }
    }
    return float4(lerp(float3(0.64,0.73,0.80),float3(0.92,0.95,0.98),snow),saturate(alpha)*intensity);
}
