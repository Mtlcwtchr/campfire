// The ground: six materials mixed by weight, lit by the vertex normal.
#include "ground.hlsli"
#include "noise.hlsli"
#include "ring_reveal.hlsli"
#include "foliage_field.hlsli"
#include "wind_field.hlsli"
#include "relief.hlsli"
#include "sand_motion.hlsli"
#include "landscape_look.hlsli"
#include "inspection.hlsli"
#include "weather.hlsli"
#include "climate_field.hlsli"
// Terrain categories (engine/biomes): the page variant binds the category
// plane and the biome table after its shadow (t13, t14). The mesh variant
// binds neither and draws every id as the engine's own ground.
#ifdef TERRAIN_PAGE_MATERIALS
#define BIOME_PLANE_SLOT t13
#define BIOME_PLANE_SAMPLER s13
#define BIOME_TABLE_SLOT t14
#define BIOME_TABLE_SAMPLER s14
#endif
#include "terrain_biomes.hlsli"

// The six materials as layers of one array, so a pixel can pick the two it is
// made of instead of sampling all six and throwing four away.
Texture2DArray groundTex : register(t0, space2);
SamplerState groundSampler : register(s0, space2);
// Page detail textures occupy t1..t9 in the page variant, so the material
// channels live after them. The mesh variant binds three contiguous samplers.
#ifdef TERRAIN_PAGE_MATERIALS
Texture2DArray groundNormalTex : register(t10, space2);
SamplerState groundNormalSampler : register(s10, space2);
Texture2DArray groundPropertiesTex : register(t11, space2);
SamplerState groundPropertiesSampler : register(s11, space2);
#else
Texture2DArray groundNormalTex : register(t1, space2);
SamplerState groundNormalSampler : register(s1, space2);
Texture2DArray groundPropertiesTex : register(t2, space2);
SamplerState groundPropertiesSampler : register(s2, space2);
#endif
#include "terrain_material.hlsli"
#ifdef TERRAIN_PAGE_MATERIALS
#define SHADOW_TEXTURE_SLOT t12
#define SHADOW_SAMPLER_SLOT s12
#else
#define SHADOW_TEXTURE_SLOT t3
#define SHADOW_SAMPLER_SLOT s3
#endif
#include "shadow_field.hlsli"

struct TerrainIn {
    float3 position : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 weights0 : TEXCOORD2;
    float2 weights1 : TEXCOORD3;
    float2 uv : TEXCOORD4;
    float waterHeight : TEXCOORD5;
    float4 waterMotion : TEXCOORD6;
    float waterCover : TEXCOORD7;
    float morphHeight : TEXCOORD8;
    float3 morphNormal : TEXCOORD9;
    float2 morphUv : TEXCOORD10;
    // What is left of the eleven climate channels this vertex used to carry:
    // the two numbers that are not climate at all. Exposure is this point
    // against its upwind skyline and openness is it against the ground around
    // it - both properties of the height at this level, so both belong to the
    // corner. The weather itself is one field over the world and is read from
    // climate_field.hlsli where the vertex stands.
    float2 relief : TEXCOORD11;   // wind exposure, openness
};
struct TerrainOut {
    float4 position : SV_Position;
    float3 normal : TEXCOORD0;
    float4 weights0 : TEXCOORD1;
    float2 weights1 : TEXCOORD2;
    float2 uv : TEXCOORD3;
    float2 worldXY : TEXCOORD4;
    float waterDepth : TEXCOORD5;
    float worldHeight : TEXCOORD6;
    float4 foliage : TEXCOORD7;
    float desertCover : TEXCOORD8;
    float windExposure : TEXCOORD9;
    float4 environment : TEXCOORD10;
    float4 geography : TEXCOORD11;
    float travelCost : TEXCOORD12;
    float4 weather : TEXCOORD13; // snow metres, wetness, ice, enabled
    float3 triangleBary : TEXCOORD14;
    float4 stageDiagnostic : TEXCOORD15;
    float forest : TEXCOORD16;   // canopy share, picks forest-floor ground
};

TerrainOut TerrainVS(TerrainIn input)
{
    TerrainOut output;
    output.triangleBary = 0.0;
    output.stageDiagnostic = 0.0;
    const float3 where = morphed(input.position, input.morphHeight);
    output.position = project(where);
    output.normal = morphedNormal(input.normal, input.morphNormal, input.position.xy);
    output.weights0 = input.weights0;
    output.weights1 = input.weights1;
    output.uv = morphedUv(input.uv, input.morphUv);
    output.worldXY = where.xy;
    output.waterDepth = waterMorphed(input.position.xy, input.waterHeight, input.morphUv) - where.z;
    output.worldHeight = where.z;
    const SurfaceClimate place = climateAt(input.position.xy);
    output.foliage = place.foliage;
    output.desertCover = place.desert;
    output.windExposure = input.relief.x;
    output.travelCost = 1.0;
    output.environment = place.environment;
    output.forest = place.forest;
    output.geography = float4(place.geography.x, place.geography.y, input.relief.y, place.geography.w);
    const WeatherVertex climate=weatherVertex(input.position,place.environment.x,place.environment.z,place.geography.w);
    output.weather=climate.surface;
    if (parameters[0].x>0.5) {
        const float baseline=18.0+(place.environment.x-0.65)*60.0-input.position.z*0.004;
        output.environment.x=(lerp(climate.air.x,baseline,table[6].z)+30.0)/80.0;
        output.environment.z=lerp(saturate(place.environment.z*0.55+climate.surface.y*0.65),place.environment.z,table[6].z);
        // Foundation reuses geography.x for immutable mud potential, not wind.
        if ((int)extra.w!=5)
            output.geography.xy*=lerp(climate.air.w/max(wind.z,0.001),1.0,table[6].z);
    }
    return output;
}

// How a material meets its neighbour comes from the scene now, not from here.
//
//   table[i].x   turns of this material's texture in one turn of the reference
//   table[i].y   how wide its border with a neighbour is, in weight
//   table[i].z   how far noise tears that border about
//   table[i].w   how big the tears are, in metres
//
// It was six triples of constants in this file, which meant that trying a
// different number meant a rebuild - and picking these by argument rather than
// by looking is how ground ends up looking argued about. They are content now
// (content/config/ground.json) and the explorer can move them while it runs.

// Smoothly cross-fade the two strongest materials near their shared border.
// Noise lets each material intrude a little into its neighbour at the border.
// Keep biome interiors crisp and reuse the same mix at both texture scales.
struct Ground {
    int top, under;
    float mix;          // nought for the second material, one for the first
    float3 colour;      // blended near read
    float4 properties;
    float3 normal;
};

