// GPU regressions execute production material code, not a CPU approximation.
#define TERRAIN_MATERIAL_PROBE
#include "terrain.hlsl"

float4 TerrainMaterialProbeVS(uint vertex : SV_VertexID) : SV_Position
{
    return float4(vertex == 1 ? 3.0 : -1.0, vertex == 2 ? 3.0 : -1.0, 0.0, 1.0);
}

float4 TerrainMaterialProbePS(float4 pixel : SV_Position) : SV_Target0
{
    const int mode = (int)extraPS.x;
    const float2 local = (pixel.xy - float2(32.5, 32.5)) * extraPS.y;
    const float c = cos(extraPS.z), s = sin(extraPS.z);
    const float3 p = float3(cameraPS.xy + float2(c * local.x - s * local.y,
                                                s * local.x + c * local.y), 100.0);
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

