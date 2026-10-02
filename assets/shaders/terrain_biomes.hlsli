// Terrain categories and layer biomes on the card
// (doc/plan_ground_types_2026-10-01.md, engine/biomes).
//
// Two pictures. The category plane is the climate's fourth: four raw ids a
// sample - the ground's category, the forest, water and decor layers' biomes
// (0: as the category says) - at the climate's spacing, read nearest. The
// table is every number of the libraries, a row each (engine/biomes/
// shader_tables.hpp has the layout): edited live, never recompiled.
//
// The code that knows which category uses which soil, noise and decal is
// generated (terrain_biomes.gen.hlsli, from content/config/terrain) and
// included by terrain_biomes_code.hlsli; without it every id draws as id 0.
//
// A shader that wants these defines BIOME_PLANE_SLOT, BIOME_TABLE_SLOT and
// their samplers' slots before including this file; without them nothing is
// bound and every function here answers "as before categories".
#ifndef TERRAIN_BIOMES_HLSLI
#define TERRAIN_BIOMES_HLSLI

#include "noise.hlsli"

// A vertex stage defines BIOME_SPACE space0 and BIOME_TRANSFORM parameters[20]
// as well (its own textures and constants); a fragment stage takes these.
#ifndef BIOME_SPACE
#define BIOME_SPACE space2
#endif
#ifndef BIOME_TRANSFORM
#define BIOME_TRANSFORM parametersPS[20]
#endif
#if defined(BIOME_PLANE_SLOT) && defined(BIOME_TABLE_SLOT)
#define BIOMES_ENABLED 1
Texture2D biomeCategoryPlane : register(BIOME_PLANE_SLOT, BIOME_SPACE);
SamplerState biomeCategorySampler : register(BIOME_PLANE_SAMPLER, BIOME_SPACE);
Texture2D biomeTable : register(BIOME_TABLE_SLOT, BIOME_SPACE);
SamplerState biomeTableSampler : register(BIOME_TABLE_SAMPLER, BIOME_SPACE);
#endif

// The table's sections (engine/biomes/shader_tables.hpp, kRow*).
#define BIOME_TABLE_COLUMNS 16
#define BIOME_ROW_CATEGORY 0
#define BIOME_ROW_SOIL 256
#define BIOME_ROW_WATER 512
#define BIOME_ROW_DECAL 768
#define BIOME_ROW_FOREST 1024
#define BIOME_ROW_FOLIAGE 1280
#define BIOME_ROW_ROCK 1536
#define BIOME_ROW_DECOR 1792
#define BIOME_TABLE_ROWS 2048

float4 biomeRow(int row, int column)
{
#ifdef BIOMES_ENABLED
    const float2 uv = (float2(column, row) + 0.5) / float2(BIOME_TABLE_COLUMNS, BIOME_TABLE_ROWS);
    return biomeTable.SampleLevel(biomeTableSampler, uv, 0);
#else
    return float4(1, 1, 1, 1);
#endif
}

// --- what is here --------------------------------------------------------

// The ground's category as a pair and the share of the second, torn by noise
// so a border of 256 m samples becomes a ragged line tens of metres wide; the
// other layers' biomes, resolved through the category's defaults.
struct BiomeHere {
    int groundA, groundB;
    float groundShare;      // of groundB
    int forest, water, decor;
    int waterB;             // the water of groundB's side, for a blended river
    bool any;               // any id here at all
};

BiomeHere noBiomeHere()
{
    BiomeHere h;
    h.groundA = 0; h.groundB = 0; h.groundShare = 0.0;
    h.forest = 0; h.water = 0; h.decor = 0; h.waterB = 0;
    h.any = false;
    return h;
}

#ifdef BIOMES_ENABLED
int4 biomeIdsAtTexel(float2 texel, float2 size)
{
    const float4 v = biomeCategoryPlane.SampleLevel(biomeCategorySampler, (texel + 0.5) / size, 0);
    return int4(v * 255.0 + 0.5);
}
#endif