// One scatter of stones: a grid of cells, each holding at most one stone,
// jittered inside its cell so the grid leaves no mark.
//
// Requirement 19 wants stones placed for geological reasons and explicitly not
// by uniform random scatter, so the density is never a constant here - every
// caller passes it in from a field that says why a stone would be there: the
// foot of a cliff, a river bed, an exposed nose, rocky ground. An empty cell
// costs two noise reads and returns.
//
// Returns the coverage mask and the height of the dome, so a stone can shade
// as a stone rather than as a stain: x lights the albedo, y bends the normal.
float2 stoneScatter(float2 worldXY, float metres, float density, float salt, float pixel)
{
    const float2 cell = worldXY / metres;
    const float2 id = floor(cell);
    // hashAt, not noiseAt. Smooth noise is correlated between neighbouring
    // cells by construction - that is what makes it smooth - so picking cells
    // with it seats the stones in drifts and rows, and the first attempt drew
    // exactly that: gravel in diagonal stripes. A hash gives each cell an
    // independent number, which is what a scatter needs.
    const float pick = hashAt(id + salt);
    const float present = (1.0 - smoothstep(density - 0.04, density + 0.04, pick)) *
                          saturate(density / 0.04);
    const float shape = hashAt(id + salt + 13.7);
    // Off the middle of its cell, or the grid is the pattern.
    const float2 centre = float2(0.26 + shape * 0.48,
                                 0.26 + frac(pick * 7.31) * 0.48);
    // A wide spread of sizes, weighted small. All one size is what made the
    // first version read as bubbles rather than stones - a bed of identical
    // circles is a texture of circles, whatever it is coloured like.
    const float grade = frac(shape * 5.17);
    const float radius = 0.05 + grade * grade * 0.26;
    const float2 off = frac(cell) - centre;
    // And not round. A stone seen from above is a blob: squashed along some
    // axis of its own and dented on one side. Two cheap distortions - an
    // anisotropic stretch and a lobe - cost four multiplies and remove the
    // thing the eye actually picks up, which is that every outline is a circle.
    const float turn = frac(pick * 3.77) * 6.2832;
    const float2 axis = float2(cos(turn), sin(turn));
    const float2 local = float2(dot(off, axis), dot(off, float2(-axis.y, axis.x)));
    const float squash = 0.72 + frac(shape * 2.13) * 0.55;
    const float2 shaped = float2(local.x / squash, local.y * squash);
    const float lobe = 1.0 + 0.22 * cos(turn * 2.0 + atan2(shaped.y, shaped.x) * 3.0);
    const float away = length(shaped) / (radius * lobe);
    const float footprint = pixel / max(metres * radius, 1e-4);
    const float resolved = 1.0 - smoothstep(0.35, 1.25, footprint);
    const float aa = min(footprint, 0.5);
    const float mask = (1.0 - smoothstep(0.62 - aa, 1.0 + aa, away)) * present * resolved;
    // A stone sits in the ground, not on it: the dome is shallow.
    return float2(mask, saturate(1.0 - away * away) * mask * metres * radius * 0.16);
}

// Original pair-specific width, noise scale and strength. The organic shape
// comes from broad/fine value noise, not from stretching it down the slope.
// Grass 0, Dirt 1, Sand 2, Rock 3, Marsh 4, Snow 5.
float3 groundEdgeStyle(int a, int b)
{
    const int low = min(a, b), high = max(a, b);
    if (high == 5) return float3(1.55, 2.2, 0.55);
    if (high == 4) return float3(0.55, 0.42, 1.35);
    if (high == 3) return low == 2 ? float3(0.50, 0.30, 1.45) : float3(0.62, 0.38, 1.30);
    if (high == 2) return float3(0.85, 0.80, 1.05);
    return float3(1.60, 1.30, 0.70);
}

// Each octave filters to its own mean, independently. Broad contours survive
// when the fine octave becomes sub-pixel; neither noise domain moves with zoom.
float groundEdgeNoise(float2 worldXY, float metres, float footprint)
{
    return filteredNoiseAt(worldXY / max(metres, 0.001), footprint / max(metres, 0.001));
}

// Analytic weight gradients (weight per metre), set by the page path before
// TerrainPS runs. Screen-space derivatives of the weights are useless close
// up: the field and the interpolated position are both quantized there, so
// ddx() is zero across a terrace and a spike at its edge.
#ifndef TERRAIN_WEIGHT_GRADIENTS
#define TERRAIN_WEIGHT_GRADIENTS
static bool gWeightGradients = false;
static float4 gWeightDX0 = 0, gWeightDY0 = 0;
static float2 gWeightDX1 = 0, gWeightDY1 = 0;
#endif

// What picks a class's variant: climate, water and slope at this pixel.
// `valid` false keeps every class on its base layer (probes, old callers).
struct GroundContext {
    bool valid;
    float moisture, desert, forest, aboveWater, steepness;
    // The terrain category here (engine/biomes): two ids and the share of
    // the second, and the forest layer's biome. All nought: the engine's own.
    int categoryA, categoryB;
    float categoryShare;
    int forestBiome;
};
GroundContext noGroundContext()
{
    GroundContext c = (GroundContext)0;
    c.valid = false;
    c.aboveWater = 100.0;
    return c;
}

// Up to two variants per class, as strengths; the mix is
// base (1-s1)(1-s2), first s1, second s2(1-s1): continuous in both, and no
// ordering swap can make a layer jump in or out. `base` is the class's own
// layer except where the class's default look is not its texture (sand).
struct GroundVariants { int base, first, second; float s1, s2; };

// Value noise evaluated in a rotated frame: its lattice no longer runs along
// world X/Y, so a thresholded patch edge does not pick up axis-aligned steps.
float organicNoise(float2 at)
{
    return noiseAt(float2(at.x * 0.8 - at.y * 0.6, at.x * 0.6 + at.y * 0.8));
}
// The filtered, rotated noise and its gradient in the caller's frame.
float3 organicNoiseGradAt(float2 at, float footprint)
{
    const float3 n = filteredNoiseGradAt(float2(at.x * 0.8 - at.y * 0.6, at.x * 0.6 + at.y * 0.8), footprint);
    return float3(n.x, n.y * 0.8 + n.z * 0.6, -n.y * 0.6 + n.z * 0.8);
}

// The strongest of three candidate strengths, faded to nothing where the
// top two tie so the chosen layer never switches under a visible weight.
void strongestOf3(int la, float a, int lb, float b, int lc, float c, out int layer, out float strength)
{
    const float top = max(a, max(b, c));
    const float mid = max(min(a, b), min(max(a, b), c));
    layer = top == a ? la : (top == b ? lb : lc);
    strength = top * saturate((top - mid) * 6.0);
}

