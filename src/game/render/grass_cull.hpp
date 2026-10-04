#pragma once

#include "engine/render/geometry/cluster_cull.hpp"
#include "game/world/ring_mesh.hpp"

namespace game {
// First runtime consumer of ClusterCuller: conservative grass frustum culling.
// Material/coverage tests remain in PageGrassVS. No GPU -> CPU -> GPU round trip.
class GrassCuller {
public:
    static constexpr std::size_t kCapacity = world::kGrassCandidateBudget * 2;
    bool setup(engine::Device& device, engine::RenderPipeline& into);
    bool dispatch(const engine::Frame& frame, engine::RenderPipeline& into,
                  std::uint32_t arenaOffset, std::size_t count);
    SDL_GPUBuffer* instances() const { return compacted_.get(); }
    SDL_GPUBuffer* arguments() const { return culler_.arguments(); }
    // The same survivors drawn with another mesh (the grass blades): their own
    // indirect arguments, written after the cull with `indexCount`.
    bool secondDraw(engine::Device& device, engine::RenderPipeline& into, std::uint32_t indexCount);
    SDL_GPUBuffer* secondArguments() const { return second_.get(); }
    // Explicit screenshot/test diagnostic only; never called by collect().
#if ASR_ENABLE_DIAGNOSTICS
    bool readCount(engine::Device& device, std::uint32_t& count) const;
#endif
    std::size_t workingBytes() const;
private:
    engine::ClusterCuller culler_;
    engine::Buffer bounds_, compacted_, ranks_, blocks_, second_;
    engine::ComputeSlot secondArguments_ = 0;
    std::uint32_t secondIndices_ = 0;
    engine::ComputeSlot prepare_ = 0, gather_ = 0;
    engine::ComputeSlot mark_ = 0, scan_ = 0, prefix_ = 0;
    bool ready_ = false;
};
}

