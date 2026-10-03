// GPU regressions execute production material code, not a CPU approximation.
#define TERRAIN_MATERIAL_PROBE
#include "terrain.hlsl"

float4 TerrainMaterialProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 TerrainMaterialProbePS(float4 pixel : SV_Position) : SV_Target0
{
    gWeightGradients = false;
    const int mode = (int)extraPS.x;
    const float2 local = (pixel.xy - float2(32.5, 32.5)) * extraPS.y;
    const float c = cos(extraPS.z), s = sin(extraPS.z);
    const float3 p = float3(cameraPS.xy + float2(c * local.x - s * local.y,
                                                s * local.x + c * local.y), 100.0);
    if (mode == 21) {
        const int a = (int)parametersPS[20].x, b = (int)parametersPS[20].y;
        const float slope = parametersPS[20].z;
        const float weight = saturate(0.5 + (pixel.x - 32.5) / 63.0 + parametersPS[20].w);
        float4 weights0 = 0.0;
        float2 weights1 = 0.0;
        if (a < 4) weights0[a] = weight; else weights1[a - 4] = weight;
        if (b < 4) weights0[b] = 1.0 - weight; else weights1[b - 4] = 1.0 - weight;
        // Independent signed-weight reference: no top/under sorting, texture
        // heights, normal direction or UV warp in the expected organic mask.
        // Worked out BEFORE the production blend: that samples its materials
        // behind per-pixel branches, and a screen derivative taken after a
        // branch the quad did not take together is not one Metal promises.
        const float3 style = groundEdgeStyle(a, b);
        const float width = min(0.90, max(0.20, (tablePS[a].y + tablePS[b].y) * 0.5) * style.x);
        const float gap = 2.0 * weight - 1.0;
        const float border = (1.0 - smoothstep(width, 2.0 * width, abs(gap))) *
                             smoothstep(0.0, 0.10, min(weight, 1.0 - weight));
        const float metres = max(0.25, min(tablePS[a].w, tablePS[b].w) * style.y);
        const float footprint = max(length(ddx(p.xy)), length(ddy(p.xy)));
        const float fine = lerp(0.5, organicNoise(p.xy/metres), 1.0-smoothstep(0.25,0.9,footprint/metres));
        const float broad = lerp(0.5, organicNoise(p.xy/(4.0*metres)), 1.0-smoothstep(0.25,0.9,footprint/(4.0*metres)));
        const float mask = broad * 0.65 + fine * 0.35;
        const float intrusion = (mask * 2.0 - 1.0) * border *
                                min(max(tablePS[a].z, tablePS[b].z) * style.z, width * 0.95 * max(1.0,style.z));
        const float blendWidth = max(width, 2.0 * fwidth(weight) + fwidth(intrusion));
        // The band in METRES: analytic weight slope of this probe (1/63 per
        // pixel, extraPS.y metres per pixel), not a derivative of the result.
        const float slopeMetres = 2.0 / (63.0 * max(extraPS.y, 1e-5));
        const float minimumBand = (qualityPS.x > 0.0 ? qualityPS.x : 3.0) * saturate(style.x) * 1.4;
        const float reach = 0.5 / slopeMetres;
        const float band = min(max(max(blendWidth / slopeMetres, minimumBand), 1.5 * footprint), reach);
        const float gapMetres = gap / slopeMetres;
        const float nearTie = (1.0 - smoothstep(band * 1.5, band * 3.0, abs(gapMetres))) *
                              smoothstep(0.0, 0.10, min(weight, 1.0 - weight));
        const float amount = min(max(tablePS[a].z, tablePS[b].z) * style.z, width * 0.95 * max(1.0,style.z));
        const float swing = min(max(amount / slopeMetres, band * 0.9), reach);
        const float inWeight = (gap + intrusion) / max(blendWidth, 1e-4);
        const float inMetres = (gapMetres + (mask * 2.0 - 1.0) * swing * nearTie) / max(band, 1e-4);
        // The tear's own slope on the ground, by finite difference of the
        // same filtered noise (independent of the production gradient code).
        const float h = 0.002 * metres;
        const float2 fp = float2(footprint / metres, footprint / (4.0 * metres));
        const float2 r = float2(1.0 - smoothstep(0.25, 0.9, fp.x), 1.0 - smoothstep(0.25, 0.9, fp.y));
        const float mxp = organicNoise((p.xy + float2(h, 0)) / metres) * r.x * 0.35 +
                          organicNoise((p.xy + float2(h, 0)) / (4.0 * metres)) * r.y * 0.65;
        const float mxn = organicNoise((p.xy - float2(h, 0)) / metres) * r.x * 0.35 +
                          organicNoise((p.xy - float2(h, 0)) / (4.0 * metres)) * r.y * 0.65;
        const float myp = organicNoise((p.xy + float2(0, h)) / metres) * r.x * 0.35 +
                          organicNoise((p.xy + float2(0, h)) / (4.0 * metres)) * r.y * 0.65;
        const float myn = organicNoise((p.xy - float2(0, h)) / metres) * r.x * 0.35 +
                          organicNoise((p.xy - float2(0, h)) / (4.0 * metres)) * r.y * 0.65;
        const float maskSlope = 2.0 * length(float2(mxp - mxn, myp - myn)) / (2.0 * h);
        const float stretchWeight = sqrt(1.0 + pow(amount * border * maskSlope / slopeMetres, 2.0));
        const float stretchMetres = sqrt(1.0 + pow(swing * nearTie * maskSlope, 2.0));
        const float sw = inWeight / stretchWeight, sm = inMetres / stretchMetres;
        const float rawMix = smoothstep(-1.0, 1.0, (abs(sw) < abs(sm) ? sw : sm) * (sw * sm > 0.0 ? 1.0 : 0.0));
        // Mirrors production: the majority side closes over a vanishing runner-up.
        const float lesser = min(weight, 1.0 - weight);
        const float majority = gap >= 0.0 ? rawMix : 1.0 - rawMix;
        const float closed = lerp(1.0, majority, smoothstep(0.0, 0.06, lesser));
        const float expected = gap >= 0.0 ? closed : 1.0 - closed;
        const Ground ground = groundHere(weights0, weights1,
            float3(p.xy, p.z + (p.x - cameraPS.x) * slope), normalize(float3(-slope, 0, 1)));
        return float4(materialCoverage(a, ground.top, ground.under, ground.mix), expected, mask, 1.0);
    }
    if (mode >= 18 && mode <= 20) {
        // A mipmapped high-frequency source must not be magnified back into
        // terrain grain by the middle/broad bands once its fine band resolves
        // to the mean. Still sample the production material implementation.
        const MaterialSample material = materialProjection(0,p.xy,ddx(p.xy),ddy(p.xy),0.37,1.0);
        if (mode == 19) return float4(material.normal * 0.5 + 0.5,1.0);
        if (mode == 20) return float4(material.properties.rgb,1.0);
        return float4(material.colour,1.0);
    }
    if (mode >= 11) {
        const VegetationCover cover = vegetationCover(mode==12?1.0:0.0,
            mode==11 || mode>=16?1.0:0.0,mode==14?1.0:0.0,mode==13?1.0:0.0,
            0.0,mode==15?1.0:0.0,mode==12?1.0:0.0,0.0,mode==12?0.0:1.0,0.0,
            0.6,mode==17?2500.0:100.0,mode==16?1.0:-100.0,1.0,0.7,0.5);
        const float3 base = float3(0.55,0.40,0.25);
        const float l = dot(base,float3(0.2126,0.7152,0.0722));
        return float4(vegetationGroundChannel(base.r,cover.red,l,cover.ground,cover.canopy),
            vegetationGroundChannel(base.g,cover.green,l,cover.ground,cover.canopy),
            vegetationGroundChannel(base.b,cover.blue,l,cover.ground,cover.canopy),1.0);
    }
    if (mode == 0) {
        // No runner-up may intrude into a pure material, at either height extreme.
        return float4(materialBorderMix(1.0, 0.2, 0.0, -1.0, 0.0),
                      materialBorderMix(1.0, 0.2, 0.0, 1.0, 0.0),
                      materialBorderMix(0.8, 0.2, 0.0, -1.0, 0.0), 1.0);
    }
    if (mode == 1) {
        const float a = materialBorderMix(0.03, 0.2, 0.04, 0.7, 0.9);
        const float b = materialBorderMix(-0.03, 0.2, -0.04, -0.7, 0.9);
        return float4(a, 1.0 - b, abs(a + b - 1.0), 1.0);
    }
    if (mode == 2) {
        const float3 n = normalize(float3(-0.6, 0.7, 0.2));
        return float4(dot(n, projectionNormal(n, float3(0, 0, 1), 0)),
                      dot(n, projectionNormal(n, float3(0, 0, 1), 1)),
                      dot(n, projectionNormal(n, float3(0, 0, 1), 2)), 1.0);
    }
    if (mode == 3) {
        return float4(stoneSupport(0.08, 0.92, 0, 0, 0),
                      stoneSupport(0.08, 0, 0.92, 0, 0),
                      stoneSupport(0.08, 0, 0, 0.92, 0),
                      stoneSupport(1, 0, 0, 0, 0));
    }
    if (mode == 6) {
        const float3 n = reliefNormal(float3(0, 0, 1), p, p.x * 0.1 + p.y * 0.2, 1.0);
        return float4(n * 0.5 + 0.5, 1.0);
    }
    if (mode >= 9) {
        const int layer = (int)parametersPS[20].x;
        const float slope = parametersPS[20].y;
        float4 weights0 = 0.0;
        float2 weights1 = 0.0;
        if (layer < 4) weights0[layer] = 1.0;
        else weights1[layer - 4] = 1.0;
        const float3 normal = normalize(float3(-slope, 0, 1));
        const float3 position = float3(p.xy, p.z + (p.x - cameraPS.x) * slope);
        if (mode == 9) {
            const Ground ground = groundHere(weights0, weights1, position, normal);
            return float4(ground.colour, 1.0);
        }
        TerrainOut input = (TerrainOut)0;
        input.position = pixel;
        input.normal = normal;
        input.weights0 = weights0;
        input.weights1 = weights1;
        input.worldXY = position.xy;
        input.frameXY = position.xy;   // frame origin nought: the world as it is
        input.worldHeight = position.z;
        input.waterDepth = -100;
        input.environment = float4(0.65, 0, 0.5, 0);
        return TerrainPS(input);
    }
    if (mode == 7) {
        TerrainOut input = (TerrainOut)0;
        input.position = pixel;
        input.normal = float3(0, 0, 1);
        input.weights0 = float4(0.96, 0, 0, 0.04);
        input.worldXY = p.xy;
        input.frameXY = p.xy;
        input.worldHeight = p.z;
        input.waterDepth = -100;
        input.environment = float4(0.65, 0, 0.5, 0);
        return TerrainPS(input);
    }
    const float grass = mode == 5 ? 0.52 : 0.96;
    const Ground ground = groundHere(float4(grass, 0, 0, 1.0 - grass), float2(0, 0),
                                     p, float3(0, 0, 1));
    if (mode == 8) return ground.properties;
    return float4(ground.colour, ground.mix);
}

