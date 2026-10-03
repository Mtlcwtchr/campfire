#ifndef WATER_SWASH_HLSLI
#define WATER_SWASH_HLSLI

// Shared scalar animation, also compiled by the continuity regression tests.
static const float kWsSwashPeak = 0.38;

// Smooth crest and trough with a tunable long back. Blend polynomial profiles
// instead of evaluating a variable pow() at every coastal pixel.
float wsProfile(float u, float rise, float back)
{
    if (u < rise) {
        const float x = u / rise;
        return x * x * (3.0 - 2.0 * x);
    }
    const float v = (u - rise) / (1.0 - rise);
    const float w = 1.0 - v, w2 = w * w;
    const float p2 = w2 * (1.0 + 2.0 * v);
    const float p3 = w2 * w * (1.0 + 3.0 * v);
    const float p4 = w2 * w2 * (1.0 + 4.0 * v);
    const float low = back - 2.0 < 1.0 ? back - 2.0 : 1.0;
    const float high = back > 3.0 ? back - 3.0 : 0.0;
    return p2 + (p3 - p2) * low + (p4 - p3) * high;
}

// Analytic face slope, without differencing quantized texture heights.
float wsProfileDerivative(float u, float rise, float back)
{
    if (u < rise) {
        const float x = u / rise;
        return 6.0 * x * (1.0 - x) / rise;
    }
    const float v = (u - rise) / (1.0 - rise);
    const float w = 1.0 - v;
    const float d2 = -6.0 * v * w;
    const float d3 = -12.0 * v * w * w;
    const float d4 = -20.0 * v * w * w * w;
    const float low = back - 2.0 < 1.0 ? back - 2.0 : 1.0;
    const float high = back > 3.0 ? back - 3.0 : 0.0;
    return (d2 + (d3 - d2) * low + (d4 - d3) * high) / (1.0 - rise);
}

// Exact mean of the blended profile: waves displace water without raising it.
float wsProfileMean(float rise, float back)
{
    const float low = back - 2.0 < 1.0 ? back - 2.0 : 1.0;
    const float high = back > 3.0 ? back - 3.0 : 0.0;
    return rise * 0.5 + (1.0 - rise) * (0.5 - 0.1 * low - high / 15.0);
}

float wsSwashEase(float x)
{
    const float a = x * x;
    const float b = (1.0 - x) * (1.0 - x);
    return a / (a + b);
}

float wsSwashEnvelope(float q)
{
    if (q <= 0.0 || q >= 1.0) return 0.0;
    return q < kWsSwashPeak ? wsSwashEase(q / kWsSwashPeak) :
           wsSwashEase((1.0 - q) / (1.0 - kWsSwashPeak));
}

float wsSwashInverse(float h)
{
    h = h < 0.0 ? 0.0 : (h > 1.0 ? 1.0 : h);
    const float a = sqrt(h), b = sqrt(1.0 - h);
    return a / (a + b);
}

float wsSwashCame(float h) { return kWsSwashPeak * wsSwashInverse(h); }
float wsSwashWent(float h) { return 1.0 - (1.0 - kWsSwashPeak) * wsSwashInverse(h); }

// Both age and opacity use this blend. The previous wave at u=0 is exactly
// the current wave at u=1; texture advection must not select an age by max().
float wsFoamRenewal(float u, float rise) { return smoothstep(0.0, rise + 0.08, u); }
float wsResidueLifetime(float age, float period)
{
    return 1.0 - smoothstep(period * 0.65, period, age);
}
#endif
