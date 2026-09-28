// Shared scalar C++/HLSL meadow field: workers and distant shading must agree.
#ifndef FOLIAGE_FIELD_HLSLI
#define FOLIAGE_FIELD_HLSLI
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace world::foliage {
using uint = std::uint32_t;
using std::floor;
inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float smoothstep(float a, float b, float x) {
    const float t = saturate((x - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}
#endif

inline float foliageHash(uint x, uint y)
{
    uint h = x * 1597334677u ^ y * 3812015801u;
    h = (h ^ (h >> 16u)) * 2246822519u;
    h = (h ^ (h >> 13u)) * 3266489917u;
    return float((h ^ (h >> 16u)) & 0x00ffffffu) / 16777215.0;
}

inline float foliageNoise(float x, float y)
{
    const int ix = int(floor(x)), iy = int(floor(y));
    float u = x - float(ix), v = y - float(iy);
    u = u * u * (3.0 - 2.0 * u);
    v = v * v * (3.0 - 2.0 * v);
    const float a = foliageHash(uint(ix), uint(iy));
    const float b = foliageHash(uint(ix + 1), uint(iy));
    const float c = foliageHash(uint(ix), uint(iy + 1));
    const float d = foliageHash(uint(ix + 1), uint(iy + 1));
    return (a + (b - a) * u) * (1.0 - v) + (c + (d - c) * u) * v;
}

inline float foliageField(float x, float y, float footprint)
{
    // Incommensurate scales: no single grid of identical tufts. Unresolved
    // octaves tend to their mean, not an aliased pattern or a different seed.
    const float fine = (foliageNoise(x / 19.0, y / 19.0) - 0.5) *
                       (1.0 - smoothstep(4.0, 19.0, footprint));
    const float medium = (foliageNoise(x / 73.0 + 9.7, y / 73.0 - 3.1) - 0.5) *
                         (1.0 - smoothstep(18.0, 73.0, footprint));
    const float broad = (foliageNoise(x / 271.0 - 5.3, y / 271.0 + 17.2) - 0.5) *
                        (1.0 - smoothstep(68.0, 271.0, footprint));
    // And a fourth, most of a kilometre across, which is what makes one
    // hillside thick and the next one thin. Without it every octave is smaller
    // than a glance and the country reads as evenly grassed everywhere the
    // climate allows grass at all.
    const float country = (foliageNoise(x / 940.0 + 41.3, y / 940.0 - 27.9) - 0.5) *
                          (1.0 - smoothstep(240.0, 940.0, footprint));
    return 0.5 + fine * 0.22 + medium * 0.38 + broad * 0.34 + country * 0.52;
}

// How well this ground suits vegetation.
//
// Requirement 12 lists what it should be made of: biome, moisture,
// temperature, soil, slope, height, sunlight, rockiness, water proximity and
// disturbance. Cover weights carry biome and soil, and slope, rockiness and
// water proximity were already here; `moisture` and `hollow` add the two that
// decide where plants actually are within one biome, which is the point of the
// requirement - a hillside is not uniformly grassy, it is green in the damp
// folds and thin on the dry shoulders.
//
// `hollow` is the concavity of the ground, nought to one. It stands in for
// water that has run somewhere and stopped: the same term that gives a dell
// its soil in the erosion pass gives it its plants here, so the two agree
// rather than each having a private idea of where the wet ground is.
//
// Temperature is deliberately not taken. It reaches the terrain shader
// remapped by the weather and the CPU raw, so the same number means two
// different things on the two sides, and vegetation that disagreed between
// the near cards and the far meadow would draw a line across the field.
// Disturbance has no field yet (requirement 16).
inline float foliageSuitability(float grass, float rock, float shore, float upright,
                                float depth, float moisture, float hollow)
{
    const float dry = 1.0 - smoothstep(0.0, 0.08, depth);
    // Plants follow the water, but dry country is not bare - it is thin.
    const float damp = 0.40 + 0.60 * smoothstep(0.12, 0.58, moisture + hollow * 0.30);
    // The grass-rock ecotone. Where the two are comparable the ground is broken
    // and stony between the tufts rather than either a sward or a slab, so it
    // carries less than the cover weights alone would suggest. One of the
    // transitional zones the requirement asks to be supported.
    const float ecotone = 1.0 - 0.45 * saturate(grass * rock * 4.0);
    return grass * grass * (1.0 - rock) * (1.0 - rock) *
           (1.0 - saturate(shore * 2.0)) * saturate((upright - 0.55) / 0.30) * dry *
           damp * ecotone;
}

inline float foliageDensity(float suitability, float field)
{
    // The field decides WHERE, the suitability decides how much.
    //
    // It used to be a gain of between 0.55 and 1.45 applied before a saturate,
    // and on any ground that suits grass at all the product was over one
    // whatever the field said. So the patchiness was clipped off exactly where
    // there was grass to be patchy - the country came out as an even carpet
    // wherever the climate allowed one, and the field only showed at the dying
    // edges where nothing grew anyway.
    //
    // Thresholded instead: below a fifth of the field there is bare ground,
    // above three fifths there is as much as the ground can carry, and between
    // them is the edge of a clump. Clumps and bare patches both exist now, at
    // all four of the field's scales.
    // Value-noise octaves summed stay close to their mean (about +-0.1), so
    // the old 0.2..0.6 threshold read as an even carpet with thin edges. The
    // field is stretched first: thick islands, bare gaps and wide meadows
    // where the kilometre octave is high.
    const float spread = saturate(0.5 + (field - 0.5) * 2.6);
    return saturate(suitability * 2.6) * smoothstep(0.28, 0.66, spread);
}

struct FoliageCommunity {
    float red, green, blue;
    float density, height;
};

inline FoliageCommunity foliageCommunity(float steppe, float boreal, float temperate,
                                        float tropical, float field)
{
    FoliageCommunity result;
    const float sum = steppe + boreal + temperate + tropical;
    const float divisor = sum > 0.0001 ? sum : 0.0001;
    // Colours are plant reflectance, not light baked into the terrain.
    result.red = (steppe * (0.52 + field * 0.19) + boreal * (0.20 + field * 0.11) +
               temperate * (0.33 + field * 0.14) + tropical * (0.16 + field * 0.13)) / divisor;
    result.green = (steppe * (0.45 + field * 0.19) + boreal * (0.34 + field * 0.13) +
                 temperate * (0.55 + field * 0.16) + tropical * (0.48 + field * 0.20)) / divisor;
    result.blue = (steppe * (0.20 + field * 0.10) + boreal * (0.27 + field * 0.07) +
                temperate * (0.14 + field * 0.10) + tropical * (0.18 + field * 0.11)) / divisor;
    // Keep the unnormalised sum here: ice has zero cover, tundra/desert sparse.
    result.density = steppe * 0.75 + boreal * 0.65 + temperate + tropical * 1.25;
    result.height = (steppe * 0.78 + boreal * 0.65 + temperate + tropical * 1.15) / divisor;
    return result;
}

// Persistent surface cover, not a replacement selected when cards disappear.
// Scalars keep the habitat/palette contract executable in CPU and GPU tests.
struct VegetationCover {
    float red, green, blue;
    float ground, grass, canopy;
};
inline VegetationCover vegetationCover(float grass, float soil, float sand, float rock,
        float marsh, float snow, float steppe, float boreal, float temperate, float tropical,
        float moisture, float height, float depth, float upright, float forestField, float field)
{
    const FoliageCommunity community = foliageCommunity(steppe,boreal,temperate,tropical,field);
    const float forest = saturate(boreal+temperate+tropical);
    const float dry = 1.0-smoothstep(-0.4,0.08,depth);
    const float alpine = 1.0-smoothstep(1800.0,2400.0,height);
    const float protectedCover = (1.0-saturate(sand+marsh+snow))*(1.0-saturate(rock));
    const float living = saturate(grass+soil*forest*0.92)*protectedCover*dry*alpine;
    VegetationCover result;
    // Atlas forestField already contains independent woodland habitat.
    result.canopy = saturate(forestField)*living;
    result.red = community.red; result.green = community.green; result.blue = community.blue;
    // No geometric normal in ground colour: coarsening a slope cannot repaint it.
    result.ground = saturate(living*community.density*1.15)*0.90;
    const float support = saturate(grass+soil*forest*0.85);
    result.grass = foliageDensity(foliageSuitability(support,rock,sand+marsh,upright,depth,
        moisture,0.0)*community.density*alpine*(1.0-saturate(snow)),field);
    // Shade-tolerant understory remains in the forest, denser in its gaps.
    result.grass *= 1.0-result.canopy*0.45;
    return result;
}
inline float vegetationGroundChannel(float substrate, float palette, float luminance,
                                      float cover, float canopy)
{
    // Retain photographed texture/contrast, but share plant reflectance with cards.
    const float grain = 0.65+0.70*saturate((luminance-0.12)/0.50);
    const float target = palette*(0.82-0.12*canopy)*grain;
    return substrate+(target-substrate)*cover;
}

inline int foliageCommunityVariant(float steppe, float boreal, float temperate,
                                    float tropical, uint random)
{
    // Layer indices into the six explicitly permitted wild-grass sprites.
    // Choosing a community probabilistically mixes forms through an ecotone.
    const float pick = float((random >> 8u) & 65535u) / 65536.0 *
                       (steppe + boreal + temperate + tropical);
    const uint slot = random % 3u;
    if (pick < steppe) return slot == 0u ? 0 : slot == 1u ? 1 : 4;
    if (pick < steppe + boreal) return slot == 0u ? 1 : slot == 1u ? 3 : 5;
    if (pick < steppe + boreal + temperate) return int(random % 6u);
    return slot == 0u ? 2 : slot == 1u ? 4 : 5;
}

#ifdef __cplusplus
} // namespace world::foliage
#endif
#endif

