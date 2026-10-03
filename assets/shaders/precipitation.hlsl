#include "world.hlsli"
#include "weather_model.hlsli"

// One screen triangle. Three procedural image layers suggest distance through
// size, opacity, speed and camera parallax; there are no particle meshes.
cbuffer WeatherDraw : register(b1, space3) {
    float4 weatherDrift[3]; // xy camera/rain drift, zw extra snow drift, reference pixels
    float4 weatherView;     // xy projected wind, z perspective, w rain foreshortening
};
Texture2D weatherScene : register(t0, space2);
SamplerState weatherSceneSampler : register(s0, space2);

struct PrecipitationOut { float4 position : SV_Position; };
PrecipitationOut PrecipitationVS(uint id : SV_VertexID) {
    PrecipitationOut o;
    o.position=float4(id==2?3.0:-1.0,id==1?3.0:-1.0,0,1);
    return o;
}

float weatherLayerVisibility(float sceneDistance, float depth) {
    // Read the distance already encoded by the opaque world. An object close
    // to the camera masks the farther image layers; sky keeps all three.
    return lerp(1.0, smoothstep(depth*0.72,depth*1.2,sceneDistance),weatherView.z);
}

float rainImage(float2 pixel,float2 direction,float clock,float intensity,
                float sceneDistance,float aa) {
    float alpha=0;
    const float2 across=float2(direction.y,-direction.x);
    [unroll] for (int layer=0;layer<3;++layer) {
        const float near=1.0-float(layer)*0.34;
        const float depth=layer==0?6.0:layer==1?18.0:48.0;
        const float visible=weatherLayerVisibility(sceneDistance,depth);
        if (visible<=0.001) continue;
        const float2 spacing=float2(lerp(32.0,67.0,near),lerp(65.0,138.0,near));
        float2 p=pixel+weatherDrift[layer].xy;
        p.y-=clock*lerp(270.0,690.0,near);
        const int2 cell=int2(floor(p/spacing));
        const float seed=wxHash(cell.x,cell.y,773u+uint(layer)*971u);
        if (wxHash(cell.x,cell.y,1973u+uint(layer)*373u)>intensity*0.82) continue;
        const float3 random=frac(seed*float3(17.73,71.91,137.31));
        // The entire mark stays inside its cell. One candidate suffices;
        // there is no 3x3 neighbour search at each pixel.
        const float2 centre=0.26+random.xy*0.48;
        const float2 offset=p-float2(cell)*spacing-centre*spacing;
        const float2 d=float2(dot(offset,across),dot(offset,direction));
        const float halfLength=lerp(4.0,17.0,near)*lerp(0.65,1.2,random.z)*
                               lerp(0.22,1.0,weatherView.w);
        const float taper=saturate(1.0-abs(d.y)/halfLength);
        const float width=lerp(0.32,0.70,near)*lerp(0.65,1.1,random.x)*(0.55+0.45*taper);
        const float mark=(1.0-smoothstep(width,width+aa,abs(d.x)))*
                         smoothstep(0.0,0.32,taper);
        alpha+=mark*visible*lerp(0.09,0.24,near)*lerp(0.7,1.0,random.z);
    }
    return alpha;
}

float snowImage(float2 pixel,float clock,float intensity,float sceneDistance,float aa) {
    float alpha=0;
    [unroll] for (int layer=0;layer<3;++layer) {
        const float near=1.0-float(layer)*0.34;
        const float depth=layer==0?6.0:layer==1?18.0:48.0;
        const float visible=weatherLayerVisibility(sceneDistance,depth);
        if (visible<=0.001) continue;
        const float spacing=lerp(25.0,71.0,near);
        float2 p=pixel+weatherDrift[layer].xy+weatherDrift[layer].zw;
        p.y-=clock*lerp(19.0,64.0,near);
        const int2 cell=int2(floor(p/spacing));
        const float seed=wxHash(cell.x,cell.y,1193u+uint(layer)*971u);
        if (wxHash(cell.x,cell.y,2153u+uint(layer)*373u)>intensity*0.88) continue;
        const float3 random=frac(seed*float3(17.73,71.91,137.31));
        float2 centre=(0.30+random.xy*0.40)*spacing;
        centre.x+=sin(clock*lerp(0.42,0.87,random.z)+seed*31.0)*lerp(1.0,5.0,near);
        centre.y+=sin(clock*0.58+seed*17.0)*near*1.2;
        const float2 d=p-float2(cell)*spacing-centre;
        const float radius=lerp(0.65,2.5,near)*lerp(0.7,1.25,random.z);
        const float distance=length(d*float2(1.0,lerp(0.85,1.15,random.y)));
        const float blur=lerp(aa,1.5,near)*lerp(0.65,1.0,random.x);
        const float mark=1.0-smoothstep(radius*0.35,radius+blur,distance);
        alpha+=mark*visible*lerp(0.15,0.48,near)*lerp(0.65,1.0,random.x);
    }
    return alpha;
}

float4 PrecipitationPS(PrecipitationOut input) : SV_Target0 {
    const float intensity=saturate(parametersPS[2].y*parametersPS[0].x);
    const float snow=1.0-wxSmooth(-1.0,2.0,parametersPS[2].x);
    const float reference=1080.0/max(viewportPS.y,1.0);
    const float2 pixel=input.position.xy*reference;
    const float aa=max(0.35,reference*0.6);
    float sceneDistance=4000000.0;
    if (weatherView.z>0.5) {
        const float encoded=weatherScene.SampleLevel(weatherSceneSampler,input.position.xy/viewportPS.xy,0).a;
        sceneDistance=0.5*exp2(encoded*22.93);
    }
    const float2 direction=normalize(float2(weatherView.x*0.24,1.0+weatherView.y*0.14));
    // These branches are uniform across the image: pure rain/snow evaluates
    // only three layers; a narrow temperature transition evaluates both.
    float rain=0,flakes=0;
    if (snow<0.999) rain=rainImage(pixel,direction,viewportPS.z,intensity,sceneDistance,aa);
    if (snow>0.001) flakes=snowImage(pixel,viewportPS.z,intensity,sceneDistance,aa);
    const float alpha=lerp(rain,flakes,snow)*intensity;
    const float light=lookPS.w>0.5?saturate(0.45+lookPS.y*0.40):0.85;
    const float3 colour=lerp(float3(0.63,0.70,0.75),float3(0.89,0.92,0.95),snow)*light;
    return float4(colour,min(alpha,0.62));
}
