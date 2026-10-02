#pragma once
// The game client: main menu, the saved worlds, and a world to explore or
// edit (client_ui.hpp is what it looks like; this is what it does).
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>

namespace client {

struct ClientOptions {
    // Where the saved worlds are; empty: the worlds folder beside the game.
    std::filesystem::path worlds;
    // No window: frames go to an offscreen target of this size.
    int headlessWidth = 0, headlessHeight = 0;
    // A scripted walk through every screen, a picture of each written into
    // this folder, then quit: menu, worlds, a new world raised, explored,
    // edited with each tool, paused, settings. Empty: none.
    std::filesystem::path tour;
};

int runClientApp(SDL_Window* window, const std::filesystem::path& assets, const ClientOptions& options);

} // namespace client

