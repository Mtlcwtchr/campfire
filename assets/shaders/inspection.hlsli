#ifndef INSPECTION_HLSLI
#define INSPECTION_HLSLI
#include "environment_fields.hlsli"
// extra.w = MapView. table[6] = temperature offset, rain, potential-only, arrow spacing.
//
// A map is a layer laid over the ground, not a repaint of it. The alpha comes
// back with the colour so the caller can composite it onto finished terrain:
// the coast, the fields and the shape of the hills stay readable underneath,
// and the reading is still the loudest thing in the picture.
//
// Two weights. A smooth field is a wash and can be thin; a marker - no data,
// impassable, an isoline, a wind arrow - carries an edge that has to survive,
// so it is nearly opaque wherever it actually lands.
static const float kInspectionWash = 0.62;
static const float kInspectionMarker = 0.94;
float3 inspectionRamp(float value) {
    const float3 low=float3(0.12,0.24,0.48), mid=float3(0.20,0.72,0.58), high=float3(0.98,0.68,0.20);
    float t=saturate(value);
    return t<0.5 ? lerp(low,mid,t*2.0) : lerp(mid,high,(t-0.5)*2.0);
}
float inspectionSegment(float2 p,float2 a,float2 b) {
    float2 d=b-a; return length(p-a-d*saturate(dot(p-a,d)/max(dot(d,d),0.00001)));
}
float4 inspectionColour(float4 environment,float4 geography,float3 normal,float depth,float height,float2 xy,float cost) {
    int mode=(int)extraPS.w;
    if (cost<0.0 && (mode==2 || mode==4 || mode==5 || mode==6 || mode==8)) {
        float stripe=step(0.5,frac((xy.x+xy.y)/max(8.0,16.0/cameraPS.w)));
        return float4(lerp(float3(0.19,0.19,0.23),float3(0.31,0.31,0.35),stripe),kInspectionMarker);
    }
    float slope=length(normal.xy)/max(normal.z,0.0001);
    float moisture=moistureReadout(environment.z,tablePS[6].y,geography.w);
    float value=0;
    if (mode==1) value=saturate(environment.x+tablePS[6].x);
    if (mode==2) value=soilReadout(environment.y,environment.w*(1.0-tablePS[6].z));
    if (mode==3) value=moisture;
    if (mode==4) value=travelReadout(cost);
    if (mode==5) value=soilFoundationReadout(slope,depth,moisture,geography.w,environment.w,geography.x);
    if (mode==6) value=1.0-saturate(geography.z/6.0);
    if (mode==7) value=saturate(height/1200.0);
    float windSpeed=length(geography.xy);
    if (mode==8) value=saturate(windSpeed*lerp(windPS.z,1.0,tablePS[6].z)/2.0);
    float3 colour=inspectionRamp(value);
    float alpha=kInspectionWash;
    if (mode==6 && (geography.z<0.0 || any(xy<tablePS[7].xy) || any(xy>=tablePS[7].zw))) {
        float stripe=step(0.5,frac((xy.x+xy.y)/max(8.0,16.0/cameraPS.w)));
        return float4(lerp(float3(0.19,0.19,0.23),float3(0.31,0.31,0.35),stripe),kInspectionMarker);
    }
    // Soil and foundation values have no meaning under water. Terrain is still
    // drawn there; the water pass is suppressed rather than washing out maps.
    // The blank is opaque enough that it cannot be misread as a low reading.
    if (depth>0.0 && (mode==2 || mode==3 || mode==5)) {
        colour=float3(0.07,0.12,0.18);
        alpha=0.86;
    }
    if (mode==1 || mode==2 || mode==3 || mode==5 || mode==8) {
        float band=value*10.0;
        float gradient=fwidth(band);
        float contour=(1.0-smoothstep(0.0,max(gradient,0.0001)*1.1,
                       abs(frac(band+0.5)-0.5)))*smoothstep(0.0001,0.002,gradient);
        contour*=1.0-smoothstep(0.3,0.7,gradient);
        colour=lerp(colour,float3(0.85,0.92,0.89),contour*0.35);
        alpha=lerp(alpha,kInspectionMarker,contour*0.6);
    }
    if (mode==4 && cost>30.0) {
        // Do not smooth a forbidden vertex into a cheap route. This is a
        // conservative sampled boundary, not a replacement for path queries.
        float stripe=step(0.5,frac((xy.x-xy.y)/max(4.0,12.0/cameraPS.w)));
        return float4(lerp(float3(0.25,0.07,0.08),float3(0.58,0.19,0.14),stripe),kInspectionMarker);
    }
    if (mode==6) {
        float nearest=min(abs(geography.z-1.0),min(abs(geography.z-3.0),abs(geography.z-6.0)));
        float gradient=fwidth(geography.z);
        float contour=(1.0-smoothstep(0.0,max(gradient,0.0001),nearest))*
                      smoothstep(0.0001,0.002,gradient);
        colour=lerp(colour,float3(0.88,0.94,0.96),contour*0.6);
        alpha=lerp(alpha,kInspectionMarker,contour*0.75);
    }
    if (mode==7) {
        float interval=pow(10.0,floor(log10(max(10.0,40.0/max(cameraPS.w,0.0001)))));
        float h=height/interval;
        float contour=1.0-smoothstep(0.0,max(fwidth(h),0.002)*1.2,abs(frac(h+0.5)-0.5));
        colour=lerp(colour,float3(0.88,0.90,0.86),contour*0.48);
        alpha=lerp(alpha,kInspectionMarker,contour*0.65);
    }
    if (mode==8 && windSpeed>0.01 && (tablePS[6].z>0.5 || windPS.z>0.01)) {
        float spacing=max(4.0,tablePS[6].w);
        float2 local=frac(xy/spacing)-0.5;
        float2 dir=geography.xy/windSpeed;
        float2 p=float2(dot(local,dir),dot(local,float2(-dir.y,dir.x)));
        float distanceToArrow=inspectionSegment(p,float2(-0.25,0),float2(0.25,0));
        distanceToArrow=min(distanceToArrow,inspectionSegment(p,float2(0.25,0),float2(0.08,0.12)));
        distanceToArrow=min(distanceToArrow,inspectionSegment(p,float2(0.25,0),float2(0.08,-0.12)));
        float aa=max(length(fwidth(xy/spacing)),0.002);
        float outline=1.0-smoothstep(0.018,0.018+aa,distanceToArrow);
        float arrow=1.0-smoothstep(0.004,0.004+aa,distanceToArrow);
        colour=lerp(colour,float3(0.03,0.05,0.07),outline);
        colour=lerp(colour,float3(0.97,0.98,0.92),arrow);
        alpha=lerp(alpha,kInspectionMarker,outline);
    }
    return float4(colour,alpha);
}
#endif
