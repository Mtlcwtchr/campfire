// The procedural environment's surface hook (doc/plan_procedural_environment_2026-10-03.md, part H).
//
// The engine's side of the style split. Every surface shader - ground, models,
// grass, water - hands its colour to styleSurfaceColour() just before it is
// lit; the engine knows only when to call and what to pass. What happens to
// the colour is the game's, in assets/shaders/game/style_surface.hlsli, which
// reads the palette rows of the region under the camera (envStylePS, filled from
// content/config/style/palettes.json) and the environment at the point.
//
// No palette: the hook returns the colour untouched, and nothing is drawn
// differently from before the environment existed.
#ifndef ENVIRONMENT_STYLE_HLSLI
#define ENVIRONMENT_STYLE_HLSLI

#include "world.hlsli"

// The environment at a point, where the shader knows it (the ground's pages
// carry it; models and grass do not, and see zeros). Mask channels are named
// by the game's masks.json in order: channel k is masksA[k] for k < 4,
// masksB[k - 4] after.
struct EnvironmentPoint {
    float4 masksA;
    float4 masksB;
    uint zone;          // strongest zone type, 0 unclassified
    uint second;        // second strongest
    float share;        // the second's share of the two
    float cover;        // the ground tier's cover multiplier, 0..2 (cover.json)
};

// What the ground pass knows about the pixel it is shading. Set by the page
// pixel shader before it shades; zero everywhere else.
static EnvironmentPoint gEnvironment = (EnvironmentPoint)0;

float environmentMask(EnvironmentPoint e, int channel)
{
    return channel < 4 ? e.masksA[channel] : e.masksB[channel - 4];
}

// What kind of surface is asking. The game's body may treat each differently.
static const uint kStyleGround = 0;
static const uint kStyleRock = 1;
static const uint kStyleVegetation = 2;
static const uint kStyleModel = 3;
static const uint kStyleWater = 4;

struct StyleSurface {
    float3 colour;          // in and out: linear albedo, before light
    float3 worldPos;
    uint kind;
    float vegetation;       // 0..1 how much of the surface is leaves or grass
    float rockShare;        // 0..1 ground: how much is rock
    EnvironmentPoint environment;
};

bool stylePresent() { return envSwitchesPS.w > 0.5; }
float4 styleRow(int i) { return envStylePS[i]; }

#include "game/style_surface.hlsli"

float3 styleSurfaceColour(float3 colour, float3 worldPos, uint kind, float vegetation, float rockShare)
{
    if (!stylePresent()) return colour;
    StyleSurface s;
    s.colour = colour;
    s.worldPos = worldPos;
    s.kind = kind;
    s.vegetation = vegetation;
    s.rockShare = rockShare;
    s.environment = gEnvironment;
    styleSurface(s);
    return s.colour;
}

#endif
