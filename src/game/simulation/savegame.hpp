#pragma once
// Saving and loading, in the game's own binary form.
//
// A save is one file. It opens with a header that says what it is and which
// version wrote it, and then carries the country's generation parameters and
// every community on the map, one after another, each community writing its
// tiles and its entities through their own write().
//
// The version is written and read but not yet branched on. It is there so that
// a later build can tell what it is looking at; while the game is being built,
// an old save that no longer matches is a save you start again from, and saying
// so plainly is better than a compatibility layer nobody has tested.
//
// The country is not stored cell by cell. Four million cells of tectonics is
// thirty megabytes that the generator will reproduce exactly from its seed and
// its parameters, so those are what the file carries; loading regenerates the
// map and checks it came out the same.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/simulation/world.hpp"

namespace sim {

// The whole game: the country, every community living on it, and which one the
// player is looking at. The client owns one of these; so does a headless run
// that simulates the whole map.
struct Game {
    generation::WorldMapParams worldParams;
    // The country, on the heap and staying there. Every community reads it
    // through a pointer (WorldConfig::sharedWorldMap) rather than keeping a copy
    // of four million cells, so its address has to outlive the Game being moved
    // or assigned - held by value it was a dangling pointer the moment newGame
    // returned, and the crash came the first time the camera pulled back far
    // enough to draw the world (D104).
    std::unique_ptr<generation::WorldMapData> country =
            std::make_unique<generation::WorldMapData>();
    std::vector<std::unique_ptr<World>> communities;
    std::size_t watching = 0;
    // The settings the communities were built with, apart from which cell each
    // one lives in.
    WorldConfig base;

    World& active() { return *communities[watching]; }
    const World& active() const { return *communities[watching]; }
    generation::WorldMapData& world() { return *country; }
    const generation::WorldMapData& world() const { return *country; }
};

// Builds a new game: generates the country from the parameters and settles every
// site on it with a founding community.
Game newGame(const content::ContentDb& db, const WorldConfig& base,
             const generation::WorldMapParams& worldParams);

// What a save file says about itself, without loading the whole thing.
struct SaveSummary {
    bool readable = false;
    std::uint32_t formatVersion = 0;
    std::uint64_t seed = 0;
    std::int64_t tick = 0;
    std::int32_t communities = 0;
    std::int32_t population = 0;
    std::string note;          // why it is unreadable, when it is
};

// The format this build writes. Bumped whenever the bytes change meaning.
inline constexpr std::uint32_t kSaveFormatVersion = 3;

bool saveGame(const Game& game, const std::filesystem::path& file, std::string* error = nullptr);
bool loadGame(const content::ContentDb& db, const std::filesystem::path& file, Game& out,
              std::string* error = nullptr);
SaveSummary inspectSave(const std::filesystem::path& file);

} // namespace sim
