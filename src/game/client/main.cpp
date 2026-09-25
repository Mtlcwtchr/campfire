// Graphical client.
//
// The client owns a World and advances it; it never reaches into simulation state
// to change anything (GDD 11). Everything on screen is read from the same world a
// headless run would produce from the same seed, so what you watch here and what
// sim_runner reports are the same run.

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <limits>
#include <string>

#include "game/client/camera.hpp"
#include "game/client/explorer.hpp"
#include "game/client/explore_view.hpp"
#include "game/client/newgame_screen.hpp"
#include "game/client/renderer.hpp"
#include "game/client/selection.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/simulation/savegame.hpp"
#include "game/simulation/world.hpp"
#include "game/simulation/zones.hpp"
#include "game/client/controls.hpp"
#include "game/ui/editor.hpp"
#include "game/ui/hud.hpp"
#include "engine/ui/ui.hpp"

namespace {

// Simulation rate. Speed multipliers run more ticks per frame; they never change
// the tick itself, so a fast-forwarded game is identical to a slow one.
constexpr double kTicksPerSecond = 0.5;

// Wall-clock time one frame may spend inside the simulation. Above this the frame
// is presented anyway, so panning and the HUD stay responsive at high speed.
constexpr double kSimBudgetMs = 12.0;

std::filesystem::path resolveContent() {
    for (const char* prefix : {".", "..", "../..", "../../..", "../../../.."}) {
        std::filesystem::path candidate = std::filesystem::path(prefix) / "content";
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return "content";
}

std::filesystem::path resolveSprites() {
    for (const char* prefix : {".", "..", "../..", "../../..", "../../../.."}) {
        std::filesystem::path candidate = std::filesystem::path(prefix) / "assets" / "sprites";
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return std::filesystem::path("assets") / "sprites";
}



// The player's own areas live in one zone per (kind, mode) pair, made on first
// use. Painting into an existing one keeps the count low and the map readable.
core::ZoneId playerZoneFor(sim::World& w, sim::ZoneKind kind, sim::ZoneMode mode) {
    for (const auto& z : w.zones())
        if (z.alive && z.playerDrawn && z.kind == kind && z.mode == mode) return z.id;

    std::string label = std::string(sim::zoneKindName(kind));
    if (mode == sim::ZoneMode::Forbidden) label += " (forbidden)";
    else if (mode == sim::ZoneMode::HighPriority) label += " (priority)";
    return w.createZone(label, kind, mode, sim::SettlementId{0}, true);
}

void paintZones(sim::World& w, const ui::Hud& hud, core::WorldPos centre, bool erase) {
    const core::Fixed radius = core::Fixed::fromInt(hud.brushRadius) + core::Fixed::ratio(1, 2);

    if (erase) {
        // Erasing lifts the player's own areas only; what the community laid out
        // for itself is not the player's to rub out with a brush.
        for (auto& z : w.zones())
            if (z.alive && z.playerDrawn) w.removeDiscFromZone(z.id, centre, radius);
        return;
    }
    const core::ZoneId id = playerZoneFor(w, hud.brushKind, hud.brushMode);
    if (id.valid()) w.addDiscToZone(id, centre, radius);
}

void cycleBrushKind(ui::Hud& hud, int delta) {
    // The kinds worth painting by hand; General and Patrol have no planner
    // meaning yet, so they are not offered.
    static const sim::ZoneKind kBrushable[] = {
        sim::ZoneKind::Farm,       sim::ZoneKind::Pasture,  sim::ZoneKind::Extraction,
        sim::ZoneKind::Hunting,    sim::ZoneKind::Storage,  sim::ZoneKind::Settlement,
        sim::ZoneKind::Fortification,
    };
    constexpr int count = static_cast<int>(sizeof(kBrushable) / sizeof(kBrushable[0]));
    int index = 0;
    for (int i = 0; i < count; ++i) if (kBrushable[i] == hud.brushKind) index = i;
    hud.brushKind = kBrushable[((index + delta) % count + count) % count];
}

void cycleBrushMode(ui::Hud& hud) {
    switch (hud.brushMode) {
        case sim::ZoneMode::Allowed:      hud.brushMode = sim::ZoneMode::Preferred; break;
        case sim::ZoneMode::Preferred:    hud.brushMode = sim::ZoneMode::HighPriority; break;
        case sim::ZoneMode::HighPriority: hud.brushMode = sim::ZoneMode::Forbidden; break;
        case sim::ZoneMode::Forbidden:    hud.brushMode = sim::ZoneMode::Allowed; break;
    }
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 11;
    std::int32_t mapSize = 180;
    std::int32_t population = 24;
    // One frame to a file and out again. Reviewing this game means looking at
    // it, and looking at it should not need a pair of hands on the keyboard: a
    // shot can be asked for at a given day, zoom and overlay, and comes out the
    // same every time.
    std::string shotPath;
    // Looking at the world instead of playing in it: no communities, no
    // simulation, just the ground the world layer describes, at every scale
    // (D118).
    bool exploring = false;
    std::int32_t worldCells = 0;
    std::string exploreAt;
    bool exploreTrace = false;
    bool exploreMenu = false;
    client::ExploreViewOptions exploreOptions;
    double shotTime = 0.0;
    // How many cards to strew over the ground, for measuring the instanced path
    // with nobody at the keyboard. A crowd is the thing the batching exists for,
    // and until the game moves onto this pipeline there is no crowd to point at.
    int exploreCrowd = 0;
    int shotFrame = 0;
    bool exploreFlight = false;
    bool exploreClose = false;
    std::string menuShotPath;
    bool shotEditor = false;
    int shotEditorTab = 1;
    std::int32_t shotDay = 0;
    double shotZoom = 0.0;
    std::string shotOverlay;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--map" && i + 1 < argc) mapSize = std::atoi(argv[++i]);
        else if (a == "--pop" && i + 1 < argc) population = std::atoi(argv[++i]);
        else if (a == "--explore") exploring = true;
        else if (a == "--world" && i + 1 < argc) {
            // A name from the list, or a number for the tools and the tests.
            const std::string want = argv[++i];
            worldCells = std::atoi(want.c_str());
            for (const generation::WorldSize& size : generation::kWorldSizes)
                if (want == size.name) worldCells = size.cells;
        }
        else if (a == "--at" && i + 1 < argc) exploreAt = argv[++i];
        else if (a == "--flight") exploreFlight = true;
        else if (a == "--flight-close") { exploreFlight = true; exploreClose = true; }
        else if (a == "--trace") exploreTrace = true;
        else if (a == "--menu") exploreMenu = true;
        else if (a == "--camera" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "map") exploreOptions.cameraMode = client::Camera::Mode::Map;
            else if (mode == "orbit") exploreOptions.cameraMode = client::Camera::Mode::Orbit;
            else if (mode == "free") exploreOptions.cameraMode = client::Camera::Mode::Free;
            else { std::cerr << "Expected --camera map|orbit|free\n"; return 1; }
        }
        else if (a == "--grid" && i + 1 < argc) {
            const std::string grid = argv[++i];
            if (grid == "off") exploreOptions.gridMode = 0;
            else if (grid == "samples") exploreOptions.gridMode = 1;
            else if (grid == "mesh") exploreOptions.gridMode = 2;
            else { std::cerr << "Expected --grid off|samples|mesh\n"; return 1; }
        }
        else if (a == "--object-mesh") exploreOptions.objectMesh = true;
        else if (a == "--yaw" && i + 1 < argc) exploreOptions.yaw = std::atof(argv[++i]);
        else if (a == "--pitch" && i + 1 < argc) exploreOptions.pitch = std::atof(argv[++i]);
        else if (a == "--height-offset" && i + 1 < argc) exploreOptions.heightOffset = std::atof(argv[++i]);
        else if (a == "--shot-time" && i + 1 < argc) shotTime = std::atof(argv[++i]);
        else if (a == "--crowd" && i + 1 < argc) exploreCrowd = std::atoi(argv[++i]);
        else if (a == "--shot-frame" && i + 1 < argc) shotFrame = std::atoi(argv[++i]);
        else if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--shot-menu" && i + 1 < argc) menuShotPath = argv[++i];
        else if (a == "--shot-editor") shotEditor = true;
        else if (a == "--shot-editor-tab" && i + 1 < argc) shotEditorTab = std::atoi(argv[++i]);
        else if (a == "--day" && i + 1 < argc) shotDay = std::atoi(argv[++i]);
        else if (a == "--zoom" && i + 1 < argc) shotZoom = std::atof(argv[++i]);
        else if (a == "--overlay" && i + 1 < argc) shotOverlay = argv[++i];
        else if (a == "--help") {
            std::cout << "usage: asr_client [--seed N] [--map N] [--pop N]\n"
                      << "       asr_client --shot-menu FILE   the world-choosing screen\n"
                      << "       asr_client --shot FILE [--day N] [--zoom PIXELS_PER_TILE]"
                      << " [--overlay none|zones|jobs]\n"
                      << "       asr_client --world tiny|smaller|small|average|medium|large|giant\n"
                      << "       asr_client --explore [--menu] [--seed N] [--world N]"
                      << " [--shot FILE] [--camera map|orbit|free] [--grid off|samples|mesh]"
                      << " [--object-mesh]"
                      << " [--yaw RADIANS] [--pitch RADIANS] [--height-offset METRES]"
                      << "   the world, without the game in it\n";
            return 0;
        }
    }

    content::ContentDb db;
    const auto contentPath = resolveContent();
    if (!db.load(contentPath)) {
        std::cerr << "content failed to load from " << contentPath << ":\n";
        for (const auto& e : db.errors()) std::cerr << "  " << e << "\n";
        return 1;
    }

    sim::WorldConfig cfg;
    cfg.seed = seed;
    cfg.worldCells = worldCells;
    cfg.mapWidth = mapSize;
    cfg.mapHeight = mapSize;
    cfg.startingPopulation = population;
    // Every community on the map, not just ours (D95). The country carries a
    // dozen or so founding bands, all the same size, all arriving on the same
    // morning - and every one of them is simulated, so what each becomes is
    // what it makes of the ground it drew. Before this the neighbours were
    // marks on a map with nobody in them, which is the one thing a settlement
    // cannot be.
    //
    // They share the country map rather than each keeping a copy of four
    // million identical cells.
    // The game itself - the country, every community on it, and which one is
    // being watched - is built once the player has settled on a world. Until
    // then there is nothing to simulate.
    sim::Game game;
    auto& communities = game.communities;
    auto& watching = game.watching;
    sim::World* active = nullptr;


    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    if (exploring) {
        SDL_Window* gpuWindow = SDL_CreateWindow("Ancient Settlement — GPU World Explorer",
                                                 1280, 800, SDL_WINDOW_RESIZABLE);
        if (!gpuWindow) {
            std::cerr << "GPU explorer window creation failed: " << SDL_GetError() << "\n";
            SDL_Quit();
            return 1;
        }
        const int outcome = client::runExploreMode(gpuWindow, seed,
                                                   worldCells > 0 ? worldCells : generation::kDefaultPlayerWorldCells,
                                                   resolveSprites(), shotPath, shotZoom,
                                                   exploreAt, exploreFlight, exploreClose,
                                                   exploreTrace, shotFrame, exploreMenu,
                                                    exploreCrowd, shotTime, "none", 0.0, 0, {},
                                                    {18.0f, 32.0f, 21.0f, 9.0f}, exploreOptions);
        SDL_DestroyWindow(gpuWindow);
        SDL_Quit();
        return outcome;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* sdlRenderer = nullptr;
    if (!SDL_CreateWindowAndRenderer("Ancient Settlement", 1280, 800, SDL_WINDOW_RESIZABLE,
                                     &window, &sdlRenderer)) {
        std::cerr << "window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    client::Renderer renderer;
    if (!renderer.init(sdlRenderer, resolveSprites())) {
        std::cerr << "renderer assets failed to load: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(sdlRenderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    client::Camera camera;


    // --- the world to settle ------------------------------------------------
    // Nothing is simulated until the player has settled on a world. The screen
    // generates one from the dials in front of them, and starting the game is
    // what puts the founding bands on it (D101).
    const std::filesystem::path saveDir = resolveContent().parent_path() / "saves";
    const auto buildNewGame = [&](const generation::WorldMapParams& params) {
        std::cout << "settling " << params.width << "x" << params.height << " world ..."
                  << std::flush;
        const auto started = std::chrono::steady_clock::now();
        sim::WorldConfig base = cfg;
        base.seed = params.seed;
        game = sim::newGame(db, base, params);
        std::cout << " " << game.communities.size() << " communities in "
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
                  << " s\n";
    };

    // A picture of the screen the game opens on, without a window in front of
    // anybody: the same code path, drawn once and written out.
    if (!menuShotPath.empty()) {
        client::NewGameScreen screen;
        screen.open(sdlRenderer, resolveContent(), saveDir);
        screen.draw(sdlRenderer, 1280, 800);
        SDL_Surface* shot = SDL_RenderReadPixels(sdlRenderer, nullptr);
        if (!shot || !IMG_SavePNG(shot, menuShotPath.c_str())) {
            std::cerr << "could not write " << menuShotPath << ": " << SDL_GetError() << "\n";
            return 1;
        }
        SDL_DestroySurface(shot);
        std::cout << "wrote " << menuShotPath << "\n";
        screen.close();
        SDL_DestroyRenderer(sdlRenderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    if (shotPath.empty()) {
        client::NewGameScreen screen;
        screen.open(sdlRenderer, resolveContent(), saveDir);
        bool choosing = true;
        while (choosing) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_EVENT_WINDOW_RESIZED || e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    continue;
                switch (screen.handle(e)) {
                    case client::NewGameScreen::Outcome::Quit:
                        screen.close();
                        SDL_DestroyRenderer(sdlRenderer);
                        SDL_DestroyWindow(window);
                        SDL_Quit();
                        return 0;
                    case client::NewGameScreen::Outcome::Start: {
                        generation::WorldMapParams params = screen.chosen();
                        sim::WorldConfig base = cfg;
                        base.seed = params.seed;
                        buildNewGame(params);
                        choosing = false;
                        break;
                    }
                    case client::NewGameScreen::Outcome::Load: {
                        std::string error;
                        if (sim::loadGame(db, screen.chosenSave(), game, &error)) {
                            std::cout << "loaded " << screen.chosenSave().string() << "\n";
                            choosing = false;
                        } else {
                            std::cerr << "could not load: " << error << "\n";
                        }
                        break;
                    }
                    case client::NewGameScreen::Outcome::Staying: break;
                }
                if (!choosing) break;
            }
            if (!choosing) break;
            int w = 1280, h = 800;
            SDL_GetRenderOutputSize(sdlRenderer, &w, &h);
            screen.draw(sdlRenderer, w, h);
            SDL_RenderPresent(sdlRenderer);
            SDL_Delay(16);
        }
        screen.close();
    } else {
        generation::WorldMapParams params;
        params.seed = seed;
        if (cfg.worldCells > 0) { params.width = cfg.worldCells; params.height = cfg.worldCells; }
        buildNewGame(params);
    }
    if (game.communities.empty()) {
        std::cerr << "no communities were settled\n";
        return 1;
    }
    active = game.communities.front().get();
    for (std::size_t i = 0; i < game.communities.size(); ++i)
        game.communities[i]->setDetail(i == game.watching ? sim::World::Detail::Detailed
                                                          : sim::World::Detail::Abstract);

    ui::Hud hud;
    ui::Ui gui;
    if (!gui.init(sdlRenderer)) {
        std::cerr << "the interface could not build its font\n";
        return 1;
    }
    ui::Input pointer;
    client::CameraControl controls;
    ui::Editor editor;
    // The look is content: if somebody has tuned it in the editor, that is what
    // the game opens with.
    ui::loadTheme(gui.theme(), resolveContent() / "config" / "ui_theme.json");

    if (!shotPath.empty()) {
        // Everybody's days pass, not just the one being photographed: a picture
        // of day sixty with the neighbours still on day zero is a picture of a
        // world that does not exist.
        for (std::int32_t day = 0; day < shotDay; ++day)
            for (auto& community : communities) community->runTicks(db.time().ticksPerDay());
        camera.centreX = (*active).settlements().front().hearth.x;
        camera.centreY = (*active).settlements().front().hearth.y;
        if (shotZoom > 0.0) camera.pixelsPerTile = shotZoom;
        camera.viewportWidth = 1280;
        camera.viewportHeight = 800;
        // Penned like the live camera is, so a picture asked for at (*active) zoom
        // comes out centred on the country rather than on the settlement with
        // the country hanging off one corner.
        camera.clampTo((*active).map().width(), (*active).map().height());
        // A picture with the inspector open is the picture that shows the HUD
        // doing its job, so a shot picks something to look at.
        for (const auto& b : active->buildings()) {
            if (!b.alive || b.state != sim::BuildState::Complete) continue;
            if (active->db().building(b.def).kind == content::BuildingKind::Other) continue;
            hud.selected.kind = client::SelectionKind::Building;
            hud.selected.building = b.id;
            hud.selected.tile = b.origin;
            break;
        }
        client::Overlay overlay = client::Overlay::None;
        if (shotOverlay == "zones") overlay = client::Overlay::Zones;
        else if (shotOverlay == "jobs") overlay = client::Overlay::Jobs;
        renderer.updatePresentation((*active), 0.0, 0.0);
        renderer.draw((*active), camera, overlay, client::Selection{}, client::BrushPreview{});
        {
            ui::Input still;
            still.mouseX = -1000;
            still.mouseY = -1000;
            gui.begin(still, camera.viewportWidth, camera.viewportHeight);
            ui::drawHud(gui, hud, *active, renderer, 0.0);
            if (shotEditor) {
                editor.open = true;
                editor.tab = static_cast<ui::Editor::Tab>(std::clamp(shotEditorTab, 0, 2));
                editor.chosen = 3;
                ui::drawEditor(gui, editor, db, resolveContent(), &game);
            }
            gui.end();
        }
        SDL_Surface* shot = SDL_RenderReadPixels(sdlRenderer, nullptr);
        if (!shot || !IMG_SavePNG(shot, shotPath.c_str())) {
            std::cerr << "could not write " << shotPath << ": " << SDL_GetError() << "\n";
            return 1;
        }
        SDL_DestroySurface(shot);
        std::cout << "wrote " << shotPath << " at day " << shotDay << ", "
                  << camera.pixelsPerTile << " px/tile\n";
        renderer.shutdown();
        SDL_DestroyRenderer(sdlRenderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
    bool painting = false;
    bool dragging = false;
    double dragFromX = 0, dragFromY = 0;
    bool erasingStroke = false;
    double simMsPerTick = 0.0;
    double tickAccumulator = 0.0;
    auto lastFrame = std::chrono::steady_clock::now();
    bool running = true;

    // Looking at somebody else's country. The (*active) map knows where eleven
    // hundred peoples live (D91) and until now they were marks you could not do
    // anything with. Clicking one picks it; entering builds the ground under it
    // at play scale and shows it. That second (*active) is never ticked - nobody
    // lives on it as far as the simulation is concerned - so this is a look at
    // the land, not a second game.
    // Going to look at one of the neighbours. They are all being simulated
    // already (D95), so this is not building anything - it is pointing the
    // camera and the panels at a different one of them.
    const generation::WorldSite* pickedSite = nullptr;
    const auto siteAt = [&](const sim::World& shown, int screenX, int screenY)
            -> const generation::WorldSite* {
        const auto& wm = shown.worldMap();
        if (wm.cells.empty()) return nullptr;
        const double cellSide = generation::kMetresPerCell;
        double wx = 0, wy = 0;
        camera.worldOfScreen(screenX, screenY, wx, wy);
        const double cx = (wx + shown.localBlock().x * cellSide) / cellSide;
        const double cy = (wy + shown.localBlock().y * cellSide) / cellSide;
        // Whatever is nearest within a few cells of the click: a marker is a few
        // pixels across at this distance and nobody hits a single cell.
        const double reach = std::max(3.0, 14.0 / std::max(0.01, cellSide * camera.pixelsPerTile));
        const generation::WorldSite* best = nullptr;
        double bestDistance = reach * reach;
        for (const auto& site : wm.sites) {
            const double dx = site.cell.x + 0.5 - cx;
            const double dy = site.cell.y + 0.5 - cy;
            const double d = dx * dx + dy * dy;
            if (d < bestDistance) { bestDistance = d; best = &site; }
        }
        return best;
    };
    const auto lookAt = [&](std::size_t which) {
        if (which >= communities.size()) return;
        watching = which;
        active = communities[watching].get();
        hud.selected = {};
        pickedSite = nullptr;
        if (!active->settlements().empty()) {
            camera.centreX = active->settlements().front().hearth.x;
            camera.centreY = active->settlements().front().hearth.y;
        }
        camera.pixelsPerTile = client::kDefaultPixelsPerTile / 2.0;
        // Only the community being watched is simulated in full. The others go
        // on living by the day's accounting - same people, same stores, same
        // buildings, no pawn deciding anything - which is what makes a map of
        // sixteen communities cost about what one costs (D103).
        for (std::size_t i = 0; i < communities.size(); ++i)
            communities[i]->setDetail(i == watching ? sim::World::Detail::Detailed
                                                    : sim::World::Detail::Abstract);
        // How far the camera may go, in map metres. The country is drawn in the
        // same coordinates as the local map - the watched cell lies exactly over
        // it (D84) - so it reaches away by however many cells lie on each side,
        // and the near edge is negative. Without this the camera could be pulled
        // back to a continent while still penned inside one local map of it.
        const auto& wm = active->worldMap();
        const double cellSide = generation::kMetresPerCell;
        const core::TilePos here = active->localCell();
        const double left = -here.x * cellSide;
        const double top = -here.y * cellSide;
        camera.setBounds(left, top, left + wm.width * cellSide, top + wm.height * cellSide);
        const double fitsWidth = camera.viewportWidth / std::max(1.0, wm.width * cellSide);
        const double fitsHeight = camera.viewportHeight / std::max(1.0, wm.height * cellSide);
        camera.minZoom = std::min({client::kMinPixelsPerTile, fitsWidth * 0.85, fitsHeight * 0.85});
    };
    const auto goHome = [&]() { lookAt(0); };
    const auto visitSite = [&](const generation::WorldSite& site) {
        for (std::size_t i = 0; i < communities.size(); ++i)
            if (communities[i]->localCell() == site.cell) { lookAt(i); return; }
    };

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    switch (e.key.key) {
                        // Saving and loading. One quick slot, written beside the
                        // content: a game is a file, and starting again is
                        // choosing not to open it (D101).
                        case SDLK_F5: {
                            std::error_code ec;
                            std::filesystem::create_directories(saveDir, ec);
                            const auto file = saveDir / "quick.save";
                            std::string error;
                            if (sim::saveGame(game, file, &error))
                                std::cout << "saved " << file.string() << "\n";
                            else
                                std::cerr << "could not save: " << error << "\n";
                            break;
                        }
                        case SDLK_F9: {
                            const auto file = saveDir / "quick.save";
                            std::string error;
                            sim::Game loaded;
                            if (sim::loadGame(db, file, loaded, &error)) {
                                game = std::move(loaded);
                                lookAt(game.watching);
                                std::cout << "loaded " << file.string() << "\n";
                            } else {
                                std::cerr << "could not load: " << error << "\n";
                            }
                            break;
                        }
                        case SDLK_F1:
                            editor.open = !editor.open;
                            break;
                        case SDLK_ESCAPE:
                            if (editor.open) { editor.open = false; break; }
                            // Out of somebody else's valley first, out of the
                            // game only when there is nowhere left to come back
                            // from.
                            if (watching != 0 || pickedSite != nullptr) goHome();
                            else running = false;
                            break;
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            if (pickedSite != nullptr) visitSite(*pickedSite);
                            break;
                        case SDLK_SPACE:
                            if (hud.speed == 0) {
                                hud.speed = hud.speedBeforePause;
                            } else {
                                hud.speedBeforePause = hud.speed;
                                hud.speed = 0;
                            }
                            break;
                        case SDLK_1: hud.speed = hud.speedBeforePause = 1; break;
                        case SDLK_2: hud.speed = hud.speedBeforePause = 2; break;
                        case SDLK_3: hud.speed = hud.speedBeforePause = 3; break;
                        case SDLK_4: hud.speed = hud.speedBeforePause = 4; break;
                        case SDLK_5: hud.speed = hud.speedBeforePause = 5; break;
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS:
                            hud.speed = std::max(0, hud.speed - 1);
                            if (hud.speed > 0) hud.speedBeforePause = hud.speed;
                            break;
                        case SDLK_EQUALS:
                        case SDLK_PLUS:
                        case SDLK_KP_PLUS:
                            hud.speed = std::min(ui::kSpeedCount - 1, hud.speed + 1);
                            hud.speedBeforePause = hud.speed;
                            break;
                        case SDLK_Z:      hud.overlay = client::Overlay::Zones; break;
                        case SDLK_J:      hud.overlay = client::Overlay::Jobs; break;
                        case SDLK_O:      hud.overlay = client::Overlay::None; break;
                        case SDLK_T:      hud.deck = ui::Deck::People; break;
                        case SDLK_B:      hud.deck = ui::Deck::Buildings; break;
                        case SDLK_E:
                            hud.drawingZones = !hud.drawingZones;
                            // Areas are invisible without the overlay, so turn it
                            // on rather than letting the player paint blind.
                            if (hud.drawingZones) hud.overlay = client::Overlay::Zones;
                            break;
                        case SDLK_LEFTBRACKET:  if (hud.drawingZones) cycleBrushKind(hud, -1); break;
                        case SDLK_RIGHTBRACKET: if (hud.drawingZones) cycleBrushKind(hud, +1); break;
                        case SDLK_M:            if (hud.drawingZones) cycleBrushMode(hud); break;
                        case SDLK_COMMA:
                            if (hud.drawingZones) hud.brushRadius = std::max(0, hud.brushRadius - 1);
                            break;
                        case SDLK_PERIOD:
                            if (hud.drawingZones) hud.brushRadius = std::min(8, hud.brushRadius + 1);
                            break;
                        default: break;
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL:
                    pointer.wheel += e.wheel.y;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                    if (e.button.button == SDL_BUTTON_MIDDLE) { pointer.middleDown = true; break; }
                    if (e.button.button == SDL_BUTTON_RIGHT) {
                        pointer.rightPressed = true;
                        break;
                    }
                    if (e.button.button != SDL_BUTTON_LEFT) break;
                    pointer.pressed = true;
                    pointer.down = true;
                    break;
                }
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (e.button.button == SDL_BUTTON_MIDDLE) { pointer.middleDown = false; break; }
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        pointer.released = true;
                        pointer.down = false;
                    }
                    painting = false;
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    pointer.mouseX = e.motion.x;
                    pointer.mouseY = e.motion.y;
                    break;
                default: break;
            }
        }
        {
            float mx = 0, my = 0;
            const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mx, &my);
            pointer.mouseX = mx;
            pointer.mouseY = my;
            pointer.middleDown = (buttons & SDL_BUTTON_MMASK) != 0;
            const SDL_Keymod mods = SDL_GetModState();
            pointer.shift = (mods & SDL_KMOD_SHIFT) != 0;
            pointer.ctrl = (mods & SDL_KMOD_CTRL) != 0;
        }

        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastFrame).count();
        lastFrame = now;

        SDL_GetWindowSize(window, &camera.viewportWidth, &camera.viewportHeight);
        // Recomputed per frame: "far enough back to see the whole (*active)" depends
        // on the size of the window, and the window is resizable.
        {
            const auto& wm = (*active).worldMap();
            const double cell = generation::kMetresPerCell;
            const double fitsWidth = camera.viewportWidth / std::max(1.0, wm.width * cell);
            const double fitsHeight = camera.viewportHeight / std::max(1.0, wm.height * cell);
            camera.minZoom = std::min({client::kMinPixelsPerTile, fitsWidth * 0.85, fitsHeight * 0.85});
        }

        // The camera: keys, the edge of the screen, a dragged middle button and
        // the wheel, with momentum on all of it. The interface gets first refusal
        // on the pointer, so reaching for a button does not scroll the map.
        const bool* keys = SDL_GetKeyboardState(nullptr);
        controls.update(camera, pointer, keys, dt, hud.overInterface);
        camera.clampTo((*active).map().width(), (*active).map().height());

        // --- what the player did with the pointer ---------------------------
        // The interface has already had its turn by now (the HUD was built at
        // the end of the last frame and set `takenByUi`), so anything left is
        // aimed at the world.
        if (!hud.overInterface) {
            if (pointer.pressed) {
                dragFromX = pointer.mouseX;
                dragFromY = pointer.mouseY;
                dragging = true;
            }
            if (hud.drawingZones && pointer.down) {
                painting = true;
                erasingStroke = false;
                double wx = 0.0, wy = 0.0;
                camera.worldOfScreen(static_cast<int>(pointer.mouseX), static_cast<int>(pointer.mouseY), wx, wy);
                paintZones(*active, hud,
                           {core::Fixed::fromDoubleForContent(wx), core::Fixed::fromDoubleForContent(wy)},
                           false);
            }
            if (pointer.rightPressed) {
                // The right button lifts an area, or clears the selection. There
                // is nothing else for it to put down: the player draws areas and
                // the community builds what it decides to (D110).
                if (hud.drawingZones) {
                    double wx = 0.0, wy = 0.0;
                    camera.worldOfScreen(static_cast<int>(pointer.mouseX), static_cast<int>(pointer.mouseY), wx, wy);
                    paintZones(*active, hud,
                               {core::Fixed::fromDoubleForContent(wx), core::Fixed::fromDoubleForContent(wy)},
                               true);
                } else {
                    hud.selected = {};
                }
            }
            if (pointer.released && dragging) {
                dragging = false;
                painting = false;
                const float dx = pointer.mouseX - static_cast<float>(dragFromX);
                const float dy = pointer.mouseY - static_cast<float>(dragFromY);
                const bool isBox = std::abs(dx) > 6 || std::abs(dy) > 6;
                if (camera.pixelsPerTile < client::kWorldViewPixelsPerTile) {
                    // Out over the country a click is aimed at a settlement, not
                    // at a tile: first click picks it, second one goes there.
                    const generation::WorldSite* hit =
                            siteAt(*active, static_cast<int>(pointer.mouseX),
                                   static_cast<int>(pointer.mouseY));
                    if (hit != nullptr && hit == pickedSite) visitSite(*hit);
                    else pickedSite = hit;
                } else if (isBox) {
                    const ui::Rect box{std::min(static_cast<float>(dragFromX), pointer.mouseX),
                                       std::min(static_cast<float>(dragFromY), pointer.mouseY),
                                       std::abs(dx), std::abs(dy)};
                    const auto caught = client::pickInBox(*active, camera, box);
                    if (!caught.empty()) hud.selected = caught.front();
                } else {
                    hud.selected = client::pickAt(*active, camera, pointer.mouseX, pointer.mouseY,
                                                  hud.selected);
                }
            }
        }
        if (pointer.released) { dragging = false; painting = false; }

        // Fixed timestep. A speed multiplier only asks for more ticks per second;
        // the tick itself never changes, so a fast-forwarded game is bit-identical
        // to a slow one and to a headless run of the same seed.
        const int multiplier = ui::kSpeedLadder[hud.speed];
        const double requestedTicksPerSecond = kTicksPerSecond * multiplier;
        tickAccumulator += dt * requestedTicksPerSecond;
        int requested = static_cast<int>(tickAccumulator);
        tickAccumulator -= requested;

        // Two guards. The count cap scales with the requested speed so 10x is
        // actually reachable, and the time budget keeps the window responsive if
        // the simulation cannot keep up - it falls behind visibly rather than
        // freezing the frame.
        const int countCap = std::max(64, multiplier * 16);
        const bool cappedByCount = requested > countCap;
        requested = std::min(requested, countCap);

        int done = 0;
        if (requested > 0) {
            const auto simStart = std::chrono::steady_clock::now();
            while (done < requested) {
                // Everybody's day passes at the same rate, whoever is being
                // watched: a neighbour who only lives while looked at is not a
                // neighbour (D95).
                for (auto& community : communities) community->tick();
                ++done;
                if (std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - simStart).count() > kSimBudgetMs)
                    break;
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                      std::chrono::steady_clock::now() - simStart).count();
            // Smoothed, so the readout is usable rather than jittery.
            simMsPerTick = simMsPerTick * 0.9 + (ms / done) * 0.1;
            // Drop the backlog we could not run. Carrying it would make the game
            // sprint to catch up the moment the load eases.
            if (done < requested) tickAccumulator = 0.0;
        }
        hud.speedLimited = multiplier > 0 && (cappedByCount || done < requested);
        renderer.updatePresentation((*active), dt, requestedTicksPerSecond);

