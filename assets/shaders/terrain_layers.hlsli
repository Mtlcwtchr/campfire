// The ground texture array's constants: how many layers, and the metres one
// turn of each covers. Generated from content/config/terrain/layers.json
// (engine/biomes, terrain_layers.gen.hlsli) when that has been made; else the
// engine's own catalogue, which the config begins with.
#ifndef TERRAIN_LAYERS_HLSLI
#define TERRAIN_LAYERS_HLSLI
#if __has_include("terrain_layers.gen.hlsli")
#include "terrain_layers.gen.hlsli"
#else
#define GROUND_LAYERS 22
static const float kLayerMetres[GROUND_LAYERS] = {
    2.0, 2.07, 3.0, 3.0, 1.3, 2.0,
    2.0, 3.0, 3.94, 2.0, 1.5, 2.53, 2.7, 3.0, 1.83, 2.0,
    1.12, 2.0, 2.35, 1.3, 1.0, 2.0};
#endif
#endif
