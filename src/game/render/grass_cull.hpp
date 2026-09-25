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
    // Explicit screenshot/test diagnostic only; never called by collect().
#if ASR_ENABLE_DIAGNOSTICS
    bool readCount(engine::Device& device, std::uint32_t& count) const;
#endif
    std::size_t workingBytes() const;
private:
    engine::ClusterCuller culler_;
    engine::Buffer bounds_, compacted_, ranks_, blocks_;
    engine::ComputeSlot prepare_ = 0, gather_ = 0;
    engine::ComputeSlot mark_ = 0, scan_ = 0, prefix_ = 0;
    bool ready_ = false;
};
}