        // People die, sites are abandoned, batches are eaten: a selection can
        // outlive what it points at.
        if (hud.selected.valid() && !client::stillExists((*active), hud.selected)) hud.selected = {};

        client::BrushPreview brush;
        if (hud.drawingZones) {
            float mx = 0, my = 0;
            SDL_GetMouseState(&mx, &my);
            double wx = 0.0, wy = 0.0;
            camera.worldOfScreen(static_cast<int>(mx), static_cast<int>(my), wx, wy);
            brush.active = true;
            brush.centre = {core::Fixed::fromDoubleForContent(wx), core::Fixed::fromDoubleForContent(wy)};
            brush.radius = hud.brushRadius;
            brush.erasing = erasingStroke && painting;
            brush.colour = brush.erasing ? SDL_FColor{0.9f, 0.35f, 0.35f, 0.30f}
                                         : SDL_FColor{0.95f, 0.95f, 0.70f, 0.28f};
        }

        renderer.draw(*active, camera, hud.overlay, hud.selected, brush);
        gui.begin(pointer, camera.viewportWidth, camera.viewportHeight);
        ui::drawHud(gui, hud, *active, renderer, simMsPerTick);
        ui::drawEditor(gui, editor, db, resolveContent(), &game);
        gui.end();
        // One-shot: a press is a press for one frame, whoever took it.
        pointer.pressed = false;
        pointer.released = false;
        pointer.rightPressed = false;
        pointer.wheel = 0;