GroundVariants groundVariants(int cls, GroundContext c, float2 xy)
{
    GroundVariants v;
    v.base = cls; v.first = cls; v.second = cls; v.s1 = 0.0; v.s2 = 0.0;
    if (!c.valid) return v;
    // Broad, stationary break-up so a variant arrives in patches, not along
    // a climate isoline.
    const float patch = organicNoise(xy / 53.0 + float(cls) * 7.3) * 0.7 +
                        organicNoise(xy / 19.0 + float(cls) * 3.1) * 0.3 - 0.5;
    // Moisture here is the map's rank of it (half of every map is below
    // 0.5), so dry from the drier third, not from the median: at 0.5 half of
    // a temperate country was painted as parched grass.
    const float dry = saturate(max((0.35 - c.moisture) * 2.6, c.desert * 1.3) + patch * 0.7);
    const float steep = smoothstep(0.22, 0.50, c.steepness);
    if (cls == 0) {
        v.first = LAYER_FOREST_FLOOR;
        v.s1 = smoothstep(0.35, 0.75, c.forest + patch * 0.5) * 0.85;
        v.second = LAYER_GRASS_DRY;
        v.s2 = smoothstep(0.30, 0.70, dry);
    } else if (cls == 1) {
        v.first = LAYER_FOREST_FLOOR;
        v.s1 = smoothstep(0.30, 0.70, c.forest + patch * 0.5);
        // Stony sand only in genuinely arid ground, and not everywhere there.
        v.second = LAYER_SAND_GRAVELLY;
        v.s2 = smoothstep(0.65, 0.95, dry) * 0.5;
    } else if (cls == 2) {
        // Plain dry sand by default. The orange scan is dunes and only dunes:
        // open desert, off the slopes. Wet at and under the waterline, pale
        // beach sand on a sea coast, stony sand on slopes and broken ground.
        v.base = LAYER_SAND_PLAIN;
        v.first = LAYER_SAND_WET;
        v.s1 = 1.0 - smoothstep(-0.2, 0.5 + patch * 0.4, c.aboveWater);
        const float dune = smoothstep(0.35, 0.65, c.desert + patch * 0.25) * (1.0 - steep);
        const float coast = (1.0 - smoothstep(1.0, 6.0 + patch * 3.0, c.aboveWater)) * 0.65 * (1.0 - c.desert);
        const float stony = smoothstep(0.14, 0.40, c.steepness + patch * 0.2);
        strongestOf3(LAYER_SAND_DUNE_ORANGE, dune, LAYER_SAND_COAST, coast,
                     LAYER_SAND_GRAVELLY, stony, v.second, v.s2);
    } else if (cls == 3) {
        // Faces take a cliff surface; which one follows the climate.
        v.first = LAYER_CLIFF_MOSSY;
        v.s1 = steep * smoothstep(0.50, 0.80, c.moisture + patch * 0.4) * (1.0 - c.desert);
        const float arid = c.desert + patch * 0.3;
        v.second = arid > 0.4 ? LAYER_CLIFF_DESERT : LAYER_CLIFF_DOLOMITE;
        v.s2 = steep * saturate(abs(arid - 0.4) * 8.0);
    } else if (cls == 4) {
        v.second = LAYER_MUD_CRACKED;
        v.s2 = smoothstep(0.40, 0.80, dry);
    }
    return v;
}

#include "terrain_biomes_code.hlsli"

MaterialSample sampleGroundVariants(GroundVariants v, int cls, float3 p, float3 normal,
                                    float3 dx, float3 dy)
{
    const float w1 = v.s1, w2 = v.s2 * (1.0 - v.s1);
    const float wb = 1.0 - w1 - w2;
    MaterialSample r = (MaterialSample)0;
    float total = 0.0;
    [branch] if (wb > 0.004) {
        const MaterialSample s = sampleGroundLayer(v.base, cls, p, normal, dx, dy);
        r.colour += s.colour * wb; r.properties += s.properties * wb;
        r.normal += s.normal * wb; r.borderHeight += s.borderHeight * wb; total += wb;
    }
    [branch] if (w1 > 0.004) {
        const MaterialSample s = sampleGroundLayer(v.first, cls, p, normal, dx, dy);
        r.colour += s.colour * w1; r.properties += s.properties * w1;
        r.normal += s.normal * w1; r.borderHeight += s.borderHeight * w1; total += w1;
    }
    [branch] if (w2 > 0.004) {
        const MaterialSample s = sampleGroundLayer(v.second, cls, p, normal, dx, dy);
        r.colour += s.colour * w2; r.properties += s.properties * w2;
        r.normal += s.normal * w2; r.borderHeight += s.borderHeight * w2; total += w2;
    }
    const float k = 1.0 / max(total, 1e-5);
    r.colour *= k; r.properties *= k; r.borderHeight *= k;
    r.normal = normalize(r.normal);
    return r;
}

// The engine's variants of a class, with the forest layer's floor in place
// of the engine's forest floor where its biome names one.
GroundVariants engineVariants(int cls, GroundContext c, float2 xy)
{
    GroundVariants v = groundVariants(cls, c, xy);
#ifdef BIOME_ANY_FLOOR
    const int floorLayer = biomeForestFloor(c.forestBiome);
    if (floorLayer >= 0 && v.first == LAYER_FOREST_FLOOR) v.first = floorLayer;
#endif
    return v;
}

// A class as one category draws it: its soils and slopes over the engine's
// variants, then its tints and strata.
MaterialSample sampleCategoryClass(int category, int cls, GroundContext c, float3 p, float3 normal,
                                   float3 dx, float3 dy)
{
    GroundVariants v = engineVariants(cls, c, p.xy);
    const float footprint = max(length(dx.xy), length(dy.xy));
    const bool changed = biomeClassVariants(category, cls, c, p.xy, footprint, v);
#ifdef BIOME_ANY_FLOOR
    // The forest's floor under its canopy, over a category's own soil too.
    if (changed && cls <= 1) {
        const int floorLayer = biomeForestFloor(c.forestBiome);
        if (floorLayer >= 0) {
            const float patch = organicNoise(p.xy / 53.0 + float(cls) * 7.3) * 0.7 +
                                organicNoise(p.xy / 19.0 + float(cls) * 3.1) * 0.3 - 0.5;
            const float canopy = smoothstep(0.35, 0.75, c.forest + patch * 0.5) * (cls == 0 ? 0.85 : 1.0);
            if (canopy > v.s2) { v.second = floorLayer; v.s2 = canopy; }
        }
    }
#endif
    MaterialSample s = sampleGroundVariants(v, cls, p, normal, dx, dy);
    if (changed) biomeClassFinish(category, cls, c, p, s.colour);
    return s;
}

