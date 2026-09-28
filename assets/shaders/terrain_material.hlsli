#ifndef TERRAIN_MATERIAL_HLSLI
#define TERRAIN_MATERIAL_HLSLI

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

MaterialBand sampleMaterialBand(int layer, float2 uv, float2 dx, float2 dy, float blend)
{
    MaterialBand s;
    // Constant offsets, continuous world-space weights: no floor-cell seams
    // and no derivative of a random offset polluting mip selection.
    const float2 a = uv + float2(0.17, 0.63);
    const float2 b = uv + float2(0.71, 0.29);
    // Variance-preserving mix of the two copies. A plain lerp at 50/50 is a
    // double exposure of one scan over a shifted copy of itself: half the
    // contrast, which is most of what read as a soapy, blurred ground close
    // up. Rescaling the deviation from the mean by 1/sqrt(w^2+(1-w)^2)
    // keeps the grain at full strength through the whole blend - where the
    // grain is resolved. Minified, the samples are already averages and
    // rescaling them would bring unresolved grain back, so it fades out.
    uint bandWidth, bandHeight, bandLayers, bandLevels;
    groundTex.GetDimensions(0, bandWidth, bandHeight, bandLayers, bandLevels);
    const float resolved = 1.0 - smoothstep(0.75, 2.0, max(length(dx), length(dy)) * float(bandWidth));
    const float k = lerp(1.0, rsqrt(blend * blend + (1.0 - blend) * (1.0 - blend)), resolved);
    const float3 mean = groundTex.SampleLevel(groundSampler, float3(0.5, 0.5, layer), 16).rgb;
    const float3 colour = lerp(groundTex.SampleGrad(groundSampler, float3(a, layer), dx, dy).rgb,
                               groundTex.SampleGrad(groundSampler, float3(b, layer), dx, dy).rgb, blend);
    s.colour = max(mean + (colour - mean) * k, 0.0);
    s.properties = lerp(groundPropertiesTex.SampleGrad(groundPropertiesSampler, float3(a, layer), dx, dy),
                        groundPropertiesTex.SampleGrad(groundPropertiesSampler, float3(b, layer), dx, dy), blend);
    s.normal = lerp(groundNormalTex.SampleGrad(groundNormalSampler, float3(a, layer), dx, dy).xyz,
                    groundNormalTex.SampleGrad(groundNormalSampler, float3(b, layer), dx, dy).xyz, blend) * 2.0 - 1.0;
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

MaterialSample materialProjection(int layer, float2 uv, float2 dx, float2 dy,
                                  float blend, float normalStrength)
{
    // Fixed physical detail plus LOW-frequency macro colour, not three copies
    // of the same microstructure. Camera zoom never changes texture scale.
    const float fineScale = 2.0, middleScale = 16.0, broadScale = 96.0;
    MaterialBand fine = sampleMaterialBand(layer,
        uv / fineScale, dx / fineScale, dy / fineScale, blend);
    // Texture scale follows distance, growing with it: close up the scan at
    // its photographed size (every texel real detail); once the fine band is
    // well minified (past ~10 texels a pixel, where its own grain is already
    // averaged away) a copy at four times the size takes over, so a hillside
    // is not the same two-metre photograph printed ten thousand times. The
    // hand-over starts where both are unresolved grain, so neither brings
    // grain back or swims with zoom.
    uint texWidth, texHeight, texLayers, texLevels;
    groundTex.GetDimensions(0, texWidth, texHeight, texLayers, texLevels);
    const float texelsPerPixel = max(length(dx), length(dy)) / fineScale * float(texWidth);
    const float far = smoothstep(10.0, 40.0, texelsPerPixel);
    [branch] if (far > 0.01) {
        const float wide = fineScale * 4.0;
        const MaterialBand w = sampleMaterialBand(layer, uv / wide + float2(0.29, 0.83),
            dx / wide, dy / wide, blend);
        fine.colour = lerp(fine.colour, w.colour, far * 0.7);
        fine.properties = lerp(fine.properties, w.properties, far * 0.7);
        fine.normal = lerp(fine.normal, w.normal, far * 0.7);
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
    s.colour = fine.colour * (1.0 + contrastStrength *
        clamp(middleLight * 0.35 + broadLight * broadContrast, -0.12, 0.12));
    s.properties = fine.properties;
    s.properties.rg = saturate(fine.properties.rg + (middle.properties - meanProperties) * 0.20 +
                              (broad.properties - meanProperties) * 0.10);
    // Fixed world-space height for material identity. A mip chosen by the
    // camera or a coarse shading band must NEVER decide grass versus rock.
    const float2 a = uv + float2(0.17, 0.63), b = uv + float2(0.71, 0.29);
    s.borderHeight = lerp(groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(a, layer), 6).b,
                          groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(b, layer), 6).b, blend);
    const float footprint = max(length(dx), length(dy));
    const float fineDetail = 1.0 - smoothstep(0.06, 0.24, footprint / fineScale);
    // Enlarging a normal map while keeping its slopes invents giant bumps.
    // Terrain geometry owns large-scale relief; the scan owns close-up detail.
    s.normal = quietNormal(fine.normal, normalStrength * 0.65 * fineDetail);
    return s;
}

