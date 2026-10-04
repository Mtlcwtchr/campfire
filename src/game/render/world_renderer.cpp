#include "game/render/world_renderer.hpp"

#include "engine/biomes/registry.hpp"
#include "engine/environment/environment.hpp"
#include "engine/biomes/shader_code.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
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
#include "engine/render/passes/grade_pass.hpp"
#include "game/render/passes/sprite_pass.hpp"
#include "game/render/passes/terrain_pass.hpp"
#include "game/render/passes/water_pass.hpp"
#include "game/render/passes/weather_pass.hpp"
#include "game/render/passes/hold_pass.hpp"
#include "game/render/passes/sketch_pass.hpp"
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
    {
        // The terrain categories (engine/biomes): read, checked, made active
        // and their shader code written before the first pipeline is built.
        const auto dir = engine::biomes::defaultDirectory();
        biomesWritten_ = engine::biomes::registryWritten(dir);
        const auto started = engine::biomes::startUp(dir, assets.parent_path() / "shaders", assets.parent_path());
        for (const auto& problem : started.problems) std::cerr << "terrain categories: " << problem << "\n";
    }
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

void WorldRenderer::holdThePicture() {
    // Only a picture there is: the copy between the surface and the post
    // stages is taken when the grade runs, and it is the world without the
    // interface, which goes on being drawn live over whatever is held.
    if (!runner_ || !graded_) return;
    const auto& targets = runner_->targets();
    if (!targets.grab() || !targets.width() || !targets.height()) return;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = engine::Device::kColourFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = targets.width();
    info.height = targets.height();
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    engine::Texture held = device_.makeTexture(info);
    SDL_GPUCommandBuffer* commands = held ? SDL_AcquireGPUCommandBuffer(device_.handle()) : nullptr;
    if (!commands) return;
    SDL_GPUBlitInfo blit{};
    blit.source.texture = targets.grab();
    blit.source.w = targets.width();
    blit.source.h = targets.height();
    blit.destination.texture = held.get();
    blit.destination.w = targets.width();
    blit.destination.h = targets.height();
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(commands, &blit);
    if (!SDL_SubmitGPUCommandBuffer(commands)) return;
    held_ = std::move(held);
    heldFrames_ = 0;
    heldView_ = drawnView_;   // the view the copied frame was drawn from
}

