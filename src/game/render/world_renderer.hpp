#pragma once

#include <functional>
#include "engine/pipeline/runner.hpp"
#include "engine/pipeline/pass.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "game/client/camera.hpp"
#include "game/content/ground_materials.hpp"
#include "game/render/calc/sprite_queue.hpp"
#include "game/render/climate_textures.hpp"
#include "game/world/environment.hpp"
#include "game/world/weather.hpp"
#include "game/world/world_system.hpp"

namespace engine { class RenderPipeline; }
namespace game {
class TerrainCollectPass;
class SpritePass;
class SceneModelsPass;
class FoliagePass;
class GpuTerrain;

struct WorldRenderSettings {
    world::weather::Snapshot weather;
    world::MapView map = world::MapView::Natural;
    generation::TerrainStage stage = generation::TerrainStage::Final;
    bool iceVisible = true, potentialOnly = false;
    std::array<float, 4> floodBounds{-1, -1, -1, -1};
};

// A presentation consumer, not a world generator. All passes share one runner,
// one instance arena and one read lease. A new publication invalidates the whole
// render state before anything can mix resources from different worlds.
class WorldRenderer {
public:
    using Overlay = std::function<std::unique_ptr<engine::DrawPass>(const GpuTerrain&)>;
    WorldRenderer();
    ~WorldRenderer();
    bool open(SDL_Window* window, const std::filesystem::path& assets,
              world::WorldSystem& source, Overlay overlay = {});
    bool draw(const client::Camera& camera, const WorldRenderSettings& settings = {});
    bool screenshot(const std::string& path);
    void holdTime(double seconds);
    void forgetTheWorld();
    std::vector<content::GroundMaterial>& ground() { return ground_; }
    SpriteQueue& sprites() { return spriteQueue_; }
    bool settled() const;
    const GpuTerrain& terrain() const;
    const std::string& error() const { return device_.error(); }
    const engine::Runner& runner() const { return *runner_; }
    const TerrainCollectPass* collector() const { return collect_; }
    const engine::RenderPipeline* drawing() const { return render_; }
    const SpritePass* spriteDrawing() const { return sprites_; }
    double frameMillis() const { return runner_->frameMillis(); }
    bool terrainSkirts() const { return skirts_; }
    void toggleTerrainSkirts();
    void cycleTerrainGrid();
    // Draws scene objects as edges: what the cluster cut, the impostor cards
    // and the region aggregates actually put on screen.
    void toggleObjectWireframe();
    [[nodiscard]] bool objectWireframe() const;
#if ASR_ENABLE_PROFILING
    std::string vegetationReport() const;
    bool compareGrassCulling(const client::Camera& camera, const WorldRenderSettings& settings,
                             const std::string& path);
#endif
private:
    bool synchronize();
    world::WorldSystem* source_ = nullptr; // requests/leases, never worker callbacks
    world::WorldBuilder::Snapshot world_;
    std::shared_ptr<world::WorldPreparation> preparation_;
    engine::Device device_; // outlives every GPU resource below
    client::Camera camera_;
    std::unique_ptr<world::HeightField> field_;
    engine::MeshCache cache_;
    ClimateTextures climate_;
    std::vector<content::GroundMaterial> ground_;
    SpriteQueue spriteQueue_;
    std::unique_ptr<engine::Runner> runner_;
    TerrainCollectPass* collect_ = nullptr;
    SpritePass* sprites_ = nullptr;
    SceneModelsPass* models_ = nullptr;
    bool objectWireframe_ = false;
    FoliagePass* foliage_ = nullptr;
    engine::RenderPipeline* render_ = nullptr;
    Overlay overlay_;
    double heldTime_ = -1;
    bool skirts_ = true;
    int grid_ = 0;
};
} // namespace game
