#include "game/client/explore_view.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>

#include "game/client/controls.hpp"
#include "game/client/explore_diagnostics.hpp"
#include "game/client/world_inspection.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/render/calc/terrain_collect.hpp"
#include "game/client/explore_menu.hpp"
#include "game/render/passes/menu_pass.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"
#include "game/world/foliage_catalog.hpp"
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
                       const Camera& camera, ExploreMenu& menu) {
    menu_ = &menu;
    source_ = &world;
    return renderer_.open(window, assets, world, [&menu](const game::GpuTerrain& terrain) {
        return std::make_unique<game::MenuPass>(menu, &terrain);
    });
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
    const auto snapshot = source_->read();
    const auto climate=snapshot->climate().at(focus).environment;
    const auto scalar=[](core::Fixed f) { return static_cast<float>(f.toDouble()); };
    const auto local=weather.at(scalar(climate[0]),scalar(climate[2]),static_cast<float>(camera.focusHeight),
        static_cast<float>(camera.centreX),static_cast<float>(camera.centreY),scalar(climate[5]));
    menu_->weatherReadout(local);
    menu_->stagesAvailable(bool(snapshot->worldMap().terrainFoundation));
    settings_.weather = weather;
    settings_.map = menu_->mapView();
    settings_.stage = menu_->terrainStage();
    settings_.iceVisible = menu_->iceVisible();
    settings_.potentialOnly = menu_->potentialOnly();
    return renderer_.draw(camera, settings_);
}

#if ASR_ENABLE_PROFILING
bool ExploreView::compareGrassCulling(const Camera& camera, const std::string& path) {
    return renderer_.compareGrassCulling(camera, settings_, path);
}
#endif