int biomeDefault(int category, int which)   // which: 0 forest, 1 water, 2 decor
{
    const float4 d = biomeRow(BIOME_ROW_CATEGORY + category, 9);
    return which == 0 ? int(d.x + 0.5) : which == 1 ? int(d.y + 0.5) : int(d.z + 0.5);
}

BiomeHere biomeHereAt(float2 worldXY, float pixel)
{
    BiomeHere h = noBiomeHere();
#ifdef BIOMES_ENABLED
    float width, height;
    biomeCategoryPlane.GetDimensions(width, height);
    const float2 size = float2(width, height);
    // Sample (column, row) stands at world (column, row) * metres: the climate
    // transform puts texel centres there.
    const float2 uv = worldXY * BIOME_TRANSFORM.xy + BIOME_TRANSFORM.zw;
    const float2 t = uv * size - 0.5;
    const float2 i0 = floor(t), f = t - i0;
    const int4 c00 = biomeIdsAtTexel(i0, size), c10 = biomeIdsAtTexel(i0 + float2(1, 0), size);
    const int4 c01 = biomeIdsAtTexel(i0 + float2(0, 1), size), c11 = biomeIdsAtTexel(i0 + float2(1, 1), size);
    const float w[4] = {(1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y};
    const int4 c[4] = {c00, c10, c01, c11};
    // The ground ids' weights, by id: at most four distinct.
    int id[4]; float sum[4];
    [unroll] for (int k = 0; k < 4; ++k) { id[k] = -1; sum[k] = 0.0; }
    [unroll] for (int k = 0; k < 4; ++k) {
        const int g = c[k].x;
        [unroll] for (int j = 0; j < 4; ++j) {
            if (id[j] == g) { sum[j] += w[k]; break; }
            if (id[j] < 0) { id[j] = g; sum[j] = w[k]; break; }
        }
    }
    int best = 0, next = -1;
    [unroll] for (int k = 1; k < 4; ++k)
        if (id[k] >= 0 && sum[k] > sum[best]) best = k;
    [unroll] for (int k = 0; k < 4; ++k)
        if (k != best && id[k] >= 0 && (next < 0 || sum[k] > sum[next])) next = k;
    h.groundA = id[best];
    h.groundB = next >= 0 ? id[next] : id[best];
    const float raw = next >= 0 ? sum[next] / max(sum[best] + sum[next], 1e-5) : 0.0;
    // Torn: broad and fine noise move the tie line about by up to a third of
    // a sample either way, then a band a few tens of metres wide.
    const float tear = (noiseAt(worldXY / 140.0 + 17.3) * 0.65 +
                        filteredNoiseAt(worldXY / 37.0 + 3.1, pixel / 37.0) * 0.35 - 0.5) * 0.7;
    h.groundShare = next >= 0 ? smoothstep(0.42, 0.58, raw + tear) : 0.0;
    // The other layers: the sample nearest the point, its own id or its
    // category's default.
    const int4 near = f.x < 0.5 ? (f.y < 0.5 ? c00 : c01) : (f.y < 0.5 ? c10 : c11);
    const int g = h.groundShare > 0.5 ? h.groundB : h.groundA;
    h.forest = near.y != 0 ? near.y : biomeDefault(g, 0);
    h.water = near.z != 0 ? near.z : biomeDefault(h.groundA, 1);
    h.waterB = near.z != 0 ? near.z : biomeDefault(h.groundB, 1);
    h.decor = near.w != 0 ? near.w : biomeDefault(g, 2);
    h.any = (c00.x | c10.x | c01.x | c11.x | c00.y | c00.z | c00.w | near.y | near.z | near.w) != 0;
#endif
    return h;
}

// --- a category's choices -------------------------------------------------
// What the generated code answers with (terrain_biomes.gen.hlsli): no code
// of its own, only which row, which noise kind, which layers. One copy of
// each noise kind and decal family then serves every category.

struct BiomeSoil {
    int row;            // BIOME_ROW_SOIL + its index; -1: none
    int kind;           // 0 fbm, 1 cellular, 2 streaks, 3 ridged
    int layers;         // one to three
    int l0, l1, l2;     // texture layers
};
BiomeSoil noBiomeSoil()
{
    BiomeSoil s;
    s.row = -1; s.kind = 0; s.layers = 0; s.l0 = 0; s.l1 = 0; s.l2 = 0;
    return s;
}
BiomeSoil biomeSoilOf(int row, int kind, int layers, int l0, int l1, int l2)
{
    BiomeSoil s;
    s.row = row; s.kind = kind; s.layers = layers; s.l0 = l0; s.l1 = l1; s.l2 = l2;
    return s;
}

struct BiomeSlope {
    BiomeSoil soil;     // the rock class's ground between the faces; none: the engine's
    int face;           // the face's layer; -1: the engine's base
    int rockRow;        // BIOME_ROW_ROCK + its index; -1: none
    int scree;          // the scree's layer; -1: none
    bool strata;
};
BiomeSlope noBiomeSlope()
{
    BiomeSlope s;
    s.soil = noBiomeSoil(); s.face = -1; s.rockRow = -1; s.scree = -1; s.strata = false;
    return s;
}

// --- noise kinds ----------------------------------------------------------
// A noise row: metres, contrast, warp, angle (radians). Each answers roughly
// evenly over 0..1, so a share can be cut from it.

float biomeNoiseFbm(float2 xy, float4 n, float footprint)
{
    float2 at = xy / max(n.x, 0.01);
    float fp = footprint / max(n.x, 0.01);
    at += domainWarp(at * 0.5) * n.z;
    float value = 0.0, amplitude = 0.5, total = 0.0;
    [unroll] for (int i = 0; i < 3; ++i) {
        value += filteredNoiseAt(at, fp) * amplitude;
        total += amplitude;
        at = at * 2.03 + float2(3.7, 8.1);
        fp *= 2.03;
        amplitude *= 0.5;
    }
    // Stretched back towards 0..1: three octaves gather about the middle.
    return saturate((value / total - 0.5) * 1.9 + 0.5);
}

float biomeNoiseCellular(float2 xy, float4 n, float footprint)
{
    float2 at = xy / max(n.x, 0.01);
    at += domainWarp(at * 0.7) * (0.15 + n.z);
    const float2 cell = floor(at), f = at - cell;
    float first = 8.0, second = 8.0, value = 0.5;
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float2 o = float2(i, j);
            const float2 c = fmod(cell + o + 4096.0, 4096.0);
            const float2 p = o + float2(hashAt(c + 0.37), hashAt(c + 71.9)) * 0.8 + 0.1;
            const float d = length(p - f);
            if (d < first) { second = first; first = d; value = hashAt(c + 13.1); }
            else if (d < second) second = d;
        }
    // Patches with an edge: the cell's own number, softened across its
    // border as far as the contrast allows - and to the mean once a patch is
    // smaller than a pixel.
    const float edge = saturate((second - first) / max(0.02, 0.35 * (1.0 - n.y)));
    const float resolved = 1.0 - smoothstep(0.25, 0.9, footprint / max(n.x, 0.01));
    return lerp(0.5, lerp(0.5, value, edge), resolved);
}

