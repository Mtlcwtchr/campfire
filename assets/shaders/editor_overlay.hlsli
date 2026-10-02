#ifndef EDITOR_OVERLAY_HLSLI
#define EDITOR_OVERLAY_HLSLI
// The world editor's marks on the world: the region grid, the regions that are
// selected, the ones that are still empty, the one under the pointer and the
// brush. Drawn by the terrain AND the water, so they read over land and sea
// alike - an empty world is all sea, and its grid still has to be seen.
//
// editor[0]  region metres, on (1: regions and brush, 2: the brush alone),
//            regions across, regions down
// editor[1]  hovered region x, y (-1 none), border band half-width m,
//            texel metres of the layer being painted (0: the regions)
// editor[2]  brush centre x, y (world m), brush radius m,
//            1 paint / select / raise, 2 erase / clear / lower,
//            3 a procedural brush, 4 plant
// editor[3..18]   selected regions, 16 bits to a component, index y * 32 + x
//                 (32 x 32 regions, the most a world has)
// editor[19..34]  generated regions, the same way
//
// Needs world.hlsli.

bool editorBit(int first, uint index)
{
    const uint component = index / 16u;
    const float4 packed = editorPS[first + int(component / 4u)];
    const uint word = uint(packed[component % 4u] + 0.5);
    return ((word >> (index % 16u)) & 1u) != 0u;
}

// rgb what to lay over the picture, a how much of it. `pixel` is metres to a
// pixel, taken by the caller before anything could discard.
float4 editorMarks(float2 xy, float pixel)
{
    if (editorPS[0].y < 0.5) return 0;
    pixel = max(pixel, 1e-3);
    float3 colour = 0;
    float amount = 0;
    const float region = max(editorPS[0].x, 1.0);
    const int2 regions = int2(editorPS[0].zw + 0.5);
    const float2 cellFloor = floor(xy / region);
    const int2 cell = int2(cellFloor);
    // The ground tools want their brush and nothing else: a region grid over
    // the ground being sculpted is noise.
    if (editorPS[0].y < 1.5 && all(cell >= 0) && all(cell < regions)) {

    const uint index = uint(cell.y * 32 + cell.x);
    const bool selected = editorBit(3, index);
    const bool generated = editorBit(19, index);
    const bool hovered = all(cell == int2(floor(editorPS[1].xy + 0.5)));

    if (!generated) {
        // Empty: a faint shade, so it reads as "nothing made here yet". Not a
        // hatch: with the sea drawn past the world's edge, a striped grid over
        // most of the map read as a field laid on the world, not as sea.
        colour = float3(0.07, 0.09, 0.13);
        amount = 0.08;
    }
    if (selected) {
        colour = lerp(colour, float3(1.0, 0.70, 0.22), amount > 0 ? 0.8 : 1.0);
        amount = max(amount, 0.32);
    }
    if (hovered) {
        colour = lerp(colour, float3(1.0, 1.0, 1.0), 0.35);
        amount = max(amount, 0.12) + 0.08;
    }

    // The transition band either side of every border between two regions:
    // barely there, so it says where the country will change without looking
    // like a second grid.
    const float2 local = xy - cellFloor * region;
    const float2 toEdge = min(local, region - local);
    const float edge = min(toEdge.x, toEdge.y);
    const float band = editorPS[1].z;
    const bool interior = (toEdge.x < toEdge.y)
        ? ((local.x < region * 0.5 && cell.x > 0) || (local.x >= region * 0.5 && cell.x + 1 < regions.x))
        : ((local.y < region * 0.5 && cell.y > 0) || (local.y >= region * 0.5 && cell.y + 1 < regions.y));
    if (band > 0 && interior && edge < band) {
        colour = lerp(colour, float3(0.75, 0.85, 1.0), amount > 0 ? 0.15 : 1.0);
        amount = max(amount, 0.07);
    }

    // The grid line itself, a couple of pixels wide at any zoom.
    const float rule = 1.0 - smoothstep(pixel * 1.2, pixel * 2.6, edge);
    const float3 ink = selected ? float3(1.0, 0.82, 0.38) : float3(0.93, 0.96, 1.0);
    colour = lerp(colour, ink, rule);
    amount = max(amount, rule * 0.85);
    }

    // The brush: a ring, amber to paint, red to wipe, cyan for a generator
    // pass run as a brush.
    if (editorPS[2].z > 0.0) {
        const float away = length(xy - editorPS[2].xy);
        const float ring = 1.0 - smoothstep(pixel * 1.5, pixel * 3.2, abs(away - editorPS[2].z));
        const float3 brush = editorPS[2].w > 3.5 ? float3(0.45, 0.95, 0.45)
                           : editorPS[2].w > 2.5 ? float3(0.35, 0.90, 1.0)
                           : editorPS[2].w > 1.5 ? float3(1.0, 0.32, 0.25) : float3(1.0, 0.85, 0.35);
        // A layer is being painted: inside the brush, the layer's own texels,
        // so what one dab can hold is on the ground where it lands - and only
        // once a texel is a few pixels across, or the grid is a grey smear.
        const float texel = editorPS[1].w;
        if (texel > 0.5 && texel > pixel * 5.0) {
            const float inside = 1.0 - smoothstep(editorPS[2].z - pixel * 2.0, editorPS[2].z, away);
            const float2 inTexel = xy - floor(xy / texel) * texel;
            const float2 toLines = min(inTexel, texel - inTexel);
            const float grid = (1.0 - smoothstep(pixel * 0.6, pixel * 1.6, min(toLines.x, toLines.y))) * inside;
            colour = lerp(colour, lerp(brush, float3(1.0, 1.0, 1.0), 0.5), grid * 0.8);
            amount = max(amount, grid * 0.45);
            colour = lerp(colour, brush, inside * 0.15 * (1.0 - grid));
            amount = max(amount, inside * 0.06);
        }
        colour = lerp(colour, brush, ring);
        amount = max(amount, ring * 0.95);
    }
    return float4(colour, saturate(amount));
}

// The same marks over a blended surface (water): over its colour, and at least
// as opaque as themselves, so the grid and the selection read over the sea.
float4 withEditorMarks(float4 result, float2 xy, float pixel)
{
    const float4 marks = editorMarks(xy, pixel);
    result.rgb = lerp(result.rgb, marks.rgb, marks.a);
    result.a = max(result.a, marks.a);
    return result;
}
#endif

