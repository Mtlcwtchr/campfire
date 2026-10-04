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
    // Flower fields are the islands of the shared field (patchFieldAt), thick on
    // their best ground, and the flowers stand in them by the hundred.
    const float drift = smoothstep(0.50, 0.64, patchFieldAt(p));
    const float flowers = min(0.85, drift * open * (temperate * 0.7 + steppe * 0.5 + tropical * 0.4) *
                          (1.0 - reeds) * (0.60 + 0.40 * smoothstep(0.2, 0.6, moisture)) * flowerScale * 0.45);
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
        // A drift is one species (its colour from a field ~90 m across), with
        // a quarter of strays, so a meadow of flowers is fields of colour and
        // not confetti.
        const float field = foliageNoise(p.x / 90.0 + 41.0, p.y / 90.0 - 17.0);
        const float stray = frac(pick * 91.7);
        const float hue = stray < 0.25 ? frac(field + stray * 3.1) : field;
        const int kinds[10] = {kCardFlowersWhite, kCardFlowersYellow, kCardFlowersPurple, kCardFlowersPink,
                               kCardFlowersLilac, kCardFlowersGazania, kCardFlowersCelandine,
                               kCardFlowersUrsinia, kCardFlowersRed, kCardFlowersDandelion};
        return float(kinds[min(int(hue * 10.0), 9)]);
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
// What a root grows, worked out once for the cards and once for the blades
// (BladeVS below): where it stands, how big, its colour and which plant. A
// root with nothing on it has tint.a nought. `turf` is which of the root's
// turf cards is asked for, 0 for the root itself.
FoliageInstanceIn pageGrassRoot(PageGrassIn root, float turf)
{
    FoliageInstanceIn instance = (FoliageInstanceIn)0;
    const float2 p = root.position.xy;
    const GrassRun run = decodeGrassRun(root.run);
    const float blockMorph = run.morph;
    const float cellMetres = run.cell;
    const int level = run.level;
    const bool coarse = cellMetres>2.5;
    const float nearby = (1.0-smoothstep(144.0,192.0,length(p-morphReplacement.zw)))*morphReplacement.y;
    const float range = coarse?1.0-nearby:nearby;
    if (range <= 0.0) return instance;
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
    // Islands: the one field the ground, the shrubs and the bog pools share
    // (noise.hlsli patchFieldAt). Grass stands thick on its high ground and
    // thins to a worn path between; where it is broad country the islands are
    // fields, and the flowers gather on their best ground.
    const float patch = patchFieldAt(p);
    const float broad = patchRegionAt(p);
    // No background sprinkling, including the coarse representation. The
    // same island mask survives LOD changes and leaves real open ground.
    const float island = smoothstep(0.50, 0.62, patch);
    const float flowerFields = 1.0 + 2.4 * broad * smoothstep(0.50, 0.66, patch);
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
            biome.flowers * flowerFields, biome.dryness * 0.8);
    }
    // More occupied sites, not more candidates or larger overlapping cards.
    // Under a canopy the floor carries ferns and low leaves where grass would
    // be too thin to stand: a wood's floor is never bare earth from wall to wall.
    const float wood = coarse ? 0.0 : smoothstep(0.10, 0.60, cover.canopy);
    const float understory = wood * 0.45 * saturate((w0.x + w0.y) * 1.5) * (1.0 - saturate(w0.w * 1.5)) *
                             (1.0 - saturate(w1.y * 2.0)) * (1.0 - saturate(w0.z * 2.0));
    const float turfSlope = 1.0 - smoothstep(0.14, 0.36, 1.0 - root.heights.w);
    // The environment's ground tier (content/config/environment/cover.json):
    // a wet hollow thickens the cover, a talus fan or a clearing's path thins it.
    const float occupied = biome.none ? 0.0 : saturate(island * turfSlope *
        saturate(max(cover.grass * 2.8 * biome.density, understory * biome.under * 1.4)) *
        pageCoverAt(address, level));
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
        const float near = coarse ? 0.0 : 1.0 - smoothstep(112.0, 160.0, length(p - camera.xy));
        present = own < occupied * near ? 1.0 : 0.0;
    }
    instance.tint = float4(grassTint,
        (present > 0.0 && address.z!=0.0 && head<=z+0.02?0.92:0.0)*range);
    instance.climate = float4(climate.environment.x,climate.environment.z,climate.geography.w,z);
    return instance;
}

// Close to the eye the grass is blades (BladeVS), not cards: a grass card hands
// over to them between these distances, and flowers, ferns and reeds stay cards.
static const float kBladeNear = 38.0;
static const float kBladeFar = 62.0;
// How far a point is from the eye, for the blades' hand-over: the depth of
// the view in perspective (camera.xy is the focus, metres ahead of the eye),
// and out of reach in an orthographic map view, where no blade is worth drawing.
float bladeReach(float3 p)
{
    const bool perspective = dot(abs(viewProjection[3].xyz), float3(1.0, 1.0, 1.0)) > 0.0;
    return perspective ? mul(viewProjection, float4(p, 1.0)).w : 1.0e6;
}
bool bladeGrass(float variant)
{
    const int v = int(variant + 0.5);
    return v < 6 || v == kCardShortGrass || v == kCardDryGrass || v == kCardSeedGrass;
}

