// Local grass on the active adaptive cut; no private height bake or mesh LOD grid.
#define FOLIAGE_PAGES
#include "foliage.hlsl"
#include "foliage_field.hlsli"
#include "climate_field.hlsli"
#include "terrain_pages.hlsli"
// The terrain categories' ground cover (engine/biomes), in the vertex stage
// after the page fields (t12, t13 space0).
#define BIOME_SPACE space0
#define BIOME_TRANSFORM parameters[20]
#define BIOME_PLANE_SLOT t12
#define BIOME_PLANE_SAMPLER s12
#define BIOME_TABLE_SLOT t13
#define BIOME_TABLE_SAMPLER s13
#include "terrain_biomes.hlsli"

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

// Which plant stands on a near candidate. The imported views are the meadow's
// body; around them the ground is broken up the way a painted countryside is:
//  - flowers in drifts of one colour (a patch tens of metres across is white,
//    the next one yellow), on open temperate and steppe ground only;
//  - ferns and low leafy undergrowth under a canopy, where tall grass does not
//    grow, and short grass between them - the floor of a wood shows;
//  - reeds and sedge on wet ground and just above standing water;
//  - dry grass and seed heads where the country is dry.
// Every choice is a pattern fixed to the ground (cell hashes, value noise), so
// nothing changes as the camera moves.
float pageFloraCard(float2 p, float4 foliage, float canopy, float moisture, float marsh,
                    float aboveWater, float baseVariant, float pick, float flowerScale = 1.0, float parched = 0.0)
{
    const float sum = max(foliage.x + foliage.y + foliage.z + foliage.w, 1e-3);
    const float steppe = foliage.x / sum, boreal = foliage.y / sum;
    const float temperate = foliage.z / sum, tropical = foliage.w / sum;
    const float open = 1.0 - smoothstep(0.15, 0.55, canopy);
    // Reeds: the wet margin and the marsh.
    const float shore = 1.0 - smoothstep(0.15, 0.9, aboveWater);
    const float reeds = saturate(shore * (0.35 + 0.65 * saturate(moisture * 1.4)) + marsh * 0.8) * 0.75;
    // Drifts of flowers: a coarse field says where, a finer one breaks the edge.
    const float drift = smoothstep(0.50, 0.70, foliageNoise(p.x / 37.0 + 13.1, p.y / 37.0 - 5.7)) *
                        smoothstep(0.30, 0.55, foliageNoise(p.x / 9.0 - 2.3, p.y / 9.0 + 8.9));
    const float flowers = drift * open * (temperate * 0.55 + steppe * 0.40 + tropical * 0.30) *
                          (1.0 - reeds) * (0.55 + 0.45 * smoothstep(0.2, 0.6, moisture)) * flowerScale;
    // A scatter of single flowers anywhere open, so a meadow is never only grass.
    const float stray = open * (temperate + steppe * 0.6) * 0.035 * flowerScale;
    // The floor of a wood.
    const float wood = smoothstep(0.10, 0.60, canopy);
    const float ferns = wood * (0.42 + boreal * 0.25 + temperate * 0.10) * (0.6 + 0.4 * saturate(moisture * 1.6));
    const float under = wood * 0.22 + (1.0 - wood) * open * temperate * 0.05;
    // Dry country.
    const float dry = saturate(saturate(steppe * 0.9 + (1.0 - saturate(moisture * 1.8)) * 0.5) * open * 0.55 + parched);

    float t = pick;
    if ((t -= reeds) < 0.0) return float(kCardReeds);
    if ((t -= flowers + stray) < 0.0) {
        const float hue = foliageNoise(p.x / 23.0 + 41.0, p.y / 23.0 - 17.0);
        if (hue < 0.36) return float(kCardFlowersWhite);
        if (hue < 0.58) return float(kCardFlowersYellow);
        if (hue < 0.80 || tropical > 0.5) return float(kCardFlowersPurple);
        return float(kCardFlowersRed);
    }
    if ((t -= ferns) < 0.0) return float(kCardFern);
    if ((t -= under) < 0.0) return float(kCardUndergrowth);
    if ((t -= dry) < 0.0) return frac(pick * 7.13) < 0.6 ? float(kCardDryGrass) : float(kCardSeedGrass);
    // Grass: under trees short, in the open the imported meadow grass with
    // short grass and seed heads through it.
    const float rest = frac(pick * 13.7);
    if (rest < wood * 0.85) return float(kCardShortGrass);
    if (rest < 0.22) return float(kCardShortGrass);
    if (rest < 0.32) return float(kCardSeedGrass);
    return baseVariant;
}
FoliageOut PageGrassVS(FoliageVertexIn vertex, PageGrassIn root)
{
    FoliageInstanceIn instance = (FoliageInstanceIn)0;
    // Which of the root's turf cards this corner belongs to (foliage_pass.cpp:
    // the quad holds world::kTurfCards cards, four apart in corner.x).
    const float turf = floor((vertex.corner.x + 2.0) * 0.25);
    vertex.corner.x -= 4.0 * turf;
    const float2 p = root.position.xy;
    const GrassRun run = decodeGrassRun(root.run);
    const float blockMorph = run.morph;
    const float cellMetres = run.cell;
    const int level = run.level;
    const bool coarse = cellMetres>2.5;
    const float nearby = (1.0-smoothstep(144.0,192.0,length(p-morphReplacement.zw)))*morphReplacement.y;
    const float range = coarse?1.0-nearby:nearby;
    if (range <= 0.0) return FoliageVS(vertex, instance);
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
    // The terrain category's ground cover (engine/biomes): bare, or its own
    // density, height, dryness, flowers and colour; a forest biome's floor.
    const BiomeCover biome = biomeCoverAt(p, cellMetres, foliageHash(cell.x ^ 0x6d2bu, cell.y ^ 0x1f3fu));
    instance.position = float3(p,z-0.025);
    instance.scale = (0.45+0.40*shape)*community.height*biome.height;
    if (coarse) instance.scale *= min(cellMetres,64.0)*0.32; // wider groups, height remains capped
    instance.phase = shape*6.2831853;
    instance.variant = foliageCommunityVariant(climate.foliage.x,climate.foliage.y,
        climate.foliage.z,climate.foliage.w,uint(shape*16777215.0));
    if (!coarse) {
        // A dry page's water head sits at the bottom of its height window; a
        // real surface stands clear of it.
        const float aboveWater = head > morphWindow.z + 0.5 ? z - head : 1000.0;
        instance.variant = pageFloraCard(p, climate.foliage, cover.canopy, climate.environment.z, w1.x,
            aboveWater, instance.variant, foliageHash(cell.x ^ 0x2c1bu, cell.y ^ 0x77a3u),
            biome.flowers, biome.dryness * 0.8);
    }
    // More occupied sites, not more candidates or larger overlapping cards.
    // Under a canopy the floor carries ferns and low leaves where grass would
    // be too thin to stand: a wood's floor is never bare earth from wall to wall.
    const float wood = coarse ? 0.0 : smoothstep(0.10, 0.60, cover.canopy);
    const float understory = wood * 0.45 * saturate((w0.x + w0.y) * 1.5) * (1.0 - saturate(w0.w * 1.5)) *
                             (1.0 - saturate(w1.y * 2.0)) * (1.0 - saturate(w0.z * 2.0));
    const float occupied = biome.none ? 0.0 : max(saturate(cover.grass * 1.5 * biome.density), understory * biome.under);
    // Parched ground takes the straw out of the green.
    const float3 grassTint = lerp(float3(cover.red, cover.green, cover.blue),
                                  float3(cover.red, cover.green, cover.blue) * float3(1.12, 0.96, 0.62), saturate(biome.dryness)) * biome.tint;
    float present = random < occupied ? 1.0 : 0.0;
    if (turf > 0.0) {
        // Turf: the rest of the cell filled in near the eye, so grass on the
        // ground reads as a sward and not as tufts two metres apart. Each card
        // its own place, size and beat, the same plant as the root's; how many
        // stand follows the same cover, so a thin meadow stays thin.
        const uint k = uint(turf);
        const float2 jitter = float2(foliageHash(cell.x ^ (k * 0x9e37u), cell.y ^ 0x3c5au),
                                     foliageHash(cell.x ^ 0x51edu, cell.y ^ (k * 0x7f4bu))) - 0.5;
        instance.position.xy += jitter * 1.6;   // world::kTurfReach either way
        instance.position.z -= 0.06;            // the root's height, on ground that may slope
        const float own = foliageHash(cell.x ^ (k * 0x2f1du), cell.y ^ (k * 0x61c9u));
        instance.scale *= 0.70 + 0.30 * own;
        instance.phase = frac(shape + own * 0.73) * 6.2831853;
        const float near = coarse ? 0.0 : 1.0 - smoothstep(36.0, 64.0, length(p - camera.xy));
        present = own < occupied * near ? 1.0 : 0.0;
    }
    instance.tint = float4(grassTint,
        (present > 0.0 && address.z!=0.0 && head<=z+0.02?0.92:0.0)*range);
    instance.climate = float4(climate.environment.x,climate.environment.z,climate.geography.w,z);
    return FoliageVS(vertex,instance);
}


