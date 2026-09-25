// A procedural swash cycle, not a fluid solver. Phase is in [0, 1].
// All envelopes close at the wrap, so the next wave never pops into existence.
#ifndef SHORE_MOTION_HLSLI
#define SHORE_MOTION_HLSLI

static const float kSurfResidueHold = 0.035;
static const float kSurfResidueLifetime = 0.18;

float surfWarpedBank(float bank, float broad, float tongues, float notches)
{
    // Stronger, multi-scale bites in the actual water edge. Keep the warp
    // inside the apron, so geometry/support fades cannot cut off its tips.
    const float band = smoothstep(-0.92, -0.60, bank) *
                       (1.0 - smoothstep(0.60, 0.92, bank));
    return bank + (broad * 0.14 + tongues * 0.20 + notches * 0.18) * band;
}

float surfRunup(float phase)
{
    // Fast approach, a held lip, then a retreat over more than half the cycle.
    return smoothstep(0.04, 0.28, phase) * (1.0 - smoothstep(0.44, 0.96, phase));
}

float surfStrength(float phase)
{
    return smoothstep(0.0, 0.12, phase) * (1.0 - smoothstep(0.70, 1.0, phase));
}

float surfResidue(float phase)
{
    // Broad wet-film memory only; white residue uses local recession time below.
    return smoothstep(0.18, 0.42, phase) * (1.0 - smoothstep(0.70, 1.0, phase));
}

float surfDryFront(float phase, float blowing)
{
    // The dry end of the water alpha ramp, not its opaque/foamy centre.
    // Use much more of the render apron, including the exposed shallows on
    // retreat. Residue inherits this wider swept strip, never a separate band.
    return 0.18 - (0.18 + 0.48 * blowing) * surfRunup(phase);
}

float surfFoamFront(float phase, float blowing)
{
    return surfDryFront(phase, blowing) + 0.22 + (1.0 - surfRunup(phase)) * 0.22;
}

float surfContactJoin(float phase, float bank, float blowing)
{
    const float dry = surfDryFront(phase, blowing);
    // The retreat ramp and its residue complement must meet at the same place.
    // Keep replacement continuous at cycle wrap even when active foam is zero;
    // otherwise the just-hidden deposit reappears as a second wave of white.
    return smoothstep(dry - 0.10, dry - 0.065, bank);
}

float surfContactFoam(float phase, float bank, float blowing)
{
    const float dry = surfDryFront(phase, blowing);
    // Bridge the water's transparent ramp instead of multiplying the bridge
    // by that same ramp and punching the gap back into it.
    // Only on retreat may this lip overlap the freshly exposed residue.
    const float farthest = surfDryFront(0.30, blowing);
    const float swept = smoothstep(farthest, farthest + 0.035, bank);
    const float retreat = smoothstep(0.44, 0.48, phase);
    const float advance = smoothstep(dry, dry + 0.035, bank);
    const float towardsSand = advance +
            (surfContactJoin(phase, bank, blowing) - advance) * retreat;
    const float front = surfFoamFront(phase, blowing);
    const float towardsWater = 1.0 - smoothstep(front + 0.05, front + 0.18, bank);
    return surfStrength(phase) * towardsSand * swept * towardsWater;
}

float surfBlendResidualFoam(float join, float retreatFoam, float residualFoam)
{
    // retreatFoam already contains join via surfContactFoam. Its complement
    // replaces residue exactly once, rather than fading residue ahead of the
    // contact ramp (which carved a trough and made two separate foam bands).
    // At full contact only the retreat's patterned opacity remains.
    return retreatFoam + residualFoam * (1.0 - join);
}

float surfResidueAge(float waveTime, float bank, float blowing)
{
    const float farthest = surfDryFront(0.30, blowing);
    const float resting = surfDryFront(1.0, blowing);
    // Only the strip swept by the waterline can leave a residual deposit.
    if (bank <= farthest || bank >= resting) return -1.0;
    const float progress = (bank - farthest) / (resting - farthest);
    // Inverse of smoothstep: find WHEN the retreating dry edge passed here.
    const float crossed = 0.44 + 0.52 * (0.5 - sin(asin(1.0 - 2.0 * progress) / 3.0));
    // Signed age in THIS cycle. Never relight the previous deposit when the
    // next wave's strength rises; the old one drains to zero before the wrap.
    return frac(waveTime) - crossed;
}

float surfResidualFoam(float waveTime, float bank, float blowing)
{
    const float age = surfResidueAge(waveTime, bank, blowing);
    const float phase = frac(waveTime);
    const float dry = surfDryFront(phase, blowing);
    const float exposed = 1.0 - smoothstep(dry - 0.025, dry, bank);
    const float farthest = surfDryFront(0.30, blowing);
    const float swept = smoothstep(farthest, farthest + 0.035, bank);
    // Drain in place over at most 1.26--1.62 seconds, with only a brief hold.
    // Share the retreat's strength AND its fixed landward footprint: releasing
    // the contact mask must not uncover a brighter deposit as a second front.
    // Exposure is only a dry-ground guard, not a translating residue texture.
    return smoothstep(0.0, 0.018, age) *
           (1.0 - smoothstep(kSurfResidueHold, kSurfResidueLifetime, age)) *
           exposed * swept * surfStrength(phase);
}

#endif
