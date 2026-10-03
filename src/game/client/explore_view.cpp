#include "game/client/explore_view.hpp"

#include "engine/core/profiler.hpp"
#include "engine/ui/profiler_window.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_import.hpp"
#include "game/client/explore_bench.hpp"
#include "game/client/character_controller.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <numbers>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <nlohmann/json.hpp>
#include <SDL3_image/SDL_image.h>

#include "game/client/controls.hpp"
#include "game/client/explore_diagnostics.hpp"
#include "game/client/world_inspection.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/world_layout.hpp"
#include "game/render/calc/terrain_collect.hpp"
#include "game/client/explore_menu.hpp"
#include "game/render/passes/menu_pass.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"
#include "game/world/foliage_catalog.hpp"
#include "game/world/world_delta.hpp"
#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/codec.hpp"
#include "engine/ui/ui.hpp"

namespace client {
namespace {

// content/, found by walking up, the way everything else in this project finds
// it.
std::filesystem::path groundFile() {
    std::filesystem::path content = "content";
    for (int up = 0; up < 5 && !std::filesystem::exists(content); ++up) content = ".." / content;
    return content / "config" / "ground.json";
}
// Per-machine graphics choices, beside editor.json rather than in content/.
std::filesystem::path graphicsFile() { return groundFile().parent_path().parent_path().parent_path() / "graphics.json"; }
// The world the editor makes, beside them: worlds/world.json.
std::filesystem::path worldFile() { return graphicsFile().parent_path() / "worlds" / "world.json"; }

// The world that is showing, described as regions, for the editor to start
// from: as many whole regions as fit, every one generated with the world's own
// dials and the world's own seed. One seed across every region is one noise
// field across the whole map, so this is the world on the screen, not a new one.
generation::WorldLayout layoutShowing(const generation::WorldMapParams& params,
                                      const std::vector<generation::WorldPreset>& presets) {
    generation::WorldLayout layout =
            generation::emptyLayout(std::max(1, params.width / generation::kCellsPerRegion),
                                    std::max(1, params.height / generation::kCellsPerRegion), params.seed);
    layout.plates = params.plates;
    std::string preset;
    for (const auto& p : presets)
        if (p.params.seaPercent == params.seaPercent && p.params.erosionPasses == params.erosionPasses &&
            p.params.rainfallPercent == params.rainfallPercent)
            preset = p.name;
    for (auto& region : layout.regions) {
        region.generated = true;
        region.settings.preset = preset;
        region.settings.seed = params.seed;
        region.settings.seaPercent = params.seaPercent;
        region.settings.erosionPasses = params.erosionPasses;
        region.settings.rainfallPercent = params.rainfallPercent;
    }
    return layout;
}


// The file the editor writes, watched.
//
// The editor is its own program - opened beside the game rather than inside it -
// so the file between them is the channel. Its write time is looked at a few
// times a second and the numbers read again when it moves. Polled rather than
// watched because a poll is a dozen lines and a file watcher is a platform
// each, and a third of a second is faster than a person can press a button and
// look up.
class GroundWatch {
public:
    explicit GroundWatch(std::filesystem::path file) : file_(std::move(file)) {
        std::error_code ec;
        stamp_ = std::filesystem::last_write_time(file_, ec);
    }