        // What the (*active) knows about the place under the cursor's last click,
        // and how to get into it. Drawn here rather than in the HUD because it
        // is about the (*active) map, which is the client's business and not the
        // settlement's.
        if (pickedSite != nullptr || watching != 0) {
            const generation::WorldSite* site = pickedSite;
            if (site == nullptr) {
                for (const auto& s : active->worldMap().sites)
                    if (s.cell == active->localCell()) site = &s;
            }
            if (site != nullptr) {
                const auto& cell = (*active).worldMap().at(site->cell);
                std::vector<std::string> lines;
                const bool isHere = site->cell == active->localCell();
                lines.push_back(site->name + (site->played ? "  (ours)" : "") +
                                (isHere && !site->played ? "  (watching)" : ""));
                lines.push_back(std::string(site->ethnos) + ", " +
                                generation::livelihoodName(site->livelihood));
                // What is actually alive there, not what the map guessed: every
                // community is simulated, so this is a count of people.
                const sim::World* theirs = nullptr;
                for (const auto& community : communities)
                    if (community->localCell() == site->cell) theirs = community.get();
                lines.push_back(theirs != nullptr
                                        ? std::to_string(theirs->report().population) + " alive, " +
                                                  std::to_string(theirs->report().births) + " born, " +
                                                  std::to_string(theirs->report().deaths) + " dead"
                                        : std::to_string(site->population) + " people");
                lines.push_back(std::string(cell.river ? "on a river" : "away from the river") +
                                ", ground " + std::to_string(cell.fertility * 100 / 255) + "/100" +
                                ", " + std::to_string(cell.elevation * 10) + " m up");
                lines.push_back(watching != 0 ? "esc  back to our own valley"
                                              : "enter  go and watch them");

                int widest = 0;
                for (const auto& l : lines) widest = std::max(widest, static_cast<int>(l.size()));
                const float boxWidth = widest * 8.0f + 18.0f;
                const float boxHeight = lines.size() * 14.0f + 12.0f;
                const SDL_FRect box{12.0f, camera.viewportHeight - boxHeight - 12.0f, boxWidth,
                                    boxHeight};
                SDL_SetRenderDrawBlendMode(sdlRenderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(sdlRenderer, 22, 20, 16, 225);
                SDL_RenderFillRect(sdlRenderer, &box);
                SDL_SetRenderDrawColor(sdlRenderer, 210, 190, 130, 220);
                SDL_RenderRect(sdlRenderer, &box);
                for (std::size_t i = 0; i < lines.size(); ++i) {
                    SDL_SetRenderDrawColor(sdlRenderer, i == 0 ? 255 : 226, i == 0 ? 240 : 220,
                                           i == 0 ? 180 : 196, 255);
                    SDL_RenderDebugText(sdlRenderer, box.x + 9.0f, box.y + 8.0f + i * 14.0f,
                                        lines[i].c_str());
                }
            }
        }
        SDL_RenderPresent(sdlRenderer);
    }

    renderer.shutdown();
    SDL_DestroyRenderer(sdlRenderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
