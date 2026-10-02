// The categories' choices, generated from content/config/terrain
// (engine/biomes, terrain_biomes.gen.hlsli), and what is done with them.
// Included after GroundVariants and GroundContext are known. Without the
// generated file every category draws as the default, which is the engine's
// own ground.
#ifndef TERRAIN_BIOMES_CODE_HLSLI
#define TERRAIN_BIOMES_CODE_HLSLI
#if __has_include("terrain_biomes.gen.hlsli")
#include "terrain_biomes.gen.hlsli"
#else
bool biomeCategoryChangesGround(int category) { return false; }
bool biomeClassSoil(int category, int cls, out BiomeSoil soil) { soil = noBiomeSoil(); return false; }
bool biomeCategorySlope(int category, out BiomeSlope slope) { slope = noBiomeSlope(); return false; }
int biomeForestFloor(int forest) { return -1; }
bool biomeCategoryDecalSet(int category, out int4 rows, out int4 kinds) { rows = -1; kinds = -1; return false; }
bool biomeDecorDecalSet(int decor, out int4 rows, out int4 kinds) { rows = -1; kinds = -1; return false; }
#endif

// A soil laid into a class's variants: its first layer the base, the second
// and third cut from its noise by their shares. One noise read for both, the
// third at another place of the same noise.
void biomeApplySoil(BiomeSoil s, float2 xy, float footprint, inout GroundVariants v)
{
    v.base = s.l0; v.first = s.l1; v.second = s.l2; v.s1 = 0.0; v.s2 = 0.0;
    if (s.layers < 2) return;
    const float4 n = biomeRow(s.row, 0);
    const float4 share = biomeRow(s.row, 1);
    [loop] for (int k = 1; k < s.layers; ++k) {
        const float2 at = k == 1 ? xy : xy * 0.83 + float2(37.1, -91.7);
        const float m = biomeShareMask(biomeNoise(s.kind, at, n, footprint), k == 1 ? share.y : share.z, n.y);
        if (k == 1) v.s1 = m; else v.s2 = m;
    }
}

// The category's soils and slopes over the engine's variants of a class.
// False: the class is the engine's own in this category.
bool biomeClassVariants(int category, int cls, GroundContext c, float2 xy, float footprint, inout GroundVariants v)
{
    if (cls != 3) {
        BiomeSoil soil;
        if (!biomeClassSoil(category, cls, soil)) return false;
        biomeApplySoil(soil, xy, footprint, v);
        return true;
    }
    BiomeSlope slope;
    if (!biomeCategorySlope(category, slope)) return false;
    // The rock class: its ground between the faces (the soil's first layer,
    // its second in the second slot), the face over it on the steep, scree
    // at the foot where the second slot is free.
    bool secondTaken = false;
    if (slope.soil.row >= 0) {
        biomeApplySoil(slope.soil, xy, footprint, v);
        v.second = v.first; v.s2 = v.s1;
        secondTaken = slope.soil.layers >= 2;
    } else {
        v.base = 3; v.second = 3; v.s2 = 0.0;
    }
    const float4 f = biomeRow(BIOME_ROW_CATEGORY + category, 1);
    if (slope.face >= 0) { v.first = slope.face; v.s1 = smoothstep(f.x - 0.08, f.x + 0.08, c.steepness); }
    else { v.first = v.base; v.s1 = 0.0; }
    if (slope.scree >= 0 && !secondTaken) { v.second = slope.scree; v.s2 = biomeScree(c.steepness, f.x, xy); }
    return true;
}

// What a category does to a class's colour once it is sampled: its soil's
// tint, and on the rock class the rock's tint and strata on its faces.
void biomeClassFinish(int category, int cls, GroundContext c, float3 p, inout float3 colour)
{
    colour *= biomeRow(BIOME_ROW_CATEGORY + category, 3 + cls).rgb;
    if (cls != 3) return;
    BiomeSlope slope;
    if (!biomeCategorySlope(category, slope) || slope.rockRow < 0) return;
    const float4 f = biomeRow(BIOME_ROW_CATEGORY + category, 1);
    const float face = smoothstep(f.x - 0.08, f.x + 0.08, c.steepness);
    colour *= lerp(float3(1, 1, 1), biomeRow(slope.rockRow, 0).rgb, face);
#ifdef BIOME_ANY_STRATA
    if (slope.strata) colour *= lerp(1.0, biomeStrata(p, biomeRow(slope.rockRow, 1)), face);
#endif
}

// The decals of the category pair and the decor biome, one loop over every
// set so each family is one copy of code.
void biomeDecalsHere(BiomeHere h, BiomeDecalIn d, inout BiomeDecalOut o)
{
    [loop] for (int set = 0; set < 3; ++set) {
        int4 rows, kinds;
        float4 weights;
        float share;
        if (set == 0) {
            if (!biomeCategoryDecalSet(h.groundA, rows, kinds)) continue;
            weights = biomeRow(BIOME_ROW_CATEGORY + h.groundA, 10);
            share = 1.0 - h.groundShare;
        } else if (set == 1) {
            if (h.groundShare <= 0.004 || h.groundB == h.groundA || !biomeCategoryDecalSet(h.groundB, rows, kinds)) continue;
            weights = biomeRow(BIOME_ROW_CATEGORY + h.groundB, 10);
            share = h.groundShare;
        } else {
            if (!biomeDecorDecalSet(h.decor, rows, kinds)) continue;
            weights = biomeRow(BIOME_ROW_DECOR + h.decor, 0);
            share = 1.0;
        }
        [loop] for (int k = 0; k < 4; ++k)
            biomeDecal(kinds[k], rows[k], weights[k] * share, d, o);
    }
}
#endif