    // Puts whatever the file now says into `into` and answers true, if it has
    // changed since the last look. A file that has changed but does not read as
    // a table is not taken and not remembered either: a file being written is
    // empty for an instant, and taking it then would flatten the ground to
    // defaults for a frame. The next look picks up the finished file.
    //
    // Only the numbers the renderer reads, and matched by name. The pictures
    // were baked into one texture array in the order this table had when the
    // device opened, and every layer the shader indexes is that order; taking a
    // new order now would point each layer at the wrong stuff. So renaming a
    // material or moving one up the file needs the game started again - and
    // moving a number, which is what anybody actually tunes, does not.
    bool changed(std::vector<content::GroundMaterial>& into) {
        std::error_code ec;
        const auto now = std::filesystem::last_write_time(file_, ec);
        if (ec || now == stamp_) return false;
        std::vector<content::GroundMaterial> fresh;
        if (!content::readGroundMaterials(file_, fresh)) return false;
        stamp_ = now;
        bool moved = false;
        for (content::GroundMaterial& mine : into) {
            for (const content::GroundMaterial& theirs : fresh) {
                if (theirs.name != mine.name) continue;
                moved = moved || mine.metresPerTurn != theirs.metresPerTurn ||
                        mine.blendWidth != theirs.blendWidth || mine.tear != theirs.tear ||
                        mine.tearMetres != theirs.tearMetres;
                mine.metresPerTurn = theirs.metresPerTurn;
                mine.blendWidth = theirs.blendWidth;
                mine.tear = theirs.tear;
                mine.tearMetres = theirs.tearMetres;
                break;
            }
        }
        return moved;
    }

private:
    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
};

} // namespace

bool ExploreView::open(SDL_Window* window, const std::filesystem::path& assets, world::WorldSystem& world,
                       const Camera& camera, ExploreMenu& menu, int headlessWidth, int headlessHeight) {
    menu_ = &menu;
    source_ = &world;
    return renderer_.open(window, assets, world, [&menu](const game::GpuTerrain& terrain) {
        return std::make_unique<game::MenuPass>(menu, &terrain);
    }, headlessWidth, headlessHeight);
}

bool ExploreView::draw(const Camera& camera) {
    const core::WorldPos focus{core::Fixed::fromDoubleForContent(camera.centreX),
                               core::Fixed::fromDoubleForContent(camera.centreY)};
    menu_->applySoilRequest(focus);
    if (menu_->takeFloodRequest()) {
        const float half = float(camera.viewportWidth / std::max(0.001, camera.pixelsPerTile) * 0.5);
        settings_.floodBounds = {float(camera.centreX) - half, float(camera.centreY) - half,
                                 float(camera.centreX) + half, float(camera.centreY) + half};
    }
    auto weather = menu_->weatherSnapshot();
    // Every shader's switch for it (weather.hlsli). The explorer's own editor
    // turns it off as the client's Edit mode does.
    if (!weather_ || menu_->editor().active() || !menu_->weatherEnabled()) weather.data[0][0] = 0.0f;
    const auto snapshot = source_->read();
    const auto climate=snapshot->climate().at(focus).environment;
    const auto scalar=[](core::Fixed f) { return static_cast<float>(f.toDouble()); };
    const auto local=weather.at(scalar(climate[0]),scalar(climate[2]),static_cast<float>(camera.focusHeight),
        static_cast<float>(camera.centreX),static_cast<float>(camera.centreY),scalar(climate[5]));
    menu_->weatherReadout(local);
    menu_->stagesAvailable(bool(snapshot->worldMap().terrainFoundation));
    // The Views tab of the graphics window: a click is applied, then the
    // panel is told what is actually on (a key may have changed it).
    {
        auto& panel = menu_->panel();
        if (panel.viewsChanged) {
            panel.viewsChanged = false;
            menu_->setMapView(panel.views.map);
            menu_->setTerrainStage(panel.views.stage);
            renderer_.setTerrainGrid(panel.views.grid);
            menu_->setWeatherEnabled(panel.views.weatherOn);
            menu_->setWeatherPreset(panel.views.weather);
            if (panel.views.objectWire != renderer_.objectWireframe()) renderer_.toggleObjectWireframe();
        }
        const GraphicsPanel::Views now{int(menu_->mapView()), int(menu_->terrainStage()), renderer_.terrainGrid(),
                                       renderer_.objectWireframe(), bool(snapshot->worldMap().terrainFoundation),
                                       menu_->weatherEnabled(), menu_->weatherPreset()};
        if (now.map != panel.views.map || now.stage != panel.views.stage || now.grid != panel.views.grid ||
            now.objectWire != panel.views.objectWire || now.stages != panel.views.stages ||
            now.weatherOn != panel.views.weatherOn || now.weather != panel.views.weather) {
            panel.views = now;
            menu_->panelChanged();
        }
    }
    settings_.weather = weather;
    settings_.map = menu_->mapView();
    settings_.stage = menu_->terrainStage();
    settings_.iceVisible = menu_->iceVisible();
    settings_.potentialOnly = menu_->potentialOnly();
    settings_.drawDistance = menu_->drawDistance();
    settings_.graphics = menu_->graphics();
    settings_.useGraphics = true;
    settings_.editor = marks_ ? *marks_ : menu_->editor().overlay();
    // The coast sketch: drawn while the world is being shaped, rebuilt when a
    // stroke, a pin or an undo changed it - a few milliseconds, no generation.
    const auto& editor = menu_->editor();
    const std::uint64_t wanted = editor.active() ? editor.sketchRevision() : 0;
    if (wanted != sketchShown_) {
        sketchShown_ = wanted;
        renderer_.sketch(wanted ? std::make_shared<const generation::SketchMesh>(generation::sketchMesh(editor.layout()))
                                : nullptr);
    }
    return renderer_.draw(camera, settings_);
}

#if ASR_ENABLE_PROFILING
bool ExploreView::compareGrassCulling(const Camera& camera, const std::string& path) {
    return renderer_.compareGrassCulling(camera, settings_, path);
}
#endif

int runExploreMode(SDL_Window* window, std::uint64_t seed, std::int32_t worldCells,
                   const std::filesystem::path& assets, const std::string& shotPathAsked,
                   double startZoom, const std::string& startAt, bool measuring, bool closeUp,
                   bool tracing, int shotFrame, bool showMenu, int crowd, double shotTime,
                   const std::string& mapName, double weatherDay, int weatherPreset,
                   const core::TimeConfig& calendar, const std::array<float,4>& seasons,
                   const ExploreViewOptions& options) {
    // --shot-list: the picture being waited for changes as the list is worked
    // through; everything that asks "is this a run for a picture" still asks it
    // of this one name.
    std::string shotPath = shotPathAsked;
    struct ListedShot { std::string path, graphics; bool eye = true; double x = 0, y = 0, value = 0, yaw = 0, pitch = 0; };
    std::vector<ListedShot> shotList;
    if (!options.shotList.empty()) {
        std::ifstream list(options.shotList);
        if (!list) { std::cerr << "--shot-list: cannot read " << options.shotList << "\n"; return 1; }
        std::string line;
        while (std::getline(list, line)) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
            std::istringstream in(line);
            ListedShot shot;
            std::string kind;
            if (!(in >> shot.path)) continue;
            if (!(in >> kind >> shot.x >> shot.y >> shot.value >> shot.yaw >> shot.pitch) ||
                (kind != "eye" && kind != "orbit")) {
                std::cerr << "--shot-list: expected PATH eye|orbit X Y ABOVE|ZOOM YAW PITCH, not: " << line << "\n";
                return 1;
            }
            shot.eye = kind == "eye";
            in >> shot.graphics;   // optional: this shot's graphics.json
            shotList.push_back(shot);
        }
        if (shotList.empty()) { std::cerr << "--shot-list: no shots in " << options.shotList << "\n"; return 1; }
        shotPath = shotList.front().path;
    }
    std::size_t shotNext = 0;   // the next entry of shotList to aim at
    int shotAimedAt = 0;        // the frame the camera last moved for a listed shot
    int shotBlackRetries = 0;   // black pictures taken again for the current shot
    int shotSettledFrames = 0;  // frames the streaming has been caught up for
#if !ASR_ENABLE_PROFILING
    if (measuring || tracing || crowd > 0 || std::getenv("ASR_FRAME_PROFILE") ||
        std::getenv("ASR_TERRAIN_PROFILE") || std::getenv("ASR_GRASS_COMPARE_CULL") ||
        std::getenv("ASR_TERRAIN_COMPARE_SKIRTS")) {
        std::cerr << "Diagnostics are not compiled in; configure ASR_ENABLE_DIAGNOSTICS=ON\n";
        return 1;
    }
#endif
    ExploreMenu menu;
    menu.iceVisible(options.iceVisible);
    if (!menu.selectMap(mapName)) {
        std::cerr << "Unknown explore overlay: " << mapName << "\nExpected:";
        for (const char* name : world::kMapNames) std::cerr << ' ' << name;
        std::cerr << '\n';
        return 1;
    }
    generation::WorldMapParams params;
    params.seed = seed;
    params.width = params.height = worldCells;
    // A world made in the editor, if one was named: its regions decide the
    // size and everything else.
    const auto worldPresets = generation::loadWorldPresets(groundFile().parent_path() / "world_presets.json");
    std::optional<generation::WorldLayout> startLayout;
    if (!options.worldFile.empty() && std::filesystem::exists(options.worldFile)) {
        startLayout = generation::loadWorldLayout(options.worldFile);
        if (!startLayout) {
            std::cerr << "could not read the world " << options.worldFile << "\n";
            return 1;
        }
        // What was imported into it (the client's Import tab), beside it.
        startLayout->imported = generation::openImported(
                engine::world_store::WorldRoot::forLayout(options.worldFile) / "source", *startLayout);
        params = generation::paramsFor(*startLayout);
    } else if (!options.worldFile.empty()) {
        // Not made yet: start from the usual world, and Save writes it here.
        std::cout << "no world at " << options.worldFile << " yet - Save in the editor makes it\n";
    }
    // Say it before anything else, because everything after it is a lie
    // otherwise.
    //
    // A debug build of this engine is several times slower than a released one
    // - fixed point, templates and a std::function for every surface sample all
    // come out of the optimiser as different code - and its frame rate says
    // nothing about the frame rate. A whole day can go into "why is it thirty
    // frames a second" before anybody looks at which profile is selected, and
    // this line is what stops that happening twice.
    // Keyed on whether the compiler OPTIMISED, not on whether asserts are on.
    // Those are different questions and only the first one changes the timings.
#ifndef __OPTIMIZE__
    std::cout << "*** UNOPTIMISED BUILD: frame rate and timings here mean nothing.\n"
              << "*** Build RelWithDebInfo, or Debug, which is -Og and usable.\n";
#endif
    std::cout << "raising a " << params.width << "x" << params.height << " world ..." << std::flush;
    const auto started = std::chrono::steady_clock::now();
    // The world's history over its generated base (world_delta.hpp), in the
    // root beside the layout it belongs to: worlds/world.json keeps its delta
    // in worlds/world/. Only a world read from a file has one - a world from
    // the menu is a seed, not a place with a past.
    std::unique_ptr<world::delta::WorldDelta> worldDelta;
    if (startLayout) {
        const engine::world_store::WorldRoot root(engine::world_store::WorldRoot::forLayout(options.worldFile));
        world::delta::LoadReport report;
        worldDelta = world::delta::WorldDelta::open(root, params.seed, &report);
        for (const auto& warning : report.warnings) std::cerr << "world delta: " << warning << "\n";
        for (const auto& damaged : report.damaged) std::cerr << "world delta, unreadable: " << damaged << "\n";
        if (!worldDelta)
            std::cerr << "the world delta in " << root.directory().string()
                      << " cannot be read; nothing will be saved over it this session\n";
        else if (!report.fresh)
            std::cout << "world delta: " << report.chunks << " chunks, " << report.ops << " journal records\n";
        if (worldDelta) {
            const auto layout = engine::world_store::readFileBytes(options.worldFile);
            worldDelta->source(std::filesystem::path(options.worldFile).filename().string(),
                               layout ? engine::world_store::contentHash(*layout) : 0);
        }
    }
    // --dig: where, how wide, how deep. A world from the menu has no history
    // to dig into, so it gets one in memory, which is never saved.
    struct Dig { double x = 0, y = 0, radius = 60, metres = -12; };
    std::optional<Dig> dig;
    if (!options.dig.empty()) {
        Dig d;
        if (std::sscanf(options.dig.c_str(), "%lf,%lf,%lf,%lf", &d.x, &d.y, &d.radius, &d.metres) < 2 ||
            d.radius <= 0 || d.metres == 0) {
            std::cerr << "--dig wants X,Y[,RADIUS[,METRES]], not " << options.dig << "\n";
            return 1;
        }
        dig = d;
        if (!worldDelta) worldDelta = std::make_unique<world::delta::WorldDelta>(params.seed);
    }
    world::WorldSystem builder;
    // Objects and ground both: what was cut stays cut, and what was dug is
    // what the pages are baked over and the renderer draws.
    if (worldDelta) builder.attach(worldDelta->objects(), worldDelta->heights());
    // Every way out of the loop below goes through here, and a delta with
    // anything unsaved is saved on the way - unless the session is a probe.
    struct SaveOnExit {
        world::delta::WorldDelta* delta;
        ~SaveOnExit() {
            if (!delta || !delta->dirty() || !delta->root()) return;
            const auto report = delta->save();
            if (!report.ok) std::cerr << "world delta not saved: " << report.error << "\n";
        }
    } saveOnExit{dig ? nullptr : worldDelta.get()};
    Uint64 deltaSavedAt = SDL_GetTicks();
    // A world read from a file is its layout - its regions and what was
    // imported into them - not a world grown from its seed alone.
    if (startLayout) builder.publish(generation::generateLayoutWorld(*startLayout));
    else builder.build(params);
    auto snapshot = builder.read();
    auto* world = &snapshot->worldMap();
    auto field = snapshot->field();
    std::cout << " " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
              << " s, " << world->lakesKept << " lakes (" << world->basinsDried
              << " basins dried), " << static_cast<double>(params.width) * generation::kMetresPerCell / 1000
              << " km across\n";
    core::WorldPos start = snapshot->startingPoint();
    // "--at 12345,67890" aims at a point rather than a landmark.
    //
    // Landmarks are chosen by what a coarse cell is - the steepest is the cell
    // whose neighbours differ most, and neighbours are five hundred and forty
    // metres apart, so "mountains" is a hillside. Anything the detail layer
    // makes between those samples - a cliff, a gully head, a break of slope -
    // has no landmark and cannot be looked at, which makes it impossible to
    // check by eye. A pair of coordinates costs one branch and fixes that.
    const std::size_t comma = startAt.find(',');
    if (comma != std::string::npos) {
        start = {core::Fixed::fromDoubleForContent(std::atof(startAt.substr(0, comma).c_str())),
                  core::Fixed::fromDoubleForContent(std::atof(startAt.substr(comma + 1).c_str()))};
    } else {
        for (const auto& landmark : snapshot->landmarks())
            if (!startAt.empty() && landmark.name.find(startAt) != std::string::npos)
                start = landmark.where;
    }
    // Where --at can go, by name and as coordinates, for scripted shots.
    std::cout << "landmarks:";
    for (const auto& landmark : snapshot->landmarks())
        std::cout << " [" << landmark.name << " " << std::lround(landmark.where.x.toDouble()) << ","
                  << std::lround(landmark.where.y.toDouble()) << "]";
    std::cout << "\n";

