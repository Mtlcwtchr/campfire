#include "game/client/client_app.hpp"

#include "engine/core/profiler.hpp"
#include "engine/ui/profiler_window.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

#include "engine/core/progress.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "engine/ui/ui.hpp"
#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/codec.hpp"
#include "engine/world_store/world_root.hpp"
#include "game/client/client_ui.hpp"
#include "game/client/controls.hpp"
#include "game/client/hero_camera.hpp"
#include "game/client/explore_menu.hpp"
#include "game/client/explore_view.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_import.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/render/gpu_terrain.hpp"
#include "game/render/graphics_settings.hpp"
#include "game/render/world_renderer.hpp"
#include "game/world/world_delta.hpp"
#include "game/world/world_saves.hpp"
#include "game/world/world_system.hpp"
#include "game/world/world_tools.hpp"

namespace client {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

fs::path contentDirectory() {
    fs::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 5 && !fs::exists(content, ec); ++up) content = ".." / content;
    return content;
}
fs::path projectRoot() { return fs::absolute(contentDirectory()).lexically_normal().parent_path(); }

// A world that is open: its layout, its history, and the edits' undo.
struct Session {
    world::saves::SavedWorld save;
    generation::WorldLayout layout;              // as it is on disk
    std::unique_ptr<world::delta::WorldDelta> delta;
    std::unique_ptr<world::tools::Editing> editing;
};

// A world being raised off the frame thread.
struct Build {
    std::thread worker;
    std::atomic<bool> done{false};
    bool failed = false;
    std::string error;
    Clock::time_point began = Clock::now();
    bool entering = false;                       // a world being opened, not one being reshaped
    WorldMode mode = WorldMode::Explore;
    double generated = 0, published = 0;         // seconds each took, for the log
    double prebaked = 0;
    generation::GenerationTimings passes;
    generation::ComposeReport composed;
    std::atomic<bool> abandon{false};            // stop waiting: the client is going
    ~Build() { abandon = true; if (worker.joinable()) worker.join(); }
};

double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// The interface is compared, and repainted, in squares this many points on a
// side: small enough that a lit button or a changing number is a few of them,
// large enough that a whole screen is a thousand or so.
constexpr float kPaintTile = 32;

core::WorldPos pos(double x, double y) {
    return {core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y)};
}

} // namespace

namespace {
// What the system's file dialog picked, for which field of the Import form.
// The dialog answers on a thread of its own when it likes; the frame takes it.
struct DialogPick {
    std::mutex guard;
    int field = -1;
    std::optional<std::pair<int, std::string>> chosen;
};
void SDLCALL onPicked(void* user, const char* const* files, int) {
    auto* pick = static_cast<DialogPick*>(user);
    if (!files || !files[0]) return;
    const std::lock_guard<std::mutex> lock(pick->guard);
    pick->chosen = std::pair{pick->field, std::string(files[0])};
}
} // namespace