float biomeNoiseStreaks(float2 xy, float4 n, float footprint, float2 along)
{
    const float2 across = float2(-along.y, along.x);
    const float2 at = float2(dot(xy, along) * 0.12, dot(xy, across)) / max(n.x, 0.01);
    const float fp = footprint / max(n.x, 0.01);
    const float value = filteredNoiseAt(at + domainWarp(at * 0.3) * n.z, fp) * 0.7 +
                        filteredNoiseAt(at * 2.1 + 5.3, fp * 2.1) * 0.3;
    return saturate((value - 0.5) * 1.8 + 0.5);
}

float biomeNoiseRidged(float2 xy, float4 n, float footprint)
{
    float2 at = xy / max(n.x, 0.01);
    float fp = footprint / max(n.x, 0.01);
    at += domainWarp(at * 0.5) * n.z;
    float value = 0.0, amplitude = 0.55, total = 0.0;
    [unroll] for (int i = 0; i < 3; ++i) {
        value += (1.0 - abs(filteredNoiseAt(at, fp) * 2.0 - 1.0)) * amplitude;
        total += amplitude;
        at = at * 2.11 + float2(9.2, 1.7);
        fp *= 2.11;
        amplitude *= 0.5;
    }
    return saturate(value / total);
}

// A noise of a kind by number. Only the kinds the config uses are compiled
// in (BIOME_NOISE_*, terrain_biomes.gen.hlsli).
float biomeNoise(int kind, float2 xy, float4 n, float footprint)
{
    [branch] switch (kind) {
#ifdef BIOME_NOISE_FBM
    case 0: return biomeNoiseFbm(xy, n, footprint);
#endif
#ifdef BIOME_NOISE_CELLULAR
    case 1: return biomeNoiseCellular(xy, n, footprint);
#endif
#ifdef BIOME_NOISE_STREAKS
    case 2: return biomeNoiseStreaks(xy, n, footprint, float2(cos(n.w), sin(n.w)));
#endif
#ifdef BIOME_NOISE_RIDGED
    case 3: return biomeNoiseRidged(xy, n, footprint);
#endif
    default: return 0.5;
    }
}