    Camera camera;
    camera.centreX = start.x.toDouble();
    camera.centreY = start.y.toDouble();
    camera.isometric = true;
    camera.orbit(options.yaw - camera.yaw, options.pitch - camera.pitch);
    camera.heightOffset = std::clamp(options.heightOffset, -10000.0, 10000.0);
    camera.focusHeight = field.heightAt(start).toDouble() + camera.heightOffset;
    camera.pixelsPerTile = startZoom > 0 ? startZoom : 0.6;
    // Width and height apart: a world of regions need not be square.
    double worldMetres = static_cast<double>(params.width) * generation::kMetresPerCell;
    double worldHighMetres = static_cast<double>(params.height) * generation::kMetresPerCell;
    camera.setBounds(0, 0, worldMetres, worldHighMetres);
    // Headless, the viewport is the offscreen target and nothing else.
    const auto viewportOf = [&](Camera& into) {
        if (window) SDL_GetWindowSizeInPixels(window, &into.viewportWidth, &into.viewportHeight);
        else { into.viewportWidth = options.headlessWidth; into.viewportHeight = options.headlessHeight; }
    };
    viewportOf(camera);
    camera.setMode(options.cameraMode);
    camera.orbit(options.yaw - camera.yaw, options.pitch - camera.pitch);
    if (!options.eye.empty()) {
        double ex = 0, ey = 0, above = 1.7;
        if (std::sscanf(options.eye.c_str(), "%lf,%lf,%lf", &ex, &ey, &above) < 2) {
            std::cerr << "--eye wants X,Y[,ABOVE], not " << options.eye << "\n";
            return 1;
        }
        camera.setMode(Camera::Mode::Free);
        camera.centreX = ex;
        camera.centreY = ey;
        camera.focusHeight = field.heightAt({core::Fixed::fromDoubleForContent(ex),
                                             core::Fixed::fromDoubleForContent(ey)}).toDouble() + above;
        camera.orbit(options.yaw - camera.yaw, options.pitch - camera.pitch);
    }
    // A listed shot: on foot (free camera, metres over the ground) or third
    // person round a point (orbit, zoom in pixels per tile).
    const auto aimListed = [&](const ListedShot& shot) {
        const double ground = field.heightAt({core::Fixed::fromDoubleForContent(shot.x),
                                              core::Fixed::fromDoubleForContent(shot.y)}).toDouble();
        camera.setMode(shot.eye ? Camera::Mode::Free : Camera::Mode::Orbit);
        camera.centreX = shot.x;
        camera.centreY = shot.y;
        camera.heightOffset = 0;
        camera.focusHeight = ground + (shot.eye ? shot.value : 0.0);
        if (!shot.eye) camera.pixelsPerTile = shot.value;
        camera.orbit(shot.yaw - camera.yaw, shot.pitch - camera.pitch);
    };
    if (!shotList.empty()) aimListed(shotList[shotNext++]);
    // The dials, on the screen, over the world they made. Built before the view
    // because the view's menu pass holds on to it, and told about the ground
    // after, because the ground is the view's and is read when the view opens.
    game::WorldRenderer renderer;
    ExploreView view(renderer);
    // A run that exists to take one picture has no business showing the wind.
    if (!shotPath.empty()) renderer.holdTime(shotTime);
    if (!view.open(window, assets, builder, camera, menu, options.headlessWidth, options.headlessHeight)) {
        std::cerr << "the explorer would not start: " << renderer.error() << "\n";
        return 1;
    }
    for (int i = 0; i < std::clamp(options.gridMode, 0, 2); ++i) renderer.cycleTerrainGrid();
    if (options.objectMesh != renderer.objectWireframe()) renderer.toggleObjectWireframe();
    std::cout << "C: take / release the character (then V: 3rd / 1st person / map, WASD, Shift, Alt, Space, mouse)\n"
                 "V: map / 3rd orbit / 1st free flight; RMB: look; WASD: move; Q/E: altitude\n"
                 "Shift: fly faster; wheel: orbit distance / flight speed; Shift+G: sample / mesh grid / off;\n"
                 "Shift+M: object mesh on / off\n";
    menu.open(params, &renderer.ground(), groundFile());
    {
        // The world editor starts from the world that is showing, described as
        // regions; its presets are the same file the world menu reads.
        auto showing = startLayout ? *startLayout : layoutShowing(params, worldPresets);
        menu.editor().open(worldPresets,
                           options.worldFile.empty() ? worldFile() : std::filesystem::path(options.worldFile),
                           std::move(showing));
        // The map in its panel is drawn over the world as it came out.
        menu.editor().builtWorld(*world);
        if (!options.editLayer.empty() && !menu.editor().chooseLayerNamed(options.editLayer)) {
            std::cerr << "Unknown layer: " << options.editLayer
                      << "\nExpected: regions continents ranges hills sea weathering rain\n";
            return 1;
        }
        if (options.editing) menu.toggleEditor();
    }
    // Saved graphics first, then the command line on top: a shot asked for
    // with --draw-distance or --no-fog must get exactly that.
    menu.graphics() = game::loadGraphicsSettings(graphicsFile());
    // Reproducible runs, unless the run is there to reproduce what the saved
    // settings do (ASR_SHOT_USER_GRAPHICS=1).
    if ((!shotPath.empty() || options.benchFrames > 0) && !std::getenv("ASR_SHOT_USER_GRAPHICS"))
        menu.graphics() = options.graphicsFile.empty() ? game::GraphicsSettings{}
                                                       : game::loadGraphicsSettings(options.graphicsFile);
    if (options.drawDistance > 0) menu.drawDistance(options.drawDistance);
    if (options.clean) menu.presentation().developer = false;
    if (!options.fog) menu.graphics().fog = false;
    // A listed shot may bring its own graphics.json (lighting A/B in one
    // launch); the others get what the run started with.
    const game::GraphicsSettings shotBaseGraphics = menu.graphics();
    const auto graphicsForShot = [&](const ListedShot& shot) {
        menu.graphics() = shot.graphics.empty() ? shotBaseGraphics : game::loadGraphicsSettings(shot.graphics);
    };
    if (!shotList.empty()) graphicsForShot(shotList.front());
    view.fog(options.fog);
    menu.configureWeather(calendar,seasons,weatherDay,std::max(0,weatherPreset));
    if (weatherPreset<0) menu.setWeatherEnabled(false);
    GroundWatch groundWatch(groundFile());
    Uint64 groundLookedAt = SDL_GetTicks();
    if (showMenu) menu.toggle();
    if (showMenu && startAt == "ground") menu.showGround();

