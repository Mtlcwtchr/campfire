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
    s.colour = lerp(groundTex.SampleGrad(groundSampler, float3(a, layer), dx, dy).rgb,
                    groundTex.SampleGrad(groundSampler, float3(b, layer), dx, dy).rgb, blend);
    s.properties = lerp(groundPropertiesTex.SampleGrad(groundPropertiesSampler, float3(a, layer), dx, dy),
                        groundPropertiesTex.SampleGrad(groundPropertiesSampler, float3(b, layer), dx, dy), blend);
    s.normal = lerp(groundNormalTex.SampleGrad(groundNormalSampler, float3(a, layer), dx, dy).xyz,
                    groundNormalTex.SampleGrad(groundNormalSampler, float3(b, layer), dx, dy).xyz, blend) * 2.0 - 1.0;
    return s;
}

MaterialSample materialProjection(int layer, float2 uv, float2 dx, float2 dy,
                                  float blend, float normalStrength)
{
    // Three fixed world-space bands. Physical scans alone disappear at an RTS
    // camera height. Do not stretch UVs with zoom or replace one band by another:
    // each band's own gradients/mips remove only the frequencies it cannot show.
    // A 2 m scan gives 4 m surface grain, 32 m terrain texture and 192 m broad detail.
    const float fineScale = 2.0, middleScale = 16.0, broadScale = 96.0;
    const MaterialBand fine = sampleMaterialBand(layer,
        uv / fineScale, dx / fineScale, dy / fineScale, blend);
    const MaterialBand middle = sampleMaterialBand(layer,
        uv / middleScale + float2(0.31, 0.57), dx / middleScale, dy / middleScale, blend);
    const MaterialBand broad = sampleMaterialBand(layer,
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
    // Lush grass has much less broad contrast than exposed soil/stone scans.
    const float broadContrast = layer == 0 ? 1.35 : 1.10;
    MaterialSample s;
    s.colour = fine.colour * (1.0 + contrastStrength *
        clamp(middleLight * 0.85 + broadLight * broadContrast, -0.24, 0.24));
    s.properties = fine.properties;
    s.properties.rg = saturate(fine.properties.rg + (middle.properties.rg - meanProperties) * 0.45 +
                              (broad.properties.rg - meanProperties) * 0.25);
    // Fixed world-space height for material identity. A mip chosen by the
    // camera or a coarse shading band must NEVER decide grass versus rock.
    const float2 a = uv + float2(0.17, 0.63), b = uv + float2(0.71, 0.29);
    s.borderHeight = lerp(groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(a, layer), 6).b,
                          groundPropertiesTex.SampleLevel(groundPropertiesSampler, float3(b, layer), 6).b, blend);
    const float footprint = max(length(dx), length(dy));
    const float fineDetail = 1.0 - smoothstep(0.06, 0.24, footprint / fineScale);
    const float middleDetail = 1.0 - smoothstep(0.06, 0.24, footprint / middleScale);
    const float broadDetail = 1.0 - smoothstep(0.06, 0.24, footprint / broadScale);
    s.normal = rnmBlend(quietNormal(broad.normal, normalStrength * 0.65 * broadDetail),
                        quietNormal(middle.normal, normalStrength * 0.85 * middleDetail));
    s.normal = rnmBlend(s.normal, quietNormal(fine.normal, normalStrength * 0.65 * fineDetail));
    return s;
}

MaterialSample sampleGroundMaterial(int layer, float3 p, float3 normal,
                                     float3 dx, float3 dy)
{
    const float frequency = tablePS[layer].x / 14.0;
    float3 weights = pow(abs(normal), 4.0);
    // Cull negligible projections smoothly, then renormalize; flat land costs
    // one projection, not three. Gradients are computed before this branch.
    weights = max(weights - 0.01, 0.0);
    weights /= max(dot(weights, float3(1, 1, 1)), 1e-5);
    const float blend = smoothstep(0.15, 0.85, noiseAt(p.xy / 9.0 + float(layer) * 17.0));
    const float strengths[6] = {0.30, 0.34, 0.24, 0.46, 0.28, 0.20};
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
                gx * frequency, gy * frequency, blend, strengths[layer]);
            result.colour += s.colour * weights[axis];
            result.properties += s.properties * weights[axis];
            result.borderHeight += s.borderHeight * weights[axis];
            result.normal += projectionNormal(normal, s.normal, axis) * weights[axis];
        }
    }
    result.normal = normalize(result.normal);
    return result;
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