// The part of the ground a share of it takes, from a noise's value: hard or
// soft at its edge by the contrast.
float biomeShareMask(float value, float share, float contrast)
{
    const float threshold = 1.0 - share;
    const float soft = lerp(0.22, 0.02, saturate(contrast));
    return share <= 0.0 ? 0.0 : share >= 1.0 ? 1.0 : smoothstep(threshold - soft, threshold + soft, value);
}

// Bands up a rock face: a row of the rock table (metres apart, lean per
// metre across), as a brightness about one.
float biomeStrata(float3 p, float4 rock)
{
    if (rock.x <= 0.0) return 1.0;
    const float at = (p.z + (p.x * 0.8 + p.y * 0.6) * rock.y) / rock.x;
    const float band = frac(at + noiseAt(p.xy / (rock.x * 6.0)) * 0.35);
    const float layer = hashAt(float2(floor(at), 3.7));
    return 1.0 + (smoothstep(0.0, 0.12, band) - smoothstep(0.62, 0.78, band)) * 0.07 + (layer - 0.5) * 0.10;
}

// Scree: the soil that gathers on the slope below a face, patchy.
float biomeScree(float steepness, float steepFrom, float2 xy)
{
    const float band = smoothstep(steepFrom * 0.40, steepFrom * 0.70, steepness) *
                       (1.0 - smoothstep(steepFrom * 0.85, steepFrom + 0.04, steepness));
    return band * smoothstep(0.30, 0.65, noiseAt(xy / 11.0 + 5.7) * 0.7 + noiseAt(xy / 3.3) * 0.3) * 0.85;
}

// --- decals ---------------------------------------------------------------

struct BiomeDecalIn {
    float2 xy;          // world metres
    float pixel;        // metres one pixel covers
    float steepness;
    float aboveWater;   // metres
    float grass;        // how much the grass covers the ground here
    float eye;          // metres from the eye
    float2 wind;        // its direction
};
struct BiomeDecalOut {
    float3 colour;      // what is laid over the ground
    float cover;        // how much of it
    float emissive;
    float metal;
    float rough;        // its own, where it covers
};
BiomeDecalOut noBiomeDecal()
{
    BiomeDecalOut o;
    o.colour = 0.0; o.cover = 0.0; o.emissive = 0.0; o.metal = 0.0; o.rough = 0.8;
    return o;
}

void biomeLayDecal(inout BiomeDecalOut o, float3 colour, float cover, float emissive, float metal, float rough)
{
    if (cover <= 0.0) return;
    const float k = cover * (1.0 - o.cover);
    o.colour = (o.colour * o.cover + colour * k) / max(o.cover + k, 1e-5);
    o.emissive = max(o.emissive, emissive * cover);
    o.metal = lerp(o.metal, metal, k);
    o.rough = lerp(o.rough, rough, k);
    o.cover += k;
}

