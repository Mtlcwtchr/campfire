#pragma once
// The worlds on disk: what the host screen lists, creates and deletes.
//
// A saved world is two things side by side in one directory (worlds/ unless
// told otherwise): its layout, `<name>.json` - the regions and the painted
// layers, what the generator is run with - and its history, the root
// `<name>/` beside it (world_delta.hpp: manifest, delta chunks, caches). The
// layout is the world's identity; the root may not exist yet, for a world
// nobody has changed. A world's name is its file's name, so there is nothing
// to keep in step between a list and the files it lists.
//
// Nothing here generates or loads a world: this is bookkeeping over files, fast
// enough to call every time the screen opens.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "game/generation/world_layout.hpp"

namespace world::saves {

struct SavedWorld {
    std::string name;                     // UTF-8, the layout's file name without .json
    std::filesystem::path layout;         // <directory>/<name>.json
    std::filesystem::path root;           // <directory>/<name>/, its history (may not exist)
    std::int32_t regionsX = 0, regionsY = 0;
    std::int32_t generatedRegions = 0;    // the others are open sea
    std::uint64_t seed = 0;
    bool readable = true;                 // the layout parsed
    bool hasHistory = false;              // anything was ever saved into the root
    std::uintmax_t historyBytes = 0;
    // The later of the layout's and the history's last write.
    std::filesystem::file_time_type modified{};
    double widthKm() const { return double(regionsX) * double(generation::kRegionMetres) / 1000.0; }
    double heightKm() const { return double(regionsY) * double(generation::kRegionMetres) / 1000.0; }
};

// Every world in the directory, the most recently changed first. A layout that
// does not parse is still listed (unreadable), so it can be seen and deleted.
std::vector<SavedWorld> list(const std::filesystem::path& directory);

// A name as a file name: what a file system refuses (separators, the reserved
// punctuation, control characters) dropped, the ends trimmed of spaces and
// dots. Empty when nothing usable is left.
std::string cleanName(const std::string& name);
// The name, or the name with " 2", " 3" ... after it, that no world in the
// directory has yet.
std::string freeName(const std::filesystem::path& directory, const std::string& name);

// What a new world is made from: every region generated with one preset.
struct NewWorld {
    std::string name;
    std::string preset;                   // a WorldPreset's name; the first if unknown
    std::int32_t regions = 1;             // regions a side, when the two below are not given
    std::int32_t regionsX = 0, regionsY = 0;   // regions across and down
    std::uint64_t seed = 1;
    // Nothing generated at all: open sea of that size, ready in a moment, to
    // be filled region by region or by hand. The preset is then not used.
    bool empty = false;
};
// The most regions a side an empty world may have, and a generated one: what
// this machine holds and what it raises in reasonable time.
inline constexpr std::int32_t kMaxEmptyRegions = 32;
inline constexpr std::int32_t kMaxGeneratedRegions = 4;
// The world the game is built for: 6 x 15 regions, 786 x 1966 km - the
// "800 by 2000 kilometres" everything is measured against.
inline constexpr std::int32_t kReferenceRegionsX = 6, kReferenceRegionsY = 15;
// Writes the layout of a new world and returns it as listed; nothing and a
// reason when the name is unusable, taken, or the file cannot be written.
std::optional<SavedWorld> create(const std::filesystem::path& directory, const NewWorld& spec,
                                 const std::vector<generation::WorldPreset>& presets, std::string* error = nullptr);

// Deletes the layout and the history beside it.
bool remove(const SavedWorld& world, std::string* error = nullptr);

// A path from a UTF-8 string and back, the same on every platform.
std::filesystem::path pathOf(const std::string& utf8);
std::string utf8Of(const std::filesystem::path& path);

} // namespace world::saves