void WorldRenderer::forgetTheWorld() {
    runner_.reset(); // pass destructors drain their workers while the lease is valid
    collect_ = nullptr; sprites_ = nullptr; models_ = nullptr; foliage_ = nullptr; farTrees_ = nullptr; character_ = nullptr; render_ = nullptr;
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
        builtHalfTextures_ == wantedHalfTextures_ && !rebuildShaders_) return true;
    if (!next) { device_.fail("world renderer requires a published world"); return false; }
    holdThePicture();
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
    // World, Surface (reads a copy of World), Post (reads a copy of everything,
    // blurred down its mip chain), Interface. Aggregates in StageInfo's order:
    // clear colour, its value, depth, clear depth, grab, grab mips.
    drawing->stages({engine::StageInfo{true, {0.07f, 0.155f, 0.195f, 1.0f}, true, true},
                     engine::StageInfo{false, {0, 0, 0, 1}, true, false, true},
                     engine::StageInfo{false, {0, 0, 0, 1}, true, false, true, true},
                     engine::StageInfo{false, {0, 0, 0, 1}, true, false}});
    std::vector<std::string> materials;
    for (std::size_t i = 0; i < content::kBlendedMaterials && i < ground_.size(); ++i)
        materials.push_back("ground/" + ground_[i].name);
    // Layer order is the shader's (terrain_layers.hlsli, GROUND_LAYERS): the
    // six blended classes first, then their climate/water/slope variants, then
    // the terrain categories' own grounds - the texture catalogue of
    // content/config/terrain/layers.json (engine/biomes). Each source retains
    // its provenance; an explicit stem also admits downloaded Fab textures.
    std::vector<std::string> maps;
    {
        std::vector<std::string> names;
        if (const auto registry = engine::biomes::active())
            for (const auto& layer : registry->textureLayers()) {
                names.push_back(layer.name);
                maps.push_back(layer.path.empty() ? "../terrain/ph/" + layer.name + "/ph_" + layer.name : "../" + layer.path);
            }
        if (names.size() < std::size(engine::biomes::kBuiltInLayers)) {
            maps.clear();
            names.assign(std::begin(engine::biomes::kBuiltInLayers), std::end(engine::biomes::kBuiltInLayers));
            for (const char* own : {"moon_dusted_04", "rubble", "mud_forest", "brown_mud_leaves_01", "burned_ground_01",
                                    "red_laterite_soil_stones"})
                names.push_back(own);
        }
        if (maps.empty()) for (const auto& asset : names) maps.push_back("../terrain/ph/" + asset + "/ph_" + asset);
    }
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
    // The ground flora after the six grass views (layers 6..15, foliage.hlsl
    // kCard*): flowers, ferns, reeds, dry and short grass, drawn by
    // tools/make_foliage_cards.py. Without them the layers repeat the grass,
    // so the shader's indices always exist.
    {
        // 10 drawn by make_foliage_cards.py, then 6 species from make_flower_cards.py.
        constexpr std::size_t kFloraCards = 16, kDrawnCards = 10;
        const auto flora = device_.assets() / "../generated/foliage_cards";
        std::ifstream input(flora / "cards.json");
        const auto list = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json();
        std::vector<std::string> cards;
        if (list.is_object() && list.contains("cards") && list["cards"].is_array())
            for (const auto& entry : list["cards"]) {
                if (!entry.is_string()) break;
                const std::filesystem::path name(entry.get<std::string>());
                if (name.empty() || name.has_parent_path() || !std::filesystem::is_regular_file(flora / name)) break;
                cards.push_back("../generated/foliage_cards/" + name.string());
            }
        // Only the drawn ones so far: the species layers repeat the white flowers.
        if (cards.size() == kDrawnCards)
            while (cards.size() < kFloraCards) cards.push_back(cards[2]);
        if (cards.size() != kFloraCards) {
            std::cerr << "ground flora cards missing (run tools/make_foliage_cards.py); grass stands in\n";
            cards.clear();
            for (std::size_t i = 0; i < kFloraCards; ++i) cards.push_back(grass[i % grass.size()]);
        } else if (list.contains("meadow") && list["meadow"].is_array() && list["meadow"].size() == grass.size()) {
            // The same six views with their empty texels filled from the
            // blades, so the mip chain has no dark rim round a far clump.
            std::vector<std::string> meadow;
            for (const auto& entry : list["meadow"]) {
                if (!entry.is_string()) break;
                const std::filesystem::path name(entry.get<std::string>());
                if (name.empty() || name.has_parent_path() || !std::filesystem::is_regular_file(flora / name)) break;
                meadow.push_back("../generated/foliage_cards/" + name.string());
            }
            if (meadow.size() == grass.size()) grass = std::move(meadow);
        }
        grass.insert(grass.end(), cards.begin(), cards.end());
    }
    std::vector<std::string> spriteImages{grass.front(), grass[world::foliage::kWildGrassSprites.size() - 1]};
    foliage_ = drawing->add(std::make_unique<FoliagePass>(cache_, std::move(grass), shadows_->binding(), terrain));
    models_ = drawing->add(std::make_unique<SceneModelsPass>(preparation_->placement, shadows_->binding(), terrain));
    farTrees_ = drawing->add(std::make_unique<FarTreesPass>(shadows_->binding(), terrain));
    character_ = drawing->add(std::make_unique<CharacterPass>(shadows_->binding()));
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
    drawing->add(std::make_unique<SketchPass>(&sketch_));
    drawing->add(std::make_unique<HighlightPass>(&highlight_));
    drawing->add(std::make_unique<HoldPass>(&held_));
    // The engine's grade, placed where this game's frame wants it; the look
    // is assets/shaders/game/style_picture.hlsli.
    gradePass_ = drawing->add(std::make_unique<engine::GradePass>(
            engine::PassPlace{engine::passOf(Pass::Grade), engine::stageOf(Stage::Post),
                              static_cast<engine::PassOrder>(Order::Opaque)},
            engine::GradeLook{0.50f, 0.28f, 0.012f}));
    sprites_ = drawing->add(std::make_unique<SpritePass>(spriteQueue_, std::move(spriteImages)));
    if (overlay_) if (auto overlay = overlay_(*terrain)) drawing->add(std::move(overlay));
    render_ = runner_->add(std::move(drawing));
    const auto shaders = device_.assets().parent_path() / "shaders";
    if (runner_->build(device_)) {
        // The categories' generated code built: it is what to fall back to.
        engine::biomes::markShaderCodeGood(shaders);
        rebuildShaders_ = false;
        return true;
    }
    forgetTheWorld();
    // Generated code that does not build goes back to the last that did,
    // once, and the error stays where it was printed.
    if (!restoredShaders_ && engine::biomes::restoreShaderCode(shaders)) {
        restoredShaders_ = true;
        std::cerr << "terrain categories: the generated shader code did not build (" << device_.error()
                  << "); back to the last that did\n";
        return synchronize();
    }
    return false;
}

