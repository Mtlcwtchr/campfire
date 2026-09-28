#pragma once
// Grass, drawn as cards standing on the ground.
//
// One quad, instanced: every blade on a patch is one draw call, and where each
// stands was decided when the patch was built and lives on the card with it.
// The pass owns the quad and the pictures; it does not decide where grass grows.

#include <string>
#include <vector>
#include <map>
#include <tuple>
#include <limits>
#include <memory>
#include "engine/core/diagnostics.hpp"
#include "game/world/ring_mesh.hpp"
#include "game/render/grass_cull.hpp"

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/geometry/mesh_cache.hpp"

namespace game {
class GpuTerrain;

class FoliagePass : public engine::DrawPass {
public:
    // `cards` are sprite names under the assets directory: one picture per layer
    // of the array the shader picks between.
    FoliagePass(const engine::MeshCache& cache, std::vector<std::string> cards,
                SDL_GPUTextureSamplerBinding shadow, GpuTerrain* pages = nullptr);
    void focus(double x,double y) { x_=x;y_=y; }
    void reset() { roots_.clear();candidates_=0;ASR_DIAGNOSTIC(draws_=uploadBytes_=0); }
#if ASR_ENABLE_DIAGNOSTICS
    std::string report() const;
    bool readDrawnCandidates(engine::Device& device, std::uint32_t& count) const;
#endif
    bool gpuCulling() const { return gpuCulling_; }
    void gpuCulling(bool enabled) { gpuCulling_=enabled && culler_.workingBytes()>0; }

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    // Grass is only drawn close up. Far off a card is thinner than a pixel and
    // all it adds is shimmer and a draw call per patch; how close counts is the
    // scene's business and arrives in the spare vector.
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    SDL_GPUTextureSamplerBinding shadow_{};
    GpuTerrain* pages_ = nullptr;
    engine::RenderPipeline* renderer_ = nullptr;
    engine::BindingSet vertexBindings_ = engine::kNoBindings;
    struct Roots {
        std::shared_ptr<const world::terrain::AdaptiveMesh> mesh;
        std::vector<world::PageGrassRoot> values;
        bool used = false;
        int run = -1;   // the index stamped into `values`; re-stamped when it moves
    };
    std::map<std::tuple<const world::terrain::AdaptiveMesh*,std::int64_t,std::int64_t,int>,Roots> roots_;
    double x_=0,y_=0;
    int windowX_=std::numeric_limits<int>::min(),windowY_=windowX_;
    std::size_t candidates_=0,nearCandidates_=0,farCandidates_=0; // draw size and per-tier budgets
#if ASR_ENABLE_DIAGNOSTICS
    std::size_t draws_=0,uploadBytes_=0,runs_=0,farBlocks_=0;
#endif
    int farStep_=8;
    bool enabled_=true; // ASR_GRASS_DISABLED: bounded capture diagnostic, not a LOD switch
    bool gpuCulling_=false; // ASR_GRASS_CULL_DISABLED preserves the direct path for A/B
    GrassCuller culler_;
    const engine::MeshCache& cache_;
    std::vector<std::string> cardNames_;
    engine::Texture cards_;
    engine::Sampler sampler_;
    engine::Buffer quad_, quadIndices_;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
};

} // namespace game