// What every family shares: whether this ground takes the decal at all - its
// slope, its height over the water, its distance from the eye, the grass that
// hides it - and a clustering into patches.
float biomeDecalGate(int row, float weight, BiomeDecalIn d, out float4 look, out float4 size)
{
    look = biomeRow(row, 0);                    // colour, opacity
    size = biomeRow(row, 1);                    // cell, density, size min, max
    const float4 cluster = biomeRow(row, 2);    // cell, share, metal, rough
    const float4 lie = biomeRow(row, 3);        // slope min, max, under foliage, edge noise
    const float4 fade = biomeRow(row, 4);       // fade from, to, emissive, near water
    float gate = weight;
    gate *= smoothstep(lie.x - 0.05, lie.x + 0.02, d.steepness) * (1.0 - smoothstep(lie.y - 0.02, lie.y + 0.05, d.steepness));
    gate *= lerp(1.0, lie.z, saturate(d.grass));
    gate *= 1.0 - smoothstep(fade.y * 0.8, fade.y, d.eye);
    gate *= smoothstep(fade.x * 0.8, fade.x + 1e-3, d.eye + 1e-3);
    if (fade.w > 0.0) gate *= (1.0 - smoothstep(fade.w * 0.7, fade.w, d.aboveWater)) * smoothstep(-0.3, 0.0, d.aboveWater);
    if (cluster.x > 0.0) {
        const float patch = noiseAt(d.xy / cluster.x + float2(row * 0.37, 11.0)) * 0.7 +
                            noiseAt(d.xy / (cluster.x * 0.37) + 5.1) * 0.3;
        gate *= smoothstep(1.0 - cluster.y - 0.08, 1.0 - cluster.y + 0.08, patch);
    }
    return gate;
}

// Speckle: small flecks, one at most a cell, each hashing its own cell.
void biomeSpeckle(int row, float weight, BiomeDecalIn d, inout BiomeDecalOut o)
{
    float4 look, size;
    const float gate = biomeDecalGate(row, weight, d, look, size);
    if (gate <= 0.001) return;
    const float cellMetres = max(size.x, 0.05);
    const float2 cell = d.xy / cellMetres;
    const float2 whole = fmod(floor(cell) + 4096.0, 4096.0);
    const float salt = float(row) * 1.618;
    const float pick = hashAt(whole + salt);
    if (pick > size.y) return;
    const float shape = hashAt(whole + salt + 7.3);
    const float2 centre = float2(0.2 + shape * 0.6, 0.2 + frac(pick * 9.7) * 0.6);
    const float radius = lerp(size.z, size.w, frac(shape * 3.1)) / cellMetres * 0.5;
    const float2 off = frac(cell) - centre;
    const float turn = frac(pick * 5.3) * 6.2832;
    const float2 axis = float2(cos(turn), sin(turn));
    const float2 local = float2(dot(off, axis), dot(off, float2(-axis.y, axis.x))) / float2(1.0, 0.6 + shape * 0.5);
    const float away = length(local) / max(radius, 1e-4);
    const float footprint = d.pixel / max(radius * cellMetres * 2.0, 1e-4);
    // A fleck smaller than the pixel fades to its share of the pixel's colour.
    const float resolved = 1.0 - smoothstep(0.5, 2.0, footprint);
    const float aa = min(footprint, 0.5);
    float cover = (1.0 - smoothstep(0.8 - aa, 1.0 + aa, away)) * resolved;
    cover += (1.0 - resolved) * size.y * (radius * radius * 3.14) * 0.8;
    const float4 cluster = biomeRow(row, 2);
    const float4 fade = biomeRow(row, 4);
    biomeLayDecal(o, look.rgb, saturate(cover * look.a * gate), fade.z, cluster.z, cluster.w);
}

