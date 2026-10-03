#ifndef TERRAIN_DECAL_TEXTURES_HLSLI
#define TERRAIN_DECAL_TEXTURES_HLSLI
#ifdef BIOME_DECAL_TEXTURE
// Reuse the terrain arrays and the biome candidate/gate; no scene entities,
// additional bindings or depth fighting. Include after terrain_material.hlsli.
void biomeTexture(int row,float weight,BiomeDecalIn d,inout BiomeDecalOut o) {
    // Every candidate is already faded out at this footprint. Avoid the
    // cluster noise, neighbour hashes and texture reads for that exact zero.
    const float4 spacing=biomeRow(row,1);
    const float largest=max(max(spacing.z,spacing.w),0.05);
    if (d.pixel>=2.0*largest) return;
    float4 look,size;
    const float gate=biomeDecalGate(row,weight,d,look,size)*smoothstep(0.0,0.12,d.aboveWater);
    const int layer=int(biomeRow(row,5).z)-1;
    if (gate<=0.001 || layer<0 || layer>=GROUND_LAYERS) return;
    const float cellMetres=max(size.x,0.1);
    // Cells and the position in them from the frame (world_frame.hlsli): a
    // 0.22 m moss decal read from the world coordinate stepped in 1.6 cm -
    // seven texels of it at a time, drawn as stripes.
    float2 base,inCell;
    frameCells(d.anchor,d.local,cellMetres,base,inCell);
    const float2 dx=ddx(d.local),dy=ddy(d.local);
    // Centres are in [0.25,0.75] of each cell. A rotated square's widest
    // half-extent is sqrt(2)/2 times its size: cells beyond that cannot cover
    // this pixel. Preserve the old neighbour order for overlapping decals.
    const float reach=largest*0.707107/cellMetres+0.0001;
    const int2 first=max(int2(-1,-1),int2(ceil(inCell-0.75-reach)));
    const int2 last=min(int2(1,1),int2(floor(inCell-0.25+reach)));
    [loop] for (int j=first.y;j<=last.y;++j) [loop] for (int i=first.x;i<=last.x;++i) {
        const float2 neighbour=float2(i,j);
        const float2 c=fmod(base+neighbour+4096.0,4096.0);
        const float pick=hashAt(c+float(row)*2.414);
        if (pick>size.y) continue;
        const float shape=hashAt(c+float(row)*1.618+3.3);
        const float2 centre=neighbour+float2(0.25+shape*0.5,0.25+frac(pick*7.7)*0.5);
        const float metres=max(lerp(size.z,size.w,frac(shape*4.7)),0.05);
        const float resolved=1.0-smoothstep(0.5,2.0,d.pixel/metres);
        if (resolved<=0) continue;
        const float turn=shape*6.2831853+biomeRow(row,5).x;
        const float2 u=float2(cos(turn),sin(turn)),v=float2(-u.y,u.x);
        const float2 offset=(inCell-centre)*cellMetres;
        const float2 uv=float2(dot(offset,u),dot(offset,v))/metres+0.5;
        const float2 gx=float2(dot(dx,u),dot(dx,v))/metres;
        const float2 gy=float2(dot(dy,u),dot(dy,v))/metres;
        if (any(uv<0) || any(uv>1)) continue;
        const float edge=smoothstep(0.0,max(d.pixel/metres,0.002),min(min(uv.x,uv.y),min(1-uv.x,1-uv.y)));
        const float4 properties=groundPropertiesTex.SampleGrad(groundPropertiesSampler,float3(uv,layer),gx,gy);
        const float cover=saturate(properties.a*look.a*gate*edge*resolved);
        if (cover<=0.001) continue;
        const float3 colour=groundTex.SampleGrad(groundSampler,float3(uv,layer),gx,gy).rgb*look.rgb;
        const float3 normal=groundNormalTex.SampleGrad(groundNormalSampler,float3(uv,layer),gx,gy).xyz*2-1;
        const float added=cover*(1-o.cover);
        o.normalDelta.xy+=(u*normal.x+v*normal.y)*added*0.45;
        o.ao=lerp(o.ao,properties.r,added);
        biomeLayDecal(o,colour,cover,0.0,0.0,properties.g);
    }
}
#endif
#endif