// Texture array layers. 0..5 are the six ground classes the world blends
// (grass, dirt, sand, rock, marsh, snow); the rest are variants of those
// classes, chosen per pixel by climate, water and slope (terrain.hlsl).
// Metres one texture turn covers, from content/config/terrain_materials.json:
// a variant keeps its own physical scale relative to its class.
#define GROUND_LAYERS 16
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
// Classes 0..5 must equal content/config/ground.json metres_per_turn.
static const float kLayerMetres[GROUND_LAYERS] = {
    2.0, 2.07, 3.0, 3.0, 1.3, 2.0,
    2.0, 3.0, 3.94, 2.0, 1.5, 2.53, 2.7, 3.0, 1.83, 2.0};

// One layer of the array, laid at its class's (editable) scale adjusted by
// the layer's own physical size, with the class's normal strength.
MaterialSample sampleGroundLayer(int layer, int cls, float3 p, float3 normal,
                                 float3 dx, float3 dy)
{
    const float frequency = tablePS[cls].x / 14.0 * kLayerMetres[cls] / kLayerMetres[layer];
    float3 weights = pow(abs(normal), 4.0);
    // Cull negligible projections smoothly, then renormalize; flat land costs
    // one projection, not three. Gradients are computed before this branch.
    weights = max(weights - 0.01, 0.0);
    weights /= max(dot(weights, float3(1, 1, 1)), 1e-5);
    const float blend = smoothstep(0.15, 0.85, noiseAt(p.xy / 9.0 + float(layer) * 17.0));
    const float strengths[6] = {0.30, 0.34, 0.24, 0.46, 0.28, 0.20};
    const float strength = layer >= LAYER_CLIFF_DOLOMITE && layer <= LAYER_CLIFF_DESERT ? 0.55 : strengths[cls];
    MaterialSample result = (MaterialSample)0;
    [unroll] for (int axis = 0; axis < 3; ++axis) {
        float2 uv, gx, gy;
        if (axis == 0) {
            const float s = normal.x < 0.0 ? -1.0 : 1.0;
            uv = float2(p.y, p.z * s); gx = float2(dx.y, dx.z * s); gy = float2(dy.y, dy.z * s);
        } else if (axis == 1) {
            const float s = normal.y < 0.0 ? -1.0 : 1.0;
            uv = float2(p.x, -p.z * s); gx = float2(dx.x, -dx.z * s); gy = float2(dy.x, -dy.z * s);
        } else {
            const float s = normal.z < 0.0 ? -1.0 : 1.0;
            uv = float2(p.x, p.y * s); gx = float2(dx.x, dx.y * s); gy = float2(dy.x, dy.y * s);
        }
        [branch] if (weights[axis] > 0.0) {
            const MaterialSample s = materialProjection(layer, uv * frequency,
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

MaterialSample sampleGroundMaterial(int layer, float3 p, float3 normal,
                                     float3 dx, float3 dy)
{
    return sampleGroundLayer(layer, layer, p, normal, dx, dy);
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

