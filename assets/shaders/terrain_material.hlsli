#ifndef TERRAIN_MATERIAL_HLSLI
#define TERRAIN_MATERIAL_HLSLI

#include "world_frame.hlsli"

// A point of the ground as the material reads it (world_frame.hlsli): an exact
// frame origin and the metres from it. `local.z` is the height itself - heights
// are small enough for a float to hold to a fraction of a millimetre.
struct GroundFrame {
    float2 anchor;
    float3 local;
};

GroundFrame groundFrame(float2 worldXY, float2 frameXY, float height)
{
    GroundFrame f;
    f.anchor = frameAnchorFrom(worldXY, frameXY);
    f.local = float3(frameXY, height);
    return f;
}

// A world position taken as it is (frame origin nought): what callers without
// an interpolated offset - probes, tests - always had.
GroundFrame groundFrameOfWorld(float3 world)
{
    GroundFrame f;
    f.anchor = float2(0.0, 0.0);
    f.local = world;
    return f;
}

// The world position again, for smooth fields and metre-scale noise only.
float3 groundFrameWorld(GroundFrame f)
{
    return float3(f.anchor + f.local.xy, f.local.z);
}

// All maps describe the same surface. Never jitter colour, properties and
// normals differently, or let camera distance change their mixture.
float3 rnmBlend(float3 baseTS, float3 detailTS)
{
    const float3 t = normalize(baseTS) + float3(0.0, 0.0, 1.0);
    const float3 u = normalize(detailTS) * float3(-1.0, -1.0, 1.0);
    return normalize(t * (dot(t, u) / max(t.z, 1e-5)) - u);
}

float3 quietNormal(float3 n, float strength)
{
    // Normalize AFTER filtering. Short vectors in the mip chain are valid
    // averages, not stronger bumps that need their XY divided by a small Z.
    return normalize(float3(n.xy * strength, max(n.z, 0.2)));
}

float3 projectionNormal(float3 surface, float3 detail, int axis)
{
    // Signed right-handed frames; flat normal maps reproduce the geometric
    // normal on every projection, including negative-facing cliff walls.
    float3 tangent, bitangent, up;
    if (axis == 0) {
        const float s = surface.x < 0.0 ? -1.0 : 1.0;
        tangent = float3(0, 1, 0); bitangent = float3(0, 0, s); up = float3(s, 0, 0);
    } else if (axis == 1) {
        const float s = surface.y < 0.0 ? -1.0 : 1.0;
        tangent = float3(1, 0, 0); bitangent = float3(0, 0, -s); up = float3(0, s, 0);
    } else {
        const float s = surface.z < 0.0 ? -1.0 : 1.0;
        tangent = float3(1, 0, 0); bitangent = float3(0, s, 0); up = float3(0, 0, s);
    }
    const float3 base = float3(dot(surface, tangent), dot(surface, bitangent), dot(surface, up));
    const float3 n = rnmBlend(base, detail);
    return tangent * n.x + bitangent * n.y + up * n.z;
}

struct MaterialSample {
    float3 colour;
    float4 properties; // AO, roughness, height, asset mask (not terrain coverage)
    float3 normal;
    float borderHeight;
};

struct MaterialBand {
    float3 colour;
    float4 properties; // Preserve the physical height and asset mask as well.
    float3 normal;
};

// Hex-tiling (Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 2022): the
// plane cut into a triangle grid, every grid vertex its own copy of the scan
// - shifted and turned at random - and each point the blend of the three
// copies at the corners of its triangle, weighted towards the nearest and
// towards the brighter detail so the hand-over runs along the scan's own
// features instead of fading two pictures into a mush.
//
// It replaces the two fixed copies crossfaded by a 64 m Perlin field, which
// showed close up as the noise's own pattern and from mid range as the same
// two-metre photograph repeated: here no two neighbouring cells of the grid
// show the same piece of the scan the same way round.
static const float kHexCells = 1.6;        // grid vertices across one turn of the scan
static const float kHexRotation = 1.0;     // how much each copy may turn (1 = any angle)
static const float kHexSharpness = 7.0;
static const float kHexContrast = 0.6;

