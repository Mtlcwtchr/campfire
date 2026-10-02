#ifndef WATER_SHEET_HLSLI
#define WATER_SHEET_HLSLI
// One sea, not two - and rivers that run into it without a line.
//
//   * the sheet (WaterSheetVS/PS) is THE sea: one surface centred where the
//     camera looks, whose every vertex and pixel reads the real ground under
//     it from the page atlas - so it is shallow and clear on the shelf, deep
//     offshore, ends at the true waterline, and its vertices carry the waves
//     rolling in to the beach and the swash running up the sand
//     (water_surf.hlsli);
//   * page water draws rivers and lakes, and never anything of the sea's;
//   * a river mouth is the sheet's too, once the river has handed most of its
//     water to the sea or runs at the sea's own level (wsSheetShare). Across
//     the handover page water fades out on top of the sheet, and both shade
//     that water from the same flags in the same way, so the sea's colour and
//     its swell come in as the river's share goes out and neither ends at a
//     line.
//
// The sheet is drawn before page water (its pipeline is taken first, and the
// queue sorts by pipeline), so a river's surface always lies over the sea.
//
// Needs water.hlsl, terrain_pages.hlsli, terrain_page_detail.hlsli and
// water_scene.hlsli before it.

// --- what the sheet reads from the pages ------------------------------------

// Everything the sheet needs at a point, from the finest level resident there.
struct WsPlace {
    float bed;
    float head;
    float cover;
    float river;
    float lake;
    float2 flow;
};

// The pages as the vertex stage has them bound.
WsPlace wsPlaceVS(float2 p)
{
    WsPlace s = (WsPlace)0;
    s.bed = -60.0;
    s.cover = 1.0;
    bool found = false;
    [unroll] for (int level = 0; level < kPageDatasets; ++level) {
        if (found) continue;
        const float4 at = pageAddress(p, level);
        if (at.z == 0.0) continue;
        found = true;
        s.bed = pageHeight(p, level);
        s.head = morphWindow.z + pageFields(at.xy, level, 0).z * morphWindow.w;
        s.cover = pageFields(at.xy, level, 2).z;
        const float4 motion = pageFields(at.xy, level, 3);
        s.flow = motion.xy * 2.0 - 1.0;
        s.river = saturate(motion.z);
        s.lake = saturate(motion.w);
    }
    return s;
}
float wsBedVS(float2 p)
{
    [unroll] for (int level = 0; level < kPageDatasets; ++level)
        if (pageAddress(p, level).z != 0.0) return pageHeight(p, level);
    return -60.0;
}

// And as the pixel stage has them.
WsPlace wsPlacePS(float2 p)
{
    WsPlace s = (WsPlace)0;
    s.bed = -60.0;
    s.cover = 1.0;
    bool found = false;
    [unroll] for (int level = 0; level < kPageDatasets; ++level) {
        if (found) continue;
        float2 page;
        const float4 entry = detailEntryAt(p, level, page);
        if (entry.z == 0.0) continue;
        found = true;
        const float2 uv = entry.xy + (p - page) * entry.zw;
        s.bed = detailBedInPage(p, level, page, entry);
        s.head = ringWindow.z + detailFields(uv, level, 0).z * ringWindow.w;
        s.cover = detailFields(uv, level, 2).z;
        const float4 motion = detailFields(uv, level, 3);
        s.flow = motion.xy * 2.0 - 1.0;
        s.river = saturate(motion.z);
        s.lake = saturate(motion.w);
    }
    return s;
}
float wsBedPS(float2 p)
{
    [unroll] for (int level = 0; level < kPageDatasets; ++level) {
        float2 page;
        const float4 entry = detailEntryAt(p, level, page);
        if (entry.z != 0.0) return detailBedInPage(p, level, page, entry);
    }
    return -60.0;
}

// Whether any dataset holds ground over the 512 m square `page`.
bool wsPageResident(float2 page)
{
    const float2 centre = (page + 0.5) * 512.0;
    [unroll] for (int level = 0; level < kPageDatasets; ++level) {
        float2 origin;
        if (detailEntryAt(centre, level, origin).z != 0.0) return true;
    }
    return false;
}
// Over the last 128 m of a page whose neighbour is missing the bed is faded to
// the open ocean's 60 m shelf, so a page border is never a line across the sea.
float wsOpenSea(float2 p)
{
    const float2 page = floor(p / 512.0);
    const float2 local = p - page * 512.0;
    float open = 0.0;
    [branch] if (local.x < 128.0 && !wsPageResident(page + float2(-1, 0)))
        open = max(open, 1.0 - smoothstep(0.0, 128.0, local.x));
    [branch] if (local.x > 384.0 && !wsPageResident(page + float2(1, 0)))
        open = max(open, 1.0 - smoothstep(0.0, 128.0, 512.0 - local.x));
    [branch] if (local.y < 128.0 && !wsPageResident(page + float2(0, -1)))
        open = max(open, 1.0 - smoothstep(0.0, 128.0, local.y));
    [branch] if (local.y > 384.0 && !wsPageResident(page + float2(0, 1)))
        open = max(open, 1.0 - smoothstep(0.0, 128.0, 512.0 - local.y));
    return open;
}

