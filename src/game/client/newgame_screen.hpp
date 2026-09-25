#pragma once
// The screen the game opens on: choose a world, look at it, and start.
//
// GDD 4.2 has the world generated before anybody lives in it, and this is where
// that happens in front of the player. A preset names a set of the generator's
// dials; the dials themselves are here to be turned - how many plates the crust
// is broken into, how long the weather has had at it, how much of it is sea, how
// wet it is - and the map redraws so the choice can be seen rather than guessed.
// The marks on it are the sites a founding community could be put on.
//
// It also lists the saves, because "carry on" and "start again" are the same
// question asked at the same moment.

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "game/generation/world_map_gen.hpp"

namespace client {

class NewGameScreen {
public:
    enum class Outcome { Staying, Start, Load, Quit };

    // The presets and the saves are read once, when the screen opens.
    void open(SDL_Renderer* sdl, const std::filesystem::path& contentDir,
              const std::filesystem::path& saveDir);
    void close();

    // A key or a click. Returns what the player has decided, if anything.
    Outcome handle(const SDL_Event& e);
    void draw(SDL_Renderer* sdl, int viewportWidth, int viewportHeight);

    // Valid once handle() has returned Start: the world the player settled on,
    // already generated, so the game does not build it twice.
    const generation::WorldMapParams& chosen() const { return params_; }
    const generation::WorldMapData& world() const { return preview_; }
    // Valid once handle() has returned Load.
    const std::filesystem::path& chosenSave() const { return saves_[save_].file; }
    bool hasSaves() const { return !saves_.empty(); }

private:
    struct SaveEntry {
        std::filesystem::path file;
        std::string line;
    };

    void regenerate(SDL_Renderer* sdl);
    void applyPreset(std::size_t index);
    void adjust(int direction);

    std::vector<generation::WorldPreset> presets_;
    std::vector<SaveEntry> saves_;
    generation::WorldMapParams params_;
    generation::WorldMapData preview_;
    SDL_Texture* image_ = nullptr;
    std::size_t preset_ = 0;
    std::size_t field_ = 0;
    std::size_t save_ = 0;
    bool dirty_ = true;
    bool onSaves_ = false;
    double builtInSeconds_ = 0.0;
};

} // namespace client