void hexTriangle(float2 st, out float3 w, out int2 v1, out int2 v2, out int2 v3)
{
    st *= 3.4641016 * kHexCells;   // 2 sqrt(3) a turn, times the cells
    const float2 skewed = float2(st.x - 0.57735027 * st.y, 1.15470054 * st.y);
    const int2 base = int2(floor(skewed));
    float3 t = float3(frac(skewed), 0.0);
    t.z = 1.0 - t.x - t.y;
    const float s = step(0.0, -t.z);
    const float s2 = 2.0 * s - 1.0;
    w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    v1 = base + int2(int(s), int(s));
    v2 = base + int2(int(s), 1 - int(s));
    v3 = base + int2(1 - int(s), int(s));
}

float2 hexHash(int2 v, int layer)
{
    uint h = uint(v.x) * 0x8da6b343u ^ uint(v.y) * 0xd8163841u ^ uint(layer) * 0xcb1ab31fu;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return float2(float(h & 0xffffu), float(h >> 16)) / 65535.0;
}

// The turn of a copy, as the matrix that takes texture space to the copy.
float2x2 hexTurn(float2 random)
{
    const float angle = (random.x - 0.5) * 6.2831853 * kHexRotation;
    const float c = cos(angle), s = sin(angle);
    return float2x2(c, -s, s, c);
}

MaterialBand sampleMaterialBand(int layer, float2 uv, float2 dx, float2 dy, float blend)
{
    float3 w;
    int2 v[3];
    hexTriangle(uv, w, v[0], v[1], v[2]);
    float3 colour[3], normal[3];
    float4 properties[3];
    [unroll] for (int k = 0; k < 3; ++k) {
        const float2 random = hexHash(v[k], layer);
        const float2x2 turn = hexTurn(random);
        const float2 at = mul(turn, uv) + random.yx * 7.31;
        const float2 gx = mul(turn, dx), gy = mul(turn, dy);
        colour[k] = groundTex.SampleGrad(groundSampler, float3(at, layer), gx, gy).rgb;
        properties[k] = groundPropertiesTex.SampleGrad(groundPropertiesSampler, float3(at, layer), gx, gy);
        float3 n = groundNormalTex.SampleGrad(groundNormalSampler, float3(at, layer), gx, gy).xyz * 2.0 - 1.0;
        // The normal map's slope is in the copy's frame: turned back to the plane's.
        n.xy = mul(n.xy, turn);
        normal[k] = n;
    }
    // Nearest corner first, the brighter detail through: the scan's own
    // stones and blades decide where one copy gives way to the next.
    const float3 luminance = float3(0.2126, 0.7152, 0.0722);
    const float3 light = float3(dot(colour[0], luminance), dot(colour[1], luminance), dot(colour[2], luminance));
    float3 weights = lerp(1.0, light, kHexContrast) * pow(max(w, 0.0), kHexSharpness);
    weights /= max(dot(weights, float3(1.0, 1.0, 1.0)), 1e-5);
    MaterialBand s;
    s.colour = colour[0] * weights.x + colour[1] * weights.y + colour[2] * weights.z;
    s.properties = properties[0] * weights.x + properties[1] * weights.y + properties[2] * weights.z;
    s.normal = normal[0] * weights.x + normal[1] * weights.y + normal[2] * weights.z;
    return s;
}

struct MaterialMacro {
    float3 colour;
    float2 properties;
};

MaterialMacro sampleMaterialMacro(int layer, float2 uv, float2 dx, float2 dy, float blend)
{
    // An enlarged scan is NOT a macro texture: its pebbles/blades become
    // metre-sized grain. Low-pass in SOURCE UV space before enlarging it.
    // At most sixteen samples across a repeat, independent of source resolution
    // and camera distance. The fine band's ordinary SampleGrad is unchanged.
    const float footprint = max(1.0 / 16.0, max(length(dx), length(dy)));
    uint width, height, layers, levels;
    groundTex.GetDimensions(0, width, height, layers, levels);
    const float colourLod = max(0.0, log2(float(max(width, height)) * footprint));
    groundPropertiesTex.GetDimensions(0, width, height, layers, levels);
    const float propertiesLod = max(0.0, log2(float(max(width, height)) * footprint));
    const float2 a = uv + float2(0.17, 0.63), b = uv + float2(0.71, 0.29);
    MaterialMacro s;
    s.colour = lerp(groundTex.SampleLevel(groundSampler, float3(a, layer), colourLod).rgb,
                    groundTex.SampleLevel(groundSampler, float3(b, layer), colourLod).rgb, blend);
    s.properties = lerp(groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(a, layer), propertiesLod).rg,
                        groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(b, layer), propertiesLod).rg, blend);
    return s;
}

