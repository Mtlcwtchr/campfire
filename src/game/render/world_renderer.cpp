#include "game/render/world_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "engine/pipeline/calc_pipeline.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/calc/terrain_collect.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/passes/foliage_pass.hpp"
#include "game/render/passes/scene_models_pass.hpp"
#include "game/render/passes/sprite_pass.hpp"
#include "game/render/passes/terrain_pass.hpp"
#include "game/render/passes/water_pass.hpp"
#include "game/render/passes/weather_pass.hpp"
#include "game/world/foliage_catalog.hpp"
#include "game/world/ring_mesh.hpp"

namespace game {
WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() = default;

bool WorldRenderer::open(SDL_Window* window, const std::filesystem::path& assets,
                         world::WorldSystem& source, Overlay overlay) {
    source_ = &source;
    overlay_ = std::move(overlay);
    std::filesystem::path content = "content";
    for (int up = 0; up < 5 && !std::filesystem::exists(content); ++up) content = ".." / content;
    ground_ = content::loadGroundMaterials(content / "config" / "ground.json");
    skirts_ = std::getenv("ASR_TERRAIN_NO_SKIRTS") == nullptr;
    return device_.open(window, assets) && synchronize();
}

void WorldRenderer::forgetTheWorld() {
    runner_.reset(); // pass destructors drain their workers while the lease is valid
    collect_ = nullptr; sprites_ = nullptr; models_ = nullptr; foliage_ = nullptr; render_ = nullptr;
    cache_.clear();
    climate_ = {};
    field_.reset();
    preparation_.reset();
    world_.reset();
}

bool WorldRenderer::synchronize() {
    const auto next = source_->read();
    if (next && next == world_ && runner_) return true;
    if (!next) { device_.fail("world renderer requires a published world"); return false; }
    forgetTheWorld();
    world_ = next;
    preparation_ = source_->prepare(next);
    if (!preparation_) { device_.fail("world publication changed while acquiring preparation"); return false; }
    field_ = std::make_unique<world::HeightField>(&world_->worldMap(), world_->worldMap().seed);
    runner_ = std::make_unique<engine::Runner>();
    runner_->holdTime(heldTime_);
    runner_->add(std::make_unique<engine::CalcPipeline>(engine::PipelineId(Phase::World), engine::Cadence::Fixed));
    auto prepare = std::make_unique<engine::CalcPipeline>(engine::PipelineId(Phase::Prepare), engine::Cadence::EveryFrame);
    collect_ = prepare->add(std::make_unique<TerrainCollectPass>(preparation_, camera_, cache_));
    collect_->gpuTerrain().skirts(skirts_);
    for (int i = 0; i < grid_; ++i) collect_->gpuTerrain().cycleGrid();
    runner_->add(std::move(prepare));

    auto drawing = std::make_unique<engine::RenderPipeline>(engine::PipelineId(Phase::Render));
    drawing->stages({engine::StageInfo{true, {0.07f, 0.155f, 0.195f, 1.0f}, true, true}});
    std::vector<std::string> materials;
    for (std::size_t i = 0; i < content::kBlendedMaterials && i < ground_.size(); ++i)
        materials.push_back("ground/" + ground_[i].name);
    const std::vector<std::string> maps{"../terrain/grass/lush/grass_lush", "../terrain/soil/base/soil_base",
        "../terrain/sand/dry/sand_dry", "../terrain/stone/rock/rock_ground", "../terrain/soil/mud_wet/mud_wet",
        "../terrain/snow/clean/snow_clean"};
    auto* terrain = &collect_->gpuTerrain();
    drawing->add(std::make_unique<TerrainPass>(cache_, std::move(materials), maps, climate_, world_->climate(), terrain));
    std::vector<std::string> grass;
    for (int i : world::foliage::kWildGrassSprites)
        grass.push_back("kenney_foliageSprites/PNG/Shaded/sprite_00" + std::to_string(i) + ".png");
    foliage_ = drawing->add(std::make_unique<FoliagePass>(cache_, std::move(grass), terrain));
    models_ = drawing->add(std::make_unique<SceneModelsPass>(preparation_->placement, terrain));
    const auto& map=world_->worldMap();
    const double worldWidth=double(map.width)*generation::kMetresPerCell;
    const double worldHeight=double(map.height)*generation::kMetresPerCell;
    const auto heights=world::ringHeightBounds(map,{},
        {core::Fixed::fromDoubleForContent(worldWidth),core::Fixed::fromDoubleForContent(worldHeight)},0);
    models_->worldBounds({0,0,worldWidth,worldHeight,heights.first,heights.second});
    models_->wireframe(objectWireframe_);
    drawing->add(std::make_unique<WaterPass>(cache_, climate_, world_->climate(), terrain));
    drawing->add(std::make_unique<WeatherPass>());
    sprites_ = drawing->add(std::make_unique<SpritePass>(spriteQueue_, std::vector<std::string>{
        "kenney_foliageSprites/PNG/Shaded/sprite_0052.png", "kenney_foliageSprites/PNG/Shaded/sprite_0057.png"}));
    if (overlay_) if (auto overlay = overlay_(*terrain)) drawing->add(std::move(overlay));
    render_ = runner_->add(std::move(drawing));
    if (runner_->build(device_)) return true;
    forgetTheWorld();
    return false;
}

bool WorldRenderer::draw(const client::Camera& camera, const WorldRenderSettings& settings) {
    camera_ = camera;
    if (!synchronize()) return false;
    models_->focus(camera.centreX, camera.centreY);
    foliage_->focus(camera.centreX, camera.centreY);
    auto& terrain = collect_->gpuTerrain();
    terrain.stage(settings.stage);
    auto scene = sceneFor(camera, ground_);
    auto weather = settings.weather;
    const core::WorldPos focus{core::Fixed::fromDoubleForContent(camera.centreX),
                               core::Fixed::fromDoubleForContent(camera.centreY)};
    const auto climate = field_->surfaceClimateAt(focus).environment;
    const auto scalar = [](core::Fixed f) { return float(f.toDouble()); };
    const auto local = weather.at(scalar(climate[0]), scalar(climate[2]), float(camera.focusHeight),
        float(camera.centreX), float(camera.centreY), scalar(climate[5]));
    weather.data[1] = {scalar(climate[0]), scalar(climate[2]), scalar(climate[5]), settings.iceVisible ? 0.0f : 1.0f};
    weather.data[2] = {local.air.temperature, local.air.precipitation, local.air.cloud, local.air.wind};
    for (std::size_t i = 0; i < weather.data.size(); ++i)
        std::copy(weather.data[i].begin(), weather.data[i].end(), scene.parameters[i]);
    const double wide = std::max(1, world_->climate().wide()), high = std::max(1, world_->climate().high());
    scene.parameters[20][0] = float(1.0 / (world::ClimateField::kMetres * wide));
    scene.parameters[20][1] = float(1.0 / (world::ClimateField::kMetres * high));
    scene.parameters[20][2] = float(0.5 / wide); scene.parameters[20][3] = float(0.5 / high);
    scene.extra[3] = float(settings.map);
    if (settings.map != world::MapView::Natural || !terrain.finalStage()) scene.extra[2] = 0;
    const float windLength = std::hypot(scalar(climate[3]), scalar(climate[4]));
    if (windLength > 0.001f) { scene.wind[0] = scalar(climate[3]) / windLength; scene.wind[1] = scalar(climate[4]) / windLength; }
    scene.wind[2] = local.air.wind;
    scene.table[6][0] = 0; scene.table[6][1] = 1; scene.table[6][2] = settings.potentialOnly ? 1 : 0;
    scene.table[6][3] = float(std::max(4.0, std::pow(2.0, std::round(std::log2(100.0 / std::max(0.001, camera.pixelsPerTile))))));
    std::copy(settings.floodBounds.begin(), settings.floodBounds.end(), scene.table[7]);
    if (models_)
        models_->prepareDensity(scene, camera.viewportWidth, scene);
    return runner_->frame(device_, scene);
}

void WorldRenderer::holdTime(double seconds) { heldTime_ = seconds; if (runner_) runner_->holdTime(seconds); }
bool WorldRenderer::settled() const {
    return collect_ && terrain().settled() && models_ && models_->ready();
}
const GpuTerrain& WorldRenderer::terrain() const { return collect_->gpuTerrain(); }
void WorldRenderer::toggleTerrainSkirts() { skirts_ = !skirts_; if (collect_) collect_->gpuTerrain().skirts(skirts_); }
void WorldRenderer::cycleTerrainGrid() { grid_ = (grid_ + 1) % 3; if (collect_) collect_->gpuTerrain().cycleGrid(); }
void WorldRenderer::toggleObjectWireframe() {
    objectWireframe_ = !objectWireframe_;
    if (models_) models_->wireframe(objectWireframe_);
    // The pass refuses the mode if its line pipeline would not build, so ask
    // it what it actually did rather than reporting the request.
    if (models_) objectWireframe_ = models_->wireframe();
}
bool WorldRenderer::objectWireframe() const { return models_ && models_->wireframe(); }

#if !ASR_ENABLE_PROFILING
bool WorldRenderer::screenshot(const std::string& path) { return runner_->screenshot(device_, path); }
#endif
} // namespace game
