// Local grass on the active adaptive cut; no private height bake or mesh LOD grid.
#define FOLIAGE_PAGES
#include "foliage.hlsl"
#include "foliage_field.hlsli"
#include "climate_field.hlsli"
#include "terrain_pages.hlsli"

struct PageGrassIn {
    float3 position : TEXCOORD2;
    float4 heights : TEXCOORD3; // parent, prior, prior-parent, source upright
    float run : TEXCOORD4;      // which run of candidates, into the table below
};

// The four numbers that differ between one block of ground and the next - how
// far it has morphed towards its parent, which page level it reads, which one
// its parent reads, and how far apart its candidates stand - packed into the
// one extra float each candidate carries.
//
// A per-draw uniform is what forced one draw per block and per tier: three
// hundred and forty-one of them in a wide view. A table in a storage buffer
// would be the tidier home for them, but a vertex stage's storage buffers are
// addressed after the textures it USES, counted by reflection rather than by
// the register numbers its includes declare, and getting that wrong reads
// zeros with no error anywhere. Eighteen bits in a float the instance already
// pays for needs no such agreement.
//
// Five bits of level and parent level, four of log2 spacing, eight of morph:
// a morph step of one part in 255 is a lerp nobody can see, and the whole code
// stays well inside a float's exact integer range.
struct GrassRun { float morph; int level; int parentLevel; float cell; };
GrassRun decodeGrassRun(float packed)
{
    const uint code = (uint)(packed + 0.5);
    GrassRun run;
    run.level = (int)(code & 7u);
    run.parentLevel = (int)((code >> 3) & 7u);
    run.cell = (float)(1u << ((code >> 6) & 15u));
    run.morph = (float)((code >> 10) & 255u) / 255.0;
    return run;
}
FoliageOut PageGrassVS(FoliageVertexIn vertex, PageGrassIn root)
{
    FoliageInstanceIn instance = (FoliageInstanceIn)0;
    const float2 p = root.position.xy;
    const GrassRun run = decodeGrassRun(root.run);
    const float blockMorph = run.morph;
    const float cellMetres = run.cell;
    const int level = run.level;
    const float4 address = pageAddress(p,level);
    float4 w0 = 0.0;
    float2 w1 = 0.0;
    float head = 0.0;
    if (address.z != 0.0) {
        w0 = pageFields(address.xy,level,1);
        w1 = pageFields(address.xy,level,2).xy;
        head = morphWindow.z+pageFields(address.xy,level,0).z*morphWindow.w;
        if (blockMorph>0.0 && run.level!=run.parentLevel) {
            const float4 parent = pageAddress(p,run.parentLevel);
            if (parent.z!=0.0) {
                w0 = lerp(w0,pageFields(parent.xy,run.parentLevel,1),blockMorph);
                w1 = lerp(w1,pageFields(parent.xy,run.parentLevel,2).xy,blockMorph);
            }
        }
    }
    float z = lerp(root.position.z,root.heights.x,saturate(blockMorph));
    if (morphing.y>=3.0) {
        const float prior = lerp(root.heights.y,root.heights.z,saturate(blockMorph));
        const float t = saturate(morphing.y-3.0);
        z = lerp(prior,z,t*t*(3.0-2.0*t));
    }
    const SurfaceClimate climate = climateAt(p);
    const float field = foliageField(p.x,p.y,32.0);
    const FoliageCommunity community = foliageCommunity(climate.foliage.x,climate.foliage.y,
        climate.foliage.z,climate.foliage.w,field);
    const VegetationCover cover = vegetationCover(w0.x,w0.y,w0.z,w0.w,w1.x,w1.y,
        climate.foliage.x,climate.foliage.y,climate.foliage.z,climate.foliage.w,
        climate.environment.z,z,head-z,root.heights.w,climate.forest,field);
    const uint2 cell = uint2(int2(floor(p/2.0)));
    const float random = foliageHash(cell.x^0x9193u,cell.y);
    const float shape = foliageHash(cell.x,cell.y^0x5791u);
    instance.position = float3(p,z-0.025);
    instance.scale = (0.45+0.40*shape)*community.height;
    const bool coarse = cellMetres>2.5;
    if (coarse) instance.scale *= min(cellMetres,64.0)*0.32; // wider groups, height remains capped
    instance.phase = shape*6.2831853;
    instance.variant = foliageCommunityVariant(climate.foliage.x,climate.foliage.y,
        climate.foliage.z,climate.foliage.w,uint(shape*16777215.0));
    // morphReplacement.zw is the requested focus (not the perspective eye).
    const float nearby = (1.0-smoothstep(144.0,192.0,length(p-morphReplacement.zw)))*morphReplacement.y;
    const float range = coarse?1.0-nearby:nearby;
    instance.tint = float4(cover.red,cover.green,cover.blue,
        (random<cover.grass && address.z!=0.0 && head<=z+0.02?0.92:0.0)*range);
    instance.climate = float4(climate.environment.x,climate.environment.z,climate.geography.w,z);
    return FoliageVS(vertex,instance);
}