    std::unique_ptr<ExploreBench> bench;
    // Scene view: the frozen camera every decision is made from while the
    // live camera flies. Empty when not inspecting.
    std::optional<Camera> frozen;
    bool leftWasDown = false;
    // A world-editor brush stroke in progress (it started on the ground).
    bool editPainting = false;
    // What the resolution dropdown last put the window at; 0 = untouched.
    int appliedResolution = 0;
    if (options.benchFrames > 0)
        bench = std::make_unique<ExploreBench>(options.benchFrames, options.benchJson, options.benchLoadFrames);
    const bool scripted = bench != nullptr;
    CameraControl controls;
    // The explorer is looked at, not played: the view moves when somebody drags
    // it or presses a key, and never on its own. A shot has to be of a still
    // camera or two of them cannot be compared.
    controls.edgePan = false;
    ui::Input pointer;
    bool relativeMouse = false;
    // The player's character. C takes it (and gives it back); while it is
    // held V cycles third person, first person and the map, WASD moves it
    // relative to the camera (Shift sprints, Alt walks, Space jumps), the
    // mouse turns the camera and the wheel sets the third-person distance.
    // Clicking to walk on the map is planned, not here (needs a nav mesh).
    CharacterController hero;
    bool possessed = false, heroJump = false;
    double heroDistance = 4.2, heroMapZoom = 8.0;
    const auto groundAt = [&](double x, double y) {
        return field.heightAt({core::Fixed::fromDoubleForContent(std::clamp(x, 0.0, worldMetres - 1.0)),
                               core::Fixed::fromDoubleForContent(std::clamp(y, 0.0, worldHighMetres - 1.0))})
                .toDouble();
    };
    const auto possess = [&](bool on) {
        possessed = on;
        controls.dragging = controls.orbiting = false;
        controls.velocityX = controls.velocityY = 0;
        if (!on) {
            camera.heightOffset = 0;
            std::cout << "Character released\n";
            return;
        }
        // Where the camera is looking, on the ground, facing away from it.
        double x = camera.centreX, y = camera.centreY;
        if (camera.mode == Camera::Mode::Free) {
            x -= std::cos(camera.yaw) * 4.0;
            y -= std::sin(camera.yaw) * 4.0;
        }
        hero.place(x, y, groundAt(x, y), camera.yaw + std::numbers::pi + options.characterTurn);
        camera.setMode(Camera::Mode::Orbit);
        camera.pitch = 0.32;
        std::cout << "Character: WASD move, Shift sprint, Alt walk, Space jump, mouse look, wheel distance, "
                     "V third/first person/map, C release\n";
    };
    if (options.character) {
        possess(true);
        if (options.characterDistance > 0) heroDistance = options.characterDistance;
        if (options.characterView == 1) camera.setMode(Camera::Mode::Free);
        if (options.characterView == 2) {
            camera.setMode(Camera::Mode::Map);
            camera.pitch = kDefaultCameraPitch;
            camera.pixelsPerTile = heroMapZoom;
        }
    }
    bool running = true;
    Uint64 last = SDL_GetTicks();
    int frame = 0;
    int dug = 0;   // the frame --dig landed on
#if ASR_ENABLE_FPS
    std::uint64_t reported = 0;
#endif
    Uint64 inspectedAt = 0;
#if ASR_ENABLE_PROFILING
    ExploreDiagnostics diagnostics;
#endif
    if (options.graphicsTab >= 0) { menu.togglePanel(); menu.panel().showTab(options.graphicsTab); }
    if (options.sceneViewBack > 0) {
        // Scripted scene view: freeze at the requested camera, then draw from
        // behind and above it, so a shot shows the frozen frustum and what the
        // frozen decisions put around it.
        frozen = camera;
        const auto view = camera.viewState();
        const auto eye = camera.eyePosition();
        camera.setMode(Camera::Mode::Free);
        camera.centreX = eye[0] - view.forward[0] * options.sceneViewBack;
        camera.centreY = eye[1] - view.forward[1] * options.sceneViewBack;
        camera.focusHeight = eye[2] - view.forward[2] * options.sceneViewBack + options.sceneViewBack * 0.45;
        menu.panel().sceneView = true;
    }