// Turns of a layer after which every band of materialProjection repeats at
// once: the distance octaves 2, 4, 8, 16, 32, middle 16, broad 96. Shifting a
// texture coordinate by a multiple of it changes nothing that is drawn.
static const float kMaterialRepeat = 96.0;
// The coarsest distance octave: the scan at sixteen times its size.
static const int kMaxTilingOctaves = 4;

// The look's knobs (Scene::terrainLook, content/config/terrain_look.json):
// tile size multiplier, distance octaves, the texels a pixel at which the
// next octave starts, macro variation. Unset (x <= 0): the defaults.
struct TerrainLookKnobs { float tile; float octaves; float start; float macro; };
TerrainLookKnobs terrainLookKnobs()
{
    TerrainLookKnobs k;
    const bool given = terrainLookPS.x > 0.0;
    k.tile = given ? terrainLookPS.x : 1.0;
    k.octaves = given ? clamp(terrainLookPS.y, 0.0, float(kMaxTilingOctaves)) : 0.0;
    k.start = given ? max(terrainLookPS.z, 1.0) : 8.0;
    k.macro = given ? max(terrainLookPS.w, 0.0) : 0.55;
    return k;
}

MaterialSample materialProjection(int layer, float2 uv, float2 dx, float2 dy,
                                  float blend, float normalStrength)
{
    // Fixed physical detail plus LOW-frequency macro colour, not three copies
    // of the same microstructure.
    const float fineScale = 2.0, middleScale = 16.0, broadScale = 96.0;
    const TerrainLookKnobs knobs = terrainLookKnobs();
    // Texture scale follows the size a texel takes on the screen. Close up the
    // scan lies at its photographed size, every texel real detail. Once a
    // pixel covers `start` of its texels - its own grain already averaged
    // away - the next octave (the same scan at twice the size) takes over
    // smoothly, and the one after at four times, so the repeat stays a few
    // hundred pixels across at any zoom instead of the same two-metre
    // photograph printed ten thousand times over a hillside. Two octaves at
    // most are read at any pixel, the same as the old fixed far band; the
    // hand-over is where both are unresolved grain, so neither brings grain
    // back.
    uint texWidth, texHeight, texLayers, texLevels;
    groundTex.GetDimensions(0, texWidth, texHeight, texLayers, texLevels);
    const float texelsPerPixel = max(length(dx), length(dy)) / fineScale * float(texWidth);
    const float level = clamp(log2(max(texelsPerPixel, 1e-4) / knobs.start), 0.0, knobs.octaves);
    const float octave = min(floor(level), max(knobs.octaves - 1.0, 0.0));
    const float toNext = knobs.octaves > 0.0 ? smoothstep(0.0, 1.0, level - octave) : 0.0;
    MaterialBand fine = (MaterialBand)0;
    [branch] if (toNext < 0.999) {
        const float scale = fineScale * exp2(octave);
        const float2 shift = frac(octave * float2(0.29, 0.83));
        fine = sampleMaterialBand(layer, uv / scale + shift, dx / scale, dy / scale, blend);
    }
    [branch] if (toNext > 0.001) {
        const float next = octave + 1.0;
        const float scale = fineScale * exp2(next);
        const float2 shift = frac(next * float2(0.29, 0.83));
        const MaterialBand wide = sampleMaterialBand(layer, uv / scale + shift, dx / scale, dy / scale, blend);
        fine.colour = lerp(fine.colour, wide.colour, toNext);
        fine.properties = lerp(fine.properties, wide.properties, toNext);
        fine.normal = lerp(fine.normal, wide.normal, toNext);
    }
    const MaterialMacro middle = sampleMaterialMacro(layer,
        uv / middleScale + float2(0.31, 0.57), dx / middleScale, dy / middleScale, blend);
    const MaterialMacro broad = sampleMaterialMacro(layer,
        uv / broadScale + float2(0.67, 0.11), dx / broadScale, dy / broadScale, blend);

    // The 1x1 mip is the material's mean, not a camera-dependent reference.
    // Coarse bands add bounded luminance contrast, never a second palette.
    const float3 meanColour = groundTex.SampleLevel(groundSampler, float3(0.5, 0.5, layer), 16).rgb;
    const float2 meanProperties = groundPropertiesTex.SampleLevel(groundPropertiesSampler,
        float3(0.5, 0.5, layer), 16).rg;
    const float3 luminance = float3(0.2126, 0.7152, 0.0722);
    const float meanLight = max(dot(meanColour, luminance), 0.06);
    const float middleLight = dot(middle.colour - meanColour, luminance) / meanLight;
    const float broadLight = dot(broad.colour - meanColour, luminance) / meanLight;
    // Snow keeps a clean high-key surface, not enlarged grey patches from the scan.
    const float contrastStrength = layer == 5 ? 0.30 : layer == 4 ? 0.70 : layer == 2 ? 0.75 : 1.0;
    const float broadContrast = layer == 0 ? 0.35 : 0.50;
    MaterialSample s;
    const float variation = knobs.macro;
    // Broad light/dark AND a little of the scan's own hue drift, so a far
    // hillside is not one tile's colours repeated: the bands carry what the
    // fine copies cannot. Chroma is bounded and mean-preserving.
    const float3 drift = (middle.colour - meanColour) * 0.20 + (broad.colour - meanColour) * 0.25;
    s.colour = fine.colour * (1.0 + contrastStrength * variation *
        clamp(middleLight * 0.35 + broadLight * broadContrast, -0.20, 0.20));
    s.colour = max(s.colour + drift * contrastStrength * variation * 0.5, 0.0);
    s.properties = fine.properties;
    s.properties.rg = saturate(fine.properties.rg + ((middle.properties - meanProperties) * 0.20 +
                              (broad.properties - meanProperties) * 0.10) * variation);
    // Fixed world-space height for material identity. A mip chosen by the
    // camera or a coarse shading band must NEVER decide grass versus rock.
    const float2 a = uv + float2(0.17, 0.63), b = uv + float2(0.71, 0.29);
    s.borderHeight = lerp(groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(a, layer), 6).b,
                          groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(b, layer), 6).b, blend);
    const float footprint = max(length(dx), length(dy));
    // The scan's normals only where their bumps are a few pixels across: a
    // leaf-litter normal map minified to a pixel sparkles in the sun (every
    // texel a different facet) - the dotted ripple on a meadow at mid range.
    const float fineDetail = 1.0 - smoothstep(0.03, 0.14, footprint / fineScale);
    // Enlarging a normal map while keeping its slopes invents giant bumps.
    // Terrain geometry owns large-scale relief; the scan owns close-up detail.
    s.normal = quietNormal(fine.normal, normalStrength * 0.40 * fineDetail);
    return s;
}