// Pebbles: one small stone a fine root, out of the same candidates as the grass
// and through the same cull, drawn as a squashed octahedron the vertex stage
// deforms per instance (foliage_pass.cpp: corner.xy and uv.x carry the unit
// corner). Where the ground under the root is stony - the foot of an outcrop
// and the scree below it most, bare soil a little, a lawn hardly at all - and
// in patches, not as an even salting.
struct PebbleOut {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float4 colour : TEXCOORD1;   // rgb pigment, a: moss on the top faces
};

static const float kPebbleReach = 72.0;   // metres from the focus

PebbleOut PebbleVS(FoliageVertexIn vertex, PageGrassIn root)
{
    PebbleOut hidden = (PebbleOut)0;
    hidden.position = float4(0, 0, 0, 1);
    const float2 p = root.position.xy;
    const GrassRun run = decodeGrassRun(root.run);
    if (run.cell > 2.5) return hidden;
    const float reach = length(p - camera.xy);
    if (reach > kPebbleReach) return hidden;
    const float4 address = pageAddress(p, run.level);
    if (address.z == 0.0) return hidden;
    const float4 w0 = pageFields(address.xy, run.level, 1);    // grass, soil, sand, rock
    const float2 w1 = pageFields(address.xy, run.level, 2).xy; // marsh, snow
    const float head = morphWindow.z + pageFields(address.xy, run.level, 0).z * morphWindow.w;
    float z = lerp(root.position.z, root.heights.x, saturate(run.morph));
    if (morphing.y >= 3.0) {
        const float prior = lerp(root.heights.y, root.heights.z, saturate(run.morph));
        const float t = saturate(morphing.y - 3.0);
        z = lerp(prior, z, t * t * (3.0 - 2.0 * t));
    }
    if (head > z - 0.05 && head > morphWindow.z + 0.5) return hidden;   // under water

    const uint2 cell = uint2(int2(floor(p / 2.0)));
    // Talus: most at the rim of bare rock, where it breaks up, fewer on the
    // face itself; some on bare soil and sand; under snow and in a lawn, few.
    const float rock = w0.w;
    const float stony = saturate(smoothstep(0.05, 0.30, rock) * (1.0 - 0.55 * smoothstep(0.70, 1.0, rock)) +
                                 0.16 * w0.y + 0.10 * w0.z) *
                        (1.0 - 0.75 * saturate(w0.x * 1.4)) * (1.0 - saturate(w1.y * 1.5)) *
                        (1.0 - 0.6 * saturate(w1.x * 2.0));
    // Stones lie in spills and runs, not evenly: a patch field of its own.
    const float patch = noiseAt(p / 9.0 + float2(17.3, -4.1)) * 0.6 + noiseAt(p / 3.1 + float2(-2.2, 9.7)) * 0.4;
    const float chance = stony * (0.25 + 1.1 * smoothstep(0.35, 0.75, patch));
    const float roll = foliageHash(cell.x ^ 0x4b1du, cell.y ^ 0x9a27u);
    const float fade = 1.0 - smoothstep(kPebbleReach * 0.7, kPebbleReach, reach);
    if (roll >= chance * fade) return hidden;

    // Size: mostly gravel and fist-sized stones, the odd one a stride across,
    // larger where the rock is barer.
    const float sizeRoll = foliageHash(cell.x ^ 0x13c7u, cell.y ^ 0x2e95u);
    const float size = (0.05 + 0.32 * sizeRoll * sizeRoll * sizeRoll) * (1.0 + 1.4 * rock);
    const float yaw = foliageHash(cell.x ^ 0x7701u, cell.y) * 6.2831853;
    const float stretch = 1.0 + 0.8 * foliageHash(cell.x, cell.y ^ 0x5151u);
    const float flat = 0.38 + 0.30 * foliageHash(cell.x ^ 0x0f0fu, cell.y ^ 0x3333u);
    float3 local = float3(vertex.corner.x, vertex.corner.y, vertex.uv.x);
    // Each corner pulled in or out on its own, so no two stones are the same
    // solid; the same corner of the same stone every frame.
    const uint corner = uint(dot(local + 1.0, float3(1.0, 3.0, 9.0)) + 0.5);
    local *= 0.72 + 0.56 * foliageHash(cell.x ^ (corner * 0x2c9bu), cell.y ^ (corner * 0x61d3u));
    local.x *= stretch;
    local.z *= flat;
    const float c = cos(yaw), s = sin(yaw);
    local.xy = float2(local.x * c - local.y * s, local.x * s + local.y * c);
    const float2 jitter = float2(foliageHash(cell.x ^ 0x1d2bu, cell.y ^ 0x77e1u),
                                 foliageHash(cell.x ^ 0x66a5u, cell.y ^ 0x0b3du)) - 0.5;
    // Bedded: the lower third of the stone is in the ground.
    const float3 world = float3(p + jitter * 1.7, z - 0.035 - size * flat * 0.30) + local * size;

    PebbleOut output;
    output.position = project(world);
    output.worldPosition = world;
    // Greys between cool and warm, a little of the soil's brown on the smaller.
    const float hue = foliageHash(cell.x ^ 0x3e3eu, cell.y ^ 0x4c4cu);
    const float value = 0.36 + 0.24 * foliageHash(cell.x ^ 0x5ad1u, cell.y ^ 0x1c0fu);
    float3 pigment = value * lerp(float3(0.93, 0.97, 1.02), float3(1.08, 0.99, 0.86), hue);
    pigment = lerp(pigment, pigment * float3(1.05, 0.92, 0.78), saturate(w0.y * 1.5) * (1.0 - sizeRoll));
    // Sun-baked ground bleaches its stones (the category's dryness).
    const BiomeCover biome = biomeCoverAt(p, run.cell, roll);
    pigment = lerp(pigment, pigment * float3(1.12, 1.06, 0.94), saturate(biome.dryness));
    const SurfaceClimate climate = climateAt(p);
    const float forest = saturate(climate.foliage.y + climate.foliage.z + climate.foliage.w);
    output.colour = float4(pigment, saturate(climate.environment.z * 0.8) * forest * (1.0 - rock * 0.5));
    return output;
}

float4 PebblePS(PebbleOut input) : SV_Target0
{
    // Faceted: the face's own normal, from the screen derivatives - a stone
    // reads by its planes catching the sun, and a smooth octahedron is a blob.
    float3 normal = normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
    if (dot(normal, landscapeEye(input.worldPosition)) < 0.0) normal = -normal;
    float3 pigment = input.colour.rgb;
    // The pass's card array stays bound and read, so this stage keeps the
    // grass pipeline's resource layout (one binding set for both draws).
    pigment *= 0.97 + 0.03 * cardsTex.SampleLevel(cardsSampler, float3(0.5, 0.5, 0.0), 6.0).a;
    // Moss on the tops in damp woodland.
    pigment = lerp(pigment, float3(0.20, 0.27, 0.11), input.colour.a * smoothstep(0.55, 0.9, normal.z));
    const float shadow = proceduralShadow(input.worldPosition, normal);
    const float occlusion = 0.70 + 0.30 * saturate(normal.z * 0.5 + 0.5);
    const float3 lit = pigment * landscapeDaylight(normal, occlusion, shadow);
    return float4(landscapeFinish(lit, input.worldPosition), sceneDepthAlpha(input.worldPosition));
}