    // The runtime profiler's window (Ctrl/Cmd+P, or ASR_PROFILER=1 at start).
    ui::ProfilerWindow profiler;
    if (window && std::getenv("ASR_PROFILER")) profiler.open();
    // ASR_PROFILE_TRACE=file: sampled without a window, the frames written as a
    // Chrome trace on the way out (chrome://tracing, ui.perfetto.dev).
    struct TraceOnExit {
        const char* file = std::getenv("ASR_PROFILE_TRACE");
        TraceOnExit() { if (file) engine::profile::setEnabled(true); }
        ~TraceOnExit() {
            if (!file) return;
            std::string why;
            if (!engine::profile::writeChromeTrace(file, engine::profile::frames(), &why)) std::cerr << why << "\n";
            engine::profile::setEnabled(false);
        }
    } traceOnExit;
    while (running) {
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Begin);
#endif
        viewportOf(camera);
        camera.minZoom = std::min(camera.viewportWidth / worldMetres,
                                  camera.viewportHeight * 2.0 / worldHighMetres) *
                         0.9;
        pointer.wheel = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (profiler.handle(event)) continue;
            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_P &&
                (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) {
                if (window) profiler.toggle();
                continue;
            }
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (!shotPath.empty() || measuring || tracing || scripted) continue;
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_G && (event.key.mod & SDL_KMOD_SHIFT)) {
                renderer.cycleTerrainGrid();
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_M && (event.key.mod & SDL_KMOD_SHIFT)) {
                renderer.toggleObjectWireframe();
                const auto& work = renderer.runner().work();
                std::printf("Object mesh %s - %zu models drawn, %s%zu triangles, "
                            "%zu draws, %zu indirect\n",
                            renderer.objectWireframe() ? "ON" : "OFF",
                            std::size_t(work.instances),
                            work.unknownIndirectDraws ? ">=" : "",
                            work.triangles, work.draws, work.indirectDraws);
                std::fflush(stdout);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_C && !menu.visible() && !menu.editor().active()) {
                possess(!possessed);
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_SPACE && possessed) {
                heroJump = true;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_V && !menu.visible() && possessed) {
                // Third person -> first person -> the map -> third person.
                if (camera.mode == Camera::Mode::Orbit) camera.setMode(Camera::Mode::Free);
                else if (camera.mode == Camera::Mode::Free) {
                    camera.setMode(Camera::Mode::Map);
                    camera.pitch = kDefaultCameraPitch;
                    camera.pixelsPerTile = heroMapZoom;
                } else {
                    heroMapZoom = camera.pixelsPerTile;
                    camera.setMode(Camera::Mode::Orbit);
                    camera.pitch = 0.32;
                }
                std::cout << "Camera: " << camera.modeName() << '\n';
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     event.key.key == SDLK_V && !menu.visible()) {
                camera.setMode(camera.mode == Camera::Mode::Map ? Camera::Mode::Orbit :
                               camera.mode == Camera::Mode::Orbit ? Camera::Mode::Free : Camera::Mode::Map);
                controls.dragging = controls.orbiting = false;
                controls.velocityX = controls.velocityY = 0;
                std::cout << "Camera: " << camera.modeName() << '\n';
            }
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F7) {
                renderer.toggleTerrainSkirts();
                std::printf("Terrain skirts %s (F7 toggles seam walls only)\n",
                            renderer.terrainSkirts() ? "ON" : "OFF");
                std::fflush(stdout);
            }
            // Explore / Edit.
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_GRAVE) {
                menu.toggleEditor();
                std::cout << (menu.editor().active()
                                      ? "Edit mode: pick the regions or a layer in the panel; LMB paints, "
                                        "Alt+LMB erases, [ ] brush size, Ctrl+Z undo, Enter builds\n"
                                      : "Explore mode\n");
            }
            else if (menu.editor().handle(event)) continue;
            else if (menu.handle(event)) continue;
            else if (event.type == SDL_EVENT_MOUSE_WHEEL) pointer.wheel += event.wheel.y;
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                running = false;
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     (event.key.key == SDLK_1 || event.key.key == SDLK_KP_1) &&
                     !menu.visible() && shotPath.empty() && !measuring && !tracing && !scripted) {
                const auto& landmarks = snapshot->landmarks();
                const auto desert = std::find_if(landmarks.begin(), landmarks.end(),
                        [](const auto& landmark) { return landmark.name == "desert"; });
                if (desert == landmarks.end()) {
                    std::cout << "No desert in this world\n";
                    continue;
                }
                camera.centreX = desert->where.x.toDouble();
                camera.centreY = desert->where.y.toDouble();
                if (camera.mode == Camera::Mode::Free)
                    camera.focusHeight = field.heightAt(desert->where).toDouble() + 80.0;
                controls.velocityX = controls.velocityY = 0;
                controls.dragging = false;
                pointer.wheel = 0;
                // Focus height and terrain streaming follow the new position below.
                std::cout << "Teleported to desert\n";
            }
        }
        float mx = 0, my = 0;
        const SDL_MouseButtonFlags buttons = profiler.hasMouse() ? 0 : SDL_GetMouseState(&mx, &my);
        int windowWidth = camera.viewportWidth, windowHeight = camera.viewportHeight;
        if (window) SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        mx *= static_cast<float>(camera.viewportWidth) / std::max(1, windowWidth);
        my *= static_cast<float>(camera.viewportHeight) / std::max(1, windowHeight);
        pointer.mouseX = mx;
        pointer.mouseY = my;
        pointer.middleDown = (buttons & SDL_BUTTON_MMASK) != 0;
        pointer.rightDown = (buttons & SDL_BUTTON_RMASK) != 0;
        // The graphics window, in its own pixels (top right of the view).
        const bool leftDown = (buttons & SDL_BUTTON_LMASK) != 0;
        bool overGraphics = false;
        // The world editor's panel, in its own pixels (left, under the strip).
        bool overEditor = false;
        {
            auto& input = menu.editor().panelInput();
            input.mouseX = mx - WorldEditor::kPanelX; input.mouseY = my - WorldEditor::kPanelY;
            input.pressed = leftDown && !leftWasDown; input.released = !leftDown && leftWasDown;
            input.down = leftDown; input.wheel = 0;
            overEditor = menu.editor().active() && input.mouseX >= 0 && input.mouseY >= 0 &&
                         input.mouseX < WorldEditor::kWide && input.mouseY < WorldEditor::kHigh;
        }
        {
            auto& input = menu.panelInput();
            const float px = float(camera.viewportWidth) - GraphicsPanel::kWide - 16, py = 16;
            input.mouseX = mx - px; input.mouseY = my - py;
            input.pressed = leftDown && !leftWasDown; input.released = !leftDown && leftWasDown;
            input.down = leftDown; input.wheel = 0;
            overGraphics = menu.panelVisible() && input.mouseX >= 0 && input.mouseY >= 0 &&
                           input.mouseX < GraphicsPanel::kWide && input.mouseY < GraphicsPanel::kHigh;
            leftWasDown = leftDown;
            auto& panel = menu.panel();
            if (panel.saveRequested) {
                panel.saveRequested = false;
                panel.status = game::saveGraphicsSettings(graphicsFile(), menu.graphics()) ? "saved graphics.json" : "could not save";
                menu.panelChanged();
            }
            if (panel.resetRequested) {
                panel.resetRequested = false;
                menu.graphics() = game::GraphicsSettings{};
                panel.status = "defaults";
                menu.panelChanged();
            }
            if (panel.sceneViewToggled) {
                panel.sceneViewToggled = false;
                if (frozen) {
                    // Back to the camera the frame was frozen at.
                    camera = *frozen; frozen.reset();
                    std::cout << "Scene view OFF: culling follows the camera again\n";
                } else {
                    frozen = camera;
                    camera.setMode(Camera::Mode::Free);
                    std::cout << "Scene view ON: decisions frozen; fly with RMB + WASD, F to return\n";
                }
                panel.sceneView = frozen.has_value();
                controls.dragging = controls.orbiting = false;
                controls.velocityX = controls.velocityY = 0;
                menu.panelChanged();
            }
        }
        // The resolution dropdown. Its sizes are pixels, and SDL sizes a window
        // in points, so they are divided by the display's density: on a Retina
        // screen 1920 x 1080 is a 960 x 540 point window. The render targets
        // follow the swapchain on the next frame by themselves.
        if (window && menu.graphics().resolution != appliedResolution) {
            appliedResolution = menu.graphics().resolution;
            const auto& wanted = game::kResolutions[std::size_t(appliedResolution)];
            if (wanted.width == 0) {
                SDL_SetWindowFullscreen(window, true);
            } else if (wanted.width > 0) {
                SDL_SetWindowFullscreen(window, false);
                const float density = std::max(0.25f, SDL_GetWindowPixelDensity(window));
                SDL_SetWindowSize(window, int(std::lround(wanted.width / density)),
                                  int(std::lround(wanted.height / density)));
            }
        }
        view.cull(frozen ? &*frozen : nullptr);

        const Uint64 now = SDL_GetTicks();
        const double step = std::clamp((now - last) / 1000.0, 0.0, 0.1);
        last = now;
        if (shotPath.empty() && !measuring && !tracing && !scripted) menu.advanceWeather(step);
        // A run that exists to take one picture takes no orders. The window is
        // up while it waits for the streaming to settle, and a stray scroll from
        // whatever else is on the desktop lands in it: twice now a shot has come
        // back from the far side of the world at minimum zoom, which is not a
        // picture of the place that was asked for.
        // While the menu is up the arrows belong to it, not to the camera.
        const bool active = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool heroLook = possessed && camera.mode != Camera::Mode::Map;
        const bool capture = active && (heroLook || (camera.mode == Camera::Mode::Free && pointer.rightDown)) &&
                             !menu.visible() && shotPath.empty() && !measuring && !tracing && !scripted;
        if (capture != relativeMouse) {
            if (SDL_SetWindowRelativeMouseMode(window, capture)) relativeMouse = capture;
            controls.orbiting = false;
            SDL_GetRelativeMouseState(nullptr, nullptr);
        }
        float heroDx = 0, heroDy = 0;
        if (relativeMouse) {
            float dx = 0, dy = 0;
            SDL_GetRelativeMouseState(&dx, &dy);
            heroDx = dx; heroDy = dy;
            pointer.mouseX = controls.orbiting ? controls.dragFromX + dx : 0;
            pointer.mouseY = controls.orbiting ? controls.dragFromY + dy : 0;
        }
        if (shotPath.empty() && !measuring && !tracing && !scripted && !menu.visible() && active) {
            // The draw distance slider keeps the pointer while it is dragged,
            // even off the panel; the camera must not pan under it meanwhile.
            const bool slider = !relativeMouse && menu.pointer(mx - 16, my - 16, (buttons & SDL_BUTTON_LMASK) != 0);
            const bool overPanel = slider || overGraphics || overEditor || (!relativeMouse && mx >= 16 && my >= 16 &&
                                  mx < 16 + ExploreMenu::kWide && my < 16 + ExploreMenu::kStatusHigh);
            static const bool none[SDL_SCANCODE_COUNT]{};
            const bool* keys = profiler.hasKeyboard() ? none : SDL_GetKeyboardState(nullptr);
            if (possessed) {
                // The character takes the keys; the mouse turns the camera
                // (captured, no button held), the wheel sets the distance.
                if (heroLook && relativeMouse) {
                    camera.orbit(heroDx * 0.0035, heroDy * 0.0035);
                    if (camera.mode == Camera::Mode::Orbit)
                        camera.pitch = std::clamp(camera.pitch, -0.35, 1.35);
                }
                if (camera.mode == Camera::Mode::Orbit && pointer.wheel != 0)
                    heroDistance = std::clamp(heroDistance * std::pow(0.88, pointer.wheel), 1.8, 14.0);
                if (camera.mode == Camera::Mode::Map && pointer.wheel != 0)
                    camera.zoomAt(std::pow(1.15, pointer.wheel), camera.viewportWidth / 2, camera.viewportHeight / 2);
                pointer.wheel = 0;
                CharacterController::Input in;
                in.forward = double(keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) -
                             double(keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN]);
                in.right = double(keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) -
                           double(keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
                in.sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
                in.walk = keys[SDL_SCANCODE_LALT] || keys[SDL_SCANCODE_RALT];
                in.jump = heroJump;
                in.cameraYaw = camera.yaw;
                hero.update(in, step, groundAt);
                heroJump = false;
            } else
                controls.update(camera, pointer, keys, step, overPanel);
            // Edit mode: the brush follows the pointer over the ground, and the
            // left button paints regions into the selection (Alt: out of it).
            // A stroke has to START on the ground: a drag that began on the
            // panel - a slider - never paints the map it wanders onto.
            if (menu.editor().active()) {
                if (menu.editor().panelInput().pressed) editPainting = !overPanel;
                if (!leftDown) editPainting = false;
                const bool onScreen = mx >= 0 && my >= 0 && mx < camera.viewportWidth && my < camera.viewportHeight;
                const bool valid = !relativeMouse && !overPanel && onScreen;
                double wx = 0, wy = 0;
                if (valid) {
                    // Onto the ground rather than onto a plane: twice, the second
                    // time at the height the first one found.
                    double z = camera.focusHeight - camera.heightOffset;
                    for (int pass = 0; pass < 2; ++pass) {
                        camera.worldOfScreenAtHeight(int(mx), int(my), z, wx, wy);
                        z = field.heightAt({core::Fixed::fromDoubleForContent(std::clamp(wx, 0.0, worldMetres - 1.0)),
                                            core::Fixed::fromDoubleForContent(std::clamp(wy, 0.0, worldHighMetres - 1.0))})
                                    .toDouble();
                    }
                }
                const bool alt = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
                menu.editor().pointer(wx, wy, valid, editPainting && !alt, editPainting && alt);
            }
        } else {
            if (active && !relativeMouse && shotPath.empty() && !scripted)
                menu.pointer(mx - 16, my - 16, (buttons & SDL_BUTTON_LMASK) != 0);
            controls.dragging = controls.orbiting = false;
            controls.velocityX = controls.velocityY = 0;
        }

        // Somebody moved a number in the editor, in the other window.
        if (now - groundLookedAt > 300) {
            groundLookedAt = now;
            if (groundWatch.changed(renderer.ground())) menu.groundReread();
        }
        // History is saved as it happens, from the delta's own thread: the
        // frame only asks, and never waits for a disk.
        if (worldDelta && !dig && worldDelta->root() && now - deltaSavedAt > 20000) {
            deltaSavedAt = now;
            if (worldDelta->dirty()) worldDelta->saveInBackground();
        }

        // A different world: asked for by Enter in the world menu, or by the
        // world editor. The menu's is a new world, and the camera goes to where
        // it starts; the editor's is the same world changed, and the camera
        // stays where the person is working.
        const auto rebuild = [&](const generation::WorldMapParams& wanted, bool recentre,
                                 const generation::WorldLayout* layout = nullptr) {
            const auto began = std::chrono::steady_clock::now();
            // A layout is made of its generations (world_compose.hpp).
            if (layout) builder.publish(generation::generateLayoutWorld(*layout));
            else builder.build(wanted);
            snapshot = builder.read();
            world = &snapshot->worldMap();
            field = snapshot->field();
            menu.editor().builtWorld(*world);
            // The card is holding the old country under the new one's chunk
            // numbers; none of it means anything now.
            menu.resetEnvironment();
            worldMetres = static_cast<double>(wanted.width) * generation::kMetresPerCell;
            worldHighMetres = static_cast<double>(wanted.height) * generation::kMetresPerCell;
            camera.setBounds(0, 0, worldMetres, worldHighMetres);
            if (recentre) {
                const core::WorldPos where = snapshot->startingPoint();
                camera.centreX = where.x.toDouble();
                camera.centreY = where.y.toDouble();
            }
            camera.centreX = std::clamp(camera.centreX, 0.0, worldMetres - 1.0);
            camera.centreY = std::clamp(camera.centreY, 0.0, worldHighMetres - 1.0);
            const core::WorldPos here{core::Fixed::fromDoubleForContent(camera.centreX),
                                      core::Fixed::fromDoubleForContent(camera.centreY)};
            if (recentre || camera.mode != Camera::Mode::Free)
                camera.focusHeight = field.heightAt(here).toDouble() +
                                     (camera.mode == Camera::Mode::Free ? 80.0 : camera.heightOffset);
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        };
        if (menu.wanted()) {
            // Another world: this one's history does not belong to it.
            builder.attach(nullptr);
            rebuild(menu.params(), true);
            menu.editor().showing(layoutShowing(menu.params(), worldPresets));
            menu.built(0);
        }
        if (menu.editor().wanted()) {
            // The same world with its base changed: the history carries over,
            // the ground dug into it with it.
            if (worldDelta) builder.attach(worldDelta->objects(), worldDelta->heights());
            const generation::WorldMapParams wanted = generation::paramsFor(menu.editor().layout());
            std::cout << "building a " << menu.editor().layout().regionsX << "x" << menu.editor().layout().regionsY
                      << " region world ..." << std::flush;
            double seconds = 0;
            try {
                seconds = rebuild(wanted, false, &menu.editor().layout());
                std::cout << " " << seconds << " s\n";
            } catch (const std::exception& error) {
                std::cout << " failed: " << error.what() << "\n";
                seconds = -1;
            }
            menu.editor().built(seconds);
        }
        camera.clampTo(static_cast<int>(worldMetres), static_cast<int>(worldHighMetres));
        const core::WorldPos focus{core::Fixed::fromDoubleForContent(camera.centreX),
                                   core::Fixed::fromDoubleForContent(camera.centreY)};
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Focus);
#endif
        // The camera only needs a height, not normals/materials/soil/path costs.
        if (camera.mode != Camera::Mode::Free)
            camera.focusHeight = field.heightAt(focus).toDouble() + camera.heightOffset;
        if (possessed) {
            // Scripted shots walk it themselves (--character-run): nobody is
            // at the keys.
            if (options.characterRun != 0 && (!shotPath.empty() || scripted || !active)) {
                CharacterController::Input in;
                in.forward = 1;
                in.sprint = options.characterRun > 1.5;
                in.walk = options.characterRun < 0.75;
                in.cameraYaw = camera.yaw;
                hero.update(in, step, groundAt);
            }
            if (camera.mode == Camera::Mode::Orbit) {
                camera.pixelsPerTile = camera.viewportHeight / (std::tan(camera.verticalFov * 0.5) * heroDistance);
                // Over the right shoulder, as the genre does: the character a
                // little left of the middle, the way ahead clear of him.
                const double shoulder = std::min(0.55, heroDistance * 0.12);
                camera.centreX = hero.x() + std::sin(camera.yaw) * shoulder;
                camera.centreY = hero.y() - std::cos(camera.yaw) * shoulder;
                camera.focusHeight = hero.z() + CharacterController::kShoulderHeight;
                // The eye stays out of the ground: a slope behind the character
                // pushes it up rather than under.
                const auto eye = camera.eyePosition();
                const double floor = groundAt(eye[0], eye[1]) + 0.35;
                if (eye[2] < floor) camera.pitch = std::min(1.35, camera.pitch + (floor - eye[2]) / heroDistance);
            } else if (camera.mode == Camera::Mode::Free) {
                // At the eyes, a little in front of the face so the inside of
                // the hood is never in view (the head itself is not drawn).
                const auto* pass = renderer.characterPass();
                const auto* model = pass ? pass->model() : nullptr;
                const double eye = model ? model->eyeHeight() : CharacterController::kEyeHeight;
                const double ahead = model ? model->eyeForward() : 0.12;
                camera.centreX = hero.x() + std::cos(hero.facing()) * ahead;
                camera.centreY = hero.y() + std::sin(hero.facing()) * ahead;
                camera.focusHeight = hero.z() + eye;
            } else {
                camera.centreX = hero.x();
                camera.centreY = hero.y();
                camera.focusHeight = hero.z();
            }
        }
        {
            game::CharacterPass::State state;
            // First person: the body is not drawn (the eye is inside it).
            // First person draws the body without its head: look down and
            // the legs are there.
            state.visible = possessed;
            state.firstPerson = camera.mode == Camera::Mode::Free;
            state.x = hero.x(); state.y = hero.y(); state.z = hero.z();
            state.yaw = hero.facing();
            state.gait = hero.gait();
            state.lookX = -std::cos(camera.yaw) * std::cos(camera.pitch);
            state.lookY = -std::sin(camera.yaw) * std::cos(camera.pitch);
            state.lookZ = -std::sin(camera.pitch);
            state.ground = groundAt;
            state.seconds = step;
            renderer.character(state);
        }
        if (bench) {
            bench->aim(camera, [&](double x, double y) {
                return field.heightAt({core::Fixed::fromDoubleForContent(x),
                                       core::Fixed::fromDoubleForContent(y)}).toDouble();
            }, renderer.settled());
            if (camera.mode != Camera::Mode::Free) {
                const core::WorldPos aimed{core::Fixed::fromDoubleForContent(camera.centreX),
                                           core::Fixed::fromDoubleForContent(camera.centreY)};
                camera.focusHeight = field.heightAt(aimed).toDouble() + camera.heightOffset;
            }
        }
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Inspection);
#endif
        if (menu.inspecting() && now - inspectedAt >= 250) {
            inspectedAt = now;
            const auto weather = menu.weatherSnapshot();
            menu.inspectionReadout(inspectWorld(*world, field, camera, static_cast<int>(mx), static_cast<int>(my), &weather));
        }
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Scene);
#endif

        // A crowd, if one was asked for. Nobody in this mode has anything to
        // draw as a card - there is no game here - so this is the only way to
        // measure the instanced path before the game moves onto it: strew N
        // cards over the ground the camera is looking at and let the profiler
        // say what they cost and in how many calls.
        //
        // At the height the camera is focused on rather than at the height of
        // the ground under each card, on purpose. Asking the field for a
        // hundred thousand heights a frame measures the field, and the field has
        // been measured (D135); what is being measured here is the drawing.
        renderer.sprites().begin();