// Stain: big soft patches, torn at the edge.
void biomeStain(int row, float weight, BiomeDecalIn d, inout BiomeDecalOut o)
{
    float4 look, size;
    const float gate = biomeDecalGate(row, weight, d, look, size);
    if (gate <= 0.001) return;
    const float cellMetres = max(size.x, 0.5);
    const float2 cell = d.xy / cellMetres;
    const float2 base = floor(cell);
    const float salt = float(row) * 2.414;
    const float4 lie = biomeRow(row, 3);
    float cover = 0.0;
    // A stain may reach over its cell's edge: the cell and its neighbours.
    [unroll] for (int j = -1; j <= 1; ++j)
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float2 c = fmod(base + float2(i, j) + 4096.0, 4096.0);
            const float pick = hashAt(c + salt);
            if (pick > size.y) continue;
            const float shape = hashAt(c + salt + 3.3);
            const float2 centre = float2(i, j) + float2(0.25 + shape * 0.5, 0.25 + frac(pick * 7.7) * 0.5);
            const float radius = lerp(size.z, size.w, frac(shape * 4.7)) / cellMetres * 0.5;
            const float2 off = (cell - base) - centre;
            const float torn = (filteredNoiseAt(d.xy / max(radius * cellMetres * 0.35, 0.2) + c, d.pixel / max(radius * cellMetres * 0.35, 0.2)) - 0.5) * lie.w;
            const float away = length(off) / max(radius, 1e-4) + torn;
            cover = max(cover, 1.0 - smoothstep(0.55, 1.0, away));
        }
    const float4 cluster = biomeRow(row, 2);
    const float4 fade = biomeRow(row, 4);
    biomeLayDecal(o, look.rgb, saturate(cover * look.a * gate), fade.z, cluster.z, cluster.w);
}

// Streak: long thin smears along the wind (or a fixed direction).
void biomeStreak(int row, float weight, BiomeDecalIn d, inout BiomeDecalOut o)
{
    float4 look, size;
    const float gate = biomeDecalGate(row, weight, d, look, size);
    if (gate <= 0.001) return;
    const float4 dir = biomeRow(row, 5);    // angle, along the wind
    float2 along = float2(cos(dir.x), sin(dir.x));
    if (dir.y > 0.5 && dot(d.wind, d.wind) > 1e-6) along = normalize(d.wind);
    const float2 across = float2(-along.y, along.x);
    const float cellMetres = max(size.x, 0.2);
    const float2 at = float2(dot(d.xy, along) / (cellMetres * max(size.w / cellMetres, 1.0)), dot(d.xy, across) / cellMetres);
    const float2 base = floor(at);
    const float salt = float(row) * 3.14;
    float cover = 0.0;
    [unroll] for (int j = -1; j <= 1; ++j) {
        const float2 c = fmod(base + float2(0, j) + 4096.0, 4096.0);
        const float pick = hashAt(c + salt);
        if (pick > size.y) continue;
        const float shape = hashAt(c + salt + 1.7);
        const float centre = float(j) + 0.2 + shape * 0.6;
        const float width = lerp(0.06, 0.22, frac(shape * 5.1));
        const float lengthwise = abs(frac(at.x + shape) - 0.5) * 2.0;
        const float a = abs((at.y - base.y) - centre) / width;
        const float aa = min(d.pixel / (cellMetres * width), 0.6);
        cover = max(cover, (1.0 - smoothstep(0.5 - aa, 1.0 + aa, a)) * (1.0 - smoothstep(0.6, 1.0, lengthwise)));
    }
    const float resolved = 1.0 - smoothstep(0.6, 2.0, d.pixel / (cellMetres * 0.15));
    cover = lerp(size.y * 0.15, cover, resolved);
    const float4 cluster = biomeRow(row, 2);
    const float4 fade = biomeRow(row, 4);
    biomeLayDecal(o, look.rgb, saturate(cover * look.a * gate), fade.z, cluster.z, cluster.w);
}

// One decal by family. Only the families the config uses are compiled in.
void biomeDecal(int kind, int row, float weight, BiomeDecalIn d, inout BiomeDecalOut o)
{
    if (row < 0 || weight <= 0.0) return;
    [branch] switch (kind) {
#ifdef BIOME_DECAL_SPECKLE
    case 0: biomeSpeckle(row, weight, d, o); break;
#endif
#ifdef BIOME_DECAL_STAIN
    case 1: biomeStain(row, weight, d, o); break;
#endif
#ifdef BIOME_DECAL_STREAK
    case 2: biomeStreak(row, weight, d, o); break;
#endif
    default: break;
    }
}

