#pragma once
#include "engine/render/material.hpp"

namespace game::materials {
inline std::shared_ptr<const engine::Material> terrain(bool adaptive, bool backdrop = false,
                                                      bool cull = false) {
    auto material = std::make_shared<engine::Material>();
    material->name = backdrop ? "terrain/backdrop" : "terrain/ground";
    // The ground is a closed surface seen from above and its skirts hang
    // outward, so its back faces are never the ones you look at - and until
    // there was a mechanism at all, the engine drew all of them anyway.
    material->cull = cull ? SDL_GPU_CULLMODE_BACK : SDL_GPU_CULLMODE_NONE;
    material->shader = adaptive ? "terrain_pages.hlsl" : "terrain.hlsl";
    material->vertexEntry = adaptive ? "AdaptiveTerrainVS" : backdrop ? "TerrainBackdropVS" : "TerrainVS";
    material->fragmentEntry = adaptive ? "TerrainPagePS" : "TerrainPS";
    return material;
}
inline std::shared_ptr<const engine::Material> sceneModels() {
    auto material = std::make_shared<engine::Material>();
    material->name = "nature/mesh-and-impostor";
    material->shader = "scene_models.hlsl";
    material->vertexEntry = "ModelVS"; material->fragmentEntry = "ModelPS";
    return material;
}
// The vertex and instance streams scene_models.hlsl reads, spelled once.
//
// Here rather than inside the pass so a test can build the pipeline from the
// same description the frame does. A shader is compiled at run time, so a
// mistake in it is not a build error - it is a pass that quietly draws nothing,
// and the way that is caught cheaply is by asking the device to build it.
struct SceneModelStreams {
    // position(3), normal(3), uv(2), colour(3), texture layer, coverage,
    // cluster-parent morph target position(3) and geometric normal(3).
    //
    // Coverage is one for everything a modeller made and less than one only on
    // a crown - the surface a mass of alpha cards was merged into. It says how
    // solid the leaf mass is at that vertex, which is what keeps a lacy canopy
    // lacy once it has stopped being separate leaves.
    static constexpr std::size_t kVertexBytes = 19 * sizeof(float);
    // position(3), scale, yaw, phase, tint, vegetation, width, height, layer,
    // mesh weight, mode, coverage, next layer, view blend.
    static constexpr std::size_t kInstanceBytes = 16 * sizeof(float);
};
inline engine::VertexLayout sceneModelLayout() {
    engine::VertexLayout layout;
    layout.buffers = {{0, SceneModelStreams::kVertexBytes, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
                      {1, SceneModelStreams::kInstanceBytes, SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0}};
    layout.attributes = {
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0},                      // position
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 3 * sizeof(float)},      // normal
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 6 * sizeof(float)},      // uv
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 8 * sizeof(float)},      // colour
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 11 * sizeof(float)},     // layer, coverage
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 13 * sizeof(float)},     // morph target
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 16 * sizeof(float)},     // morph normal target
        {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 0},                      // origin, scale
        {8, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 4 * sizeof(float)},      // yaw, phase, tint, vegetation
        {9, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 8 * sizeof(float)},      // width, height, layer, mesh
        {10, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, 12 * sizeof(float)}};   // mode, coverage, next layer, blend
    return layout;
}

inline std::shared_ptr<const engine::Material> sprite(bool soft) {
    auto material = std::make_shared<engine::Material>();
    material->name = soft ? "sprite/soft" : "sprite/cutout";
    material->shader = "sprite.hlsl";
    material->vertexEntry = "SpriteVS";
    material->fragmentEntry = soft ? "SpriteSoftPS" : "SpritePS";
    material->blend = soft; material->depthWrite = !soft;
    return material;
}
} // namespace game::materials