// Texture array layers. 0..5 are the six ground classes the world blends
// (grass, dirt, sand, rock, marsh, snow); the rest are variants of those
// classes, chosen per pixel by climate, water and slope (terrain.hlsl).
// Metres one texture turn covers, from content/config/terrain_materials.json:
// a variant keeps its own physical scale relative to its class.
#include "terrain_layers.hlsli"   // GROUND_LAYERS, kLayerMetres
#define LAYER_SAND_DUNE_ORANGE 2   // the class's own scan: dunes only
#define LAYER_GRASS_DRY 6
#define LAYER_FOREST_FLOOR 7
#define LAYER_SAND_COAST 8
#define LAYER_SAND_WET 9
#define LAYER_SAND_PLAIN 10
#define LAYER_SAND_GRAVELLY 11
#define LAYER_CLIFF_DOLOMITE 12
#define LAYER_CLIFF_MOSSY 13
#define LAYER_CLIFF_DESERT 14
#define LAYER_MUD_CRACKED 15
// Metres a texture turn covers: the photographed patch itself, so with the
// fine band repeating every two turns (materialProjection's fineScale) one
// repeat covers twice the scanned ground. At the scans' own size a 2 m repeat
// was plainly visible as a grid close up; the scan's detail holds at 2x.
// Classes 0..5 must equal content/config/ground.json metres_per_turn. Kept in
// content/config/terrain/layers.json (terrain_layers.hlsli).

