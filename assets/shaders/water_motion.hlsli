// Scalar wave/flow envelopes, also compiled directly by the C++ regression tests.
#ifndef WATER_MOTION_HLSLI
#define WATER_MOTION_HLSLI
float waterWaveShape(float phase)
{
    // A bounded second harmonic sharpens crests without displacing XY shorelines.
    return sin(phase) + 0.18 * sin(2.0 * phase);
}
float waterWaveDerivative(float phase)
{
    return cos(phase) + 0.36 * cos(2.0 * phase);
}
float waterWaveResolution(float wavelength, float spacing)
{
    // Include the crest harmonic in the footprint limit. No unresolved waves
    // on coarse rings; the normal maps supply the sub-grid surface detail.
    return 1.0 - smoothstep(wavelength * 0.10, wavelength * 0.25, spacing);
}
float waterWaveRoom(float depth, float cover)
{
    return smoothstep(0.10, 5.0, depth) * smoothstep(0.20, 0.90, cover);
}
float waterWaveAmplitude(float windStrength)
{
    return 0.25 + 0.85 * smoothstep(0.0, 1.0, windStrength);
}
float waterFlowWeight(float phase)
{
    // At each UV reset that sample has zero weight. The other half-cycle is
    // fully visible, avoiding periodic jumps while the field stretches.
    const float x = phase * 2.0 - 1.0;
    return x < 0.0 ? -x : x;
}
#endif