// Which way, and how fast, the ground rises towards the shore - over sixteen
// metres, so the slope the waves are timed by bends their crests rather than
// tearing them into pieces at every hummock.
float2 wsBedRiseVS(float2 p)
{
    const float h = 8.0;
    return float2(wsBedVS(p + float2(h, 0.0)) - wsBedVS(p - float2(h, 0.0)),
                  wsBedVS(p + float2(0.0, h)) - wsBedVS(p - float2(0.0, h))) / (2.0 * h);
}
float2 wsBedRisePS(float2 p)
{
    const float h = 8.0;
    return float2(wsBedPS(p + float2(h, 0.0)) - wsBedPS(p - float2(h, 0.0)),
                  wsBedPS(p + float2(0.0, h)) - wsBedPS(p - float2(0.0, h))) / (2.0 * h);
}

// --- where the sea may run up the land --------------------------------------
//
// The swash is the sea's, so it may only climb ground that falls away to open
// sea within a few tens of metres. Low ground inland - a coastal flat, a hollow
// behind a dune, a river's bank a kilometre up from its mouth - is as low as a
// beach, and used to be flooded by every wave as if it were one.

// Open sea: under sea level, wet, and neither lake nor (much) river.
float wsOpenWater(WsPlace s)
{
    return smoothstep(-0.02, -0.25, s.bed) * smoothstep(0.01, 0.12, s.cover) *
           (1.0 - smoothstep(0.45, 0.70, s.river)) * (1.0 - smoothstep(0.02, 0.25, s.lake));
}
// Probed down the fall of the ground: short of where it would meet the water
// if it kept falling as it does here, just past that, and well past it.
float wsReachAhead(float e, float slope, int k)
{
    const float ahead = clamp(e / max(slope, 0.01), 2.0, 28.0);
    const float along = k == 0 ? ahead * 0.6 + 2.0 : (k == 1 ? ahead + 4.0 : ahead * 1.5 + 8.0);
    return min(along, 34.0);
}
float wsBeachReachVS(float2 p, float e, float2 downhill, float slope)
{
    float found = 0.0;
    [unroll] for (int k = 0; k < 3; ++k)
        found = max(found, wsOpenWater(wsPlaceVS(p + downhill * wsReachAhead(e, slope, k))));
    return found;
}
float wsBeachReachPS(float2 p, float e, float2 downhill, float slope)
{
    float found = 0.0;
    [unroll] for (int k = 0; k < 3; ++k)
        found = max(found, wsOpenWater(wsPlacePS(p + downhill * wsReachAhead(e, slope, k))));
    return found;
}

// How much of the water here the sheet draws (a lake is never the sea's).
float wsSheetOwns(WsPlace s)
{
    return saturate(wsSheetShare(s.river, s.head, s.head - s.bed) * 4.0) *
           (1.0 - smoothstep(0.02, 0.30, s.lake));
}

WsCoastIn wsCoastInput(float2 p, WsPlace here, float level, float2 rise, float sea, float reach,
                       float4 windNow, float clock, float spacing)
{
    WsCoastIn i;
    i.p = p;
    i.bed = here.bed;
    i.level = level;
    i.slope = length(rise);
    const float windLength = length(windNow.xy);
    i.windDir = windLength > 1e-4 ? windNow.xy / windLength : float2(1.0, 0.0);
    i.sea = sea;
    i.reach = reach;
    i.clock = clock;
    i.wind = windNow.z;
    i.spacing = spacing;
    return i;
}

// --- the sheet ----------------------------------------------------------------