int runExploreMode(SDL_Window* window, std::uint64_t seed, std::int32_t worldCells,
                   const std::filesystem::path& assets, const std::string& shotPath,
                   double startZoom, const std::string& startAt, bool measuring, bool closeUp,
                   bool tracing, int shotFrame, bool showMenu, int crowd, double shotTime,
                   const std::string& mapName, double weatherDay, int weatherPreset,
                   const core::TimeConfig& calendar, const std::array<float,4>& seasons,
                   const ExploreViewOptions& options) {
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
    world::WorldSystem builder;
    builder.build(params);
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

    Camera camera;
    camera.centreX = start.x.toDouble();
    camera.centreY = start.y.toDouble();
    camera.isometric = true;
    camera.orbit(options.yaw - camera.yaw, options.pitch - camera.pitch);
    camera.heightOffset = std::clamp(options.heightOffset, -10000.0, 10000.0);
    camera.focusHeight = field.heightAt(start).toDouble() + camera.heightOffset;
    camera.pixelsPerTile = startZoom > 0 ? startZoom : 0.6;
    double worldMetres = static_cast<double>(params.width) * generation::kMetresPerCell;
    camera.setBounds(0, 0, worldMetres, worldMetres);
    SDL_GetWindowSizeInPixels(window, &camera.viewportWidth, &camera.viewportHeight);
    camera.setMode(options.cameraMode);
    camera.orbit(options.yaw - camera.yaw, options.pitch - camera.pitch);

    // The dials, on the screen, over the world they made. Built before the view
    // because the view's menu pass holds on to it, and told about the ground
    // after, because the ground is the view's and is read when the view opens.
    game::WorldRenderer renderer;
    ExploreView view(renderer);
    // A run that exists to take one picture has no business showing the wind.
    if (!shotPath.empty()) renderer.holdTime(shotTime);
    if (!view.open(window, assets, builder, camera, menu)) {
        std::cerr << "the explorer would not start: " << renderer.error() << "\n";
        return 1;
    }
    for (int i = 0; i < std::clamp(options.gridMode, 0, 2); ++i) renderer.cycleTerrainGrid();
    if (options.objectMesh != renderer.objectWireframe()) renderer.toggleObjectWireframe();
    std::cout << "V: map / 3rd orbit / 1st free flight; RMB: look; WASD: move; Q/E: altitude\n"
                 "Shift: fly faster; wheel: orbit distance / flight speed; Shift+G: sample / mesh grid / off;\n"
                 "Shift+M: object mesh on / off\n";
    menu.open(params, &renderer.ground(), groundFile());
    menu.configureWeather(calendar,seasons,weatherDay,weatherPreset);
    GroundWatch groundWatch(groundFile());
    Uint64 groundLookedAt = SDL_GetTicks();
    if (showMenu) menu.toggle();
    if (showMenu && startAt == "ground") menu.showGround();

    CameraControl controls;
    // The explorer is looked at, not played: the view moves when somebody drags
    // it or presses a key, and never on its own. A shot has to be of a still
    // camera or two of them cannot be compared.
    controls.edgePan = false;
    ui::Input pointer;
    bool relativeMouse = false;
    bool running = true;
    Uint64 last = SDL_GetTicks();
    int frame = 0;
#if ASR_ENABLE_FPS
    std::uint64_t reported = 0;
#endif
    Uint64 inspectedAt = 0;
#if ASR_ENABLE_PROFILING
    ExploreDiagnostics diagnostics;
#endif

    while (running) {
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Begin);
#endif
        SDL_GetWindowSizeInPixels(window, &camera.viewportWidth, &camera.viewportHeight);
        camera.minZoom = std::min(camera.viewportWidth / worldMetres,
                                  camera.viewportHeight * 2.0 / worldMetres) *
                         0.9;
        pointer.wheel = 0;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            else if (!shotPath.empty() || measuring || tracing) continue;
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
            else if (menu.handle(event)) continue;
            else if (event.type == SDL_EVENT_MOUSE_WHEEL) pointer.wheel += event.wheel.y;
            else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE)
                running = false;
            else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
                     (event.key.key == SDLK_1 || event.key.key == SDLK_KP_1) &&
                     !menu.visible() && shotPath.empty() && !measuring && !tracing) {
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
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
        int windowWidth = 1, windowHeight = 1;
        SDL_GetWindowSize(window, &windowWidth, &windowHeight);
        mx *= static_cast<float>(camera.viewportWidth) / std::max(1, windowWidth);
        my *= static_cast<float>(camera.viewportHeight) / std::max(1, windowHeight);
        pointer.mouseX = mx;
        pointer.mouseY = my;
        pointer.middleDown = (buttons & SDL_BUTTON_MMASK) != 0;
        pointer.rightDown = (buttons & SDL_BUTTON_RMASK) != 0;

        const Uint64 now = SDL_GetTicks();
        const double step = std::clamp((now - last) / 1000.0, 0.0, 0.1);
        last = now;
        if (shotPath.empty() && !measuring && !tracing) menu.advanceWeather(step);
        // A run that exists to take one picture takes no orders. The window is
        // up while it waits for the streaming to settle, and a stray scroll from
        // whatever else is on the desktop lands in it: twice now a shot has come
        // back from the far side of the world at minimum zoom, which is not a
        // picture of the place that was asked for.
        // While the menu is up the arrows belong to it, not to the camera.
        const bool active = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool capture = active && camera.mode == Camera::Mode::Free && pointer.rightDown &&
                             !menu.visible() && shotPath.empty() && !measuring && !tracing;
        if (capture != relativeMouse) {
            if (SDL_SetWindowRelativeMouseMode(window, capture)) relativeMouse = capture;
            controls.orbiting = false;
            SDL_GetRelativeMouseState(nullptr, nullptr);
        }
        if (relativeMouse) {
            float dx = 0, dy = 0;
            SDL_GetRelativeMouseState(&dx, &dy);
            pointer.mouseX = controls.orbiting ? controls.dragFromX + dx : 0;
            pointer.mouseY = controls.orbiting ? controls.dragFromY + dy : 0;
        }
        if (shotPath.empty() && !measuring && !tracing && !menu.visible() && active) {
            const bool overPanel = !relativeMouse && mx >= 16 && my >= 16 &&
                                  mx < 16 + ExploreMenu::kWide && my < 16 + ExploreMenu::kStatusHigh;
            controls.update(camera, pointer, SDL_GetKeyboardState(nullptr), step, overPanel);
        } else {
            controls.dragging = controls.orbiting = false;
            controls.velocityX = controls.velocityY = 0;
        }

        // Somebody moved a number in the editor, in the other window.
        if (now - groundLookedAt > 300) {
            groundLookedAt = now;
            if (groundWatch.changed(renderer.ground())) menu.groundReread();
        }

        // A different world, asked for by Enter.
        if (menu.wanted()) {
            generation::WorldMapParams wanted = menu.params();
            builder.build(wanted);
            snapshot = builder.read();
            world = &snapshot->worldMap();
            field = snapshot->field();
            // The card is holding the old country under the new one's chunk
            // numbers; none of it means anything now.
            menu.resetEnvironment();
            const core::WorldPos where = snapshot->startingPoint();
            worldMetres = static_cast<double>(wanted.width) * generation::kMetresPerCell;
            camera.setBounds(0, 0, worldMetres, worldMetres);
            camera.centreX = where.x.toDouble();
            camera.centreY = where.y.toDouble();
            camera.focusHeight = field.heightAt(where).toDouble() +
                                 (camera.mode == Camera::Mode::Free ? 80.0 : camera.heightOffset);
            menu.built(0);
        }
        camera.clampTo(static_cast<int>(worldMetres), static_cast<int>(worldMetres));
        const core::WorldPos focus{core::Fixed::fromDoubleForContent(camera.centreX),
                                   core::Fixed::fromDoubleForContent(camera.centreY)};
#if ASR_ENABLE_PROFILING
        diagnostics.mark(ExploreDiagnostics::Focus);
#endif
        // The camera only needs a height, not normals/materials/soil/path costs.
        if (camera.mode != Camera::Mode::Free)
            camera.focusHeight = field.heightAt(focus).toDouble() + camera.heightOffset;
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
        ++frame;
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
                          "%sAncient Settlement - %.0f fps, %.1f ms - %s - %s%.0fk tris, %u draws, "
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
            SDL_SetWindowTitle(window, title);
        }
#endif

        // A picture of a half-built world is not a picture of the world, so the
        // shot waits until the streaming has caught up with the view - unless it
        // was asked for at a particular frame, which is how the half-built state
        // gets looked at on purpose.
        const bool settled = renderer.settled();
        if (!shotPath.empty() && (shotFrame > 0 ? frame >= shotFrame : settled)) {
            if (!renderer.screenshot(shotPath)) {
                std::cerr << "the screenshot would not save: " << renderer.error() << "\n";
                return 1;
            }
            const auto& shotWork = renderer.runner().work();
            std::cout << "wrote " << shotPath << " - frame " << frame
                      << ", seed " << world->seed << ", at " << camera.centreX << ", " << camera.centreY
                      << " - " << (shotWork.unknownIndirectDraws ? ">=" : "")
                      << shotWork.triangles << " triangles, " << shotWork.draws
                      << " draws, " << shotWork.indirectDraws << " indirect; "
                      << shotWork.unknownIndirectDraws << " indirect counts unknown; "
                      << shotWork.instances << " models drawn, " << shotWork.culledFrustum
                      << " off screen, " << shotWork.culledHorizon << " behind the ground\n";
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
            return 0;
        }
    }
    return 0;
}

} // namespace client
