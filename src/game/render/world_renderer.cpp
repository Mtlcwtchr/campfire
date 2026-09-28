#include "game/render/world_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include "engine/pipeline/calc_pipeline.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/calc/terrain_collect.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/passes/foliage_pass.hpp"
#include "game/render/passes/scene_models_pass.hpp"
#include "game/render/passes/far_trees_pass.hpp"
#include "game/render/passes/sky_pass.hpp"
#include "game/render/passes/frustum_pass.hpp"
#include "game/render/passes/sprite_pass.hpp"
#include "game/render/passes/terrain_pass.hpp"
#include "game/render/passes/water_pass.hpp"
#include "game/render/passes/weather_pass.hpp"
#include "game/render/shadow_clipmap.hpp"
#include "game/world/foliage_catalog.hpp"
#include "game/world/ring_mesh.hpp"

namespace game {
WorldRenderer::WorldRenderer() = default;
WorldRenderer::~WorldRenderer() = default;

bool WorldRenderer::open(SDL_Window* window, const std::filesystem::path& assets,
                         world::WorldSystem& source, Overlay overlay,
                         int headlessWidth, int headlessHeight) {
    source_ = &source;
    overlay_ = std::move(overlay);
    std::filesystem::path content = "content";
    for (int up = 0; up < 5 && !std::filesystem::exists(content); ++up) content = ".." / content;
    ground_ = content::loadGroundMaterials(content / "config" / "ground.json");
    skirts_ = std::getenv("ASR_TERRAIN_NO_SKIRTS") == nullptr;
    if (!window)
        return device_.openHeadless(std::uint32_t(std::max(1, headlessWidth)),
                                    std::uint32_t(std::max(1, headlessHeight)), assets) &&
               synchronize();
    return device_.open(window, assets) && synchronize();
}

void WorldRenderer::forgetTheWorld() {
    runner_.reset(); // pass destructors drain their workers while the lease is valid
    collect_ = nullptr; sprites_ = nullptr; models_ = nullptr; foliage_ = nullptr; farTrees_ = nullptr; render_ = nullptr;
    cache_.clear();
    climate_ = {};
    shadows_.reset();
    field_.reset();
    preparation_.reset();
    world_.reset();
}

bool WorldRenderer::synchronize() {
    const auto next = source_->read();
    if (next && next == world_ && runner_ && builtSamples_ == wantedSamples_ &&
        builtHalfTextures_ == wantedHalfTextures_) return true;
    if (!next) { device_.fail("world renderer requires a published world"); return false; }
    forgetTheWorld();
    builtSamples_ = wantedSamples_;
    builtHalfTextures_ = wantedHalfTextures_;
    if (!skyMeasured_) {
        // The panorama's own measured horizon and zenith become the fog colour.
        skyMeasured_ = true;
        std::ifstream input(device_.assets().parent_path() / "generated/sky/sky.json");
        const auto sky = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json();
        const auto read = [&](const char* key, std::array<float, 3>& into) {
            if (!sky.is_object() || !sky.contains(key) || !sky[key].is_array() || sky[key].size() != 3) return false;
            for (int i = 0; i < 3; ++i) into[std::size_t(i)] = sky[key][std::size_t(i)].get<float>();
            return true;
        };
        skyAvailable_ = read("horizon", skyHorizon_) && read("zenith", skyZenith_);
    }
    world_ = next;
    preparation_ = source_->prepare(next);
    if (!preparation_) { device_.fail("world publication changed while acquiring preparation"); return false; }
    field_ = std::make_unique<world::HeightField>(&world_->worldMap(), world_->worldMap().seed);
    shadows_ = std::make_unique<ShadowClipmap>(world_);
    if (!shadows_->setup(device_)) { forgetTheWorld(); return false; }
    runner_ = std::make_unique<engine::Runner>();
    runner_->samples(wantedSamples_);
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
    // Layer order is the shader's (terrain_material.hlsli, GROUND_LAYERS):
    // the six blended classes first, then their climate/water/slope variants.
    // All Poly Haven scans (CC0), see tools/terrain_set_polyhaven.py.
    std::vector<std::string> maps;
    for (const char* asset : {"leafy_grass", "dirt_floor", "red_sand", "rocks_ground_05", "brown_mud_02",
                              "snow_02", "withered_grass", "forest_leaves_02", "coast_sand_04",
                              "damp_beach_sand", "sand_01", "sandy_gravel_02", "rock_face_03", "mossy_rock",
                              "cliff_side", "mud_cracked_dry_riverbed_002"})
        maps.push_back(std::string("../terrain/ph/") + asset + "/ph_" + asset);
    auto* terrain = &collect_->gpuTerrain();
    drawing->add(std::make_unique<TerrainPass>(cache_, std::move(materials), maps, climate_, world_->climate(), shadows_->binding(), terrain))
        ->halfTextures(wantedHalfTextures_);
    std::vector<std::string> grass;
    const auto modelDirectory = device_.assets() / "../generated/scene_models";
    if (std::filesystem::exists(modelDirectory / ".ue-imported")) {
        std::ifstream input(modelDirectory / "manifest.json");
        const auto manifest = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json();
        if (!manifest.is_object() || !manifest.contains("grass") || !manifest["grass"].is_array() ||
            manifest["grass"].size() != world::foliage::kWildGrassSprites.size()) {
            device_.fail("UE scene catalogue requires six imported grass views");
            return false;
        }
        for (const auto& entry : manifest["grass"]) {
            if (!entry.is_string()) { device_.fail("invalid UE grass filename"); return false; }
            const auto name = entry.get<std::string>();
            const std::filesystem::path relative(name);
            if (relative.empty() || relative.has_parent_path() || !std::filesystem::is_regular_file(modelDirectory / relative)) {
                device_.fail("missing or unsafe UE grass view: " + name);
                return false;
            }
            grass.push_back("../generated/scene_models/" + name);
        }
    } else {
        for (int i : world::foliage::kWildGrassSprites)
            grass.push_back("kenney_foliageSprites/PNG/Shaded/sprite_00" + std::to_string(i) + ".png");
    }
    std::vector<std::string> spriteImages{grass.front(), grass.back()};
    foliage_ = drawing->add(std::make_unique<FoliagePass>(cache_, std::move(grass), shadows_->binding(), terrain));
    models_ = drawing->add(std::make_unique<SceneModelsPass>(preparation_->placement, shadows_->binding(), terrain));
    farTrees_ = drawing->add(std::make_unique<FarTreesPass>(shadows_->binding(), terrain));
    const auto& map=world_->worldMap();
    const double worldWidth=double(map.width)*generation::kMetresPerCell;
    const double worldHeight=double(map.height)*generation::kMetresPerCell;
    const auto heights=world::ringHeightBounds(map,{},
        {core::Fixed::fromDoubleForContent(worldWidth),core::Fixed::fromDoubleForContent(worldHeight)},0);
    models_->worldBounds({0,0,worldWidth,worldHeight,heights.first,heights.second});
    models_->wireframe(objectWireframe_);
    drawing->add(std::make_unique<WaterPass>(cache_, climate_, world_->climate(), terrain));
    drawing->add(std::make_unique<SkyPass>());
    drawing->add(std::make_unique<FrustumPass>());
    drawing->add(std::make_unique<WeatherPass>());
    sprites_ = drawing->add(std::make_unique<SpritePass>(spriteQueue_, std::move(spriteImages)));
    if (overlay_) if (auto overlay = overlay_(*terrain)) drawing->add(std::move(overlay));
    render_ = runner_->add(std::move(drawing));
    if (runner_->build(device_)) return true;
    forgetTheWorld();
    return false;
}

bool WorldRenderer::draw(const client::Camera& camera, const WorldRenderSettings& requested) {
    WorldRenderSettings settings = requested;
    const GraphicsSettings& graphics = settings.graphics;
    if (settings.useGraphics) {
        settings.drawDistance = double(graphics.drawDistanceKm) * 1000.0;
        settings.fog = graphics.fog;
        settings.shadows = settings.shadows && graphics.shadows;
        settings.sunDirection = graphics.sunDirection();
        wantedSamples_ = graphics.antialiasing ? 4 : 1;
        wantedHalfTextures_ = graphics.terrainTextures == 0;
    }
    // Decisions from the cull camera, pixels from the drawing camera.
    const client::Camera& cull = settings.cull ? *settings.cull : camera;
    camera_ = cull;
    if (!synchronize()) return false;
    auto state = cull.viewState();
    if (settings.useGraphics) {
        const double scale = graphics.lodScale();
        // Objects only: the terrain has its own error budget. A tree drawn at
        // a pixel of error is L0 out to kilometres; this is the knob for that.
        const double objects = scale * double(graphics.objectError);
        state.quality.geometryErrorPx *= objects;
        state.quality.impostorErrorPx *= objects;
        state.quality.parallaxErrorPx *= objects;
    }
    models_->view(state);
    models_->focus(cull.centreX, cull.centreY);
    if (settings.useGraphics) {
        models_->forestOptions(graphics.forestProxies, graphics.farForest, graphics.massClusters);
        models_->vegetationMesh(graphics.vegetationMeshPixels,
            std::size_t(graphics.vegetationMeshKiloTriangles) * 1000);
        const bool far = graphics.farTrees && farTrees_ != nullptr;
        models_->farTrees(far ? graphics.farTreesStart : 0.0, FarTreesPass::kBand);
        if (farTrees_) farTrees_->options(graphics.farTrees, graphics.farTreesStart);
    }
    foliage_->focus(cull.centreX, cull.centreY);
    auto& terrain = collect_->gpuTerrain();
    terrain.stage(settings.stage);
    auto scene = sceneFor(camera, ground_);
    {
        const auto decided = settings.cull ? sceneFor(cull, ground_) : scene;
        std::copy(std::begin(decided.viewProjection), std::end(decided.viewProjection), scene.cullViewProjection);
        std::copy(std::begin(decided.camera), std::end(decided.camera), scene.cullCamera);
        scene.cullState[0] = 1.0f;
        scene.cullState[1] = settings.cull ? 1.0f : 0.0f;
    }
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
    // Distance fog, from the draw distance: clear for the first third, then a
    // smooth rise to opaque at the draw distance itself. Inspection maps and
    // orthographic views measure no eye distance and are never fogged.
    const double reach = std::clamp(settings.drawDistance, 500.0, 400000.0);
    const bool fogged = settings.fog && camera.perspective() && settings.map == world::MapView::Natural;
    scene.fog[0] = float(reach);
    scene.fog[1] = float(reach * (settings.useGraphics ? graphics.fogStart : 0.3));
    scene.fog[2] = fogged ? 1.0f : 0.0f;
    if (settings.useGraphics) {
        scene.look[0] = graphics.sunIntensity; scene.look[1] = graphics.ambient;
        scene.look[2] = graphics.exposure; scene.look[3] = 1.0f;
        scene.quality[0] = graphics.terrainBlend; scene.quality[1] = graphics.shadowSoftness;
        const bool sky = skyAvailable_ && graphics.skybox;
        for (int i = 0; i < 3; ++i) {
            scene.skyHorizon[i] = skyAvailable_ ? skyHorizon_[std::size_t(i)] : 0.68f;
            scene.skyZenith[i] = skyAvailable_ ? skyZenith_[std::size_t(i)] : 0.55f;
        }
        scene.skyHorizon[3] = sky ? 1.0f : 0.0f;
        scene.skyZenith[3] = float(graphics.skyRotation * 3.14159265358979 / 180.0);
        scene.clouds[0] = graphics.cloudCoverage; scene.clouds[1] = graphics.cloudDensity;
        scene.clouds[2] = graphics.cloudAltitude;
        scene.clouds[3] = float(graphics.cloudSteps()); // procedural: no content needed
    }
    if (models_) models_->drawDistance(camera.perspective() ? reach : 400000.0);
    if (models_)
        models_->prepareDensity(scene, cull.viewportWidth, scene);
    if (!shadows_->update(device_,scene,settings.sunDirection,settings.shadows &&
        std::getenv("CAMPFIRE_NO_SHADOWS")==nullptr && settings.map==world::MapView::Natural && terrain.finalStage())) return false;
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