FoliageOut PageGrassVS(FoliageVertexIn vertex, PageGrassIn root)
{
    // Which of the root's turf cards this corner belongs to (foliage_pass.cpp:
    // the quad holds world::kTurfCards cards, four apart in corner.x).
    const float turf = floor((vertex.corner.x + 2.0) * 0.25);
    vertex.corner.x -= 4.0 * turf;
    FoliageInstanceIn instance = pageGrassRoot(root, turf);
    if (decodeGrassRun(root.run).cell < 2.5 && bladeGrass(instance.variant))
        instance.tint.a *= smoothstep(kBladeNear, kBladeFar, bladeReach(root.position));
    return FoliageVS(vertex, instance);
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


// Blades: the grass near the eye as geometry, not pictures.
//
// A root within kBladeFar is a clump of real blades - each a curved strip,
// tapering to a point, with a normal of its own and its own lean in the wind -
// drawn from the same candidates and the same cull as the cards (one more
// draw, like the pebbles). The clump mesh (foliage_pass.cpp) is kBlades blades
// of kBladeSegments segments: corner.x is the blade, corner.y how far up it,
// uv.x which edge (-1, +1; nought at the tip). Everything else - where each
// blade stands in the clump, how tall, how curved, which way it faces - is a
// hash of the root and the blade, so nothing moves as the camera does.
//
// Fewer blades further out, each wider so the clump keeps its cover, and by
// kBladeFar none: the cards have taken over (PageGrassVS fades them in).
static const int kBlades = 32;

struct BladeOut {
    float4 position : SV_Position;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 colour : TEXCOORD2;    // rgb pigment, a: how far up the blade
    float4 weather : TEXCOORD3;
    float2 lean : TEXCOORD4;      // bend, gust
};

BladeOut BladeVS(FoliageVertexIn vertex, PageGrassIn root)
{
    BladeOut hidden = (BladeOut)0;
    hidden.position = float4(0, 0, 0, 1);
    const float2 base = root.position.xy;
    const GrassRun run = decodeGrassRun(root.run);
    if (run.cell > 2.5) return hidden;
    const float reach = bladeReach(root.position);
    if (reach > kBladeFar) return hidden;
    // How many of the clump's blades this distance can carry.
    const float blade = vertex.corner.x;
    const float shown = lerp(float(kBlades), 7.0, smoothstep(8.0, kBladeFar, reach));
    if (blade >= shown) return hidden;
    const FoliageInstanceIn instance = pageGrassRoot(root, 0.0);
    if (instance.tint.a <= 0.0 || !bladeGrass(instance.variant)) return hidden;

    const uint2 cell = uint2(int2(floor(base / 2.0)));
    const uint b = uint(blade + 0.5);
    const float r0 = foliageHash(cell.x ^ (b * 0x9e37u), cell.y ^ 0x2b1du);
    const float r1 = foliageHash(cell.x ^ 0x5d31u, cell.y ^ (b * 0x7f4bu));
    const float r2 = foliageHash(cell.x ^ (b * 0x1f2bu), cell.y ^ (b * 0x3a5du));
    const float r3 = foliageHash(cell.x ^ (b * 0x61c9u), cell.y ^ 0x77e1u);
    // Where in the clump: four tufts over the root's cell, each a handful of
    // blades from one crown - grass grows in tufts, and a tuft reads at a
    // glance where blades scattered evenly read as stubble.
    const uint tuft = b & 3u;
    const float ta = foliageHash(cell.x ^ (tuft * 0x2c9bu), cell.y ^ 0x5151u) * 6.2831853;
    const float tr = 0.95 * sqrt(foliageHash(cell.x ^ 0x0f0fu, cell.y ^ (tuft * 0x61d3u)));
    const float angle = r0 * 6.2831853;
    const float2 foot = base + float2(cos(ta), sin(ta)) * tr + float2(cos(angle), sin(angle)) * (0.16 * sqrt(r1));
    // Which way the blade faces and leans at rest, how tall, how wide.
    // Facing out of the tuft's crown, more or less: a tuft splays.
    const float yaw = angle + (r2 - 0.5) * 1.6;
    const float2 facing = float2(cos(yaw), sin(yaw));
    const float2 side = float2(-facing.y, facing.x);
    const bool dry = int(instance.variant + 0.5) == kCardDryGrass;
    const float height = min(instance.scale, 1.3) * (0.55 + 0.55 * r3) * (dry ? 0.75 : 1.0) * 1.15;
    const float widen = sqrt(float(kBlades) / shown);
    const float width = (0.028 + 0.022 * r1) * widen * (0.8 + 0.4 * min(instance.scale, 1.3));
    const float curl = 0.15 + 0.45 * frac(r0 * 5.13);   // how far the tip falls forward at rest

    // The wind, as the cards have it, per blade: its own stiffness and beat.
    const float t = viewport.z;
    const float2 direction = wind.xy;
    const float own = frac(instance.phase + r2 * 0.61);
    const float stiffness = 0.70 + 0.80 * frac(own * 7.31);
    const float beat = frac(own * 3.77) * 6.2831853;
    const float sway = sin(own * 6.28 + t * 1.15) * 0.10 + sin(beat + t * 0.61) * 0.06;
    const float gust = windGust(foot, direction, t, wind.w, 1.0);
    const float front = windFront(foot, direction, t, 1.0);
    const float bend = (windLean(gust, front) * wind.z + sway) / stiffness;
    const float shiver = sin(t * 8.7 + beat * 2.3) * 0.06 * saturate(gust - 0.55);

    // The blade's spine: up, curling forward, pushed downwind, the length kept
    // (a leaning blade is a shorter blade, never a stretched one).
    const float up = vertex.corner.y;
    const float lean = saturate(curl * up * up + (bend + shiver * up) * 0.42 * up * up);
    const float2 bow = facing * (curl * up * up) + direction * ((bend + shiver * up) * 0.42 * up * up);
    float3 spine = float3(foot + bow * height, instance.position.z + up * height * sqrt(saturate(1.0 - lean * lean)));
    // Tapering to a point; the edge this vertex is on.
    const float edge = vertex.uv.x;
    const float halfWidth = width * 0.5 * pow(saturate(1.0 - up), 0.65);
    spine.xy += side * edge * halfWidth;

    // The normal: the blade's face, turned by the bow, and rounded across it so
    // a blade has a lit edge and a shaded one instead of one flat value.
    const float3 tangent = normalize(float3(bow * height * 2.0 / max(up, 0.05), height));
    float3 face = normalize(cross(tangent, float3(side, 0.0)));
    if (face.z < 0.0) face = -face;
    // And bent most of the way up: a sward is lit as a surface, not as a
    // thousand vertical strips each turned edge-on to a low sun.
    const float3 normal = normalize(lerp(normalize(face + float3(side, 0.0) * edge * 0.55), float3(0, 0, 1), 0.6));

    BladeOut output;
    output.position = project(spine);
    output.worldPosition = spine;
    output.normal = normal;
    // Darker at the foot, lighter and warmer at the tip; each blade a little
    // its own, dry blades straw-coloured towards their tips.
    float3 pigment = instance.tint.rgb * lerp(float3(0.92, 0.97, 0.88), float3(1.06, 1.02, 0.94), frac(r3 * 3.7));
    pigment *= lerp(0.75, 1.15, up);
    if (dry || int(instance.variant + 0.5) == kCardSeedGrass)
        pigment = lerp(pigment, pigment * float3(1.25, 1.08, 0.65), up * (dry ? 0.8 : 0.4));
    output.colour = float4(pigment, up);
    const WeatherVertex climate = weatherVertex(instance.position, instance.climate.x, instance.climate.y, instance.climate.z);
    output.weather = climate.surface;
    output.weather.w = climate.air.x;
    output.lean = float2(bend, gust);
    return output;
}

float4 BladePS(BladeOut input, bool front : SV_IsFrontFace) : SV_Target0
{
    // Both faces of a blade are its face: turn the normal to the eye.
    float3 normal = normalize(input.normal);
    const float3 eye = landscapeEye(input.worldPosition);
    if (dot(normal, eye) < 0.0) normal = -normal;
    const float up = input.colour.a;
    float3 pigment = landscapePigment(input.colour.rgb, 1.0);
    pigment = styleSurfaceColour(pigment, input.worldPosition, kStyleVegetation, 1.0, 0.0);
    pigment = weatherVegetation(pigment, parametersPS[0].z, input.weather.y, input.weather.w);
    pigment = lerp(pigment, float3(0.76, 0.80, 0.82), wxSmooth(0.01, 0.25, input.weather.x) * wxSmooth(0.45, 1.0, up) * 0.8);
    // The pass's card array stays bound and read, so this stage keeps the
    // grass pipeline's resource layout (one binding set for every draw).
    pigment *= 0.97 + 0.03 * cardsTex.SampleLevel(cardsSampler, float3(0.5, 0.5, 0.0), 6.0).a;
    const float shadow = proceduralShadow(input.worldPosition, normal);
    // The sward shades its own feet.
    const float occlusion = lookRootOcclusion(up);
    float3 lit = pigment * landscapeDaylight(normal, occlusion, shadow);
    // Light through the leaf, strongest at the thin tips against the sun.
    const float backlight = pow(saturate(dot(-landscapeSun(), eye) * 0.5 + 0.5), 3.0);
    lit += pigment * float3(1.06, 1.0, 0.70) * (backlight * up * 0.22) * shadow;
    // A gust front passing reads as a band of light over the field.
    lit *= 1.0 + (input.lean.x - 0.23) * 0.10 * up;
    return float4(landscapeFinish(lit, input.worldPosition), sceneDepthAlpha(input.worldPosition));
}
