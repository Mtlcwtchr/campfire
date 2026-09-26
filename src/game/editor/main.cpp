// The editor, on its own.
//
// It opens no world and ticks nothing: it is a window onto the data the game is
// made of and onto the look of its interface. Started separately, with its own
// settings file, so that changing a colour or reading a recipe does not mean
// waiting for a continent to be generated first.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include "game/content/content_db.hpp"
#include "game/ui/editor.hpp"
#include "engine/ui/ui.hpp"

namespace {

// Where the editor keeps its own settings. Its own, not the game's: the game's
// config is content the simulation reads, and this is a tool's window size.
struct Settings {
    std::filesystem::path contentDir = "content";
    std::filesystem::path themeFile;
    int width = 1400;
    int height = 900;
    int tab = 0;

    void read(const std::filesystem::path& file) {
        std::ifstream in(file);
        if (!in) return;
        nlohmann::json j;
        try {
            in >> j;
        } catch (const std::exception& e) {
            std::cerr << "editor settings: " << e.what() << "\n";
            return;
        }
        if (j.contains("content")) contentDir = j["content"].get<std::string>();
        if (j.contains("theme")) themeFile = j["theme"].get<std::string>();
        if (j.contains("width")) width = j["width"].get<int>();
        if (j.contains("height")) height = j["height"].get<int>();
        if (j.contains("tab")) tab = j["tab"].get<int>();
    }
    void write(const std::filesystem::path& file) const {
        nlohmann::json j;
        j["content"] = contentDir.generic_string();
        j["theme"] = themeFile.generic_string();
        j["width"] = width;
        j["height"] = height;
        j["tab"] = tab;
        std::ofstream out(file);
        if (out) out << j.dump(2) << "\n";
    }
};

// Content lives beside the source tree, not beside the binary.
std::filesystem::path resolve(const std::filesystem::path& given) {
    if (std::filesystem::exists(given)) return given;
    for (const char* prefix : {"..", "../..", "../../..", "../../../.."}) {
        const std::filesystem::path candidate = std::filesystem::path(prefix) / given;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return given;
}

} // namespace

int main(int argc, char** argv) {
    std::filesystem::path settingsFile = "editor.json";
    std::string shotPath;
    std::string dropped;
    int shotTab = -1;
    int shotKind = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--settings" && i + 1 < argc) settingsFile = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shotPath = argv[++i];
        else if (a == "--tab" && i + 1 < argc) shotTab = std::atoi(argv[++i]);
        else if (a == "--kind" && i + 1 < argc) shotKind = std::atoi(argv[++i]);
        // The same thing a drag from the desktop does, from a script: dropping
        // a file is a gesture, and a gesture cannot be checked by a test.
        else if (a == "--drop" && i + 1 < argc) dropped = argv[++i];
        // --content is read in a second pass below, after the settings file has
        // been read: what is given on the command line has to win over what the
        // file says, and the file is not read yet.
        else if (a == "--content" && i + 1 < argc) ++i;
        else if (a == "--help") {
            std::cout << "usage: campfire_editor [--settings FILE] [--content DIR]\n"
                      << "       campfire_editor --shot FILE [--tab N] [--kind N] [--drop FILE]\n"
                      << "  Opens the data and interface editor. No world is generated.\n";
            return 0;
        }
    }
    Settings settings;
    settings.read(resolve(settingsFile));
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--content" && i + 1 < argc) settings.contentDir = argv[i + 1];
    const std::filesystem::path contentDir = resolve(settings.contentDir);
    if (settings.themeFile.empty()) settings.themeFile = contentDir / "config" / "ui_theme.json";

