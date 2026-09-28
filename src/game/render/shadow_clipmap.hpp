#pragma once
#include <array>
#include <future>
#include "engine/render/device.hpp"
#include "engine/render/frame.hpp"
#include "game/world/world_builder.hpp"
#include "game/world/shadow_field.hpp"

namespace game {
class ShadowClipmap {
public:
    explicit ShadowClipmap(world::WorldBuilder::Snapshot world):world_(std::move(world)) {}
    ~ShadowClipmap();
    bool setup(engine::Device& device);
    bool update(engine::Device& device, engine::Scene& scene, std::array<float,3> sun, bool enabled);
    SDL_GPUTextureSamplerBinding binding() const { return {texture_.get(),sampler_.get()}; }
private:
    struct Profile { double width=1,height=1; float density=0.25f; };
    struct Result {
        world::shadow::Tile tile;
        std::uint64_t revision=0;
        int level=0;
        double radius=0;
    };
    static Result compile(world::WorldBuilder::Snapshot world, std::shared_ptr<const world::ecology::Delta> delta,
                          Result result, const std::array<Profile,world::decor::kModels.size()>& profiles);
    bool dirty(const Result& result, const world::ecology::Delta& delta) const;
    world::WorldBuilder::Snapshot world_;
    std::array<Profile,world::decor::kModels.size()> profiles_{};
    std::array<Result,4> resident_;
    std::array<bool,4> valid_{};
    std::future<Result> job_;
    int next_=0;
    engine::Texture texture_;
    engine::Sampler sampler_;
};
} // namespace game

