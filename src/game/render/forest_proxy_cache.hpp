#pragma once
#include "engine/render/impostor_gpu_cache.hpp"
#include "engine/render/impostor_hierarchy.hpp"
#include "engine/render/material.hpp"
#include "game/world/scene_placement.hpp"
#include <functional>
#include <map>

namespace game {
class ForestProxyCache {
public:
    static constexpr int kCell=32;
    bool setup(engine::Device& device,engine::RenderPipeline& pipeline,std::vector<std::shared_ptr<const engine::render::ImpostorAtlas>> sources,
               SDL_GPUTextureSamplerBinding shadow,const engine::VertexLayout& layout);
    // `covered` reports ground a coarser far node already owns this frame; a
    // 32 m proxy there would draw the same trees twice.
    void update(const engine::Frame& frame,const world::ScenePlacementSnapshot* placement,
                const engine::camera::ViewState& view,bool enabled,
                const std::function<bool(double,double)>& covered={});
    void clear();
    static engine::render::ImpostorKey key(double x,double y);
    struct Draw {engine::render::ImpostorKey key;std::size_t slot;engine::camera::Vec3 centre;double side;double weight=1;};
    // How far the proxy of this 32 m cell has faded in (0 = members only).
    double fade(engine::render::ImpostorKey key) const;
    static constexpr double kFadeSeconds=0.45;
    const std::vector<Draw>& draws() const { return drawn_; }
    bool covers(std::size_t model) const {return model<sources_.size() && bool(sources_[model]);}
    void protect(std::size_t slot,std::uint64_t serial) {gpu_.cache.protect(slot,serial);}
    void apply(engine::DrawItem& draw) const {material_.apply(draw);}
    std::uint64_t bakes() const {return gpu_.cache.completedBakes();}
    std::uint64_t uploads() const {return gpu_.uploadedViews();}
    std::uint64_t drawnTotal() const {return drawnTotal_;}
private:
    struct Group {
        std::vector<engine::render::ImpostorPlacement> members;
        engine::camera::Vec3 centre{};
        double radius=0;
        std::uint64_t revision=0;
    };
    void rebuild(const world::ScenePlacementSnapshot* placement);
    // Baked like the far hierarchy: the recursive bake, then its error measured
    // against the members it replaces (screen error, not an analytic bound).
    engine::render::ImpostorGpuCache gpu_{engine::render::ImpostorGpuCache::kSlots,engine::render::ImpostorGpuCache::kResolution,
        16*1024*1024,[](std::span<const engine::render::ImpostorPlacement> members,const engine::render::ImpostorBakeOptions& options,
                        const std::atomic_bool* cancel) {
            return engine::render::withMeasuredError(engine::render::bakeRecursiveImpostor(members,options,cancel),members,cancel);
        }};
    std::map<engine::render::ImpostorKey,double> fade_;
    engine::MaterialInstance material_;
    std::vector<std::shared_ptr<const engine::render::ImpostorAtlas>> sources_;
    std::map<engine::render::ImpostorKey,Group> groups_;
    std::vector<Draw> drawn_;
    std::uint64_t objectsVersion_=~std::uint64_t(0);
    std::uint64_t drawnTotal_=0;
    double clock_=0;
    bool ready_=false;
};
} // namespace game