void WorldRenderer::pollBiomes() {
    // A second between looks: a directory of a dozen small files.
    const auto now = std::chrono::steady_clock::now();
    if (now - biomesPolled_ < std::chrono::seconds(1)) return;
    biomesPolled_ = now;
    {
        // The ground's texture laying (the ground panel's Textures section).
        std::error_code ec;
        const auto file = terrainLookFile();
        const auto written = std::filesystem::last_write_time(file, ec);
        if (ec || written != terrainLookWritten_) {
            terrainLookWritten_ = ec ? std::filesystem::file_time_type{} : written;
            terrainLook_.load(file);
        }
    }
    // The procedural environment's content: rebuilt for the world on a change.
    if (world_) {
        std::filesystem::file_time_type newest{};
        std::error_code ec;
        for (const auto& root : {engine::environment::defaultContentDirectory(), engine::environment::defaultStyleDirectory()})
            for (const auto& e : std::filesystem::recursive_directory_iterator(root, ec))
                if (e.is_regular_file(ec)) newest = std::max(newest, e.last_write_time(ec));
        if (environmentWritten_ == std::filesystem::file_time_type{}) environmentWritten_ = newest;
        else if (newest != environmentWritten_) {
            environmentWritten_ = newest;
            world_->reloadEnvironment();
            std::cerr << "environment: reloaded\n";
        }
    }
    const auto dir = engine::biomes::defaultDirectory();
    const auto written = engine::biomes::registryWritten(dir);
    if (written == biomesWritten_) return;
    biomesWritten_ = written;
    const auto shaders = device_.assets().parent_path() / "shaders";
    const auto started = engine::biomes::startUp(dir, shaders, device_.assets().parent_path());
    for (const auto& problem : started.problems) std::cerr << "terrain categories: " << problem << "\n";
    if (!started.loaded) return;
    std::cerr << "terrain categories: reloaded" << (started.structureChanged ? " - the shaders build again" : "")
              << "\n";
    // Numbers reach the table by themselves (BiomeTextures::refresh); a new
    // structure is new shader code, built once.
    if (started.structureChanged) {
        rebuildShaders_ = true;
        restoredShaders_ = false;
    }
}

