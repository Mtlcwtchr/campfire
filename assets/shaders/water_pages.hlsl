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

WaterOut resolveWaterPageDetail(WaterOut input, out float bed)
{
    const PageDetail detail = pageDetail(input.worldXY);
    bed = detail.bed;
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

WaterOut resolveWaterPageDetail(WaterOut input) { float bed; return resolveWaterPageDetail(input, bed); }

// One sea, not two.
//
// The sea used to be drawn twice: by every page square that carried any of
// it (with the square's own triangles deciding where it ended, and cracks
// wherever two squares of different detail met), and again by the open-sea
// sheet under everything, 60 m deep wherever it was, tilted by waves its
// sixteen-kilometre triangles could not carry. Two colours, two edges, two
// sets of seams. Now:
//
//   * the sheet (WaterSheetVS/PS) is THE sea: one flat plane at sea level
//     whose every pixel reads the real bed under it from the page atlas, so
//     it is shallow and clear on the shelf, deep offshore, and ends at the
//     true waterline, per pixel, with no triangle or square in it anywhere;
//   * page water draws rivers and lakes only; the sheet's vertices roll with
//     the waves and its swash runs up the sand (water_surf.hlsli);
//   * where a river meets the sea, the river fades out over the sheet as its
//     flow share falls, so the two mix instead of butting.
//
// The sheet is drawn before page water (its pipeline is taken first, and the
// queue sorts by pipeline), so a river's surface always lies over the sea.

// The sheet's vertices: a grid centred where the camera looks (morphWindow.xy,
// see WaterPass), dense there - under a metre between vertices - and
// exponentially coarser outwards; morphUv.x carries each vertex's own spacing.
// Each vertex reads the bed under it from the page atlas and is lifted by the
// swell and by the wave rolling in to the beach (water_surf.hlsli), so the
// water itself moves and the foam drawn on it rides with it.
// The finest page level resident at p, or -1 over the open ocean.
int wsSheetLevelVS(float2 p)
{
    [unroll] for (int level = 0; level < 4; ++level)
        if (pageAddress(p, level).z != 0.0) return level;
    return -1;
}
// Whether this is the SEA: no lake share in the page's water flags, no water
// standing above sea level, and as much of it as the river's share has handed
// over. A lake is page water's and is never flooded by the sea; a river gives
// way to it across its estuary (the baker grades its share down over the last
// stretch before the coast), so the sheet comes in exactly as the river goes.
float wsSeaWeight(float4 motion, float head)
{
    return (1.0 - smoothstep(0.02, 0.30, saturate(motion.w))) *
           (1.0 - smoothstep(0.0, 1.0, saturate(motion.z))) *
           (1.0 - smoothstep(0.6, 1.5, head));
}
WaterOut WaterSheetVS(WaterIn input)
{
    WaterOut o = (WaterOut)0;
    const float2 xy = morphWindow.xy + input.position.xy;
    const float spacing = max(input.morphUv.x, 0.25);
    const int level = wsSheetLevelVS(xy);
    float bed = -60.0, sea = 1.0, slope = 0.02;
    if (level >= 0) {
        bed = pageHeight(xy, level);
        const float4 at = pageAddress(xy, level);
        const float head = morphWindow.z + pageFields(at.xy, level, 0).z * morphWindow.w;
        sea = wsSeaWeight(pageFields(at.xy, level, 3), head);
        const float bx = pageHeight(xy + float2(3.0, 0.0), level);
        const float by = pageHeight(xy + float2(0.0, 3.0), level);
        slope = length(float2(bx - bed, by - bed)) / 3.0;
    }
    const WsSurfState surf = wsSurfAt(xy, bed, slope, viewport.z, wind.z);
    float2 swellSlope;
    const float depth = max(-bed, 0.0);
    const float swell = waveField(xy, wind.xy, viewport.z, spacing, swellSlope) *
                        waterWaveRoom(depth, 1.0) * waterWaveAmplitude(wind.z) * 1.6 * surf.calm;
    float height = (surf.surface + swell) * sea;
    // Every vertex resource stays referenced (the climate field included):
    // slots are handed out to what a shader reads, and one gone unused would
    // move the page textures onto the wrong slots.
    const SurfaceClimate climate = climateAt(xy);
    if (climate.environment.x < -1.0e9) height += 1.0;
    o.position = project(float3(xy, height));
    o.worldXY = xy;
    o.worldHeight = height;
    o.baseLevel = 0.0;
    o.depth = height - bed;
    o.cover = 1.0;
    o.lift = height;
    return o;
}

bool wsPageResident(float2 page)
{
    [unroll] for (int level = 0; level < 4; ++level)
        if (detailPageEntry(page, level).z != 0.0) return true;
    return false;
}

// The bed under the sheet: the finest resident page level, and the 60 m shelf
// of the open ocean where no page is. Faded to that shelf over the last 128 m
// of a page whose neighbour is missing, so a page border is never a line
// across the sea.
float wsSheetBed(float2 p, out float wet)
{
    const float2 page = floor(p / 512.0);
    float bed = -60.0;
    bool found = false;
    wet = 1.0;
    [unroll] for (int level = 0; level < 4; ++level) {
        if (found) continue;
        const float4 entry = detailPageEntry(page, level);
        if (entry.z != 0.0) {
            bed = detailBedInPage(p, level, page, entry);
            // The page's own wetness share: a hollow inland that dips below
            // sea level is dry ground, not sea.
            wet = detailFields(entry.xy + (p - page * 512.0) * entry.zw, level, 2).z;
            found = true;
        }
    }
    if (!found) return -60.0;
    const float2 local = p - page * 512.0;
    float open = 0.0;
    if (!wsPageResident(page + float2(-1, 0))) open = max(open, 1.0 - smoothstep(0.0, 128.0, local.x));
    if (!wsPageResident(page + float2( 1, 0))) open = max(open, 1.0 - smoothstep(0.0, 128.0, 512.0 - local.x));
    if (!wsPageResident(page + float2(0, -1))) open = max(open, 1.0 - smoothstep(0.0, 128.0, local.y));
    if (!wsPageResident(page + float2(0,  1))) open = max(open, 1.0 - smoothstep(0.0, 128.0, 512.0 - local.y));
    return lerp(bed, min(bed, -60.0), open);
}

// Everything the sheet's pixel needs from the pages, at the finest resident
// level: the sea's own flags, and the slope of the bed (over 3 m, the same
// way the vertex stage takes it).
void wsSheetSample(float2 p, out float sea, out float slope)
{
    sea = 1.0;
    slope = 0.02;
    const float2 page = floor(p / 512.0);
    bool found = false;
    [unroll] for (int level = 0; level < 4; ++level) {
        if (found) continue;
        const float4 entry = detailPageEntry(page, level);
        if (entry.z == 0.0) continue;
        found = true;
        const float2 uv = entry.xy + (p - page * 512.0) * entry.zw;
        const float head = ringWindow.z + detailFields(uv, level, 0).z * ringWindow.w;
        sea = wsSeaWeight(detailFields(uv, level, 3), head);
        const float b0 = detailBedAt(p, level);
        const float bx = detailBedAt(p + float2(3.0, 0.0), level);
        const float by = detailBedAt(p + float2(0.0, 3.0), level);
        slope = length(float2(bx - b0, by - b0)) / 3.0;
    }
}

float4 WaterSheetPS(WaterOut input) : SV_Target0
{
    float wet, sea, slope;
    const float bed = wsSheetBed(input.worldXY, wet);
    wsSheetSample(input.worldXY, sea, slope);
    const WsSurfState surf = wsSurfAt(input.worldXY, bed, slope, viewportPS.z, windPS.z);
    // The breaking wave's own slope, in world metres, from screen derivatives
    // (taken before any discard).
    const float2 dx = ddx(input.worldXY), dy = ddy(input.worldXY);
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float inv = abs(det) > 1e-7 ? 1.0 / det : 0.0;
    const float hx = ddx(surf.surface), hy = ddy(surf.surface);
    float2 face = float2(hx * dy.y - hy * dx.y, dx.x * hy - dy.x * hx) * inv;
    const float faceLength = length(face);
    face *= faceLength > 0.8 ? 0.8 / faceLength : 1.0;
    // Lakes and rivers are page water's: the sea never floods them.
    if (sea < 0.02) discard;
    // The water's height here is the rolling surface; it stands wherever
    // that is above the bed - out on the sea, and up the sand as swash.
    const float surfaceHeight = max(input.worldHeight, surf.surface * sea);
    const float depth = surfaceHeight - bed;
    // Ground below sea level that the world says is dry (a hollow inland) is
    // not the sea's either.
    if (depth <= 0.0 || (bed < 0.0 && wet < 0.02)) discard;
    gSheetSurf = surf;
    gSheetSlope = face * sea;
    gSheetCalm = surf.calm;
    gSheetHasSurf = true;
    input.depth = depth;
    input.lift = input.worldHeight;
    float4 result = WaterScenePS(input);
    // Where a river runs into it, the sea fades in under the river.
    result.a *= smoothstep(0.0, 1.0, sea);
    return result;
}

float4 WaterPagePS(WaterOut input) : SV_Target0
{
    float bed;
    const WaterOut water = resolveWaterPageDetail(input, bed);
    const float flow = saturate(water.motion.z) + saturate(water.motion.w);
    // The sea - its surf and its swash on the sand - is the sheet's.
    if (flow <= 0.0001) discard;
    float4 result = WaterScenePS(water);
    // And across an estuary the river thins out over the sea as its share is
    // handed over, rather than ending at the texel where the ground crossed
    // sea level.
    result.a *= smoothstep(0.0, 1.0, saturate(flow));
    return result;
}