// --- ground cover ---------------------------------------------------------

// What a category does to the grass and a forest biome to the floor under its
// canopy (foliage.json rows): whether anything grows, and how.
struct BiomeCover {
    bool none;          // bare: no ground cover at all
    float density;      // times the climate's grass
    float height;       // times the card's own
    float dryness;      // 0 the climate's, 1 parched
    float flowers;      // times the meadow's flowers
    float3 tint;
    float under;        // the floor under the canopy: times the engine's understory
};
BiomeCover noBiomeCover()
{
    BiomeCover c;
    c.none = false; c.density = 1.0; c.height = 1.0; c.dryness = 0.0; c.flowers = 1.0; c.tint = 1.0; c.under = 1.0;
    return c;
}
// One of the pair, by a hash fixed to the ground: grass does not blend, a
// border of two covers is a ragged mix of the two.
BiomeCover biomeCoverAt(float2 p, float pixel, float pick)
{
    BiomeCover c = noBiomeCover();
#ifdef BIOMES_ENABLED
    const BiomeHere h = biomeHereAt(p, pixel);
    if (!h.any) return c;
    const int category = pick < h.groundShare ? h.groundB : h.groundA;
    const float which = biomeRow(BIOME_ROW_CATEGORY + category, 9).w;
    if (which < -0.5) { c.none = true; return c; }
    if (which > 0.5) {
        const int row = BIOME_ROW_FOLIAGE + int(which + 0.5) - 1;
        const float4 a = biomeRow(row, 0);
        c.density = a.x; c.height = a.y; c.dryness = a.z; c.flowers = a.w;
        c.tint = biomeRow(row, 1).rgb;
    }
    // The forest's own floor: none, the engine's, or a cover's density.
    if (h.forest > 0) {
        const float under = biomeRow(BIOME_ROW_FOREST + h.forest, 2).x;
        c.under = under < -0.5 ? 0.0 : under > 0.5 ? biomeRow(BIOME_ROW_FOLIAGE + int(under + 0.5) - 1, 0).x : 1.0;
    }
#endif
    return c;
}

// --- water ----------------------------------------------------------------

// A kind of water over the engine's own: colour (with how much it takes),
// turbidity (negative: the engine's), scum, foam, glow.
struct BiomeWater {
    float3 colour;
    float colourShare;
    float turbidity;
    float scum, foam, emissive;
};
BiomeWater biomeWaterOf(int id)
{
    BiomeWater w;
    w.colour = 0.0; w.colourShare = 0.0; w.turbidity = -1.0; w.scum = 0.0; w.foam = 1.0; w.emissive = 0.0;
    if (id <= 0) return w;
    const float4 a = biomeRow(BIOME_ROW_WATER + id, 0);
    const float4 b = biomeRow(BIOME_ROW_WATER + id, 1);
    w.colour = a.rgb; w.colourShare = a.a;
    w.turbidity = b.x; w.scum = b.y; w.foam = b.z; w.emissive = b.w;
    return w;
}
BiomeWater biomeWaterMix(BiomeWater a, BiomeWater b, float t)
{
    BiomeWater w;
    w.colour = lerp(a.colour * a.colourShare, b.colour * b.colourShare, t) /
               max(lerp(a.colourShare, b.colourShare, t), 1e-4);
    w.colourShare = lerp(a.colourShare, b.colourShare, t);
    // A turbidity of "the engine's own" mixes as the engine's own.
    w.turbidity = a.turbidity < 0.0 && b.turbidity < 0.0 ? -1.0
                  : lerp(a.turbidity < 0.0 ? 0.0 : a.turbidity, b.turbidity < 0.0 ? 0.0 : b.turbidity, t);
    w.scum = lerp(a.scum, b.scum, t);
    w.foam = lerp(a.foam, b.foam, t);
    w.emissive = lerp(a.emissive, b.emissive, t);
    return w;
}

#endif