    content::ContentDb db;
    if (!db.load(contentDir)) {
        std::cerr << "content failed to load from " << contentDir << ":\n";
        for (const auto& e : db.errors()) std::cerr << "  " << e << "\n";
        // Not fatal: showing a maker why their data is broken is the whole point
        // of the thing, so it opens anyway and says so.
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }
    SDL_Window* window = nullptr;
    SDL_Renderer* sdl = nullptr;
    if (!SDL_CreateWindowAndRenderer("Campfire - editor", settings.width, settings.height,
                                     SDL_WINDOW_RESIZABLE, &window, &sdl)) {
        std::cerr << "window creation failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    ui::Ui gui;
    if (!gui.init(sdl)) {
        std::cerr << "the interface could not build its font\n";
        return 1;
    }
    ui::loadTheme(gui.theme(), settings.themeFile);

    ui::Editor editor;
    editor.open = true;
    constexpr int kLastTab = static_cast<int>(ui::Editor::Tab::Count) - 1;
    editor.tab = static_cast<ui::Editor::Tab>(std::clamp(settings.tab, 0, kLastTab));

    ui::Input pointer;
    if (shotTab >= 0) editor.tab = static_cast<ui::Editor::Tab>(std::clamp(shotTab, 0, kLastTab));
    if (shotKind >= 0) editor.contentKind = std::clamp(shotKind, 0, 4);
    if (!dropped.empty()) editor.dropped = dropped;
    if (!shotPath.empty()) {
        ui::Input still;
        still.mouseX = -1000;
        still.mouseY = -1000;
        SDL_SetRenderDrawColor(sdl, 18, 18, 20, 255);
        SDL_RenderClear(sdl);
        gui.begin(still, settings.width, settings.height);
        ui::drawEditor(gui, editor, db, contentDir, nullptr);
        gui.end();
        SDL_Surface* shot = SDL_RenderReadPixels(sdl, nullptr);
        if (!shot || !IMG_SavePNG(shot, shotPath.c_str())) {
            std::cerr << "could not write " << shotPath << ": " << SDL_GetError() << "\n";
            return 1;
        }
        SDL_DestroySurface(shot);
        std::cout << "wrote " << shotPath << "\n";
        return 0;
    }
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
                case SDL_EVENT_QUIT: running = false; break;
                case SDL_EVENT_KEY_DOWN:
                    if (e.key.key == SDLK_ESCAPE) running = false;
                    if (e.key.key == SDLK_F5) editor.wantsReload = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        pointer.pressed = true;
                        pointer.down = true;
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (e.button.button == SDL_BUTTON_LEFT) {
                        pointer.released = true;
                        pointer.down = false;
                    }
                    break;
                case SDL_EVENT_MOUSE_WHEEL: pointer.wheel += e.wheel.y; break;
                // A file dragged onto the window from the desktop. Kept as a
                // path for the editor to look at on the next frame rather than
                // acted on here: what a file is for depends on what is in it,
                // and this loop knows nothing about that.
                case SDL_EVENT_DROP_FILE:
                    if (e.drop.data != nullptr) editor.dropped = e.drop.data;
                    break;
                default: break;
            }
        }
        float mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        pointer.mouseX = mx;
        pointer.mouseY = my;

        if (editor.wantsReload) {
            editor.wantsReload = false;
            content::ContentDb fresh;
            if (fresh.load(contentDir)) {
                db = std::move(fresh);
                editor.message = "content read again";
            } else {
                db = std::move(fresh);
                editor.message = "content read again, with errors";
            }
            editor.chosen = -1;
        }

        int width = settings.width, height = settings.height;
        SDL_GetRenderOutputSize(sdl, &width, &height);
        SDL_SetRenderDrawColor(sdl, 18, 18, 20, 255);
        SDL_RenderClear(sdl);
        gui.begin(pointer, width, height);
        // The editor fills the window here rather than floating over a game.
        ui::drawEditor(gui, editor, db, contentDir, nullptr);
        gui.end();
        SDL_RenderPresent(sdl);

        pointer.pressed = false;
        pointer.released = false;
        pointer.wheel = 0;
        if (!editor.open) running = false;
        SDL_Delay(16);
    }

    SDL_GetWindowSize(window, &settings.width, &settings.height);
    settings.tab = static_cast<int>(editor.tab);
    settings.write(resolve(settingsFile));

    gui.shutdown();
    SDL_DestroyRenderer(sdl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