// Its vertices: a grid centred where the camera looks (morphWindow.xy, see
// WaterPass), about half a metre between vertices there and exponentially
// coarser outwards; morphUv.x carries each vertex's own spacing, which decides
// the shortest wave it may carry. Each vertex is lifted by the open sea's swell
// and, near a coast, by the wave rolling in and the swash running up the sand.
WaterOut WaterSheetVS(WaterIn input)
{
    WaterOut o = (WaterOut)0;
    const float2 xy = morphWindow.xy + input.position.xy;
    const float spacing = max(input.morphUv.x, 0.25);
    const WsPlace here = wsPlaceVS(xy);
    const float owned = wsSheetOwns(here);
    const float sea = wsSeaShare(here.river, here.lake) * owned;
    const float level = wsSheetLevel(here.head, here.river);
    const float2 rise = wsBedRiseVS(xy);
    const float slope = length(rise);
    const float e = here.bed - level;
    float reach = e <= 0.0 ? 1.0 : 0.0;
    [branch] if (e > 0.0 && e < kWsMaxRunup && sea > 0.0 && slope > 0.004)
        reach = wsBeachReachVS(xy, e, -rise / slope, slope);
    const WsCoast coast = wsCoastAt(wsCoastInput(xy, here, level, rise, sea, reach,
                                                 wind, viewport.z, spacing));
    // The open sea's own swell, handed over to the coast's waves as they shoal.
    float2 swellSlope;
    const float swell = waveField(xy, wind.xy, viewport.z, spacing, swellSlope) *
                        waterWaveRoom(max(level - here.bed, 0.0), 1.0) * waterWaveAmplitude(wind.z) *
                        1.6 * coast.calm * sea;
    float height = level + coast.wave + swell;
    // The swash: the sheet over the sand as far as it has climbed, and just
    // clear of the ground over the strip the sea has lately been over, where
    // the foam it left and the wet sand are drawn. A little above both, as the
    // ground between two vertices is not the straight line between them; the
    // pixel stage decides exactly where the water ends.
    const float margin = 0.02 + spacing * min(slope, 0.5) * 0.3;
    if (coast.swashTop > kWsNone * 0.5) height = max(height, level + coast.swashTop + margin);
    if (e > 0.0 && e < coast.swept * 1.08 + 0.03) height = max(height, here.bed + 0.03 + margin);
    // Every vertex resource stays referenced (the climate field included):
    // slots are handed out to what a shader reads, and one gone unused would
    // move the page textures onto the wrong slots.
    const SurfaceClimate climate = climateAt(xy);
    if (climate.environment.x < -1.0e9) height += 1.0;
    o.position = project(float3(xy, height));
    o.worldXY = xy;
    o.worldHeight = height;
    o.baseLevel = level;
    o.depth = height - here.bed;
    o.cover = 1.0;
    o.lift = coast.wave + swell;
    o.motion = float4(here.flow, here.river, here.lake);
    return o;
}

float4 WaterSheetPS(WaterOut input) : SV_Target0
{
    const float2 p = input.worldXY;
    const float2 dx = ddx(p), dy = ddy(p);
    const float footprint = max(length(dx), length(dy));
    WsPlace here = wsPlacePS(p);
    here.bed = lerp(here.bed, min(here.bed, -60.0), wsOpenSea(p));
    const float owned = wsSheetOwns(here);
    const float seaShare = wsSeaShare(here.river, here.lake);
    const float sea = seaShare * owned;
    const float level = wsSheetLevel(here.head, here.river);
    const float2 rise = wsBedRisePS(p);
    const float slope = length(rise);
    const float e = here.bed - level;
    float reach = e <= 0.0 ? 1.0 : 0.0;
    [branch] if (e > 0.0 && e < kWsMaxRunup && sea > 0.0 && slope > 0.004)
        reach = wsBeachReachPS(p, e, -rise / slope, slope);
    const WsCoastIn coastIn = wsCoastInput(p, here, level, rise, sea, reach,
                                           windPS, viewportPS.z, footprint);
    const WsCoast coast = wsCoastAt(coastIn);
    // The incoming wave's own slope, in world metres, from screen derivatives
    // (taken before anything can discard), for the light to see its face.
    const float det = dx.x * dy.y - dx.y * dy.x;
    const float inv = abs(det) > 1e-7 ? 1.0 / det : 0.0;
    const float hx = ddx(coast.wave), hy = ddy(coast.wave);
    float2 face = float2(hx * dy.y - hy * dx.y, dx.x * hy - dy.x * hx) * inv;
    const float faceLength = length(face);
    face *= faceLength > 0.8 ? 0.8 / faceLength : 1.0;

    // Lakes, and rivers still above the sea, are page water's.
    if (extraPS.w > 0.5 || owned < 0.004) discard;
    // Ground below sea level that the world says is dry (a hollow inland) is
    // not the sea's either.
    if (here.bed < level && here.cover < 0.02) discard;
    // The water stands wherever its rolling surface - the wave, or the swash
    // over the sand - is above the ground, and nowhere else.
    const float surfaceHeight = level + wsCoastSurface(coast);
    const float depth = surfaceHeight - here.bed;
    [branch] if (depth <= 0.0) {
        // Sand the sea has just left: what it stranded there, and the wet.
        if (coast.residue < 0.004 && coast.wetSand < 0.004) discard;
        float4 decal = wsSandDecal(float3(p, here.bed), coast, footprint);
        decal.a *= owned;
        return decal;
    }
    gCoast = coast;
    gCoastShoreward = slope > 1e-4 ? rise / slope : coastIn.windDir;
    gCoastFace = face;
    gSheetHasCoast = true;
    input.depth = depth;
    input.worldHeight = surfaceHeight;
    input.baseLevel = level;
    input.lift = coast.wave;
    // The sea needs no coverage; up a river mouth the page's does the same
    // work it does for page water.
    input.cover = lerp(1.0, here.cover, 1.0 - seaShare);
    input.motion = float4(here.flow, here.river, here.lake);
    input.ice = 0.0;
    float4 result = WaterScenePS(input);
    result.a *= owned;
    return withEditorMarks(result, p, footprint);
}
#endif

