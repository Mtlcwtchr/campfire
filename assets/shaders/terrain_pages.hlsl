#define TERRAIN_PAGE_MATERIALS
#include "terrain.hlsl"
#include "terrain_pages.hlsli"
#include "terrain_page_detail.hlsli"

TerrainOut terrainPageSurfaceVertex(PageSurface s)
{
    TerrainIn vertex = (TerrainIn)0;
    vertex.position = s.position;
    vertex.normal = s.normal;
    vertex.weights0 = s.weights0;
    vertex.weights1 = s.weights1;
    vertex.uv = s.position.xy / morphReplacement.w;
    vertex.morphUv = vertex.uv;
    vertex.morphHeight = s.position.z;
    vertex.morphNormal = s.normal;
    vertex.waterHeight = s.waterHeight;
    vertex.waterCover = s.waterCover;
    vertex.waterMotion = s.waterMotion;
    vertex.relief = s.relief;
    return TerrainVS(vertex);
}

TerrainOut TerrainPageVS(PageGridIn input) { return terrainPageSurfaceVertex(gridSurface(input)); }
TerrainOut AdaptiveTerrainVS(AdaptivePageIn input)
{
    TerrainOut output = terrainPageSurfaceVertex(adaptiveSurface(input));
    output.stageDiagnostic = input.diagnostic;
    if ((int)extra.w == 15) output.geography.xy=input.drainage;
    output.triangleBary = input.grid.w == 0 ? float3(1,0,0) :
                          input.grid.w == 1 ? float3(0,1,0) : float3(0,0,1);
    return output;
}

float sampleGrid(float2 p, float step)
{
    const float2 cell = p / step;
    const float2 pixel = max(fwidth(cell), 0.000001);
    const float2 distance = abs(frac(cell + 0.5) - 0.5) / pixel;
    // Fade rather than alias when several grid lines fit inside a pixel.
    return saturate(1.25 - min(distance.x, distance.y)) *
           (1.0 - smoothstep(0.15, 0.45, max(pixel.x, pixel.y)));
}

float3 gridColour(float step)
{
    if (step <= 4.0) return float3(0.1, 0.9, 1.0);
    if (step <= 8.0) return float3(0.2, 1.0, 0.3);
    if (step <= 16.0) return float3(1.0, 0.9, 0.15);
    if (step <= 32.0) return float3(1.0, 0.45, 0.1);
    if (step <= 64.0) return float3(1.0, 0.25, 0.8);
    if (step <= 128.0) return float3(0.3, 0.5, 1.0);
    return float3(0.7, 0.45, 1.0);
}

float dataStep(float dataset)
{
    return dataset < 0.5 ? 4.0 : dataset < 1.5 ? 8.0 : dataset < 2.5 ? 16.0 : 64.0;
}

float4 TerrainPagePS(TerrainOut input) : SV_Target0
{
    const PageDetail detail = pageDetail(input.worldXY);
    const float3 shape = pageShape(input.worldXY);
    const bool staged = ringState.y >= 3.0;
    const float3 geometric = normalize(cross(ddx(float3(input.worldXY,input.worldHeight)),
        ddy(float3(input.worldXY,input.worldHeight))));
    const float3 normal = geometric.z < 0.0 ? -geometric : geometric;
    input.normal = staged ? normal : normalize(float3(-shape.xy, 1.0));
    input.geography.z = shape.z;
    input.weights0 = detail.weights0;
    input.weights1 = detail.weights1;
    if (!staged) input.waterDepth = detail.head - detail.bed;
    // Triplanar UVs and height tint use the shading field, not the morphing mesh.
    if (!staged) input.worldHeight = detail.bed;
    const int mode = (int)extraPS.w;
    if (mode == 7 || mode >= 9) {
        const float slope = length(input.normal.xy) / max(0.001,input.normal.z);
        const float lighting = 0.35 + 0.65*saturate(dot(input.normal,normalize(float3(-0.4,-0.6,0.8))));
        float3 colour = 0.65;
        if (mode == 7) {
            colour = lerp(float3(0.12,0.3,0.18),float3(0.95,0.88,0.72),saturate(input.worldHeight/2200.0));
            if (input.worldHeight < 0) colour=float3(0.12,0.25,0.42);
            const float contour=abs(frac(input.worldHeight/50.0+0.5)-0.5)/max(fwidth(input.worldHeight/50.0),0.0001);
            colour *= lerp(0.45,1.0,saturate(contour));
        } else if (mode == 10) colour=lerp(float3(0.12,0.4,0.2),float3(1,0.2,0.05),saturate(slope));
        else if (mode == 11) {
            const float delta=input.stageDiagnostic.x;
            colour=lerp(float3(0.6,0.6,0.6),delta<0?float3(0.1,0.3,1):float3(1,0.2,0.08),saturate(abs(delta)/100.0));
        } else if (mode == 12) colour=lerp(float3(0.75,0.72,0.65),float3(0.4,0.03,0.02),saturate(input.stageDiagnostic.y/150.0));
        else if (mode == 13) colour=lerp(float3(0.3,0.25,0.15),float3(0.05,0.55,1),saturate(input.stageDiagnostic.z/16.0));
        else if (mode == 14) colour=0.25+0.65*frac(float3(0.37,0.61,0.83)*(floor(input.stageDiagnostic.w+0.5)+1));
        else if (mode == 15) {
            const float magnitude=length(input.geography.xy);
            const float2 direction=input.geography.xy/max(0.001,magnitude);
            colour=float3(direction*0.35+0.45,saturate(input.stageDiagnostic.z/16.0));
            const float2 p=frac(input.worldXY/64.0)-0.5;
            const float along=dot(p,direction),across=abs(dot(p,float2(-direction.y,direction.x)));
            const float width=max(0.02,length(fwidth(p)));
            const float shaft=(1-smoothstep(width,width*2,across))*step(abs(along),0.28);
            const float tip=(1-smoothstep(width,width*2,abs(across-(0.28-along)*0.7)))*step(0.06,along)*step(along,0.28);
            const float visible=(1-smoothstep(0.15,0.45,max(fwidth(p.x),fwidth(p.y))))*step(0.01,magnitude);
            colour=lerp(colour,float3(0.9,0.95,1),max(shaft,tip)*visible);
        }
        return float4(colour*lighting,1);
    }
    float4 colour = TerrainPS(input);
    // Bit 0 still enables the vertex skirt floor; bits 1.. select the overlay.
    const int grid = (int)ringReserved.w / 2;
    if (grid != 0) {
        const float step = grid == 1 ? dataStep(ringState.z) : ringState.w;
        float ink = sampleGrid(input.worldXY, step);
        float3 tint = gridColour(step);
        if (grid == 2) {
            const float3 edge = input.triangleBary / max(fwidth(input.triangleBary), 0.000001);
            ink = 1.0 - smoothstep(0.5,1.5,min(edge.x,min(edge.y,edge.z)));
            // Real triangle edges, coloured by their source lattice, NOT by H4/H8/H16/H64.
            tint = gridColour(step);
        }
        if (grid == 1 && ringState.z != ringReplacement.x) {
            const float parentStep = dataStep(ringReplacement.x);
            const float parentInk = sampleGrid(input.worldXY, parentStep);
            const float morph = saturate(ringState.x);
            tint = lerp(tint * ink, gridColour(parentStep) * parentInk, morph);
            ink = lerp(ink, parentInk, morph);
            tint /= max(ink, 0.000001);
        }
        colour.rgb = lerp(colour.rgb, tint, ink * 0.9);
    }
    return colour;
}
