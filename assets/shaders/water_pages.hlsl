#include "water.hlsl"
#include "terrain_pages.hlsli"
#include "terrain_page_detail.hlsli"
#include "water_scene.hlsli"

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

WaterOut resolveWaterPageDetail(WaterOut input, out float bed, out float head)
{
    const PageDetail detail = pageDetail(input.worldXY);
    bed = detail.bed;
    head = detail.head;
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

WaterOut resolveWaterPageDetail(WaterOut input, out float bed)
{
    float head;
    return resolveWaterPageDetail(input, bed, head);
}
WaterOut resolveWaterPageDetail(WaterOut input) { float bed, head; return resolveWaterPageDetail(input, bed, head); }

// The sea: its own sheet, its waves and its shore (water_sheet.hlsli).
#include "water_sheet.hlsli"

float4 WaterPagePS(WaterOut input) : SV_Target0
{
    float bed, head;
    WaterOut water = resolveWaterPageDetail(input, bed, head);
    const float river = saturate(water.motion.z), lake = saturate(water.motion.w);
    // The sea - its waves and its swash on the sand - is the sheet's, and so
    // is a river's last stretch at the sea's own level. Across the handover
    // page water fades out on top of the sheet, which draws the same water the
    // same way, instead of the two ending against each other. Nothing of the
    // sea's behaviour is ever run here (WaterScenePS: only the sheet is sea).
    const float lakeOwns = smoothstep(0.0, 0.25, lake);
    const float handedOver = wsSheetShare(river, head, head - bed) * (1.0 - lakeOwns);
    const float weight = max(lakeOwns, 1.0 - handedOver);
    if (river + lake <= 0.0001 || weight <= 0.002) discard;
    // A river's ice gives out where the sea takes its mouth over.
    water.ice *= 1.0 - handedOver;
    float4 result = WaterScenePS(water);
    result.a *= weight;
    return result;
}

