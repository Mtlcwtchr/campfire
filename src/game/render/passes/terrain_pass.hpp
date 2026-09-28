#pragma once
// The ground.
//
// All this pass does is say where it belongs, hand its graphics pipeline and its
// material textures to the render pipeline, and then push one draw item per
// patch. It binds nothing and opens nothing. What order its items come out in,
// and how many state changes they end up costing, is the batcher's business.

#include <string>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "engine/render/mesh_renderer.hpp"
#include "game/render/climate_textures.hpp"

namespace game {
class GpuTerrain;

// Bit the collector sets in a mesh's tags when there is water standing on it.
inline constexpr std::uint32_t kMeshHasWater = 1u << 0;
// Terrain tiles all instance one immutable 65x65 grid. Their Mesh entry owns
// attributes only; ground and water passes bind this shared uint16 topology.
inline constexpr std::uint32_t kMeshUsesSharedGrid = 1u << 2;

// Set on a patch, for this frame only, when it is the level the view asked for.
//
// A patch without it is a stand-in: the same ground, sampled more coarsely,
// on screen only because the sharp one has not been cut yet. It is drawn as a
// backdrop - painted first, with the depth buffer untouched - and never as
// ground.
//
// Drawing it as ground was the bug that made a half-loaded view look broken. A
// coarse patch averages its ground, so in a valley or on a coast it sits
// *above* the real thing; drawn first with depth writing, it then won the depth
// test against the sharp patch that arrived after it. What that looks like is
// flat plates with straight edges lying over the relief, and green lying over
// the sea - and it looks like a level-of-detail problem, which is what sent me
// looking in the wrong place first.
inline constexpr std::uint32_t kMeshIsGround = 1u << 1;

// How far this patch has walked towards the level above it, this frame, in the
// second byte of the same tags.
//
// In the per-frame tags rather than baked into the mesh because it is a fact
// about the zoom and not about the ground: the same patch is unmorphed when its
// level has just been entered and fully morphed a moment before the next level
// takes over, and it is uploaded once for both.
inline std::uint32_t meshMorphTag(float morph) {
    const float clamped = morph < 0 ? 0 : (morph > 1 ? 1 : morph);
    return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f) << 8;
}
inline float meshMorph(std::uint32_t tags) {
    return static_cast<float>((tags >> 8) & 0xffu) / 255.0f;
}

class TerrainPass : public engine::DrawPass {
public:
    // `materials` are sprite names under the assets directory, in the order the
    // weights come in the vertex - which is the order the shader samples them.
    // `climate` is the world's weather - a value per place at sixty-four
    // metres, which is where it belongs rather than as eleven floats on every
    // corner of every tile. The textures are shared with the water, which
    // reads the same channels, and filled the first time a pass asks.
    TerrainPass(const engine::MeshCache& cache, std::vector<std::string> materials,
                std::vector<std::string> materialMaps,
                ClimateTextures& climate, const world::ClimateField& field,
                SDL_GPUTextureSamplerBinding shadow, GpuTerrain* pages = nullptr);

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;
    // Graphics setting: start the material chains at name@2 (half resolution,
    // a quarter of the memory) instead of the full-resolution base level.
    void halfTextures(bool half) { halfTextures_ = half; }

private:
    bool halfTextures_ = false;
    SDL_GPUTextureSamplerBinding shadow_{};
    GpuTerrain* pages_ = nullptr;
    const engine::MeshCache& cache_;
    std::vector<std::string> materialNames_;
    std::vector<std::string> materialMaps_;
    ClimateTextures& climate_;
    const world::ClimateField& climateField_;
    engine::Texture materials_;   // one array, a layer to a material
    engine::Texture materialNormals_;
    engine::Texture materialProperties_;
    engine::Sampler sampler_;
    engine::Buffer gridIndices_;
    std::uint32_t gridIndexCount_ = 0;
    engine::SmartTerrainRenderer terrainRenderer_;
    engine::MeshRenderer backdropRenderer_;
};

// The vertex layout the ground and the water share: both draw the same triangles
// out of the same buffer and differ only in the shader, which is why the
// description is here rather than written out twice.
const std::vector<SDL_GPUVertexBufferDescription>& terrainBuffers();
const std::vector<SDL_GPUVertexAttribute>& terrainAttributes();
const std::vector<SDL_GPUVertexBufferDescription>& pageGridBuffers();
const std::vector<SDL_GPUVertexAttribute>& pageGridAttributes();

} // namespace game