#if ASR_ENABLE_PROFILING
        ExploreDiagnostics::crowd(renderer.sprites(), camera, crowd);
#endif
        renderer.sprites().sort();

        // The streaming is a pass of the calculations pipeline now, so this is
        // the whole of the frame: hand the camera over and let the runner run.
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Draw);
#endif
        menu.viewReadout(camera.modeName());
        menu.stagesAvailable(world->terrainFoundation != nullptr);
        if (!view.draw(camera)) {
            std::cerr << "the frame would not draw: " << renderer.error() << "\n";
            return 1;
        }
        engine::profile::frame();
        profiler.draw();
        ++frame;
        {
        const auto& cutWork = renderer.runner().work();
        // ASR_TRACE_CUT: one line per frame of what the ground and water
        // cut drew, to catch a cut that flips between two states while
        // the camera is still (the shoreline flicker).
        if (std::getenv("ASR_TRACE_CUT")) {
            std::printf("cut frame %llu tris %llu draws %u terrain %u water %u levels",
                    (unsigned long long)frame, (unsigned long long)cutWork.triangles, cutWork.draws,
                    cutWork.drawsByAuthor[1], cutWork.drawsByAuthor[3]);
            for (const auto count : cutWork.groundByLevel) std::printf(" %u", count);
            std::printf("\n");
        }
        }
        if (bench && bench->record(renderer.runner(), renderer.settled())) return 0;