bool WorldRenderer::draw(const client::Camera& camera, const WorldRenderSettings& requested) {
    pollBiomes();
    WorldRenderSettings settings = requested;
    const GraphicsSettings& graphics = settings.graphics;
    if (settings.useGraphics) {
        settings.drawDistance = double(graphics.drawDistanceKm) * 1000.0;
        settings.fog = graphics.fog;
        settings.shadows = settings.shadows && graphics.shadows;
        settings.sunDirection = graphics.sunDirection();
        wantedSamples_ = graphics.antialiasing == 1 ? 4 : 1;
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
    if (character_) character_->set(characterState_);
    foliage_->reach(settings.useGraphics ? graphics.foliageDistance : GraphicsSettings{}.foliageDistance);
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
    auto local = weather.at(scalar(climate[0]), scalar(climate[2]), float(camera.focusHeight),
        float(camera.centreX), float(camera.centreY), scalar(climate[5]));
    // No weather: no rain falling past the camera and no overcast either,
    // whatever day of the forecast it is.
    if (weather.data[0][0] < 0.5f) local.air.precipitation = local.air.cloud = 0.0f;
    weather.data[1] = {scalar(climate[0]), scalar(climate[2]), scalar(climate[5]), settings.iceVisible ? 0.0f : 1.0f};
    weather.data[2] = {local.air.temperature, local.air.precipitation, local.air.cloud, local.air.wind};
    for (std::size_t i = 0; i < weather.data.size(); ++i)
        std::copy(weather.data[i].begin(), weather.data[i].end(), scene.parameters[i]);
    const double wide = std::max(1, world_->climate().wide()), high = std::max(1, world_->climate().high());
    scene.parameters[20][0] = float(1.0 / (world_->climate().metres() * wide));
    scene.parameters[20][1] = float(1.0 / (world_->climate().metres() * high));
    scene.parameters[20][2] = float(0.5 / wide); scene.parameters[20][3] = float(0.5 / high);
    scene.extra[3] = float(settings.map);
    styleScene(scene, camera.centreX, camera.centreY);
    scene.environment[1] = float(std::clamp(settings.environmentChannel, 0, 7));
    for (std::size_t i = 0; i < settings.editor.size(); ++i)
        std::copy(settings.editor[i].begin(), settings.editor[i].end(), scene.editor[i]);
    if (settings.map != world::MapView::Natural || !terrain.finalStage()) scene.extra[2] = 0;
    const float windLength = std::hypot(scalar(climate[3]), scalar(climate[4]));
    if (windLength > 0.001f) { scene.wind[0] = scalar(climate[3]) / windLength; scene.wind[1] = scalar(climate[4]) / windLength; }
    scene.wind[2] = local.air.wind;
    // Long sea waves are forced at a fixed world reference, never at the
    // camera. A tiny eye-dependent change in direction multiplied by world
    // position (or strength multiplied by uptime) retimed every visible crest.
    const auto swellClimate = field_->surfaceClimateAt(core::WorldPos{}).environment;
    const float swellLength = std::hypot(scalar(swellClimate[3]), scalar(swellClimate[4]));
    if (swellLength > 0.001f) {
        scene.swellWind[0] = scalar(swellClimate[3]) / swellLength;
        scene.swellWind[1] = scalar(swellClimate[4]) / swellLength;
    }
    const auto swellWeather = settings.weather.at(scalar(swellClimate[0]), scalar(swellClimate[2]),
        0.0f, 0.0f, 0.0f, scalar(swellClimate[5]));
    scene.swellWind[2] = swellWeather.air.wind;
    scene.table[6][0] = 0; scene.table[6][1] = 1; scene.table[6][2] = settings.potentialOnly ? 1 : 0;
    scene.table[6][3] = float(std::max(4.0, std::pow(2.0, std::round(std::log2(100.0 / std::max(0.001, camera.pixelsPerTile))))));
    std::copy(settings.floodBounds.begin(), settings.floodBounds.end(), scene.table[7]);
    {
        const auto look = terrainLook_.uniform();
        std::copy(look.begin(), look.end(), scene.terrainLook);
    }
    // Distance fog, from the draw distance: clear for the first third, then a
    // smooth rise to opaque at the draw distance itself. Inspection maps and
    // orthographic views measure no eye distance and are never fogged.
    // The whole world is the one terrain, at every height: its far end and
    // a view of most of it are drawn from the coarse levels of the same
    // pages (H256, H1024 - tile_layout.hpp) and morph into the finer ones as
    // the camera comes down, so nothing stands in for it. The fog goes as far
    // as a camera that high can see.
    const double detail = std::clamp(settings.drawDistance, 500.0, 400000.0);
    const double altitude = camera.perspective() ? std::max(0.0, camera.eyePosition()[2] - camera.focusHeight) : 0.0;
    terrain.window(0.0);
    const double reach = std::max(detail, std::min(3000000.0, altitude * 4.0));
    // Fog weather: the view closes in to a couple of kilometres, starting almost
    // at the eye, whatever the draw distance. Only with the weather on.
    const bool fogWeather = settings.weather.data[0][0] > 0.5f &&
        int(settings.weather.data[0][3]) == world::weather::kFogPreset;
    const bool fogged = (settings.fog || fogWeather) && camera.perspective() && settings.map == world::MapView::Natural;
    scene.fog[0] = float(fogWeather ? std::min(reach, 2200.0) : reach);
    scene.fog[1] = float(fogWeather ? 60.0 : reach * (settings.useGraphics ? graphics.fogStart : 0.3));
    scene.fog[2] = fogged ? 1.0f : 0.0f;
    // Where grass ends (FoliageVS fades over its last fifth).
    scene.fog[3] = float(settings.useGraphics ? graphics.foliageDistance : GraphicsSettings{}.foliageDistance);
    if (settings.useGraphics) {
        scene.look[0] = graphics.sunIntensity; scene.look[1] = graphics.ambient;
        scene.look[2] = graphics.exposure; scene.look[3] = 1.0f;
        scene.grading[0] = graphics.brightness; scene.grading[1] = graphics.contrast;
        scene.grading[2] = graphics.saturation; scene.grading[3] = 1.0f;
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
        // Procedural: no content needed. And none at all with the weather off
        // (the editor): clouds over the ground being made are weather too.
        scene.clouds[3] = settings.weather.data[0][0] < 0.5f ? 0.0f : float(graphics.cloudSteps());
    }
    // The grade (GradePass): the picture's own look, so on unless the
    // settings turn it down. Inspection maps and the unfinished generator
    // stages are measurements, not pictures, and are never graded.
    scene.quality[2] = settings.useGraphics ? (graphics.grade ? graphics.gradeStrength : 0.0f) : 1.0f;
    if (settings.map != world::MapView::Natural || !terrain.finalStage()) scene.quality[2] = 0.0f;
    // Post-process anti-aliasing (FXAA in GradePass), independent of the grade:
    // edges are smoothed in every view, the measurement maps included.
    scene.quality[3] = settings.useGraphics ? (graphics.antialiasing == 2 ? 1.0f : 0.0f) : 1.0f;
    // Objects by the graphics' own draw distance, not by how far the fog goes:
    // from high up the fog reaches a thousand kilometres, and the forest's
    // hierarchy walked every root of it each frame for trees nobody could see.
    if (models_) models_->drawDistance(camera.perspective() ? detail : 400000.0);
    if (models_)
        models_->prepareDensity(scene, cull.viewportWidth, scene);
    if (!shadows_->update(device_,scene,settings.sunDirection,settings.shadows &&
        std::getenv("CAMPFIRE_NO_SHADOWS")==nullptr && settings.map==world::MapView::Natural && terrain.finalStage())) return false;
    graded_ = scene.quality[2] > 0.0f || scene.quality[3] > 0.0f;
    // The held picture goes once the new ground in view is in: something on
    // the card, nothing the view wants still missing. A few frames at least,
    // so a plan has been made at all; and not for ever, whatever happens.
    if (held_) {
        ++heldFrames_;
        const auto& ground = collect_->gpuTerrain();
        const auto now = viewOf(camera);
        const bool moved = std::abs(now[0] - heldView_[0]) > 0.5 || std::abs(now[1] - heldView_[1]) > 0.5 ||
                           std::abs(now[2] - heldView_[2]) > 1e-4 || std::abs(now[3] - heldView_[3]) > 1e-4 ||
                           std::abs(now[4] - heldView_[4]) > heldView_[4] * 1e-3;
        // A plan that wants nothing (a view of open sea) is
        // in as soon as it is made; one that never quite gets every page in
        // is let go after a few seconds, not twenty.
        if (moved || (heldFrames_ > 3 && ground.planned() && ground.missing() == 0) || heldFrames_ > 240)
            held_.reset();
    }
    drawnView_ = viewOf(camera);
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

namespace game {

void WorldRenderer::styleScene(engine::Scene& scene, double x, double y) {
    const auto& environment = world_ ? world_->environment() : nullptr;
    if (!environment) return;
    scene.environment[0] = environment->writesMasks() ? 1.0f : 0.0f;
    // A new environment (a world loaded, content reloaded): start the blends
    // over, snapped to where the camera is.
    const bool fresh = styleGeneration_ != environment->generation();
    if (fresh) {
        styleGeneration_ = environment->generation();
        paletteBlend_ = environment->palettes()
            ? std::make_unique<engine::environment::GradeBlend>(
                      std::shared_ptr<const engine::environment::StyleTable>(environment, environment->palettes()))
            : nullptr;
        gradeBlend_ = environment->grades() ? std::make_unique<engine::environment::GradeBlend>(environment->gradesShared())
                                            : nullptr;
        gradeLuts_.clear();
        if (const auto* grades = environment->grades())
            for (const auto& profile : grades->entries()) {
                std::optional<engine::environment::Lut3d> lut;
                if (!profile.lut.empty()) {
                    std::string problem;
                    lut = engine::environment::loadCube(grades->directory() / profile.lut, &problem);
                    if (!lut) std::fprintf(stderr, "style: %s: %s\n", profile.name.c_str(), problem.c_str());
                }
                gradeLuts_.push_back(std::move(lut));
            }
    }
    if (!paletteBlend_ && !gradeBlend_) return;
    // What the camera stands on: its ground category and its zone.
    std::string category, zone;
    if (const auto registry = engine::biomes::active()) {
        const auto ids = world_->climate().categoriesAt(x, y);
        if (const auto* c = registry->category(ids[0])) category = c->name;
    }
    if (const auto* zones = environment->zones()) {
        const auto here = zones->at(x, y);
        const auto types = environment->catalogue().zones();
        if (here.type() < types.size()) zone = types[here.type()].name;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = fresh ? 0.0 : std::chrono::duration<double>(now - styled_).count();
    styled_ = now;
    const auto blend = [&](engine::environment::GradeBlend& b, const engine::environment::StyleTable& table, float (*rows)[4]) {
        const auto target = table.match(category, zone);
        if (fresh) b.snap(target); else b.step(target, std::min(seconds, 0.5));
        const auto out = b.rows();
        for (std::size_t r = 0; r < out.size() && r < 8; ++r)
            for (int k = 0; k < 4; ++k) rows[r][k] = out[r][k];
    };
    if (paletteBlend_) {
        blend(*paletteBlend_, *environment->palettes(), scene.style);
        scene.environment[3] = 1.0f;
    }
    if (gradeBlend_) {
        blend(*gradeBlend_, *environment->grades(), scene.gradeStyle);
        scene.environment[3] += 2.0f;
        // The two strongest profiles' lookups. A profile without one stands
        // for "no lookup", which the identity of the other's size is.
        if (gradePass_) {
            const auto pair = gradeBlend_->strongest();
            const auto* a = pair.a < gradeLuts_.size() && gradeLuts_[pair.a] ? &*gradeLuts_[pair.a] : nullptr;
            const auto* b = pair.b < gradeLuts_.size() && gradeLuts_[pair.b] ? &*gradeLuts_[pair.b] : nullptr;
            const int size = a ? a->size : b ? b->size : 0;
            static thread_local engine::environment::Lut3d identity;
            if (size >= 2 && identity.size != size) {
                identity.size = size;
                identity.rgb.resize(std::size_t(size) * size * size);
                for (int bl = 0; bl < size; ++bl)
                    for (int g = 0; g < size; ++g)
                        for (int r = 0; r < size; ++r)
                            identity.rgb[(std::size_t(bl) * size + g) * size + r] = {
                                    float(r) / float(size - 1), float(g) / float(size - 1), float(bl) / float(size - 1)};
            }
            const auto& first = a && a->size == size ? *a : identity;
            const auto& second = b && b->size == size ? *b : identity;
            if (size >= 2) gradePass_->luts(size, first.rgb, second.rgb, pair.t);
            else gradePass_->luts(0, {}, {}, 0);
        }
    }
}

} // namespace game
