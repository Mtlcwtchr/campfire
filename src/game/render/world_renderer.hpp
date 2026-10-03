#pragma once
#include "game/render/passes/character_pass.hpp"
#include <chrono>
#include <filesystem>

#include <functional>
#include "engine/pipeline/runner.hpp"
#include "engine/pipeline/pass.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "game/client/camera.hpp"
#include "game/content/ground_materials.hpp"
#include "game/render/calc/sprite_queue.hpp"
#include "game/render/climate_textures.hpp"
#include "game/render/graphics_settings.hpp"
#include "game/render/terrain_look.hpp"
#include "game/render/passes/sketch_pass.hpp"
#include "game/render/passes/highlight_pass.hpp"
#include "game/world/environment.hpp"
#include "game/world/weather.hpp"
#include "game/world/world_system.hpp"

namespace engine { class RenderPipeline; }
namespace game {
class TerrainCollectPass;
class SpritePass;
class SceneModelsPass;
class FoliagePass;
class FarTreesPass;
class GpuTerrain;
class ShadowClipmap;

struct WorldRenderSettings {
    world::weather::Snapshot weather;
    world::MapView map = world::MapView::Natural;
    generation::TerrainStage stage = generation::TerrainStage::Final;
    bool iceVisible = true, potentialOnly = false;
    bool shadows = true;
    std::array<float,3> sunDirection{-0.55f,-0.55f,0.63f};
    std::array<float, 4> floodBounds{-1, -1, -1, -1};
    // Metres of ground drawn from the eye. The distance fog is opaque there,
    // and the far forest and object placement stop there. Perspective only.
    double drawDistance = 30000;
    bool fog = true;
    // Everything the graphics settings panel controls. `drawDistance`, `fog`,
    // `shadows` and `sunDirection` above are overridden from it when
    // `useGraphics` is set (the explorer), and kept for older callers.
    GraphicsSettings graphics;
    bool useGraphics = false;
    // The camera every decision (culling, LOD, streaming, placement, shadows)
    // is made from. Null: the drawing camera. Set by the scene view to inspect
    // a frozen frame from elsewhere without recomputing it.
    const client::Camera* cull = nullptr;
    // The world editor's overlay (region grid, selection, brush), laid on the
    // terrain and the water as-is: see assets/shaders/editor_overlay.hlsli for
    // what each number means. All zero draws nothing.
    std::array<std::array<float, 4>, engine::kSceneEditorVectors> editor{};
};

// A presentation consumer, not a world generator. All passes share one runner,
// one instance arena and one read lease. A new publication invalidates the whole
// render state before anything can mix resources from different worlds.
class WorldRenderer {
public:
    using Overlay = std::function<std::unique_ptr<engine::DrawPass>(const GpuTerrain&)>;
    WorldRenderer();
    ~WorldRenderer();
    // A null window with a size opens headless: nothing is presented.
    bool open(SDL_Window* window, const std::filesystem::path& assets,
              world::WorldSystem& source, Overlay overlay = {},
              int headlessWidth = 0, int headlessHeight = 0);
    bool draw(const client::Camera& camera, const WorldRenderSettings& settings = {});
    // The player's character for the next frames (CharacterPass); not visible
    // until set. Kept here, not in the pass, so a rebuilt pipeline keeps it.
    void character(const CharacterPass::State& state) { characterState_ = state; }
    [[nodiscard]] const CharacterPass* characterPass() const { return character_; }
    bool screenshot(const std::string& path);
    void holdTime(double seconds);
    void forgetTheWorld();
    // The world editor's coast sketch (SketchPass); null draws none. Kept
    // across worlds: a sketch is the person's, not the world's.
    void sketch(std::shared_ptr<const generation::SketchMesh> mesh) {
        sketch_.mesh = std::move(mesh);
        ++sketch_.revision;
    }
    // What the editor has picked out, as line segments (HighlightPass): pairs
    // of points, x y z. Null or empty draws nothing.
    void highlight(std::shared_ptr<const std::vector<float>> lines) {
        highlight_.lines = std::move(lines);
        ++highlight_.revision;
    }
    std::vector<content::GroundMaterial>& ground() { return ground_; }
    SpriteQueue& sprites() { return spriteQueue_; }
    bool settled() const;
    // A picture of the world before is on the screen while the new one comes in.
    bool holding() const { return bool(held_); }
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
    [[nodiscard]] int terrainGrid() const { return grid_; }
    void setTerrainGrid(int grid) { while (grid_ != grid % 3) cycleTerrainGrid(); }
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
    // The terrain categories' config, watched (engine/biomes): numbers go
    // to the table live, a new structure rebuilds the shaders once.
    void pollBiomes();
    std::chrono::steady_clock::time_point biomesPolled_{};
    std::filesystem::file_time_type biomesWritten_{};
    // content/config/terrain_look.json, read again when it is written.
    TerrainLook terrainLook_{};
    std::filesystem::file_time_type terrainLookWritten_{};
    bool rebuildShaders_ = false;
    bool restoredShaders_ = false;
    world::WorldSystem* source_ = nullptr; // requests/leases, never worker callbacks
    world::WorldBuilder::Snapshot world_;
    std::shared_ptr<world::WorldPreparation> preparation_;
    engine::Device device_; // outlives every GPU resource below
    client::Camera camera_;
    std::unique_ptr<world::HeightField> field_;
    engine::MeshCache cache_;
    ClimateTextures climate_;
    std::unique_ptr<ShadowClipmap> shadows_;
    std::vector<content::GroundMaterial> ground_;
    SpriteQueue spriteQueue_;
    std::unique_ptr<engine::Runner> runner_;
    TerrainCollectPass* collect_ = nullptr;
    SpritePass* sprites_ = nullptr;
    SceneModelsPass* models_ = nullptr;
    FarTreesPass* farTrees_ = nullptr;
    CharacterPass* character_ = nullptr;
    CharacterPass::State characterState_;
    bool objectWireframe_ = false;
    FoliagePass* foliage_ = nullptr;
    engine::RenderPipeline* render_ = nullptr;
    Overlay overlay_;
    double heldTime_ = -1;
    bool skirts_ = true;
    int grid_ = 0;
    // Multisampling the render state was built with, and what is wanted. A
    // pipeline cannot change its sample count, so a change rebuilds.
    int builtSamples_ = 0, wantedSamples_ = 4;
    bool builtHalfTextures_ = false, wantedHalfTextures_ = false;
    std::array<float, 3> skyHorizon_{0.68f, 0.71f, 0.70f}, skyZenith_{0.43f, 0.55f, 0.66f};
    bool skyMeasured_ = false, skyAvailable_ = false;
    // The replaced world's last picture (HoldPass), while the new one's ground
    // is on its way; empty otherwise. Owned here: it outlives the pipeline
    // that drew it and the one that draws it now.
    engine::Texture held_;
    int heldFrames_ = 0;
    SketchSource sketch_;
    HighlightSource highlight_;
    // Where the camera was when the picture was held: a picture of another
    // view is not the world, and it goes the moment the camera moves.
    std::array<double, 6> heldView_{}, drawnView_{};   // the held picture's view; the last frame's
    std::array<double, 6> viewOf(const client::Camera& camera) const {
        return {camera.centreX, camera.centreY, camera.yaw, camera.pitch, camera.pixelsPerTile, camera.focusHeight};
    }
    bool graded_ = false;   // the last frame drew the grade, so its copy was taken
    void holdThePicture();
};
} // namespace game