MaterialSample sampleGroundClass(int cls, GroundContext c, float3 p, float3 normal,
                                 float3 dx, float3 dy)
{
#ifdef BIOME_ANY_GROUND
    [branch] if (biomeCategoryChangesGround(c.categoryA) ||
                 (c.categoryShare > 0.004 && biomeCategoryChangesGround(c.categoryB))) {
        // A category that changes nothing draws through the same code as the
        // default, so the two sides of a border meet in one blend. One copy of
        // the sampling, looped over the pair.
        MaterialSample r = (MaterialSample)0;
        [loop] for (int k = 0; k < 2; ++k) {
            const float w = k == 0 ? 1.0 - c.categoryShare : c.categoryShare;
            if (w <= 0.004) continue;
            const MaterialSample s = sampleCategoryClass(k == 0 ? c.categoryA : c.categoryB, cls, c, p, normal, dx, dy);
            r.colour += s.colour * w; r.properties += s.properties * w;
            r.borderHeight += s.borderHeight * w; r.normal += s.normal * w;
        }
        r.normal = normalize(r.normal);
        return r;
    }
#endif
    return sampleGroundVariants(engineVariants(cls, c, p.xy), cls, p, normal, dx, dy);
}

Ground groundHereIn(float4 weights0, float2 weights1, float3 worldPos, float3 normal,
                    GroundContext context)
{
    const float total = dot(max(weights0, 0.0), float4(1, 1, 1, 1)) +
                        dot(max(weights1, 0.0), float2(1, 1));
    weights0 = max(weights0, 0.0) / max(total, 1e-5);
    weights1 = max(weights1, 0.0) / max(total, 1e-5);
    if (total < 1e-5) weights0.x = 1.0;
    const float2 worldXY = worldPos.xy;
    // Filter the mask in its own coordinates, not the warped texture domain.
    const float footprint = max(length(ddx(worldXY)), length(ddy(worldXY)));
    const float weightFootprint = gWeightGradients
        ? footprint * (dot(abs(gWeightDX0) + abs(gWeightDY0), float4(1,1,1,1)) +
                       dot(abs(gWeightDX1) + abs(gWeightDY1), float2(1,1))) / max(total, 1e-5)
        : dot(fwidth(weights0), float4(1,1,1,1)) + dot(fwidth(weights1), float2(1,1));
    // Warp measured in metres, not texture turns. Never scale it with zoom.
    const float3 p = worldPos + float3(domainWarp(worldXY / 47.0) * 0.35, 0.0);
    const float3 dx = ddx(p), dy = ddy(p);
    float w[6] = {weights0.x, weights0.y, weights0.z, weights0.w, weights1.x, weights1.y};
    Ground here;
    here.top = 0;
    here.under = 1;
    float best = -1, next = -1, third = -1;
    int third_i = 2;
    for (int i = 0; i < 6; ++i) {
        if (w[i] > best) {
            third = next; third_i = here.under;
            next = best; here.under = here.top; best = w[i]; here.top = i;
        } else if (w[i] > next) {
            third = next; third_i = here.under;
            next = w[i]; here.under = i;
        } else if (w[i] > third) { third = w[i]; third_i = i; }
    }

    float4 profileA = tablePS[here.top], profileB = tablePS[here.under];
    // A small minimum keeps narrow rock/sand borders visibly soft as well.
    const float3 style = groundEdgeStyle(here.top, here.under);
    float width = min(0.90, max(0.20, 0.5 * (profileA.y + profileB.y)) * style.x);
    const MaterialSample top = sampleGroundClass(here.top, context, p, normal, dx, dy);

    const float gap = best - next;
    const float border = (1.0 - smoothstep(width, width * 2.0, gap)) *
                         smoothstep(0.0, 0.10, next);
    const float metres = max(0.25, min(profileA.w, profileB.w) * style.y);
    const float3 fineN = organicNoiseGradAt(worldXY / metres, footprint / metres);
    const float3 broadN = organicNoiseGradAt(worldXY / (metres * 4.0), footprint / (metres * 4.0));
    const float fine = fineN.x * 2.0 - 1.0;
    const float broad = broadN.x * 2.0 - 1.0;
    const float mask = lerp(broad, fine, 0.35);
    // How fast the tear mask changes on the ground, per metre.
    const float maskSlope = length(lerp(broadN.yz * (2.0 / (metres * 4.0)),
                                        fineN.yz * (2.0 / metres), 0.35));
    const float amount = min(max(profileA.z, profileB.z) * style.z,
                             width * 0.95 * max(1.0, style.z));
    // A fixed material order makes the noise change sign when top/under swap.
    // Otherwise the same boundary would jump to a different colour at equal weights.
    const float orientation = here.top < here.under ? 1.0 : -1.0;
    const float intrusion = mask * amount * border * orientation;

    // Lerp the actual materials across an antialiased band, not a thresholded
    // noise mask. Canonical sign keeps the filter continuous when top swaps.
    const float blendWidth = max(width, weightFootprint + fwidth(mask * amount * border));
    // Measured in metres, not in weight units. The weights are bilinear page
    // fields (4..64 m texels) or vertex values: where a field steps from one
    // material to the next between two texels, a band of 0.2 in weight is a
    // fraction of a metre - a hard line - and its polygonal tie line shows as
    // teeth along the texel grid. gap/|grad gap| is the distance to that line,
    // and the band and the noise that bends it get a floor in metres.
    // The slope of the SIGNED difference in a fixed material order: gap itself
    // is |difference| and its derivative vanishes in the quad on the tie line.
    const float signedGap = here.top < here.under ? gap : -gap;
    float gapSlope;
    if (gWeightGradients) {
        const float gx[6] = {gWeightDX0.x, gWeightDX0.y, gWeightDX0.z, gWeightDX0.w, gWeightDX1.x, gWeightDX1.y};
        const float gy[6] = {gWeightDY0.x, gWeightDY0.y, gWeightDY0.z, gWeightDY0.w, gWeightDY1.x, gWeightDY1.y};
        const float2 slope = float2(gx[here.top] - gx[here.under], gy[here.top] - gy[here.under]);
        gapSlope = max(length(slope) / max(total, 1e-5), 1e-5);
    } else {
        gapSlope = max(length(float2(ddx(signedGap), ddy(signedGap))) / max(footprint, 1e-5), 1e-5);
    }
    const float minimumBand = (qualityPS.x > 0.0 ? qualityPS.x : 3.0) * saturate(style.x) * 1.4;
    // The band and its noise must both be spent before the second weight
    // reaches nought (gap = 1): past that point there is no "under" material
    // to lerp to and the early-out below would be a hard edge.
    const float reach = 0.5 / gapSlope;
    // At least ~4 pixels wide on the page path: far away a band of a few
    // metres is one pixel, and that reads as a cut line rather than one
    // ground giving way to another. (The vertex path keeps its 1.5 px.)
    const float pixelBand = gWeightGradients ? 4.0 : 1.5;
    const float bandMetres = min(max(max(blendWidth / gapSlope, minimumBand), pixelBand * footprint), reach);
    const float gapMetres = gap / gapSlope;
    const float nearTie = (1.0 - smoothstep(bandMetres * 1.5, bandMetres * 3.0, gapMetres)) *
                          smoothstep(0.0, 0.10, next);
    const float swingMetres = min(max(amount / gapSlope, bandMetres * 0.9), reach);
    const float intrusionMetres = mask * swingMetres * nearTie * orientation;
    [branch] if (next <= 0.0) {
        here.mix = 1.0;
        here.colour = top.colour;
        here.properties = top.properties;
        here.normal = top.normal;
        return here;
    }
    MaterialSample under = sampleGroundClass(here.under, context, p, normal, dx, dy);
    // Where the runner-up and the third material swap places the "under"
    // texture used to switch from one to the other in a single pixel - a hard
    // seam through the middle of a soft border. Near that swap the third is
    // sampled too and the two meet at an even mix, so both sides agree.
    const float underSure = lerp(1.0, smoothstep(0.0, max(0.5 * blendWidth, 0.04), next - third),
                                 smoothstep(0.0, 0.04, third));
    [branch] if (underSure < 1.0) {
        const MaterialSample other = sampleGroundClass(third_i, context, p, normal, dx, dy);
        const float keep = lerp(0.5, 1.0, underSure);
        under.colour = lerp(other.colour, under.colour, keep);
        under.properties = lerp(other.properties, under.properties, keep);
        under.normal = normalize(lerp(other.normal, under.normal, keep));
    }
    // Two readings of the same boundary; the softer one wins. A broad weight
    // gradient (a biome fading over hundreds of metres) keeps its wide blend
    // in weight units; a one-texel step gets at least the metre band.
    //
    // Each reading is divided by how much the tear steepens it. The tear
    // moves the boundary by up to a band's width over a fraction of a metre,
    // so without this the edge along every finger of it is many times
    // sharper than the band: soft across the border, hard teeth along it.
    // Divided through, the edge keeps the band's softness wherever it runs.
    const float stretchWeight = sqrt(1.0 + pow(amount * border * maskSlope / gapSlope, 2.0));
    const float stretchMetres = sqrt(1.0 + pow(swingMetres * nearTie * maskSlope, 2.0));
    const float inWeight = (gap + intrusion) / (max(blendWidth, 1e-4) * stretchWeight);
    const float inMetres = (gapMetres + intrusionMetres) / (max(bandMetres, 1e-4) * stretchMetres);
    // The softer one wins where they agree; where they disagree it is zero.
    // "Smaller |x| wins" alone jumps from +a to -b wherever the readings
    // disagree in sign and cross in magnitude - a hard line in a soft border.
    const float softer = (abs(inWeight) < abs(inMetres) ? inWeight : inMetres) *
                         (inWeight * inMetres > 0.0 ? 1.0 : 0.0);
    here.mix = smoothstep(-1.0, 1.0, softer);
    // A runner-up with almost no weight cannot own the pixel whatever the
    // noise says: where it fades out the top material closes over it. (At a
    // clamped weight the screen-space slope halves and the metre reading
    // could otherwise flip sign in the very last texel.)
    here.mix = lerp(1.0, here.mix, smoothstep(0.0, 0.06, next));
    // Height-based blend for what is drawn (coverage stays here.mix). Two
    // materials meeting do not cross-fade like two slides: the sand settles
    // into the hollows between the stones and the grass stands up out of the
    // earth. Each side's scanned height biases its share, so a border is
    // crisp and organic close up instead of a soft double exposure. Far away
    // the heights are mip-averaged to their mean and this is the plain mix.
    const float contrast = 0.65;
    const float ha = top.properties.b * contrast + here.mix;
    const float hb = under.properties.b * contrast + (1.0 - here.mix);
    const float ridge = max(ha, hb) - 0.18;
    const float wa = max(ha - ridge, 0.0), wb = max(hb - ridge, 0.0);
    const float drawn = wa / max(wa + wb, 1e-5);
    here.colour = lerp(under.colour, top.colour, drawn);
    here.properties = lerp(under.properties, top.properties, drawn);
    here.normal = normalize(lerp(under.normal, top.normal, drawn));
    return here;
}