int runClientApp(SDL_Window* window, const fs::path& assets, const ClientOptions& options) {
    const fs::path root = projectRoot();
    const fs::path content = root / "content";
    const bool touring = !options.tour.empty();
    fs::path worldsDir = options.worlds.empty() ? root / "worlds" : options.worlds;
    // A tour of a world that is already made (CAMPFIRE_TOUR_OPEN) reads the
    // worlds where it was told to; every other tour makes its own afresh.
    if (touring && std::getenv("CAMPFIRE_TOUR_OPEN") && !options.worlds.empty()) {
        std::error_code ec;
        fs::create_directories(options.tour, ec);
    } else if (touring) {
        std::error_code ec;
        fs::create_directories(options.tour, ec);
        worldsDir = options.tour / "worlds";
        fs::remove_all(worldsDir, ec);
        fs::create_directories(worldsDir, ec);
    }
    const fs::path graphicsFile = root / "graphics.json";
    const auto presets = generation::loadWorldPresets(content / "config" / "world_presets.json");
    const fs::path fonts = assets.parent_path() / "fonts";
    const auto regular = ui::FontFace::load(fonts / "NotoSans-Regular.ttf");
    const auto bold = ui::FontFace::load(fonts / "NotoSans-Bold.ttf");
    if (!regular) std::cerr << "no interface font in " << fonts.string() << "; falling back to the debug font\n";
    if (window) SDL_SetWindowTitle(window, "Campfire");

    // --- the world behind the menu -------------------------------------------
    // Something to look at while choosing: a small world, raised in a moment.
    generation::WorldMapParams backdrop;
    backdrop.seed = 11;
    backdrop.width = backdrop.height = 64;
    world::WorldSystem system;
    system.build(backdrop);
    auto snapshot = system.read();
    auto field = snapshot->field();
    double worldW = double(backdrop.width) * generation::kMetresPerCell;
    double worldH = double(backdrop.height) * generation::kMetresPerCell;

    Camera camera;
    camera.isometric = true;   // the map view, when V gets there
    camera.setBounds(0, 0, worldW, worldH);
    const auto viewportOf = [&](Camera& into) {
        if (window) SDL_GetWindowSizeInPixels(window, &into.viewportWidth, &into.viewportHeight);
        else { into.viewportWidth = options.headlessWidth; into.viewportHeight = options.headlessHeight; }
    };
    viewportOf(camera);
    // A third-person view at a distance that shows country, not a lawn.
    const auto frame = [&](core::WorldPos at, double distance) {
        camera.setMode(Camera::Mode::Orbit);
        camera.centreX = at.x.toDouble();
        camera.centreY = at.y.toDouble();
        camera.pitch = 0.42;
        camera.heightOffset = 0;
        camera.pixelsPerTile = double(std::max(1, camera.viewportHeight)) /
                               (std::tan(camera.verticalFov * 0.5) * distance);
        camera.focusHeight = field.heightAt(at).toDouble();
    };
    frame(snapshot->startingPoint(), 1400);

    // --- the renderer and its windows ------------------------------------------
    ExploreMenu menu;
    menu.graphics() = game::loadGraphicsSettings(graphicsFile);
    if (touring) menu.graphics() = game::GraphicsSettings{};
    ui::Canvas canvas;
    auto& shown = menu.presentation();
    shown.developer = false;
    shown.canvas = &canvas;
    shown.font = regular;
    shown.bold = bold;
    shown.fontPoints = 12;
    game::WorldRenderer renderer;
    ExploreView view(renderer);
    if (touring) renderer.holdTime(0);
    if (!view.open(window, assets, system, camera, menu, options.headlessWidth, options.headlessHeight)) {
        std::cerr << "the client would not start: " << renderer.error() << "\n";
        return 1;
    }
    menu.open(backdrop, &renderer.ground(), content / "config" / "ground.json");
    menu.configureWeather({}, {18.0f, 32.0f, 21.0f, 9.0f}, 0.0, 0);

    ui::Ui ui;
    ClientUi screens;
    ui::Input input;
    std::uint64_t painted = 0;
    std::vector<std::uint64_t> paintedTiles;   // the picture on the canvas, tile by tile

    std::vector<world::saves::SavedWorld> worlds = world::saves::list(worldsDir);
    std::unique_ptr<Session> session;
    std::unique_ptr<Build> build;
    std::optional<Clock::time_point> settling;     // raised; waiting for the ground to be drawn
    WorldMode settlingMode = WorldMode::Explore;
    std::string loadingTitle;

    CameraControl controls;
    controls.edgePan = false;
    bool relativeMouse = false;
    // The player's character (hero_camera.hpp): put down where the world
    // starts when the world is shown; C lets it go and takes it back, V
    // cycles third person, first person and the map while it is held. Only
    // in Explore: Edit's tools want the pointer.
    HeroCamera hero;
    bool heroJump = false;
    const auto heroGround = [&](double x, double y) {
        return field.heightAt(pos(std::clamp(x, 0.0, worldW - 1), std::clamp(y, 0.0, worldH - 1))).toDouble();
    };
    bool leftWasDown = false;
    bool strokeOnGround = false;                   // the current press began on the world
    double lastApplyX = 0, lastApplyY = 0;
    double strokeLevel = 0;                        // the ground under a flatten when it began
    bool applied = false;
    std::mt19937 random(std::random_device{}());
    Uint64 last = SDL_GetTicks();
    const auto start = Clock::now();
    Clock::time_point savedAt = Clock::now();
    int appliedResolution = 0;
    bool unsavedNow = false;
    double paintMs = 0, paintMax = 0;   // what painting the interface costs
    double paintedArea = 0;             // and how much of it, in square points
    int paints = 0;
    bool running = true;
    int frameIndex = 0;

    DialogPick dialogPick;

    // --- what the actions do ----------------------------------------------------
    const auto startBuild = [&](const generation::WorldLayout& layout, bool entering, WorldMode mode) {
        build.reset();
        build = std::make_unique<Build>();
        build->entering = entering;
        build->mode = mode;
        const auto ticket = system.request();
        Build* b = build.get();
        build->worker = std::thread([&system, ticket, layout, b] {
            try {
                // A world made of its generations: each run on its own terms,
                // the empty regions not computed at all (world_compose.hpp).
                generation::setGenerationTimings(&b->passes);
                auto map = generation::generateLayoutWorld(layout, &b->composed);
                generation::setGenerationTimings(nullptr);
                b->generated = since(b->began);
                // Raised and its pages baked in memory BEFORE it is shown: the
                // world on the screen goes on being drawn the whole time, and
                // the new one arrives with its ground already there - no
                // blank country while the prebake holds every terrain worker.
                auto raised = system.raise(ticket, std::move(map));
                b->published = since(b->began) - b->generated;
                while (raised && raised->pages().prebakeProgress().running && !b->abandon && system.current(ticket))
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                b->prebaked = since(b->began) - b->generated - b->published;
                if (!raised || b->abandon || !system.publish(ticket, std::move(raised)))
                    b->error = "a newer world was asked for";
            } catch (const std::exception& e) {
                b->error = e.what();
                b->failed = true;
            }
            b->done = true;
        });
    };
    const auto unsaved = [&]() {
        if (!session) return false;
        return (session->delta && session->delta->dirty()) ||
               (menu.editor().layout().regionsX > 0 && menu.editor().layout() != session->layout);
    };
    const auto save = [&](bool background) {
        if (!session) return true;
        bool ok = true;
        if (menu.editor().layout() != session->layout) {
            if (generation::saveWorldLayout(menu.editor().layout(), session->save.layout)) session->layout = menu.editor().layout();
            else ok = false;
        }
        if (session->delta && session->delta->dirty()) {
            if (background) session->delta->saveInBackground();
            else {
                const auto report = session->delta->save();
                if (!report.ok) { ok = false; std::cerr << "world not saved: " << report.error << "\n"; }
            }
        }
        savedAt = Clock::now();
        return ok;
    };
    const auto closeSession = [&]() {
        if (hero.active()) hero.release(camera);
        if (!session) return;
        if (session->editing) session->editing->end();
        save(false);
        if (session->delta) session->delta->waitForSave();
        if (menu.editor().active()) menu.editor().toggle();
        system.attach(nullptr);
        session.reset();
        worlds = world::saves::list(worldsDir);
    };
    const auto open = [&](const world::saves::SavedWorld& chosen, WorldMode mode) {
        closeSession();
        auto layout = generation::loadWorldLayout(chosen.layout);
        if (!layout) {
            screens.alert("Cannot open " + chosen.name, "Its layout could not be read.");
            return;
        }
        world::delta::LoadReport report;
        auto history = world::delta::WorldDelta::open(engine::world_store::WorldRoot(chosen.root), layout->seed, &report);
        if (!history) {
            screens.alert("Cannot open " + chosen.name,
                          "Its history could not be read, and opening it anyway could overwrite it at the first save.");
            return;
        }
        for (const auto& warning : report.warnings) std::cerr << "world history: " << warning << "\n";
        const auto bytes = engine::world_store::readFileBytes(chosen.layout);
        history->source(world::saves::utf8Of(chosen.layout.filename()),
                        bytes ? engine::world_store::contentHash(*bytes) : 0);
        session = std::make_unique<Session>();
        session->save = chosen;
        session->layout = *layout;
        session->delta = std::move(history);
        session->editing = std::make_unique<world::tools::Editing>(*session->delta);
        system.attach(session->delta->objects(), session->delta->heights());
        // What was imported into the world (the Import tab) is built on; its
        // drained height made first only when a region was taken that far.
        layout->imported = generation::openImported(chosen.root / "source", *layout);
        if (screens.importForm.exportTo.empty())
            screens.importForm.exportTo = world::saves::utf8Of(chosen.root.parent_path() / (chosen.name + " maps"));
        menu.editor().open(presets, chosen.layout, *layout);
        // The details tool's "remove": the derived instance nearest the point,
        // from the scatter of the world as it stands (engine/biomes details).
        menu.editor().objectPicker([&system](double x, double y) -> std::optional<client::WorldEditor::PickedObject> {
            const auto now = system.read();
            if (!now) return std::nullopt;
            const auto cx = std::int64_t(std::floor(x)), cy = std::int64_t(std::floor(y));
            const auto found = now->scatter(world::decor::ScatterBounds{cx - 24, cy - 24, cx + 24, cy + 24});
            std::optional<client::WorldEditor::PickedObject> best;
            double nearest = 1e9;
            for (const auto& o : found.objects)
                if (const double d = std::hypot(o.x - x, o.y - y); d < nearest) {
                    nearest = d;
                    best = client::WorldEditor::PickedObject{o.id, o.x, o.y};
                }
            return best;
        });
        // The client saves the world (the top bar, the minute, the way out),
        // and a build swaps in without the world leaving the screen: the
        // shape is raised again after every stroke.
        menu.editor().ownFile(false);
        menu.editor().autoBuild(true);
        menu.editor().showWindow(false);
        screens.selectWorld(chosen.name);
        loadingTitle = "Raising " + chosen.name;
        screens.screen = Screen::Loading;
        screens.paused = false;
        startBuild(*layout, true, mode);
    };

    const auto act = [&](const ClientActions& a) {
        // The stages of the world's shape, through the world editor that
        // holds it.
        if (session) {
            auto& editor = menu.editor();
            const auto& shape = a.shape;
            if (shape.reshape) {
                const auto [west, east, north, south] = *shape.reshape;
                if (editor.reshape(west, east, north, south)) {
                    // What was there stays under the camera.
                    camera.centreX += double(west) * double(generation::kRegionMetres);
                    camera.centreY += double(north) * double(generation::kRegionMetres);
                }
            }
            if (shape.dials) editor.setAuthoring(*shape.dials);
            if (shape.latitude) editor.setLatitude(shape.latitude->first, shape.latitude->second);
            if (shape.pin) editor.pin();
            if (shape.water) editor.water();
            if (shape.undo) editor.undoStroke();
            if (shape.build) editor.build();
            // The Import tab.
            if (shape.selectAll) editor.selectAll(*shape.selectAll);
            if (shape.import) {
                WorldEditor::ImportRequest request;
                request.height = world::saves::pathOf(shape.import->height);
                request.control = world::saves::pathOf(shape.import->control);
                request.package = world::saves::pathOf(shape.import->package);
                request.lowMetres = shape.import->lowMetres;
                request.seaGrey = shape.import->autoSea ? -1.0 : double(shape.import->seaGrey);
                request.highMetres = shape.import->highMetres;
                request.featherKm = shape.import->featherKm;
                editor.importMaps(request);
            }
            // Or the same maps generated, a layer at a time.
            const auto generateRequest = [&](const GenerateForm& form) {
                WorldEditor::GenerateRequest request;
                // A number is that seed; anything else is a word, hashed.
                std::uint64_t seed = 0;
                bool digits = !form.seed.empty() && form.seed.size() <= 19;
                for (const char c : form.seed) digits = digits && c >= '0' && c <= '9';
                if (digits) seed = std::stoull(form.seed);
                else {
                    seed = 1469598103934665603ull;
                    for (const char c : form.seed) seed = (seed ^ std::uint8_t(c)) * 1099511628211ull;
                }
                request.seed = seed;
                if (form.preset >= 0 && std::size_t(form.preset) < presets.size())
                    request.settings = generation::regionSettingsFrom(presets[std::size_t(form.preset)], seed);
                request.settings.seed = seed;
                request.settings.seaPercent = std::clamp(int(std::lround(form.seaPercent)), 0, 100);
                request.settings.erosionPasses = std::clamp(int(std::lround(form.erosionPasses)), 0, 24);
                request.settings.rainfallPercent = std::clamp(int(std::lround(form.rainPercent)), 20, 250);
                request.ownSeeds = form.ownSeeds;
                request.featherKm = form.featherKm;
                request.variation = std::clamp(form.variation / 100.0f, 0.0f, 2.0f);
                return request;
            };
            if (shape.generateHeights) editor.generateHeights(generateRequest(*shape.generateHeights));
            if (shape.generateControls) editor.generateControls(generateRequest(*shape.generateControls));
            if (shape.importStage) editor.stageImported(*shape.importStage);
            if (shape.clearImport) editor.clearImport();
            if (shape.exportTo) editor.exportMaps(world::saves::pathOf(*shape.exportTo));
            if (shape.browse) {
                {
                    const std::lock_guard<std::mutex> lock(dialogPick.guard);
                    dialogPick.field = *shape.browse;
                }
                if (*shape.browse >= 2) {
                    SDL_ShowOpenFolderDialog(onPicked, &dialogPick, window, nullptr, false);
                } else {
                    static const SDL_DialogFileFilter png[] = {{"PNG images", "png"}};
                    SDL_ShowOpenFileDialog(onPicked, &dialogPick, window, png, 1, nullptr, false);
                }
            }
        }
        if (a.quit) running = false;
        if (a.refreshWorlds) worlds = world::saves::list(worldsDir);
        if (a.toggleSettings) menu.togglePanel();
        if (a.load && *a.load < worlds.size()) open(worlds[*a.load], a.loadAs);
        if (a.create) {
            auto spec = *a.create;
            if (world::saves::cleanName(spec.name).empty()) spec.name = world::saves::freeName(worldsDir, "New world");
            std::string error;
            if (const auto made = world::saves::create(worldsDir, spec, presets, &error)) {
                worlds = world::saves::list(worldsDir);
                open(*made, a.createAs);
            } else {
                screens.toast(error, since(start), true);
            }
        }
        if (a.remove && *a.remove < worlds.size()) {
            const auto doomed = worlds[*a.remove];
            if (session && session->save.name == doomed.name) closeSession();
            std::string error;
            if (world::saves::remove(doomed, &error)) screens.toast("Deleted " + doomed.name, since(start));
            else screens.toast(error, since(start), true);
            worlds = world::saves::list(worldsDir);
        }
        if (a.toMenu) {
            closeSession();
            screens.screen = Screen::Menu;
        }
        if (a.save && session) {
            if (session->editing) session->editing->end();
            screens.toast(save(true) ? "Saved" : "Could not save - see the console", since(start), false);
        }
        if (a.undo && session && session->editing) {
            const auto label = session->editing->undoLabel();
            if (session->editing->undo()) screens.toast("Undone: " + label, since(start));
        }
        if (a.redo && session && session->editing) {
            const auto label = session->editing->redoLabel();
            if (session->editing->redo()) screens.toast("Redone: " + label, since(start));
        }
        if (a.cycleCamera && hero.active() && screens.mode == WorldMode::Explore) {
            hero.cycleView(camera);
        } else if (a.cycleCamera) {
            camera.setMode(camera.mode == Camera::Mode::Map ? Camera::Mode::Orbit
                           : camera.mode == Camera::Mode::Orbit ? Camera::Mode::Free : Camera::Mode::Map);
            controls.dragging = controls.orbiting = false;
            controls.velocityX = controls.velocityY = 0;
        }
        if (a.mode) {
            screens.mode = *a.mode;
            if (*a.mode == WorldMode::Explore) screens.showHints(since(start));
        }
    };

    // The ground in view is loaded: what the loading screen waits for. Not
    // renderer.settled(), which also waits for the last morph and the last
    // coarse square - a perspective view over a wide country may never get
    // there, and a person does not need it to start walking.
    //
    // Nothing missing means nothing only once something could have been asked
    // for: while the pages of the whole world are baked in memory, the
    // streaming under the camera has not started yet, and "missing" is nought
    // over a view with no ground in it.
    const auto groundReady = [&] {
        if (renderer.settled()) return true;
        const auto now = system.read();
        return now && !now->pages().prebakeProgress().running && renderer.terrain().missing() == 0;
    };

    // --- the scripted tour --------------------------------------------------------
    int tourStep = 0;
    int tourFrames = 0;
    int worldFrames = 0;   // frames drawn since the world's screen came up
    std::optional<std::pair<float, float>> tourPointer;   // a pointer where a person's would be
    bool tourDown = false;                                // and its left button held
    std::pair<double, double> tourAt{};                   // where the stroke went, world metres
    generation::WorldLayout tourLayout;                   // the world the tour grows
    // What the ground's streaming is doing, for the log.
    const auto tourStatus = [&](const char* when) {
        const auto& terrain = renderer.terrain();
        std::cout << "tour: terrain " << when << ": " << (renderer.settled() ? "settled" : "not settled") << ", "
                  << terrain.missing() << " pages missing, " << terrain.coarse() << " coarse, " << terrain.stale()
                  << " stale, " << terrain.refreshed() << " refreshed so far, " << terrain.resident() << " resident"
                  << (renderer.holding() ? ", holding the picture before" : "");
        if (const auto now = system.read()) {
            const auto pages = now->pages().stats();
            const auto prebake = now->pages().prebakeProgress();
            std::cout << "; pages baked " << pages.baked << ", from disk " << pages.diskLoaded << ", disk errors "
                      << pages.diskErrors << ", stale " << pages.stale << ", prebake " << prebake.done << "/"
                      << prebake.total << (prebake.running ? " running" : "");
        }
        std::cout << std::endl;
    };
    // A layout check nobody has to look at: every line of writing on the
    // screen, any two that lie on each other and any that run off the edge.
    int tourProblems = 0;
    const auto auditBoxes = [&](const std::vector<ui::Ui::TextBox>& boxes, float w, float h, const std::string& where) {
        for (const auto& b : boxes)
            if (b.box.x < -0.5f || b.box.y < -0.5f || b.box.right() > w + 0.5f || b.box.bottom() > h + 0.5f) {
                std::cout << "tour audit: " << where << ": off the edge: \"" << b.text << "\"\n";
                ++tourProblems;
            }
        for (std::size_t i = 0; i < boxes.size(); ++i)
            for (std::size_t j = i + 1; j < boxes.size(); ++j) {
                // Writing inside a control is the control's label: only like
                // is compared with like.
                const bool controlA = !boxes[i].text.empty() && boxes[i].text[0] == '\x01';
                const bool controlB = !boxes[j].text.empty() && boxes[j].text[0] == '\x01';
                if (controlA != controlB) continue;
                const auto& a = boxes[i].box;
                const auto& c = boxes[j].box;
                const float across = std::min(a.right(), c.right()) - std::max(a.x, c.x);
                const float down = std::min(a.bottom(), c.bottom()) - std::max(a.y, c.y);
                if (across > 1.0f && down > 1.0f) {
                    std::cout << "tour audit: " << where << ": \"" << boxes[i].text << "\" lies on \"" << boxes[j].text
                              << "\"\n";
                    ++tourProblems;
                }
            }
    };
    const auto auditWindow = [&](int wide, int high, const std::string& where, const auto& draw) {
        SDL_Surface* surface = SDL_CreateSurface(wide, high, SDL_PIXELFORMAT_ABGR8888);
        SDL_Renderer* software = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
        if (!software) { if (surface) SDL_DestroySurface(surface); return; }
        {
            ui::Ui scratch;
            scratch.init(software);
            if (regular) scratch.setFont(regular, bold, shown.fontPoints, 1.0f);
            std::vector<ui::Ui::TextBox> boxes;
            scratch.audit(&boxes);
            draw(scratch);
            std::cout << "tour: " << where << ": " << boxes.size() << " lines of writing checked\n";
            auditBoxes(boxes, float(wide), float(high), where);
            scratch.shutdown();
        }
        SDL_DestroyRenderer(software);
        SDL_DestroySurface(surface);
    };
    ClientView lastView;
    float viewPointsW = 0, viewPointsH = 0;   // the interface's size, in points
    const auto tourShot = [&](const char* name) {
        const fs::path file = options.tour / name;
        if (renderer.screenshot(file.string())) std::cout << "tour: " << file.string() << "\n";
        else std::cerr << "tour: could not write " << file.string() << "\n";
        std::vector<ui::Ui::TextBox> boxes;
        ui::Input quiet = input;
        quiet.pressed = quiet.released = false;
        ClientActions ignored;
        ui.audit(&boxes);
        ui.begin(quiet, int(viewPointsW), int(viewPointsH));
        ui.painting(false);
        screens.build(ui, lastView, ignored);
        ui.end();
        ui.audit(nullptr);
        std::cout << "tour: " << name << ": " << boxes.size() << " lines of writing checked\n";
        auditBoxes(boxes, viewPointsW, viewPointsH, name);
        if (menu.panelVisible())
            auditWindow(GraphicsPanel::kWide, int(shown.graphicsHigh), std::string(name) + " / graphics window",
                        [&](ui::Ui& scratch) {
                            auto settings = menu.graphics();
                            ui::Input away;
                            away.mouseX = away.mouseY = -1000;
                            scratch.begin(away, GraphicsPanel::kWide, int(shown.graphicsHigh));
                            menu.panel().draw(scratch, settings);
                            scratch.end();
                        });
        if (menu.editor().windowShown())
            auditWindow(WorldEditor::kWide, int(shown.editorHigh), std::string(name) + " / world shape window",
                        [&](ui::Ui& scratch) {
                            scratch.begin(menu.editor().panelInput(), WorldEditor::kWide, int(shown.editorHigh));
                            menu.editor().draw(scratch);
                            scratch.end();
                        });
    };

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
        ++frameIndex;
        viewportOf(camera);
        const Uint64 now = SDL_GetTicks();
        const double dt = std::clamp((now - last) / 1000.0, 0.0, 0.1);
        last = now;
        const double seconds = since(start);

        // --- input -----------------------------------------------------------------
        input.wheel = 0;
        input.text.clear();
        input.backspace = input.enter = input.escape = input.tab = false;
        input.doubleClick = false;
        ClientActions actions;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (profiler.handle(event)) continue;
            if (event.type == SDL_EVENT_QUIT) { running = false; continue; }
            if (touring) continue;
            if (event.type == SDL_EVENT_TEXT_INPUT && ui.typing()) { input.text += event.text.text; continue; }
            if (event.type == SDL_EVENT_MOUSE_WHEEL) { input.wheel += event.wheel.y; continue; }
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && event.button.clicks >= 2)
                input.doubleClick = true;
            if (event.type != SDL_EVENT_KEY_DOWN) continue;
            const SDL_Keycode key = event.key.key;
            if (ui.typing()) {
                if (key == SDLK_BACKSPACE) input.backspace = true;
                else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) input.enter = true;
                else if (key == SDLK_ESCAPE) input.escape = true;
                else if (key == SDLK_TAB) input.tab = true;
                continue;
            }
            if (event.key.repeat && key != SDLK_LEFTBRACKET && key != SDLK_RIGHTBRACKET) continue;
            if (key == SDLK_F3) { shown.developer = !shown.developer; menu.panelChanged(); continue; }
            if (key == SDLK_P && (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI))) { if (window) profiler.toggle(); continue; }
            if (key == SDLK_ESCAPE && menu.panelVisible()) { menu.togglePanel(); continue; }
            {
                const bool exploring = screens.screen == Screen::World && !screens.paused &&
                                       screens.mode == WorldMode::Explore;
                if (exploring && key == SDLK_C) {
                    if (hero.active()) hero.release(camera); else hero.take(camera, heroGround);
                    controls.dragging = controls.orbiting = false;
                    controls.velocityX = controls.velocityY = 0;
                    continue;
                }
                if (exploring && hero.active() && key == SDLK_SPACE) { heroJump = true; continue; }
                if (exploring && hero.active() && key == SDLK_V) { hero.cycleView(camera); continue; }
            }
            if (screens.key(key, SDL_Keymod(event.key.mod), actions, false)) continue;
            // The developer's views of the ground, which the explorer has and
            // the game lost when it got its own key handling: Shift+G cycles
            // the overlay (off, source step, real triangle edges), Shift+M the
            // objects' wireframe, M / 0 / F2..F8 the map views, Shift+1..8 the
            // generation stage. Only what the screens did not take.
            if (screens.screen != Screen::World || screens.paused) continue;
            const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
            if (shift && key == SDLK_G) { renderer.cycleTerrainGrid(); continue; }
            if (shift && key == SDLK_M) { renderer.toggleObjectWireframe(); continue; }
            const bool stage = shift && event.key.scancode >= SDL_SCANCODE_1 && event.key.scancode <= SDL_SCANCODE_8;
            const bool map = !shift && (key == SDLK_M || key == SDLK_0 || (key >= SDLK_F2 && key <= SDLK_F8));
            if (stage || map) menu.handle(event);
        }
        if (window) {
            if (ui.typing() && !SDL_TextInputActive(window)) SDL_StartTextInput(window);
            else if (!ui.typing() && SDL_TextInputActive(window)) SDL_StopTextInput(window);
        }

        float mx = 0, my = 0;
        SDL_MouseButtonFlags buttons = window ? SDL_GetMouseState(&mx, &my) : 0;
        if (profiler.hasMouse()) buttons = 0;   // the pointer is on the profiler's window
        int windowW = camera.viewportWidth, windowH = camera.viewportHeight;
        if (window) SDL_GetWindowSize(window, &windowW, &windowH);
        float pixelsPerPoint = float(camera.viewportWidth) / float(std::max(1, windowW));
        // Headless has no display to ask: a scale can be given, to look at the
        // interface as a dense screen draws it.
        if (!window)
            if (const char* forced = std::getenv("CAMPFIRE_UI_SCALE")) pixelsPerPoint = std::clamp(float(std::atof(forced)), 1.0f, 4.0f);
        if (tourPointer) { mx = tourPointer->first; my = tourPointer->second; buttons = tourDown ? SDL_BUTTON_LMASK : 0; }
        const bool leftDown = (buttons & SDL_BUTTON_LMASK) != 0;
        input.mouseX = mx;
        input.mouseY = my;
        input.down = leftDown;
        input.pressed = leftDown && !leftWasDown;
        input.released = !leftDown && leftWasDown;
        input.rightDown = (buttons & SDL_BUTTON_RMASK) != 0;
        input.middleDown = (buttons & SDL_BUTTON_MMASK) != 0;
        const SDL_Keymod mods = SDL_GetModState();
        input.shift = (mods & SDL_KMOD_SHIFT) != 0;
        input.ctrl = (mods & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        leftWasDown = leftDown;
        const float pointsW = float(camera.viewportWidth) / pixelsPerPoint;
        const float pointsH = float(camera.viewportHeight) / pixelsPerPoint;
        viewPointsW = pointsW;
        viewPointsH = pointsH;

        // --- where the two windows go ------------------------------------------------
        const bool inWorld = screens.screen == Screen::World;
        const float top = inWorld ? 56.0f : 24.0f;
        // The world shape window under the tab strip, above the status bar,
        // and the settings window under the top bar: at the interface's own
        // scale, as tall as there is room for, and what does not fit scrolls
        // inside them.
        shown.panelScale = pixelsPerPoint;
        shown.editorHigh = std::min(float(WorldEditor::kHigh), pointsH - 110.0f - 30.0f - 10.0f);
        shown.graphicsHigh = std::min(float(GraphicsPanel::kHigh), pointsH - top - 40.0f);
        const float gw = GraphicsPanel::kWide * shown.panelScale, gh = shown.graphicsHigh * shown.panelScale;
        shown.graphicsX = std::max(0.0f, float(camera.viewportWidth) - gw - 16 * pixelsPerPoint);
        shown.graphicsY = top * pixelsPerPoint;
        shown.editorX = 12 * pixelsPerPoint;
        shown.editorY = 110 * pixelsPerPoint;
        // The stages of the world's shape (Size to Climate): the world editor
        // is the model under them - it holds the layout, the strokes and the
        // stages - and the client's panels are its face; its own window is
        // not shown.
        const bool shapeOpen = inWorld && screens.mode == WorldMode::Edit && shapeTab(screens.tab) && !screens.paused;
        if (shapeOpen != menu.editor().active()) menu.editor().toggle();
        if (shapeOpen) {
            auto& editor = menu.editor();
            using L = generation::LayerId;
            using B = generation::BrushTool;
            WorldEditor::LayerStroke stroke;
            switch (screens.tab) {
                case EditTab::Land:
                    stroke.tool = B::Paint;
                    stroke.radiusMetres = screens.land.radiusKm * 1000.0;
                    stroke.value = 900;           // well over the shore: land
                    stroke.strength = 1.0f;
                    stroke.hardness = screens.land.hardness / 100.0f;
                    editor.paintLayer(L::Continents, stroke);
                    break;
                case EditTab::Mountains:
                    stroke.tool = B::Paint;
                    stroke.radiusMetres = screens.mountains.radiusKm * 1000.0;
                    stroke.value = screens.mountains.height;
                    stroke.strength = screens.mountains.strength / 100.0f;
                    stroke.hardness = 0.25f;
                    editor.paintLayer(L::Ranges, stroke);
                    break;
                case EditTab::Water:
                    stroke.tool = B::Paint;
                    stroke.radiusMetres = screens.climate.radiusKm * 1000.0;
                    stroke.value = screens.climate.rain;
                    stroke.strength = 0.8f;
                    stroke.hardness = 0.2f;
                    editor.paintLayer(L::Rain, stroke);
                    break;
                case EditTab::Import: editor.selectRegions(); break;
                default: editor.paintNothing(); break;
            }
        }

        // --- the interface: what it was asked -------------------------------------------
        ClientView shownView;
        shownView.worlds = &worlds;
        if (session) {
            const auto& editor = menu.editor();
            const auto& layout = editor.layout();
            auto& shape = shownView.shape;
            shape.regionsX = std::max(1, layout.regionsX);
            shape.regionsY = std::max(1, layout.regionsY);
            const auto counts = editor.stages();
            for (std::size_t i = 0; i < shape.stages.size(); ++i) shape.stages[i] = counts[i];
            for (const auto& region : layout.regions) shape.generatedRegions += region.generated ? 1 : 0;
            shape.unbuilt = editor.unbuilt() || editor.wanted();
            shape.building = build && !build->entering;
            shape.canUndo = editor.canUndo();
            shape.historyEmpty = !session || !session->delta ||
                                 (session->delta->chunks().empty() && session->delta->heights()->empty());
            shape.status = shape.building ? "Building: " + core::progressLine() : editor.statusLine();
            const auto lat = editor.latitudeShown();
            shape.northDegrees = lat.northDegrees;
            shape.kmPerDegree = lat.kmPerDegree;
            shape.dials = layout.authoring;
            shape.selectedRegions = editor.selectedRegions();
            shape.importedRegions = editor.importedRegions();
            shape.importedStages = editor.importedStages();
            shape.importing = editor.importing();
            shape.importLine = editor.importLine();
            shape.sourcePath = world::saves::utf8Of(editor.sourceRoot());
        }
        shownView.presets = &presets;
        shownView.now = seconds;
        shownView.loadingTitle = loadingTitle;
        shownView.loadingDetail = build && !build->done ? "Generating the country: " + core::progressLine()
                                                        : "Laying the ground and the forests";
        shownView.loadingSeconds = build ? since(build->began) : settling ? since(*settling) : 0;
        if (session) {
            shownView.worldName = session->save.name;
            // Asked a few times a second: it walks the history's records.
            if (frameIndex % 10 == 0 || !inWorld) unsavedNow = unsaved();
            shownView.unsaved = unsavedNow;
            if (session->editing) {
                shownView.undoLabel = session->editing->undoLabel();
                shownView.redoLabel = session->editing->redoLabel();
            }
        }
        shownView.cameraName = camera.mode == Camera::Mode::Map ? "Map" : camera.mode == Camera::Mode::Orbit ? "Orbit" : "Fly";
        shownView.rebuilding = build && !build->entering;
        shownView.settingsOpen = menu.panelVisible();
        shownView.settingsRect = {shown.graphicsX / pixelsPerPoint, shown.graphicsY / pixelsPerPoint, gw / pixelsPerPoint,
                                  gh / pixelsPerPoint};
        shownView.shapeEditorOpen = shapeOpen && menu.editor().windowShown();
        shownView.shapeEditorRect = {shown.editorX / pixelsPerPoint, shown.editorY / pixelsPerPoint,
                                     WorldEditor::kWide * shown.panelScale / pixelsPerPoint,
                                     shown.editorHigh * shown.panelScale / pixelsPerPoint};

        // Where the pointer meets the ground.
        const float px = mx * pixelsPerPoint, py = my * pixelsPerPoint;
        bool onGround = false;
        double gx = 0, gy = 0, gz = 0;
        if (inWorld && !relativeMouse && mx >= 0 && my >= 0 && mx < pointsW && my < pointsH) {
            double z = camera.focusHeight - camera.heightOffset;
            for (int pass = 0; pass < 4; ++pass) {
                camera.worldOfScreenAtHeight(int(px), int(py), z, gx, gy);
                z = field.heightAt(pos(std::clamp(gx, 0.0, worldW - 1), std::clamp(gy, 0.0, worldH - 1))).toDouble();
            }
            gz = z;
            onGround = gx >= 0 && gy >= 0 && gx < worldW && gy < worldH;
        }
        shownView.pointerOnGround = onGround;
        shownView.pointerX = gx;
        shownView.pointerY = gy;
        shownView.pointerHeight = gz;

        // The canvas the size of the view, the interface in points on it.
        if (canvas.wouldResize(camera.viewportWidth, camera.viewportHeight)) {
            ui.shutdown();
            canvas.size(camera.viewportWidth, camera.viewportHeight);
            if (canvas) {
                ui.init(canvas.renderer());
                SDL_SetRenderScale(canvas.renderer(), pixelsPerPoint, pixelsPerPoint);
                if (regular) ui.setFont(regular, bold, 15, pixelsPerPoint);
                painted = 0;
                paintedTiles.clear();
            }
        }
        if (!canvas) { std::cerr << "no interface surface\n"; return 1; }
        ui.begin(input, int(pointsW), int(pointsH));
        ui.painting(false);
        screens.build(ui, shownView, actions);
        ui.end();
        const bool overUi = input.overUi || ui.capturing() || ui.typing();
        act(actions);

        // --- the legacy windows: graphics settings and the world's shape --------------------
        {
            auto& in = menu.panelInput();
            in.mouseX = (px - shown.graphicsX) / shown.panelScale;
            in.mouseY = (py - shown.graphicsY) / shown.panelScale;
            in.pressed = input.pressed; in.released = input.released; in.down = input.down; in.wheel = input.wheel;
            auto& panel = menu.panel();
            if (panel.saveRequested) {
                panel.saveRequested = false;
                panel.status = game::saveGraphicsSettings(graphicsFile, menu.graphics()) ? "saved graphics.json" : "could not save";
                menu.panelChanged();
            }
            if (panel.resetRequested) {
                panel.resetRequested = false;
                menu.graphics() = game::GraphicsSettings{};
                panel.status = "defaults";
                menu.panelChanged();
            }
            panel.sceneViewToggled = false;
            auto& editorIn = menu.editor().panelInput();
            editorIn.mouseX = (px - shown.editorX) / shown.panelScale;
            editorIn.mouseY = (py - shown.editorY) / shown.panelScale;
            editorIn.pressed = input.pressed; editorIn.released = input.released; editorIn.down = input.down;
            editorIn.wheel = input.wheel;   // its scroll area takes it when the pointer is over it
        }
        if (window && menu.graphics().resolution != appliedResolution) {
            appliedResolution = menu.graphics().resolution;
            const auto& wanted = game::kResolutions[std::size_t(appliedResolution)];
            if (wanted.width == 0) SDL_SetWindowFullscreen(window, true);
            else if (wanted.width > 0) {
                SDL_SetWindowFullscreen(window, false);
                const float density = std::max(0.25f, SDL_GetWindowPixelDensity(window));
                SDL_SetWindowSize(window, int(std::lround(wanted.width / density)), int(std::lround(wanted.height / density)));
            }
        }

        // --- a world raised --------------------------------------------------------------
        if (build && build->done) {
            const bool entering = build->entering;
            const WorldMode mode = build->mode;
            const bool failed = build->failed;
            const std::string error = build->error;
            const double took = since(build->began);
            if (!failed) {
                const auto& c = build->composed;
                std::cout << "world raised in " << took << " s: generated " << build->generated << " s ("
                          << (c.wholeWorld ? std::string("one run over the whole world")
                                           : std::to_string(c.runs) + " run(s), " + std::to_string(c.seaRegions) +
                                                     " sea region(s) not computed, " + std::to_string(c.seams) +
                                                     " seam(s), " + std::to_string(c.landmasses) +
                                                     " land mass(es) drained again")
                          << "), published " << build->published << " s, pages baked " << build->prebaked << " s";
                if (const auto now = system.read()) {
                    const auto& t = now->timings();
                    std::cout << " (drainage " << t.graph / 1000 << " s, climate " << t.climate / 1000 << " s)";
                }
                std::cout << std::endl;
            }
            build.reset();
            if (failed) {
                screens.alert("The world could not be raised", error);
                if (entering) { closeSession(); screens.screen = Screen::Host; }
                else menu.editor().built(-1);
            } else {
                snapshot = system.read();
                field = snapshot->field();
                const auto& map = snapshot->worldMap();
                worldW = double(map.width) * generation::kMetresPerCell;
                worldH = double(map.height) * generation::kMetresPerCell;
                camera.setBounds(0, 0, worldW, worldH);
                menu.editor().builtWorld(map);
                if (entering) {
                    frame(snapshot->startingPoint(), 900);
                    settling = Clock::now();
                    settlingMode = mode;
                    if (session && session->editing) session->editing->forget();
                } else {
                    menu.editor().built(took);
                    screens.toast("The world's new shape is raised", seconds);
                }
            }
        }
        if (settling && std::getenv("CAMPFIRE_LOAD_TRACE")) {
            const auto& terrain = renderer.terrain();
            const auto baking = system.read()->pages().prebakeProgress();
            std::cout << "load trace " << since(*settling) << " s, frame " << dt * 1000 << " ms: missing " << terrain.missing() << ", coarse "
                      << terrain.coarse() << ", prebake " << baking.done << "/" << baking.total
                      << (baking.running ? " running" : "") << std::endl;
        }
        if (settling && ((groundReady() && since(*settling) > 0.3) || since(*settling) > 25)) {
            if (const auto now = system.read()) {
                const auto baking = now->pages().prebakeProgress();
                std::cout << "ground ready " << since(*settling) << " s after the world was raised; prebake "
                          << baking.done << "/" << baking.total << (baking.running ? " still running" : "") << std::endl;
            }
            settling.reset();
            screens.screen = Screen::World;
            screens.mode = settlingMode;
            screens.paused = false;
            // The character where the world starts, the camera behind it.
            // (A tour takes it only when asked: its pictures are of the country.)
            if (settlingMode == WorldMode::Explore && (!touring || std::getenv("CAMPFIRE_TOUR_HERO")))
                hero.take(camera, heroGround);
            if (settlingMode == WorldMode::Explore) screens.showHints(seconds);
        }
        // A file the dialog picked goes into its field; an import that has
        // finished makes its regions imported ones (and wants a build).
        {
            const std::lock_guard<std::mutex> lock(dialogPick.guard);
            if (dialogPick.chosen) {
                auto& f = screens.importForm;
                const auto& [field, path] = *dialogPick.chosen;
                (field == 0 ? f.height : field == 1 ? f.control : field == 2 ? f.package : f.exportTo) = path;
                dialogPick.chosen.reset();
            }
        }
        if (session) menu.editor().update();
        if (menu.editor().wanted() && session && !build) {
            menu.editor().started();
            startBuild(menu.editor().layout(), false, screens.mode);
        }

        // --- the camera ------------------------------------------------------------------
        const bool playing = inWorld && !screens.paused && !menu.panelVisible();
        const bool focused = window && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        const bool heroOn = hero.active() && screens.mode == WorldMode::Explore;
        const bool capture = playing && focused &&
                             ((heroOn && hero.wantsMouse(camera)) ||
                              (!heroOn && camera.mode == Camera::Mode::Free && input.rightDown && !overUi));
        if (window && capture != relativeMouse) {
            if (SDL_SetWindowRelativeMouseMode(window, capture)) relativeMouse = capture;
            controls.orbiting = false;
            SDL_GetRelativeMouseState(nullptr, nullptr);
        }
        ui::Input pointer = input;
        pointer.mouseX = px;
        pointer.mouseY = py;
        float heroDx = 0, heroDy = 0;
        if (relativeMouse) {
            float rdx = 0, rdy = 0;
            SDL_GetRelativeMouseState(&rdx, &rdy);
            heroDx = rdx; heroDy = rdy;
            pointer.mouseX = controls.orbiting ? float(controls.dragFromX) + rdx : 0;
            pointer.mouseY = controls.orbiting ? float(controls.dragFromY) + rdy : 0;
        }
        camera.minZoom = std::min(camera.viewportWidth / worldW, camera.viewportHeight * 2.0 / worldH) * 0.9;
        if (playing && focused && !touring) {
            {
                static const bool none[SDL_SCANCODE_COUNT]{};
                const bool* keys = profiler.hasKeyboard() ? none : SDL_GetKeyboardState(nullptr);
                if (heroOn) {
                    HeroCamera::Input in;
                    in.keys = keys;
                    in.mouseDx = heroDx; in.mouseDy = heroDy;
                    in.wheel = overUi ? 0.0f : float(input.wheel);
                    in.jump = heroJump;
                    hero.update(camera, in, dt, heroGround);
                } else {
                    controls.update(camera, pointer, keys, dt, overUi);
                }
                heroJump = false;
            }
        } else if ((screens.screen == Screen::Menu || screens.screen == Screen::Host) && !touring) {
            // The world behind the menu turns slowly, like a globe on a desk.
            camera.orbit(dt * 0.035, 0);
            controls.dragging = controls.orbiting = false;
        }
        camera.clampTo(int(worldW), int(worldH));
        if (heroOn) {
            const auto* pass = renderer.characterPass();
            hero.follow(camera, heroGround, pass ? pass->model() : nullptr);
        } else if (camera.mode != Camera::Mode::Free) {
            camera.heightOffset = std::clamp(camera.heightOffset, -10000.0, 10000.0);
            // The ground under the centre, never under the water (the sea bed
            // put the eye below the sea), eased towards rather than snapped
            // to: crossing a ridge moved the whole view by the ridge's height
            // in one frame.
            const auto groundAt = [&](double x, double y) {
                return std::max(0.0, field.heightAt(pos(std::clamp(x, 0.0, worldW - 1), std::clamp(y, 0.0, worldH - 1)))
                                             .toDouble());
            };
            const double target = groundAt(camera.centreX, camera.centreY) + camera.heightOffset;
            static double eased = target;
            static bool easedSet = false;
            const double rate = 1.0 - std::exp(-dt * 10.0);
            eased = easedSet && std::abs(target - eased) < 20000.0 ? eased + (target - eased) * rate : target;
            easedSet = true;
            camera.focusHeight = eased;
            // And the eye kept clear of the ground: under it and along the way
            // to the centre the ground is at least a little below the eye, or
            // the view is lifted until it is - an eye inside a hill saw
            // through it, the ground's back faces culled.
            if (camera.perspective()) {
                const auto eye = camera.eyePosition();
                const double clearance = std::max(3.0, camera.orbitDistance() * 0.04);
                double highest = -1e9;
                for (const double t : {1.0, 0.75, 0.5, 0.25})
                    highest = std::max(highest, groundAt(camera.centreX + (eye[0] - camera.centreX) * t,
                                                         camera.centreY + (eye[1] - camera.centreY) * t) -
                                                        (1.0 - t) * (eye[2] - camera.focusHeight));
                const double lift = highest + clearance - eye[2];
                if (lift > 0) camera.focusHeight += lift;
            }
        }
        // The planes follow how high the camera is: from two hundred
        // kilometres up the far plane at two hundred put the whole of the
        // ground at the back of the depth buffer, and the sea over it.
        if (camera.perspective()) {
            // How high the eye is over the ground under it - not over the
            // centre, which a pitched view can stand far below a hill beside
            // the eye.
            const auto eyeNow = camera.eyePosition();
            const double under = field.heightAt(pos(std::clamp(eyeNow[0], 0.0, worldW - 1),
                                                    std::clamp(eyeNow[1], 0.0, worldH - 1))).toDouble();
            const double altitude = std::max(0.0, std::min(eyeNow[2] - camera.focusHeight, eyeNow[2] - std::max(0.0, under)));
            camera.farPlane = std::max(200000.0, std::max(0.0, eyeNow[2] - camera.focusHeight) * 8.0);
            // And the near plane as far out as the ground lets it: nothing of
            // the ground is nearer than the height over it, less its
            // mountains. Held at two hundred metres it left the depth buffer
            // seven hundred metres a step at the far end of a continent seen
            // from a thousand kilometres up, and the sea came through the
            // lowlands as a pale shallows over the land.
            camera.nearPlane = std::max(std::clamp(altitude / 4000.0, 0.5, 200.0), (altitude - 5000.0) * 0.5);
        }

        // --- the tools ---------------------------------------------------------------------
        const bool editing = playing && screens.mode == WorldMode::Edit && session && session->editing;
        const bool alt = (mods & SDL_KMOD_ALT) != 0;
        if (input.pressed) strokeOnGround = !overUi && onGround;
        if (!input.down) strokeOnGround = false;
        std::optional<ExploreView::EditorMarks> marks;
        if (!inWorld || screens.mode == WorldMode::Explore) marks = ExploreView::EditorMarks{};
        if (editing && !shapeTab(screens.tab)) {
            auto& edit = *session->editing;
            const auto& map = snapshot->worldMap();
            ExploreView::EditorMarks m{};
            m[0] = {float(generation::kRegionMetres), 2.0f, float(std::max(1, map.width / generation::kCellsPerRegion)),
                    float(std::max(1, map.height / generation::kCellsPerRegion))};
            const bool stroking = strokeOnGround && input.down && onGround;
            if (screens.tab == EditTab::Terrain) {
                auto kind = screens.terrain.kind;
                if (alt && kind == world::BrushKind::Raise) kind = world::BrushKind::Lower;
                else if (alt && kind == world::BrushKind::Lower) kind = world::BrushKind::Raise;
                if (input.pressed && stroking) {
                    edit.begin(world::tools::brushName(kind));
                    // Flatten keeps the level it began at, so a drag across a
                    // slope cuts a terrace rather than following the slope.
                    strokeLevel = gz;
                    lastApplyX = gx; lastApplyY = gy;
                }
                if (stroking) {
                    world::Brush brush;
                    brush.kind = kind;
                    brush.radiusMetres = screens.terrain.radius;
                    brush.strength = screens.terrain.strength;
                    brush.softness = screens.terrain.softness / 100.0;
                    brush.scaleMetres = screens.terrain.scale;
                    brush.seed = session->layout.seed;
                    if (kind == world::BrushKind::Flatten) brush.level = strokeLevel;
                    const world::GroundAt ground = [&field](core::Fixed x, core::Fixed y) { return field.heightAt({x, y}); };
                    // Laid along the way the pointer went since the last
                    // frame, a quarter of the brush apart, with the frame's
                    // time shared between them: a quick drag is a line and
                    // not a string of beads, and it lays down no more than a
                    // slow one over the same time.
                    const double apart = std::max(2.0, double(screens.terrain.radius) * 0.25);
                    const double travelled = std::hypot(gx - lastApplyX, gy - lastApplyY);
                    const int dabs = std::clamp(int(std::ceil(travelled / apart)), 1, 32);
                    for (int d = 1; d <= dabs; ++d) {
                        const double t = double(d) / dabs;
                        edit.sculpt(brush, ground, pos(lastApplyX + (gx - lastApplyX) * t, lastApplyY + (gy - lastApplyY) * t),
                                    dt / dabs);
                    }
                    lastApplyX = gx; lastApplyY = gy;
                }
                const bool procedural = kind == world::BrushKind::Noise || kind == world::BrushKind::Thermal ||
                                        kind == world::BrushKind::Hydraulic || kind == world::BrushKind::Carve ||
                                        kind == world::BrushKind::Restore;
                if (onGround && !overUi)
                    m[2] = {float(gx), float(gy), screens.terrain.radius,
                            kind == world::BrushKind::Lower ? 2.0f : procedural ? 3.0f : 1.0f};
            } else if (screens.tab == EditTab::Objects) {
                const auto& tool = screens.objects;
                if (tool.mode == ObjectMode::Remove) {
                    if (input.pressed && stroking) { edit.begin("Remove objects"); applied = false; }
                    if (stroking && (!applied || std::hypot(gx - lastApplyX, gy - lastApplyY) > tool.radius * 0.3)) {
                        const auto present = snapshot->scatter(world::tools::boundsAround(gx, gy, tool.radius));
                        const auto gone = edit.clear(present.objects, gx, gy, tool.radius, tool.kinds);
                        (void)gone;
                        lastApplyX = gx; lastApplyY = gy; applied = true;
                    }
                    if (onGround && !overUi) m[2] = {float(gx), float(gy), tool.radius, 2.0f};
                } else {
                    if (input.pressed && stroking) { edit.begin(std::string("Plant ") + world::tools::modelName(tool.model)); applied = false; }
                    if (stroking && (!applied || std::hypot(gx - lastApplyX, gy - lastApplyY) >= tool.spacing)) {
                        std::uniform_real_distribution<float> turn(0.0f, 6.2831853f), size(0.85f, 1.2f);
                        world::ecology::Added object;
                        object.x = gx;
                        object.y = gy;
                        object.model = tool.model;
                        object.yaw = turn(random);
                        object.scale = size(random);
                        edit.plant(object);
                        lastApplyX = gx; lastApplyY = gy; applied = true;
                    }
                    if (onGround && !overUi) m[2] = {float(gx), float(gy), std::max(2.0f, tool.spacing * 0.5f), 4.0f};
                }
            }
            if (input.released || !input.down) edit.end();
            marks = m;
        } else if (session && session->editing && session->editing->stroking()) {
            session->editing->end();
        }
        if (shapeOpen) {
            menu.editor().pointer(gx, gy, onGround && !overUi && !relativeMouse,
                                  strokeOnGround && input.down && !alt, strokeOnGround && input.down && alt);
            marks.reset();
        }
        view.marks(marks);
        // The editor works in clear daylight: no rain or snow across the view
        // and no season on the ground while the ground is being shaped.
        view.weather(!(inWorld && screens.mode == WorldMode::Edit));

        // Saved as it goes: the history on its own thread, now and then.
        if (session && !touring && since(savedAt) > 60 && session->delta && session->delta->dirty() &&
            !(session->editing && session->editing->stroking())) {
            session->delta->saveInBackground();
            savedAt = Clock::now();
        }
        if (!touring) menu.advanceWeather(dt);

        // --- the tour -----------------------------------------------------------------------
        if (touring) {
            ++tourFrames;
            // A picture is of the frame drawn before it: the world's screen
            // counts once it has been drawn, not once it was asked for.
            worldFrames = screens.screen == Screen::World ? worldFrames + 1 : 0;
            const auto next = [&] { ++tourStep; tourFrames = 0; };
            ClientActions scripted;
            const float cx = pointsW * 0.5f, cy = pointsH * 0.55f;
            // The stages of making a world by hand, end to end through the
            // pointer as a person drives it (CAMPFIRE_TOUR_AUTHORING=1): an
            // empty world the reference size, land painted across a region
            // border, worked out, a range painted on it, the water.
            static const bool authoringTour = std::getenv("CAMPFIRE_TOUR_AUTHORING") != nullptr;
            const auto overBorder = [&] {
                const double r = double(generation::kRegionMetres);
                frame(pos(3.0 * r, 7.5 * r), 260000);
                camera.pitch = 1.25;
            };
            const auto drag = [&](float fromX, float toX, float atY, int frames) {
                const float t = std::min(1.0f, float(tourFrames) / float(frames));
                tourPointer = {{fromX + (toX - fromX) * t, atY}};
                tourDown = tourFrames > 2 && tourFrames < frames;
                return tourFrames > frames + 2;
            };
            // Built, and the new world on the screen for a while: the renderer
            // takes it up on the frame after, and its ground streams in after that.
            static int quietFrames = 0;
            quietFrames = build || menu.editor().wanted() ? 0 : quietFrames + 1;
            const auto waitBuilt = [&] { return quietFrames > 90 && groundReady() && tourFrames > 30; };
            // An existing world looked at in Edit from far and near
            // (CAMPFIRE_TOUR_OPEN=name): what the sea and the region grid do
            // at the scale of a continent.
            static const char* openTour = std::getenv("CAMPFIRE_TOUR_OPEN");
            const auto worldCentre = [&] {
                const auto& layout = menu.editor().layout();
                return pos(0.5 * double(layout.widthMetres()), 0.5 * double(layout.heightMetres()));
            };
            if (openTour) switch (tourStep) {
                case 0:
                    if (tourFrames > 30) {
                        for (std::size_t i = 0; i < worlds.size(); ++i)
                            if (worlds[i].name == openTour) { scripted.load = i; scripted.loadAs = WorldMode::Edit; }
                        if (!scripted.load) { std::cerr << "tour: no world " << openTour << "\n"; running = false; break; }
                        act(scripted);
                        next();
                    }
                    break;
                case 1:
                    if (worldFrames > 30 && groundReady() && !build) {
                        screens.tab = EditTab::Import;
                        frame(worldCentre(), 1600000);
                        camera.pitch = 1.5;
                        next();
                    }
                    break;
                case 2: if (waitBuilt()) { tourShot("o1-continent-top.png"); frame(worldCentre(), 700000); camera.pitch = 0.9; next(); } break;
                case 3: if (waitBuilt()) { tourShot("o2-continent-tilted.png"); frame(worldCentre(), 160000); camera.pitch = 0.7; next(); } break;
                case 4: if (waitBuilt()) { tourShot("o3-region-scale.png"); screens.mode = WorldMode::Explore; next(); } break;
                case 5: if (waitBuilt()) { tourShot("o4-explore-region-scale.png"); frame(worldCentre(), 1600000); camera.pitch = 1.5; next(); } break;
                case 6: if (waitBuilt()) {
                    tourShot("o5-explore-continent-top.png");
                    // The mountains of Atrod, third person, as a person walks it.
                    const auto& layout = menu.editor().layout();
                    frame(pos(0.45 * double(layout.widthMetres()), 0.52 * double(layout.heightMetres())), 1400);
                    next();
                } break;
                case 7: if (waitBuilt()) { tourShot("o6-close.png"); next(); } break;
                case 8: if (tourFrames == 3) { tourShot("o6b-close-3-frames-later.png"); camera.orbit(0.02, 0); }
                        if (tourFrames == 6) { tourShot("o6c-close-turned.png"); next(); } break;
                case 9: {
                    const auto& layout = menu.editor().layout();
                    frame(pos(0.45 * double(layout.widthMetres()), 0.52 * double(layout.heightMetres())), 6000);
                    next();
                } break;
                case 10: if (waitBuilt()) { tourShot("o7-6km.png"); next(); } break;
                case 11:
                    // The same view held still: what changes in it is the
                    // terrain changing its mind, not the camera.
                    if (tourFrames == 20) tourShot("o7b-6km-20.png");
                    if (tourFrames == 40) tourShot("o7c-6km-40.png");
                    if (tourFrames == 60) { tourShot("o7d-6km-60.png"); next(); }
                    break;
                case 12: {
                    const auto& layout = menu.editor().layout();
                    frame(pos(0.45 * double(layout.widthMetres()), 0.52 * double(layout.heightMetres())), 40000);
                    next();
                } break;
                case 13: if (waitBuilt()) { tourShot("o8-40km.png"); next(); } break;
                default: running = false; break;
            }
            else if (authoringTour) switch (tourStep) {
                case 0:
                    if (tourFrames > 60) {
                        world::saves::NewWorld spec;
                        spec.name = "Reference";
                        spec.empty = true;
                        spec.regionsX = world::saves::kReferenceRegionsX;
                        spec.regionsY = world::saves::kReferenceRegionsY;
                        // CAMPFIRE_TOUR_REGIONS=16x16: the tour on another
                        // size of world (a profile of the largest one, say).
                        if (const char* size = std::getenv("CAMPFIRE_TOUR_REGIONS"))
                            std::sscanf(size, "%dx%d", &spec.regionsX, &spec.regionsY);
                        spec.seed = 4242;
                        scripted.create = spec;
                        scripted.createAs = WorldMode::Edit;
                        act(scripted);
                        next();
                    }
                    break;
                case 1:
                    if (worldFrames > 30 && groundReady()) {
                        screens.tab = EditTab::Size;
                        overBorder();
                        next();
                    }
                    break;
                case 2: if (tourFrames > 40) { tourShot("a1-size.png"); screens.tab = EditTab::Land; next(); } break;
                case 3: if (tourFrames > 20) { tourShot("a2-land-empty.png"); next(); } break;
                case 4:
                    if (drag(cx - pointsW * 0.22f, cx + pointsW * 0.22f, cy, 90)) {
                        tourDown = false;
                        tourStatus("land painted");
                        next();
                    }
                    break;
                case 5: if (tourFrames == 3) tourShot("a3-land-sketch.png");
                        if (tourFrames > 6) { screens.tab = EditTab::Coast; next(); }
                        break;
                case 6:
                    if (tourFrames == 10) tourShot("a4a-coast-panel.png");
                    if (tourFrames == 12) { scripted.shape.pin = true; act(scripted); next(); }
                    break;
                case 7: if (waitBuilt()) { tourStatus("worked out");
                    tourShot("a4-coast-worked-out.png"); screens.tab = EditTab::Mountains; next(); } break;
                case 8:
                    if (drag(cx - pointsW * 0.12f, cx + pointsW * 0.12f, cy, 60)) { tourDown = false; next(); }
                    break;
                case 9: if (waitBuilt()) { tourShot("a5-mountains.png"); tourStatus("mountains"); screens.tab = EditTab::Water; next(); } break;
                case 10: if (tourFrames == 10) { scripted.shape.water = true; act(scripted); next(); } break;
                case 11: if (waitBuilt()) { tourShot("a6-water.png"); tourStatus("water"); next(); } break;
                case 12:
                    // Down to the ground it made, and the tools that shape it by hand.
                    if (const char* at = std::getenv("CAMPFIRE_TOUR_CLOSE")) {
                        double fx = 3.0, fy = 7.5, fd = 20000;
                        std::sscanf(at, "%lf,%lf,%lf", &fx, &fy, &fd);
                        frame(pos(fx * double(generation::kRegionMetres), fy * double(generation::kRegionMetres)), fd);
                    } else
                    frame(pos(3.0 * double(generation::kRegionMetres), 7.5 * double(generation::kRegionMetres)), 20000);
                    screens.tab = EditTab::Terrain;
                    next();
                    break;
                case 13: if (tourFrames > 60 && groundReady()) { tourShot("a7-close.png"); next(); } break;
                case 14:
                    // A brush two kilometres across, raised along a line: kept
                    // at a quarter of a kilometre to a sample, not four metres.
                    screens.terrain.kind = world::BrushKind::Raise;
                    screens.terrain.radius = 2000;
                    screens.terrain.strength = 30;
                    if (drag(cx - pointsW * 0.15f, cx + pointsW * 0.15f, cy + pointsH * 0.1f, 90)) {
                        tourDown = false;
                        if (session && session->delta)
                            std::cout << "tour: sculpted at " << world::EditLayer::stepOf(world::EditLayer::levelFor(2000))
                                      << " m a sample, " << session->delta->heights()->blocks() << " block(s) held\n";
                        next();
                    }
                    break;
                case 15: if (tourFrames > 60 && groundReady()) { tourShot("a8-sculpted-wide.png"); next(); } break;
                default:
                    std::cout << "tour: layout audit found " << tourProblems << " problem(s)\n";
                    running = false;
                    break;
            }
            else switch (tourStep) {
                case 0: if (tourFrames > 120 && groundReady()) { tourShot("01-main-menu.png"); next(); } break;
                case 1: screens.screen = Screen::Host; next(); break;
                case 2:
                    if (tourFrames == 10) {
                        tourShot("02-worlds-empty.png");
                        screens.showCreateForm(true, world::saves::kReferenceRegionsX, world::saves::kReferenceRegionsY);
                    }
                    if (tourFrames == 20) { tourShot("02b-new-empty-reference.png"); screens.showCreateForm(false, 1, 1); next(); }
                    break;
                case 3: {
                    world::saves::NewWorld spec;
                    spec.name = "Tour world";
                    spec.preset = presets.empty() ? "" : presets.front().name;
                    spec.regions = 1;
                    spec.seed = 4242;
                    scripted.create = spec;
                    scripted.createAs = WorldMode::Explore;
                    act(scripted);
                    next();
                    break;
                }
                case 4: if (tourFrames > 20) { tourShot("03-loading.png"); next(); } break;
                case 5:
                    if (worldFrames > 30 && groundReady()) { tourShot("04-explore.png"); tourStatus("explore"); next(); }
                    else if (tourFrames % 600 == 0) tourStatus("still loading");
                    break;
                // Nothing moves and nothing is asked for: the pictures a few
                // frames apart should be the same picture (flicker shows here).
                case 6:
                    if (tourFrames == 6) tourShot("04b-explore-still.png");
                    if (tourFrames == 12) { tourShot("04c-explore-still.png"); next(); }
                    break;
                case 7: screens.mode = WorldMode::Edit; screens.tab = EditTab::Terrain; tourPointer = {{cx, cy}}; next(); break;
                case 8:
                    if (tourFrames > 20) {
                        tourShot("05-edit-terrain.png");
                        tourAt = {gx, gy};
                        std::cout << "tour: ground under the brush " << gz << " m\n";
                        next();
                    }
                    break;
                case 9:
                    // The button held on the ground, as a person holds it: the
                    // real input path, the default brush, a second and a half.
                    tourDown = true;
                    if (tourFrames == 45) tourShot("05b-edit-stroking.png");
                    if (tourFrames >= 90) {
                        tourDown = false;
                        std::cout << "tour: after the stroke the ground there is "
                                  << field.heightAt(pos(tourAt.first, tourAt.second)).toDouble() << " m\n";
                        tourStatus("stroke released");
                        next();
                    }
                    break;
                case 10:
                    if (tourFrames % 60 == 0) tourStatus("after the stroke");
                    if (tourFrames == 20) tourShot("05c-edit-released.png");
                    if (tourFrames > 240 && groundReady()) {
                        tourShot("06-edit-raised.png");
                        tourStatus("raised");
                        next();
                    }
                    break;
                case 11:
                    if (tourFrames == 6) tourShot("06b-edit-still.png");
                    if (tourFrames == 12) { tourShot("06c-edit-still.png"); next(); }
                    break;
                case 12: screens.tab = EditTab::Objects; next(); break;
                case 13: if (tourFrames > 10) { tourShot("07-edit-objects.png"); next(); } break;
                case 14: screens.objects.mode = ObjectMode::Plant; next(); break;
                case 15:
                    if (tourFrames == 11) {
                        tourShot("08-edit-plant.png");
                        screens.tab = EditTab::Ground;
                        // Ground presets round trip: saved, loaded back over
                        // the same files, byte for byte, and both tour presets
                        // taken away again.
                        std::string why;
                        const auto file = engine::biomes::defaultDirectory() / "categories.json";
                        const auto read = [](const std::filesystem::path& p) {
                            std::ifstream in(p, std::ios::binary);
                            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        };
                        const auto before = read(file);
                        const bool ok = TerrainPanel::savePreset("_tour-check", &why) &&
                                        TerrainPanel::loadPreset("_tour-check", &why) && read(file) == before;
                        TerrainPanel::removePreset("_tour-check");
                        TerrainPanel::removePreset("_before-load");
                        std::cout << "tour: ground presets " << (ok ? "round trip ok" : "FAILED: " + why) << "\n";
                    }
                    // The Ground tab (terrain_panel.hpp): laid out and audited,
                    // then its window over what the build has, pictures loaded.
                    if (tourFrames == 23) { tourShot("08g-edit-ground.png"); screens.ground.openCatalogue(); }
                    if (tourFrames > 60) {
                        tourShot("08h-ground-catalogue.png");
                        screens.ground.closeWindows();
                        screens.tab = EditTab::Objects;
                        next();
                    }
                    break;
                // The world's shape changed and raised again while it is in
                // view: the old world stays drawn until the new one is ready,
                // and the new one arrives with its ground.
                case 16: {
                    auto reshaped = menu.editor().layout();
                    auto settings = reshaped.at(0, 0).settings;
                    settings.seaPercent -= 6;
                    generation::generateWholeWorld(reshaped, settings, reshaped.seed, true);
                    menu.editor().showing(reshaped);
                    startBuild(reshaped, false, screens.mode);
                    tourStatus("reshaping");
                    next();
                    break;
                }
                case 17:
                    if (tourFrames == 30) { tourShot("08b-edit-reshaping.png"); tourStatus("while reshaping"); }
                    if (!build) { tourStatus("reshaped, just swapped"); next(); }
                    break;
                case 18:
                    if (tourFrames <= 90 && tourFrames % 15 == 0) tourStatus("after the swap");
                    if (tourFrames == 2) tourShot("08c-edit-reshaped-swap.png");
                    if (tourFrames > 90 && groundReady()) { tourShot("08d-edit-reshaped.png"); next(); }
                    break;
                // The world grows by a region to the east, left empty: what
                // was there stays exactly as it was. Then the new region is
                // made on its own, and the camera looks along the border.
                case 19: {
                    tourLayout = menu.editor().layout();
                    generation::resizeLayout(tourLayout, tourLayout.regionsX + 1, tourLayout.regionsY);
                    menu.editor().showing(tourLayout);
                    startBuild(tourLayout, false, screens.mode);
                    next();
                    break;
                }
                case 20:
                    if (!build) {
                        generation::generateRegions(tourLayout, {{tourLayout.regionsX - 1, 0}}, tourLayout.at(0, 0).settings,
                                                    tourLayout.seed + 17, true);
                        menu.editor().showing(tourLayout);
                        startBuild(tourLayout, false, screens.mode);
                        next();
                    }
                    break;
                case 21:
                    if (!build) {
                        const double border = double(generation::kRegionMetres) * (tourLayout.regionsX - 1);
                        frame(pos(border, double(generation::kRegionMetres) * 0.5), 30000);
                        next();
                    }
                    break;
                case 22:
                    if (tourFrames > 60 && groundReady()) {
                        tourShot("08e-grown-border.png");
                        tourStatus("grown");
                        const auto at = pos(gx, gy);
                        const auto& map = snapshot->worldMap();
                        const auto& cell = map.at({std::int32_t(gx / generation::kMetresPerCell), std::int32_t(gy / generation::kMetresPerCell)});
                        std::cout << "tour: under the pointer (" << gx << ", " << gy << "): cell " << (cell.sea ? "sea" : "land")
                                  << " elevation " << int(cell.elevation) << ", ground " << field.heightAt(at).toDouble()
                                  << " m, water " << field.waterLevelAt(at).toDouble() << " m"
                                  << (field.underWater(at) ? ", under water" : ", dry") << std::endl;
                        next();
                    }
                    break;
                case 23: screens.tab = EditTab::Land; next(); break;
                case 24: if (tourFrames > 20) { tourShot("09-edit-world-shape.png"); next(); } break;
                case 25: screens.tab = EditTab::Terrain; screens.paused = true; next(); break;
                case 26: if (tourFrames > 10) { tourShot("10-paused.png"); next(); } break;
                case 27: screens.paused = false; menu.togglePanel(); next(); break;
                case 28: if (tourFrames > 20) { tourShot("11-settings.png"); next(); } break;
                case 29: menu.togglePanel(); scripted.toMenu = true; act(scripted); screens.screen = Screen::Host; tourPointer.reset(); next(); break;
                case 30: if (tourFrames > 20) { tourShot("12-worlds.png"); next(); } break;
                default:
                    std::cout << "tour: layout audit found " << tourProblems << " problem(s)\n";
                    std::cout << "tour: interface painted " << paints << " times in " << frameIndex << " frames, "
                              << (paints ? paintMs / paints : 0.0) << " ms on average, " << paintMax << " ms at most, "
                              << (paints ? 100.0 * paintedArea / paints / std::max(1.0, double(pointsW) * pointsH) : 0.0)
                              << "% of the window each time\n";
                    running = false;
                    break;
            }
            if (tourFrames > 60 * 120) {
                std::cerr << "tour: step " << tourStep << " did not finish\n";
                running = false;
            }
        }

        // --- the picture of the interface: painted only where it changed -------------------------------
        {
            ui::Input quiet = input;
            quiet.pressed = quiet.released = quiet.rightPressed = quiet.doubleClick = false;
            quiet.wheel = 0;
            quiet.text.clear();
            quiet.backspace = quiet.enter = quiet.escape = quiet.tab = false;
            ClientActions ignored;
            shownView.now = since(start);
            lastView = shownView;
            // Built once more without painting, tile by tile: what changed,
            // and where.
            ui.trackTiles(kPaintTile);
            ui.begin(quiet, int(pointsW), int(pointsH));
            ui.painting(false);
            screens.build(ui, shownView, ignored);
            ui.end();
            ui.trackTiles(0);
            if (ui.signature() != painted) {
                const auto paintStart = Clock::now();
                const auto areas = painted == 0 ? std::vector<ui::Rect>{{0, 0, pointsW, pointsH}}
                                                : ui::changedAreas(paintedTiles, ui.tileSignatures(), ui.tileColumns(),
                                                                   ui.tileRows(), kPaintTile, pointsW, pointsH);
                paintedTiles = ui.tileSignatures();
                SDL_Renderer* sw = canvas.renderer();
                for (const auto& area : areas) {
                    // Cleared and built again inside the area alone: whatever
                    // lies outside it is neither drawn nor paid for.
                    ui.paintOnly(&area);
                    SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(sw, 0, 0, 0, 0);
                    const SDL_FRect clear = area.sdl();
                    SDL_RenderFillRect(sw, &clear);
                    SDL_SetRenderDrawBlendMode(sw, SDL_BLENDMODE_BLEND);
                    ui.begin(quiet, int(pointsW), int(pointsH));
                    screens.build(ui, shownView, ignored);
                    ui.end();
                    const int x0 = int(std::floor(area.x * pixelsPerPoint)), y0 = int(std::floor(area.y * pixelsPerPoint));
                    const int x1 = int(std::ceil(area.right() * pixelsPerPoint)),
                              y1 = int(std::ceil(area.bottom() * pixelsPerPoint));
                    canvas.painted(SDL_Rect{x0, y0, x1 - x0, y1 - y0});
                    paintedArea += double(area.w) * double(area.h);
                }
                ui.paintOnly(nullptr);
                SDL_RenderPresent(sw);
                painted = ui.signature();
                const double ms = since(paintStart) * 1000.0;
                paintMs += ms;
                paintMax = std::max(paintMax, ms);
                ++paints;
            }
        }

        renderer.character(hero.active() && screens.mode == WorldMode::Explore && screens.screen == Screen::World
                                   ? hero.state(camera, heroGround, dt) : game::CharacterPass::State{});
        if (!view.draw(camera)) {
            std::cerr << "the frame would not draw: " << renderer.error() << "\n";
            break;
        }
        engine::profile::frame();
        profiler.draw();
    }

    // Everything unsaved is saved on the way out.
    if (build) build.reset();   // joins the worker
    closeSession();
    if (window && SDL_TextInputActive(window)) SDL_StopTextInput(window);
    ui.shutdown();
    renderer.forgetTheWorld();
    return 0;
}

} // namespace client

