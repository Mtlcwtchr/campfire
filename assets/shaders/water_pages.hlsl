#include "water.hlsl"
#include "terrain_pages.hlsli"
#include "terrain_page_detail.hlsli"

WaterOut waterPageSurfaceVertex(PageSurface s, uint skirt)
{
    WaterIn vertex = (WaterIn)0;
    vertex.position = s.position;
    vertex.normal = s.normal;
    vertex.waterHeight = s.waterHeight;
    // Other bits carry stitched-edge and explicit-source flags, not skirts.
    vertex.waterCover = (skirt & 1) == 0 ? s.waterCover : 0.0;
    vertex.waterMotion = s.waterMotion;
    vertex.morphHeight = s.position.z;
    vertex.morphNormal = s.normal;
    return WaterVS(vertex);
}

WaterOut WaterPageVS(PageGridIn input) { return waterPageSurfaceVertex(gridSurface(input),input.grid.z); }
WaterOut AdaptiveWaterVS(AdaptivePageIn input)
{
    PageSurface s = adaptiveSurface(input);
    const uint explicitSurface = (input.grid.z & 4) != 0;
    // Explicit bed/head define the shoreline. A coarse wetness mask may miss
    // the entire narrow channel. Skirts are still suppressed in the helper.
    if (explicitSurface != 0) s.waterCover = 1.0;
    WaterOut output = waterPageSurfaceVertex(s,input.grid.z);
    output.explicitSurface = explicitSurface;
    return output;
}

WaterOut resolveWaterPageDetail(WaterOut input)
{
    const PageDetail detail = pageDetail(input.worldXY);
    // The waterline per FRAGMENT, always - not per vertex on the adaptive path.
    //
    // Depth used to come from the page only where the mesh had no explicit bed
    // and head of its own, and the adaptive path sets that flag on every vertex
    // it makes. So the shoreline there was the difference of two numbers
    // interpolated across a triangle, and a triangle at the coarse end of the
    // pyramid is hundreds of metres across: the line where the difference
    // reaches nought is then a straight cut over that whole distance. Chain
    // those together and a lake is a polygon with mitred corners and a river is
    // a row of rectangles - which is exactly what they looked like.
    //
    // The page holds the bed and the head as textures, so the fragment can ask
    // where the water really ends instead of guessing between two corners. The
    // edge stops depending on how finely the ground happened to be triangulated,
    // which is the whole point of drawing water by shading.
    //
    // Cover is the one thing the explicit path still keeps to itself. It is a
    // coarse wetness share, and a channel narrower than the square that sampled
    // it has none - so trusting the page's cover there would lose the whole
    // brook. The mesh knows it is wet; only WHERE it ends is the page's answer.
    input.depth = shoreDepthAt(input.worldXY, detail.head - detail.bed);
    if (input.explicitSurface == 0) input.cover = detail.cover;
    input.motion = detail.motion;
    return input;
}

float4 WaterPagePS(WaterOut input) : SV_Target0 { return WaterPS(resolveWaterPageDetail(input)); }