#if ASR_ENABLE_PROFILING
        diagnostics.observe(renderer, frame, measuring);
        if (diagnostics.advance(camera, renderer, frame, measuring, closeUp, tracing)) return 0;
#endif
#if ASR_ENABLE_FPS
        const auto& rate = renderer.runner().frameRate();
        if (shotPath.empty() && rate.revision != reported) {
            reported = rate.revision;
            // Triangles as SUBMITTED, which is the only figure that is a fact.
            //
            // What the rasteriser then discards to back faces is not reported
            // by SDL's GPU interface at all - there are no pipeline statistics
            // to ask - so a "culled by back face" number here would be a guess
            // dressed as a measurement. Occlusion culling is not a number
            // either: there is none in this renderer, so nothing is culled by
            // it and saying "0" would read as a result rather than an absence.
            //
            // CPU-authored indirect plans (models) have exact counts. Only
            // GPU-authored counts (grass culling) remain unknown, so mark the
            // total as a lower bound whenever those are present.
            const auto& work = renderer.runner().work();
            // What the frame SPENT against what it WAITED for. `wait` is the
            // swapchain acquire, not a GPU timestamp: it can be display pacing
            // OR GPU backpressure. Reducing GPU work helps only the latter.
            const auto& timing = renderer.runner().frameTiming();
            char title[320];
            std::snprintf(title, sizeof(title),
                          "%sCampfire - %.0f fps, %.1f ms - %s - %s%.0fk tris, %u draws, "
                          "%u indirect (%u uncounted) - models %u drawn / %u occluded"
                          " - cpu %.1f (fix %.1f wait %.1f set %.1f work %.1f sub %.1f)%s",
#ifndef __OPTIMIZE__
                          "[UNOPTIMISED - timings meaningless] ",
#else
                          "",
#endif
                          rate.fps, rate.millis, camera.modeName(),
                          work.unknownIndirectDraws ? ">=" : "",
                          double(work.triangles) / 1000.0, work.draws, work.indirectDraws,
                          work.unknownIndirectDraws,
                          work.instances, work.culledHorizon,
                          timing.cpu(), timing.fixed,
                          timing.acquire, timing.setup, timing.pipelines, timing.submit,
                          ASR_ENABLE_PROFILING ? " [profiling]" : "");
            if (window) SDL_SetWindowTitle(window, title);
            else if (!bench) std::printf("%s\n", title);
        }
