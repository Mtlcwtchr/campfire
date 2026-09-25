#ifndef TERRAIN_EROSION_HLSLI
#define TERRAIN_EROSION_HLSLI
#include "relief.hlsli"

struct ErosionRelief {
    float depth; // virtual incision in metres, NEVER vertex displacement
    float cut;
};

// A shading approximation of unresolved erosion, not another hydraulic solver.
// The source's removed-height evidence gates it; a flat/unweathered surface owes
// no grooves. Bands stay in world metres through zoom and mesh replacement.
// Smear locally along the fall line, never rotate world XY about the origin.
ErosionRelief erosionRelief(float2 worldXY, float3 normal, float pixelMetres,
                           float erosionMetres)
{
    ErosionRelief result = (ErosionRelief)0;
    const float support = smoothstep(0.02, 0.24, 1.0-normal.z) *
                          smoothstep(0.5, 12.0, erosionMetres);
    const float2 downhill = normalize(normal.xy + float2(0.00001,0.00001));
    [unroll] for (int band=0; band<3; ++band) {
        const float metres = band==0 ? 12.0 : band==1 ? 36.0 : 108.0;
        // The groove is narrower than a noise cell. Fade BEFORE it is subpixel,
        // independently of tile size, source residency and the camera's zoom UI.
        const float resolved = 1.0-smoothstep(0.07,0.24,pixelMetres/metres);
        const float streak = reliefStreak(worldXY+float2(31.7,-19.3)*float(band),
                                          downhill,metres,metres*1.2,1);
        const float groove = 1.0-smoothstep(0.30,0.53,streak);
        const float amplitude = min(max(0.0,erosionMetres)*0.08,metres*0.055);
        result.depth -= groove*amplitude*support*resolved;
        result.cut += groove*support*resolved/3.0;
    }
    return result;
}
#endif