Ground groundHere(float4 weights0, float2 weights1, float3 worldPos, float3 normal)
{
    return groundHereIn(weights0, weights1, worldPos, normal, noGroundContext());
}

// Ground that is standing in for a level not yet cut. Same in every way but the
// depth it writes.
TerrainOut TerrainBackdropVS(TerrainIn input)
{
    TerrainOut output = TerrainVS(input);
    output.position = projectBehind(morphed(input.position, input.morphHeight));
    return output;
}

float4 terrainSurface(TerrainOut input);

// The vertex-weight entry: no analytic weight gradients. Reset explicitly -
// a static's initializer is not something to trust across every backend.
float4 TerrainPS(TerrainOut input) : SV_Target0
{
    gWeightGradients = false;
    return terrainSurface(input);
}

float4 terrainSurface(TerrainOut input)
{
#ifndef TERRAIN_PAGE_MATERIALS
    if (extraPS.w > 0.5 && ringReserved.x > 0.5) {
        float stripe=step(0.5,frac((input.position.x+input.position.y)/16.0));
        return float4(lerp(float3(0.19,0.19,0.23),float3(0.31,0.31,0.35),stripe),1.0);
    }
#endif
    const float3 worldPos = float3(input.worldXY, input.worldHeight);

    // The shore, as a band of ground rather than a line where the water stops.
    //
    // Requirement 8 asks for deep water, shallow water, wet shoreline, a sand or
    // mud band, dry bank and a vegetation fringe, and what was here was two of
    // them: water, and then grass with a dark wet rim a few centimetres wide.
    // Read from the height above the water rather than from a distance to it -
    // the same thing on any bank worth the name, and it is already carried on
    // the vertex, where a distance is not. Capillary rise works this way round
    // too: what makes a beach is how far the ground is above the water table,
    // not how far it is from the edge in plan.
    //
    // The width of the band is itself noisy in world space, so the sand does not
    // arrive at a contour line. Two octaves, stationary, the same trick the
    // water's own edge uses - a tiling mask would put the shore on a grid.
    const float aboveWater = -input.waterDepth;
    // How much ground one pixel covers. Wanted this early because everything
    // below that tears a boundary with noise has to be told what it may still
    // resolve; a tear finer than this is not a tear, it is a shimmer.
    const float pixel = max(length(ddx(input.worldXY)), length(ddy(input.worldXY)));
    const float shoreBreak = fbmAt(input.worldXY / 19.0 +
                                   domainWarp(input.worldXY / 19.0) * 0.35) * 0.62 +
                             filteredNoiseAt(input.worldXY / 5.7 + 31.4, pixel / 5.7) * 0.38;
    const float bankWidth = 1.1 + shoreBreak * 2.6;
    const float shoreBand = 1.0 - smoothstep(-0.4, bankWidth, aboveWater);
    // Not up a cliff and not on a wall: a beach is flat ground at the water.
    const float shoreFlat = smoothstep(0.55, 0.88, normalize(input.normal).z);
    // Curvature of the ground over eight metres, carried on the vertex. What a
    // break of slope is, and - requirement 6 - what decides where material
    // collects and where it is stripped.
    const float bend = extraPS.w > 0.5 ? 0.0 : input.geography.z;
    // Curvature comes from the source field. Perturbing even zero curvature
    // with noise invented hollows/noses on an otherwise smooth inclined plane.
    const float steepness = 1.0 - normalize(input.normal).z;

    // Erosion and deposition, requirement 6: "in concave areas strengthen the
    // accumulation of moisture, soil and vegetation; on convex and exposed
    // areas expose rock more often".
    //
    // Both directions off the one number, and it is the right number: a hollow
    // is where water slows and drops what it was carrying, and a nose is where
    // it speeds up and takes the soil with it. Weights rather than a repaint,
    // so everything downstream - the material blend, the meadow, what a walker
    // feels - agrees about it. Modest: this is meant to read as the ground
    // having a history, not as green stripes down every gully.
    const float hollow = saturate(-bend * 16.0);
    const float nose = saturate(bend * 16.0);
    float4 shoreWeights0 = input.weights0;
    float2 shoreWeights1 = input.weights1;
    [branch] if (shoreBand > 0.002) {
        // Only a bank the world already made sandy becomes a beach: the band
        // strengthens sand that is there, it does not pave every river bank.
        const float sandy = shoreBand * shoreFlat *
                            saturate(input.weights0.z * 3.0 + input.desertCover * 0.8);
        // Under the water it is mud, above it sand: the bed of a channel is not
        // a beach, and drawing beach sand under two metres of water is what made
        // the bottom read as bright as the bank.
        const float drowned = smoothstep(0.0, 0.9, input.waterDepth);
        shoreWeights0.z = saturate(shoreWeights0.z + sandy * (1.0 - drowned) * 0.90);
        shoreWeights1.x = saturate(shoreWeights1.x + sandy * drowned * 0.75);
        shoreWeights0.x *= 1.0 - sandy * 0.92;   // grass gives way at the water
        shoreWeights0.y *= 1.0 - sandy * 0.45;   // and so, less, does bare soil
    }
    // Applied after the shore band, which is a stronger statement about the
    // same ground: a beach is a beach whether or not it sits in a hollow.
    shoreWeights0.x = saturate(shoreWeights0.x * (1.0 + hollow * 0.45) *
                                                 (1.0 - nose * 0.30));   // grass
    shoreWeights0.y = saturate(shoreWeights0.y * (1.0 + hollow * 0.30));  // soil collects
    shoreWeights0.w = saturate(shoreWeights0.w * (1.0 - hollow * 0.35) +
                               nose * steepness * 0.35);                  // rock comes through
    const float3 slopeNormal = normalize(input.normal);
    GroundContext context;
    // environment.w is the climate field's constant one; a zeroed probe
    // input has none, and keeps every class on its base layer.
    context.valid = input.environment.w > 0.5;
    context.moisture = saturate(input.environment.z);
    context.desert = saturate(input.desertCover);
    context.forest = saturate(input.forest);
    context.aboveWater = aboveWater;
    context.steepness = steepness;
    // The terrain category and the layers' biomes here (engine/biomes).
    const BiomeHere biome = biomeHereAt(input.worldXY, pixel);
    context.categoryA = biome.groundA;
    context.categoryB = biome.groundB;
    context.categoryShare = biome.groundShare;
    context.forestBiome = biome.forest;
    Ground ground = groundHereIn(shoreWeights0, shoreWeights1, worldPos, slopeNormal, context);
    float3 colour = ground.colour;
#ifdef BIOMES_ENABLED
    // The category's look: a tint and how far it is pulled to grey, blended
    // across the border with the ground's own.
    [branch] if (biome.any) {
        const float4 lookA = biomeRow(BIOME_ROW_CATEGORY + biome.groundA, 0);
        const float4 lookB = biomeRow(BIOME_ROW_CATEGORY + biome.groundB, 0);
        const float4 look = lerp(lookA, lookB, biome.groundShare);
        [branch] if (any(look != float4(1, 1, 1, 1))) {
            const float grey = dot(colour, float3(0.30, 0.59, 0.11));
            colour = lerp(float3(grey, grey, grey), colour, look.a) * look.rgb;
        }
    }
#endif
    // Imported albedo owns the ground colour: no height/noise colour tint -
    // except the grass's season. The photographed blade carries its own, and
    // on its own it read as one olive-brown over every country. A painted
    // world's grass follows its climate: lush green where it is wet, gold
    // where it is dry. Luma-preserving, and only as far as grass covers the
    // ground, so the scan's own detail stays.
    const float grassCover = materialCoverage(0, ground.top, ground.under, ground.mix);
    [branch] if (context.valid && grassCover > 0.001) {
        // The scan under it is a brown-olive photograph (hue ~40): half a tint
        // left the meadows of a temperate country orange in clear weather.
        // Green over all but the driest ground, gold only there, and four
        // fifths of the way to the climate's colour (the scan keeps its grain).
        const float lush = saturate((context.moisture - 0.04) * 1.8) * (1.0 - context.desert);
        const float3 season = lerp(float3(0.98, 0.86, 0.50), float3(0.58, 1.00, 0.36), lush);
        const float blade = dot(colour, float3(0.30, 0.59, 0.11));
        const float3 tinted = lerp(colour, blade * season * 1.22, 0.80);
        colour = lerp(colour, tinted, grassCover);
    }
    const float rockCover = materialCoverage(3, ground.top, ground.under, ground.mix);

    const float3 sun = landscapeSun();
    float3 normal = normalize(input.normal);

    // Only resolvable, local ground clutter needs this fade. Large-scale
    // slope relief is already in the heightfield; do not add noise-based cuts.
    const float detail = 1.0 - smoothstep(0.28, 0.80, pixel);

    // Source curvature distinguishes a convex lip from a concave foot.
    // These masks admit occasional local fragments; they do not invent grooves
    // over the whole face or repaint the slope according to its mesh LOD.
    const float cliffLip = smoothstep(0.10, 0.34, steepness) *
                           smoothstep(0.005, 0.045, bend);
    // The foot: the concave landing. Not on the face, which is straight, and
    // not out on the flat, where nothing has fallen.
    const float cliffFoot = smoothstep(0.05, 0.20, steepness) *
                            (1.0 - smoothstep(0.26, 0.50, steepness)) *
                             smoothstep(0.005, 0.045, -bend);

    // table.x is relative to the 14-metre reference turn. Three projections
    // avoid the infinitely stretched top-down UVs of a near-vertical face.
    // `colour` is the material blend resolved from stable world-space weights.
    float3 groundColour = colour;
    const float sandSupport = sandDriftSupport(input.desertCover, input.weights0.z,
            input.weights0.w, input.weights0.x, input.weights1.y, input.weights1.x,
            input.waterDepth, normal.z);
    float3 shadingNormal = ground.normal;
    // stageDiagnostic.y is total height removed since the volcano stage, NOT
    // a spatial micro-height field. Using it to seed 12/36/108 m noise carved
    // random virtual grooves across every eroded slope (including grass).
    // Keep the actual eroded geometry and material normals, not a second relief.

    // Normal maps already contain the scan's relief. Differentiating the
    // displacement as well doubled it, and screen XY was not a world gradient.
    // Ground cover is persistent at ALL distances, never a distant-only repaint.
    // The material itself is never replaced by a triplanar rock projection.
    // This keeps a cliff's identity identical to the flat ground at every LOD.

    // The lip, and the talus under it - requirement 5's sequence, flat grass ->
    // broken rocky lip -> cliff face -> scree -> soil. Both read off the same
    // curvature that separated the zones, so they arrive wherever a break of
    // slope does and nowhere else; nothing is placed.
    // Lip and scree remain available as masks for small stone detail below,
    // but they do not repaint the substrate with a second material.
    float sandCover = 0.0;
    float sandRoughness = 0.86;
    [branch] if (sandSupport > 0.001 && pixel < 1.8 && dot(windPS.xy, windPS.xy) > 1e-6) {
        const float2 direction = normalize(windPS.xy);
        const float2 across = float2(-direction.y, direction.x);
        const float alongMetres = dot(input.worldXY, direction);
        const float acrossMetres = dot(input.worldXY, across);
        // A stationary patch mask breaks up the response without sliding the
        // substrate. Terrain horizons suppress transport behind ridges/banks.
        // Broad and gentle: a 7 m value-noise threshold cut the ripple field
        // into blobs whose edges read as combs of half-crests.
        const float patch = smoothstep(0.15, 0.85, organicNoise(input.worldXY / 37.0));
        const float exposure = saturate(input.windExposure);
        // Ripples belong to the sand actually drawn here, not to the smooth
        // data field under it: the drawn border is torn by noise, and ripples
        // running on past it onto grass fingers were the teeth along it.
        const float drawnSand = smoothstep(0.45, 0.95,
            materialCoverage(2, ground.top, ground.under, ground.mix));
        sandCover = sandSupport * drawnSand * (0.55 + 0.45 * patch) *
                    (1.0 - smoothstep(1.2, 1.8, pixel));
        const SandRipple ripple = sandRipple(alongMetres, acrossMetres, viewportPS.z,
                                               windPS.z * exposure, pixel);
        const float2 slopeXY = direction * ripple.alongSlope + across * ripple.acrossSlope;
        const float3 gradient = float3(slopeXY, 0.0);
        const float3 tangentGradient = gradient - normal * dot(gradient, normal);
        shadingNormal = normalize(shadingNormal - tangentGradient * sandCover);
        sandRoughness = ripple.roughness;
        const float drift = sandDriftOpacity(alongMetres, acrossMetres,
                viewportPS.z, windPS.z, windPS.w, pixel);
        groundColour *= 1.0 + drift * sandCover * exposure * 0.08;
        sandRoughness = lerp(sandRoughness, 0.96, drift * exposure);
    }
    // Restrained scree on EXPOSED rock only. Sand is not evidence of a river,
    // and a small residual rock weight is not permission to cover a meadow.
    const float exposed = stoneSupport(rockCover, grassCover,
        materialCoverage(2, ground.top, ground.under, ground.mix),
        materialCoverage(5, ground.top, ground.under, ground.mix),
        materialCoverage(4, ground.top, ground.under, ground.mix));
    const float patch = smoothstep(0.45, 0.72, noiseAt(input.worldXY / 13.0 + 37.1));
    const float washed = shoreBand * shoreFlat * smoothstep(0.0, 0.8, input.waterDepth);
    const float2 scree = stoneScatter(input.worldXY, 0.85,
        exposed * patch * (cliffFoot * 0.18 + cliffLip * 0.06), 3.1, pixel);
    const float2 gravel = stoneScatter(input.worldXY, 1.7,
        exposed * patch * washed * 0.12, 21.7, pixel);
    // No extra rock-colour overlay or fake multi-metre boulders. The substrate
    // already contains stones; just let occasional fragments catch the light.
    shadingNormal = reliefNormal(shadingNormal, worldPos, scree.y + gravel.y, 0.35);

    // Ground clutter - requirement 11, "remove the feeling of a clean texture".
    //
    // The stones above are one class of it; these are the rest. Same rule as
    // the stones and for the same reason: nothing is scattered uniformly, every
    // density comes from a field that says why the thing would be lying there.
    // None of it is an entity - it is a few reads of noise and a tint, which is
    // what the requirement asks for explicitly.
    //
    // Under the same `detail` gate: clutter smaller than a pixel is not clutter,
    // it is a dirty texture, which is the thing this is meant to cure.
    float clutterHeight = 0.0;
    [branch] if (detail > 0.001) {
        const float grassy = grassCover;
        // Litter under vegetation: leaves, twigs, the debris of things growing
        // and dropping. Where the meadow is, patchy rather than even, and it
        // takes the ground's own colour down rather than adding a new one -
        // leaf litter is the same palette as what shed it.
        const float2 litter = stoneScatter(input.worldXY, 0.55,
                grassy * 0.20 * saturate(input.foliage.z + input.foliage.w + 0.3), 91.3, pixel);
        groundColour *= 1.0 - litter.x * 0.06;
        // Tussocks: grass grows in clumps on damp ground, not as a lawn. A
        // lighter, drier crown on a small dome, which is what a tussock is from
        // above.
        const float2 tussock = stoneScatter(input.worldXY, 0.9,
                grassy * (0.10 + 0.15 * hollow), 57.7, pixel);
        groundColour *= 1.0 + tussock.x * 0.04;
        // Mud: standing water leaves it, so it goes where the ground is damp and
        // flat - the floor of a hollow, the back of a bank - and nowhere on a
        // slope, because mud does not stay on one.
        const float2 mud = stoneScatter(input.worldXY, 2.2,
                hollow * (1.0 - smoothstep(0.06, 0.22, steepness)) * 0.20, 33.1, pixel);
        groundColour *= 1.0 - mud.x * 0.06;
        // The strand line: shells and driftwood, only on a shore, and only just
        // above the water where the last tide left them.
        const float strand = shoreBand * (1.0 - smoothstep(0.0, 0.9, input.waterDepth)) *
                             saturate(shoreWeights0.z * 1.5);
        const float2 flotsam = stoneScatter(input.worldXY, 1.4, strand * 0.12, 77.5, pixel);
        groundColour *= 1.0 + flotsam.x * 0.04;
        // All of it sits on the ground rather than in it.
        clutterHeight = (litter.y * 0.4 + tussock.y + flotsam.y * 0.6) * detail;
    }
    shadingNormal = reliefNormal(shadingNormal, worldPos, clutterHeight, 0.20);

    // Decals of the category and the decor layer (engine/biomes): stored
    // nowhere, each cell of each set hashing itself on the ground.
    float decalGlow = 0.0, decalMetal = 0.0;
    float decalRough = -1.0;
#if defined(BIOMES_ENABLED) && defined(BIOME_ANY_DECALS)
    [branch] if (biome.any) {
        BiomeDecalIn decalIn;
        decalIn.xy = input.worldXY;
        decalIn.pixel = pixel;
        decalIn.steepness = steepness;
        decalIn.aboveWater = aboveWater;
        decalIn.grass = grassCover;
        decalIn.eye = length(worldPos - cameraPS.xyz);
        decalIn.wind = windPS.xy;
        BiomeDecalOut decals = noBiomeDecal();
        biomeDecalsHere(biome, decalIn, decals);
        [branch] if (decals.cover > 0.001) {
            groundColour = lerp(groundColour, decals.colour, decals.cover);
            decalGlow = decals.emissive;
            decalMetal = decals.metal * decals.cover;
            decalRough = lerp(-1.0, decals.rough, decals.cover);
        }
    }
#endif

    // Vegetation density belongs to plants, not a painted biome palette.
    // Keep lighting, wetness and snow, without seasonal albedo recolouring.
    const float weatherWet=input.weather.y*input.weather.w;
    const float snow=wxSnowMask(input.weather.x,normal.z,input.waterDepth,
                               noiseAt(input.worldXY/11.0))*input.weather.w;
    const float3 worldDx = ddx(worldPos), worldDy = ddy(worldPos);
    float4 surfaceProperties = ground.properties;
    [branch] if (snow > 0.001) {
        const MaterialSample snowSurface = sampleGroundMaterial(5, worldPos, normal, worldDx, worldDy);
        groundColour = lerp(groundColour, snowSurface.colour, snow);
        surfaceProperties = lerp(surfaceProperties, snowSurface.properties, snow);
    }
    shadingNormal=normalize(lerp(shadingNormal,normal,snow*0.8));
    sandCover*=1.0-snow;
    // And a hollow holds its water after a nose has shed it - the third of the
    // three things requirement 6 says a concavity accumulates, after soil and
    // vegetation. Bounded: this darkens a dell, it does not paint a puddle.
    const float wetness = max(max(lookWetness(input.waterDepth, normal.z), weatherWet),
                              hollow * 0.35) *
                          (1.0-max(input.weights1.y,snow));
    groundColour *= 1.0 - wetness * 0.18;

    // Stylized roughness microvariation. The current renderer is deliberately
    // diffuse-first, so this modulates the small broad grain lobe below rather
    // than pretending to be a full metallic/BRDF path.
    const float roughNoise = filteredMaterialNoise(input.worldXY / 0.7, pixel / 0.7);
    float materialRoughness = clamp(surfaceProperties.g + (roughNoise - 0.5) * 0.06 -
                                    wetness * 0.18, 0.38, 0.98);
    if (decalRough >= 0.0) materialRoughness = lerp(materialRoughness, decalRough, saturate(decalRough + 1.0));
    // AO removes ambient light only. Do not multiply photographed albedo by
    // another black cavity layer or fade AO away with camera distance.
    const float skyVisibility = lerp(1.0, surfaceProperties.r, 0.38);
    const float shadow = proceduralShadow(float3(input.worldXY,input.worldHeight),normal);
    float3 lit = groundColour * landscapeDaylight(shadingNormal, skyVisibility, shadow);
    // A weak, broad dry-grain lobe: roughness really affects lighting, without
    // wet-looking glitter. Perspective uses the eye-to-surface direction.
    const float3 eye = landscapeEye(float3(input.worldXY, input.worldHeight));
    const float roughness = lerp(materialRoughness, sandRoughness, sandCover);
    const float lobe = pow(saturate(dot(shadingNormal, normalize(sun + eye))),
                           lerp(48.0, 5.0, roughness));
    const float sunVisibility = shadow * saturate(dot(shadingNormal, sun)) *
        (1.0 - saturate(parametersPS[2].z * parametersPS[0].x) * 0.78);
    // Quiet dielectric highlight for every material, not just animated sand.
    lit += float3(1.0, 0.97, 0.90) * (0.045 * (1.0 - roughness) * lobe * sunVisibility);
    // A metal fleck catches the sun in its own colour; a glowing one shines
    // whatever the light.
    lit += groundColour * (decalMetal * 0.6 * lobe * sunVisibility) + groundColour * decalGlow;
    float3 finished = landscapeFinish(lit, float3(input.worldXY, input.worldHeight));
    // The map goes on last, over finished ground rather than instead of it.
    //
    // A flat wash would take the hills out along with the material, and the
    // reading would float over a shape that is no longer there. So the layer is
    // modulated by what it covers: the terrain's own light comes back up through
    // the colour, a slope still reads as a slope, and a coast is still a coast.
    // The ramp keeps its meaning because the modulation is a scale and not a
    // second hue - equal readings stay equal in colour, lighter or darker only
    // by how the ground beneath them is lit.
    [branch] if (extraPS.w > 0.5) {
        // The water pass is suppressed under a map rather than washing it out,
        // so the sea would otherwise arrive as bare seabed. Sink it first: a
        // coastline is half of what makes any of these readable.
        finished = lerp(finished, float3(0.05, 0.09, 0.15),
                        smoothstep(0.0, 2.5, input.waterDepth) * 0.72);
        const float4 map = inspectionColour(input.environment, input.geography, normal,
                input.waterDepth, input.worldHeight, input.worldXY, input.travelCost);
        // Bounded either side: dark ground must not swallow the reading, and
        // snow must not push the ramp past white, which would clip one channel
        // before the others and quietly change the colour's meaning.
        const float ground = dot(finished, float3(0.30, 0.59, 0.11));
        const float3 layer = saturate(map.rgb * lerp(0.68, 1.12, saturate(ground)));
        finished = lerp(finished, layer, map.a);
    }
    return float4(finished, sceneDepthAlpha(float3(input.worldXY, input.worldHeight)));
}