#endif

        // A picture of a half-built world is not a picture of the world, so the
        // shot waits until the streaming has caught up with the view - unless it
        // was asked for at a particular frame, which is how the half-built state
        // gets looked at on purpose.
        const bool settled = renderer.settled();
        // A picture waits for the streaming to have been caught up for a
        // while, not for one frame of it: the first frames of a place can be
        // settled ground under trees still standing in as their coarse
        // stand-ins, which is a picture of the loading, not of the place.
        // Counted, not consecutive: a view whose streaming flickers between
        // done and one page short would otherwise never be taken.
        shotSettledFrames += settled ? 1 : 0;
        // --dig lands on settled ground, so what follows is the live path - the
        // resident pages going stale and being written over - and the shot
        // waits for the ground to settle a second time.
        if (dig && !dug && settled) {
            world::Brush brush;
            brush.kind = dig->metres < 0 ? world::BrushKind::Lower : world::BrushKind::Raise;
            brush.radiusMetres = dig->radius;
            brush.strength = std::abs(dig->metres);
            const auto current = builder.read()->field();
            const world::GroundAt ground = [&current](core::Fixed x, core::Fixed y) {
                return current.heightAt({x, y});
            };
            const auto samples = worldDelta->brush(
                brush, ground, {core::Fixed::fromDoubleForContent(dig->x), core::Fixed::fromDoubleForContent(dig->y)},
                1.0, world::delta::Origin::Authoring);
            std::cout << "dug " << samples << " samples at " << dig->x << ", " << dig->y << " ("
                      << dig->metres << " m over " << dig->radius << " m) - frame " << frame << "\n";
            dug = frame;
            continue;
        }
        // A listed shot after the first waits for the camera's move to reach
        // the streaming: "settled" is about the view the last frame asked for.
        const bool aimedLongEnough = shotFrame > 0 || frame >= shotAimedAt + 8;
        // And never for ever: a listed view that has not settled in 1500
        // frames is taken as it is (and said so), so one stubborn view does
        // not hold up the rest of the list.
        const bool shotOverdue = !shotList.empty() && shotFrame <= 0 && frame >= shotAimedAt + 1500;
        if (shotOverdue && !shotPath.empty())
            std::cout << "not settled after 1500 frames: " << shotPath << " taken as it is\n";
        if (!shotPath.empty() && aimedLongEnough &&
            (shotOverdue || (shotFrame > 0 ? frame >= shotAimedAt + shotFrame
                           : settled && shotSettledFrames >= 45 && (!dig || (dug && frame > dug + 2))))) {
            if (!renderer.screenshot(shotPath)) {
                std::cerr << "the screenshot would not save: " << renderer.error() << "\n";
                return 1;
            }
            // A black picture is a frame that went wrong, not the place: say
            // so and take it again a little later (twice at most).
            if (shotBlackRetries < 2) {
                double mean = -1;
                if (SDL_Surface* written = IMG_Load(shotPath.c_str())) {
                    if (SDL_Surface* rgba = SDL_ConvertSurface(written, SDL_PIXELFORMAT_RGBA32)) {
                        double sum = 0; std::size_t n = 0;
                        for (int y = 0; y < rgba->h; y += 8) {
                            const auto* row = static_cast<const Uint8*>(rgba->pixels) + std::size_t(y) * rgba->pitch;
                            for (int x = 0; x < rgba->w; x += 8, ++n) sum += row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2];
                        }
                        mean = n ? sum / (n * 3 * 255.0) : -1;
                        SDL_DestroySurface(rgba);
                    }
                    SDL_DestroySurface(written);
                }
                if (mean >= 0 && mean < 0.015) {
                    std::cout << "black frame at " << shotPath << " (frame " << frame << ", error '"
                              << renderer.error() << "') - taking it again\n";
                    ++shotBlackRetries;
                    shotAimedAt = frame + 60;
                    continue;
                }
            }
            shotBlackRetries = 0;
            const auto& shotWork = renderer.runner().work();
            const auto shotEye = camera.eyePosition();
            std::cout << "wrote " << shotPath << " - frame " << frame
                      << ", seed " << world->seed << ", at " << camera.centreX << ", " << camera.centreY
                      << ", eye z " << shotEye[2] << " over ground "
                      << field.heightAt({core::Fixed::fromDoubleForContent(shotEye[0]),
                                         core::Fixed::fromDoubleForContent(shotEye[1])}).toDouble()
                      << " (water " << field.waterLevelAt({core::Fixed::fromDoubleForContent(shotEye[0]),
                                         core::Fixed::fromDoubleForContent(shotEye[1])}).toDouble() << ")"
                      << ", yaw " << camera.yaw << " pitch " << camera.pitch << " (" << camera.modeName() << ")"
                      << " - " << (shotWork.unknownIndirectDraws ? ">=" : "")
                      << shotWork.triangles << " triangles, " << shotWork.draws
                      << " draws, " << shotWork.indirectDraws << " indirect; "
                      << shotWork.unknownIndirectDraws << " indirect counts unknown; "
                      << shotWork.instances << " models drawn, " << shotWork.culledFrustum
                      << " off screen, " << shotWork.culledHorizon << " behind the ground\n";
            std::cout << "  draws by author:";
            for (std::size_t author = 0; author < shotWork.drawsByAuthor.size(); ++author)
                std::cout << ' ' << author << '=' << shotWork.drawsByAuthor[author];
            std::cout << '\n';
            std::cout << "  ground squares by level (4 m first):";
            for (std::size_t level = 0; level < shotWork.groundByLevel.size(); ++level)
                if (shotWork.groundByLevel[level] > 0)
                    std::cout << "  " << (4 << level) << "m:" << shotWork.groundByLevel[level]
                              << "/" << shotWork.groundTrianglesByLevel[level] << "tri";
            static const char* const kAuthors[]{"other","ground","backdrop","water",
                                                 "foliage","models","weather","-"};
            std::cout << "\n  triangles by pass:";
            for (std::size_t a = 0; a < shotWork.trianglesByAuthor.size(); ++a)
                if (shotWork.trianglesByAuthor[a] > 0)
                    std::cout << "  " << kAuthors[a] << ":" << shotWork.trianglesByAuthor[a];
            std::cout << '\n';
#if ASR_ENABLE_PROFILING
            if (std::getenv("ASR_GRASS_COMPARE_CULL") && !view.compareGrassCulling(camera,shotPath)) {
                std::cerr << "grass comparison failed: " << renderer.error() << '\n';return 1;
            }
            if (std::getenv("ASR_TERRAIN_COMPARE_SKIRTS")) {
                // Same world, camera and held shader time: only seam walls differ.
                // Useful for verifying that buried skirts cannot occlude the surface.
                const bool hadSkirts = renderer.terrainSkirts();
                const std::string comparison = shotPath + (hadSkirts ? ".no-skirts.png" : ".skirts.png");
                renderer.toggleTerrainSkirts();
                const bool ok = view.draw(camera) && renderer.screenshot(comparison);
                renderer.toggleTerrainSkirts();
                if (!ok) {
                    std::cerr << "skirt comparison failed: " << renderer.error() << '\n';
                    return 1;
                }
                std::cout << "wrote skirt comparison " << comparison << '\n';
            }
#endif
            if (shotNext < shotList.size()) {
                const auto& next = shotList[shotNext++];
                aimListed(next);
                graphicsForShot(next);
                shotPath = next.path;
                shotAimedAt = frame;
                shotSettledFrames = 0;
                std::cout.flush();
                continue;
            }
            return 0;
        }
    }
    return 0;
}

} // namespace client
