#pragma once
// The editor: a way to look at the game's own data and to tune how it looks,
// without rebuilding it.
//
// Four tabs, because there are four kinds of question a maker asks while the
// game runs. What does the interface look like - every colour and measure of the
// theme, changed live and written back to content. What is the ground made of -
// the scale, the borders and the tint of every material, written to the file the
// running game reads, so a number moved here moves the ground in a game that is
// already open. What is in the content - the items, buildings, recipes and
// resources as the simulation sees them, which is the fastest way to find out
// that a building nobody builds costs timber nobody has. And what is the game
// doing - the communities on the map, their detail level, their tick and their
// checksum.
//
// It is client-side and it never writes simulation state: looking at a world
// should not change it.

#include <string>
#include <vector>
#include <filesystem>

#include "game/content/ground_materials.hpp"
#include "game/simulation/savegame.hpp"
#include "engine/ui/ui.hpp"

namespace ui {

struct EditorMaterial {
    std::string id;
    std::string group;
    float worldScale = 2.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float saturation = 1.0f;
    float normalStrength = 0.5f;
    float aoStrength = 0.4f;
    float roughnessMultiplier = 1.0f;
    float heightStrength = 0.5f;
    float slopePreference = 0.0f;
    float moistureResponse = 0.5f;
    float snowCompatibility = 0.9f;
};

struct Editor {
    bool open = false;
    enum class Tab : std::uint8_t { Theme, Ground, Content, Data, Game, Count };
    Tab tab = Tab::Theme;
    int contentKind = 0;      // items, buildings, recipes, resources
    int scroll = 0;
    int chosen = -1;
    // The data tab: which file is open and how far down it we are.
    int fileChosen = -1;
    int fileScroll = 0;
    std::vector<std::filesystem::path> files;
    std::vector<std::string> openFileLines;
    std::string message;
    // The ground tab: the table being edited, what it was when it was read, and
    // where it came from. Held here rather than reread each frame because it is
    // being changed - the frame after a button is pressed has to show the change
    // rather than the file.
    std::vector<content::GroundMaterial> ground;
    std::vector<content::GroundMaterial> groundAsRead;
    std::filesystem::path groundFile;
    int groundChosen = 0;
    int groundScroll = 0;
    // The PBR library used by the terrain shader. Textures themselves stay in
    // assets/terrain; this is the editable, human-sized part of the material.
    std::filesystem::path materialFile;
    std::vector<EditorMaterial> materials;
    std::vector<EditorMaterial> materialsAsRead;
    int textureTarget = -1; // albedo, normal, roughness, AO, height, mask
    // A file dropped on the window, waiting to be looked at. The host fills it
    // from the drop event and the editor empties it: which file this is meant
    // for depends on what is in it, and only the editor knows that.
    std::string dropped;
    // Set when the editor wants the content read from disk again. The host does
    // it, because the host owns the database.
    bool wantsReload = false;
};

// Draws the editor and applies whatever is changed. The game is optional: the
// standalone editor has no world, and everything except the Game tab is about
// data and looks rather than about anything that ticks.
void drawEditor(Ui& ui, Editor& editor, const content::ContentDb& db,
                const std::filesystem::path& contentDir, sim::Game* game);

// The theme is content like everything else: written to and read from a file so
// a look can be kept, shared and reverted.
bool saveTheme(const Theme& theme, const std::filesystem::path& file);
bool loadTheme(Theme& theme, const std::filesystem::path& file);

} // namespace ui
