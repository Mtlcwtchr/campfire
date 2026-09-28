#pragma once
// The far forest: 128 / 512 / 2048 m nodes of the engine impostor hierarchy,
// baked from the real deterministic scatter (not from what happens to be
// resident in the placement) and from each other, kept in a bounded store and
// published to a fixed GPU atlas. The 32 m level stays in ForestProxyCache.
#include "engine/render/impostor_gpu_cache.hpp"
#include "engine/render/impostor_hierarchy.hpp"
#include "engine/render/material.hpp"
#include "game/world/terrain_view.hpp"
#include "game/world/world_builder.hpp"
#include <future>
#include <map>
#include <optional>
#include <unordered_map>

namespace engine { struct Frame; }
namespace game {
class ForestHierarchy {
public:
    static constexpr unsigned kSlots=96,kResolution=32,kLeafResolution=16;
    // One atlas view at 32² with its mips and depth is ~15 KiB.
    static constexpr unsigned kViewsPerFrame=8;
    static constexpr std::size_t kMaxRegionMembers=4096;
    ForestHierarchy();
    ~ForestHierarchy();
    ForestHierarchy(const ForestHierarchy&)=delete;
    ForestHierarchy& operator=(const ForestHierarchy&)=delete;
    // `leaves` is indexed by model; null = not represented by the hierarchy.
    bool setup(engine::Device& device,engine::RenderPipeline& pipeline,
               const std::vector<std::shared_ptr<const engine::render::ImpostorAtlas>>& leaves,
               SDL_GPUTextureSamplerBinding shadow,const engine::VertexLayout& layout);
    struct Inputs {
        world::WorldBuilder::Snapshot world;
        world::terrain::ViewBounds bounds;
        engine::camera::ViewState view;
        const float (*density)[4]=nullptr; // Scene::vegetationDensity, 8 entries
        double drawDistance=60000;
        bool enabled=true;
    };
    void update(const engine::Frame& frame,const Inputs& inputs);
    void clear();
    struct Draw {engine::render::ImpostorKey key;std::size_t slot;engine::camera::Vec3 centre;double side;
                 double weight=1;   // own fade 0..1
                 double coverage=1; // what the shader gets: weight, or -ancestor for a complement
                };
    const std::vector<Draw>& draws() const { return drawn_; }
    // Whether a drawn far node owns this ground position completely...
    bool covered(double x,double y) const;
    // ...and how far the coarsest one covering it has faded in (0 = not at all).
    double coverage(double x,double y) const;
    static constexpr double kFadeSeconds=0.45;
    bool covers(std::size_t model) const {return model<leaves_.size() && bool(leaves_[model]);}
    void protect(std::size_t slot,std::uint64_t serial) {gpu_.cache.protect(slot,serial);}
    void apply(engine::DrawItem& draw) const {material_.apply(draw);}
    bool ready() const { return ready_; }
    struct Stats {
        std::uint64_t bakes=0,failed=0,uploads=0,drawnTotal=0;
        std::size_t visited=0,wanted=0,jobs=0,storeBytes=0,storeEntries=0;
        std::size_t invisible=0,below=0,coarse=0,pruned=0;
        double pressure=1;bool limited=false;
        double updateMs=0,worstUpdateMs=0,uploadMs=0; // frame-thread cost: last, worst so far, upload part
        // Per level (index = level): drawn and wanted this frame, and the mean
        // baked total/measured error over every non-empty bake so far.
        std::array<std::size_t,4> drawnAt{},wantedAt{},bakedAt{};
        std::array<double,4> totalError{},viewError{},bakeMs{};
    };
    Stats stats() const;
private:
    struct Result {bool ok=false;std::shared_ptr<const engine::render::ImpostorAtlas> atlas;double ms=0;};
    struct Job {
        engine::render::ImpostorKey key;std::uint64_t revision=0,generation=0;
        std::future<Result> work;std::shared_ptr<std::atomic_bool> cancel;
    };
    using Leaves=std::vector<std::shared_ptr<const engine::render::ImpostorAtlas>>;
    std::uint64_t revision(engine::render::ImpostorKey key) const;
    void refreshWorld(const world::WorldBuilder::Snapshot& world);
    double groundAt(engine::render::ImpostorKey key);
    void collectJobs();
    // Returns true when key is in the store; otherwise records what has to be
    // baked first (children before parents) into `bakes`.
    bool need(engine::render::ImpostorKey key,double priority,std::vector<std::pair<engine::render::ImpostorKey,double>>& bakes,int budget);
    bool start(engine::render::ImpostorKey key);
    engine::render::ImpostorGpuCache gpu_;
    engine::render::ImpostorStore store_;
    engine::MaterialInstance material_;
    Leaves leaves_;
    std::shared_ptr<const Leaves> coarse_;
    world::WorldBuilder::Snapshot world_;
    std::optional<world::HeightField> field_;
    std::shared_ptr<const world::ecology::Delta> delta_;
    std::uint64_t ecologyRevision_=~std::uint64_t(0);
    using KeyHash=engine::render::ImpostorKeyHash;
    std::unordered_map<engine::render::ImpostorKey,std::uint64_t,KeyHash> edits_; // per-node hash of ecology edits
    std::unordered_map<engine::render::ImpostorKey,double,KeyHash> ground_;       // memoised node ground height
    std::unordered_map<engine::render::ImpostorKey,std::uint64_t,KeyHash> failed_;
    std::vector<Job> jobs_;
    std::vector<Draw> drawn_;
    std::vector<engine::render::ImpostorKey> keys_; // drawn keys, sorted
    std::unordered_map<engine::render::ImpostorKey,double,engine::render::ImpostorKeyHash> fade_;
    bool coveredOld(double x,double y) const;
    std::vector<engine::render::ImpostorKey> previous_;
    std::uint64_t generation_=0,bakes_=0,failures_=0,drawnTotal_=0;
    std::size_t visited_=0,wanted_=0;
    engine::render::HierarchyCut last_;
    engine::render::HierarchyCut lastCut_;   // reused between walks, see update()
    engine::camera::ViewState lastView_;
    double lastReach_=0;
    int sinceCut_=0;
    bool haveCut_=false;
    std::array<std::size_t,4> wantedAt_{},bakedAt_{};
    std::array<double,4> totalSum_{},viewSum_{},msSum_{};
    double clock_=0,pressure_=1,groundFallback_=0,updateMs_=0,worstMs_=0,uploadMs_=0;
    int groundBudget_=0;
    bool limited_=false,ready_=false;
};
} // namespace game