// One layer of the array, laid at its class's (editable) scale adjusted by
// the layer's own physical size, with the class's normal strength.
//
// The texture coordinate is the frame's exact start (world_frame.hlsli) plus
// the exact offset from it, never the world coordinate times the scale: far
// from the origin that product holds the ground in 1.6 cm steps and the scan
// came out as stripes of one colour. dx/dy are derivatives of `at.local`.
MaterialSample sampleGroundLayer(int layer, int cls, GroundFrame at, float3 normal,
                                 float3 dx, float3 dy)
{
    const float frequency = tablePS[cls].x / 14.0 * kLayerMetres[cls] / kLayerMetres[layer] /
                            terrainLookKnobs().tile;
    const float3 p = groundFrameWorld(at);
    float3 weights = pow(abs(normal), 4.0);
    // Cull negligible projections smoothly, then renormalize; flat land costs
    // one projection, not three. Gradients are computed before this branch.
    weights = max(weights - 0.01, 0.0);
    weights /= max(dot(weights, float3(1, 1, 1)), 1e-5);
    // Which of the two offset copies, as soft organic patches. Value noise on
    // a 9 m lattice, contrast-stretched, put the hand-over on its axes: square
    // patches of one copy against the other all over the ground. Gradient
    // noise turned off the world axes has no lattice to show.
    const float2 turned = float2(dot(p.xy, float2(0.799, 0.602)), dot(p.xy, float2(-0.602, 0.799))) / 64.0 +
                          float(layer) * 17.0;
    const float blend = smoothstep(0.2, 0.8, perlinAt(turned));
    const float strengths[6] = {0.30, 0.34, 0.24, 0.46, 0.28, 0.20};
    const float strength = layer >= LAYER_CLIFF_DOLOMITE && layer <= LAYER_CLIFF_DESERT ? 0.55 : strengths[cls];
    MaterialSample result = (MaterialSample)0;
    const float3 l = at.local;
    [unroll] for (int axis = 0; axis < 3; ++axis) {
        // The projection's coordinate in two parts: the frame origin's share
        // (heights have none) and the offset's.
        float2 uv, origin, gx, gy;
        if (axis == 0) {
            const float s = normal.x < 0.0 ? -1.0 : 1.0;
            uv = float2(l.y, l.z * s); origin = float2(at.anchor.y, 0.0);
            gx = float2(dx.y, dx.z * s); gy = float2(dy.y, dy.z * s);
        } else if (axis == 1) {
            const float s = normal.y < 0.0 ? -1.0 : 1.0;
            uv = float2(l.x, -l.z * s); origin = float2(at.anchor.x, 0.0);
            gx = float2(dx.x, -dx.z * s); gy = float2(dy.x, -dy.z * s);
        } else {
            const float s = normal.z < 0.0 ? -1.0 : 1.0;
            uv = float2(l.x, l.y * s); origin = float2(at.anchor.x, at.anchor.y * s);
            gx = float2(dx.x, dx.y * s); gy = float2(dy.x, dy.y * s);
        }
        [branch] if (weights[axis] > 0.0) {
            const float2 start = frameTurns(origin, frequency, kMaterialRepeat);
            const MaterialSample s = materialProjection(layer, start + uv * frequency,
                gx * frequency, gy * frequency, blend, strength);
            result.colour += s.colour * weights[axis];
            result.properties += s.properties * weights[axis];
            result.borderHeight += s.borderHeight * weights[axis];
            result.normal += projectionNormal(normal, s.normal, axis) * weights[axis];
        }
    }
    result.normal = normalize(result.normal);
    return result;
}

MaterialSample sampleGroundMaterial(int layer, GroundFrame at, float3 normal,
                                     float3 dx, float3 dy)
{
    return sampleGroundLayer(layer, layer, at, normal, dx, dy);
}

// Height only breaks a shared border. It cannot expose the runner-up material
// throughout the interior, even with opposite black/white displacement maps.
float materialBorderMix(float gap, float width, float intrusion,
                         float heightDifference, float border)
{
    const float raised = gap + intrusion + heightDifference * width * 0.28 * border;
    return smoothstep(-width, width, raised);
}

float materialCoverage(int material, int top, int under, float mixAmount)
{
    return (top == material ? mixAmount : 0.0) + (under == material ? 1.0 - mixAmount : 0.0);
}

float stoneSupport(float rock, float grass, float sand, float snow, float marsh)
{
    return smoothstep(0.30, 0.75, rock) *
           (1.0 - smoothstep(0.08, 0.35, max(max(grass, sand), max(snow, marsh))));
}

// Octaves disappear into their mean, not into a different material or UV.
float filteredMaterialNoise(float2 p, float footprint)
{
    float value = 0.0, amplitude = 0.5, total = 0.0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const float resolved = 1.0 - smoothstep(0.25, 0.75, footprint);
        value += lerp(0.5, noiseAt(p), resolved) * amplitude;
        total += amplitude;
        p = p * 2.03 + float2(3.7, 8.1);
        footprint *= 2.03;
        amplitude *= 0.5;
    }
    return value / total;
}
#endif
