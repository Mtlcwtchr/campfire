#pragma once
// The Ground tab: how the control maps' categories paint the ground, edited
// live, and whole sets of those settings kept as presets.
//
// Everything here is a file the renderer already watches - content/config/
// terrain/*.json (engine/biomes, read again within a second; a new soil or
// rock is new shader code, built once), content/config/ground.json (the
// materials' numbers) and content/config/terrain_look.json (how the textures
// are laid). The panel only reads and writes those files, so what it shows is
// what the world draws and what it saves is what the next start draws.
//
// A preset is a copy of all of them under content/presets/terrain/<name>.
// Loading one first keeps what was there as the preset "_before-load", so a
// load is never a loss.
//
// Every choice of a texture, a model or a library entry is made in a window
// beside the panel (asset_browser.hpp) that shows what the build has, with
// pictures; a texture the ground's list does not name yet is added to it
// when chosen.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/ui/ui.hpp"
#include "game/client/asset_browser.hpp"
#include "game/render/terrain_look.hpp"

namespace client {

class TerrainPanel {
public:
    // Lays the panel out from (x, y), `w` wide, inside the caller's scroll
    // region; `y` ends below the last row.
    void draw(ui::Ui& ui, float x, float& y, float w);
    // The choosing window, when one is open, inside `area` (the screen beside
    // the panel). Drawn after the panel, outside its scroll region.
    void drawWindows(ui::Ui& ui, const ui::Rect& area);
    [[nodiscard]] bool browsing() const { return browser_.isOpen(); }
    // The window over everything the build has, to look at (no choice).
    void openCatalogue();
    void closeWindows() { browser_.close(); }
    // What the last action did, for the status line under the panel.
    [[nodiscard]] const std::string& status() const { return status_; }

    // --- presets (also what a test drives) ---------------------------------
    [[nodiscard]] static std::filesystem::path presetsDirectory();
    [[nodiscard]] static std::vector<std::string> presets();
    static bool savePreset(const std::string& name, std::string* why = nullptr);
    static bool loadPreset(const std::string& name, std::string* why = nullptr);
    static bool removePreset(const std::string& name, std::string* why = nullptr);

private:
    // A JSON file read again when it is written by someone else, written
    // whole (to a .partial, then renamed) when changed here.
    struct File {
        std::filesystem::path path;
        nlohmann::ordered_json doc;
        std::filesystem::file_time_type stamp{};
        bool loaded = false;
        nlohmann::ordered_json& data();
        bool save(std::string* why);
    };
    // One entry of a list file ("categories", "soils"...) by name; a file
    // that is a bare list (ground.json) is searched as it is.
    nlohmann::ordered_json* entry(File& file, const char* list, const std::string& name);
    // Writes the file and says what came of it; the terrain library is
    // validated as the renderer will, so a change it will refuse says so here.
    void saved(File& file, const std::string& what, bool validate);
    void editCategory(const std::function<void(nlohmann::ordered_json&)>& edit);
    void editSoil(const std::function<void(nlohmann::ordered_json&)>& edit);
    // An entry of a library file, edited and saved (validated).
    void editEntry(File& file, const char* list, const std::string& name, const std::string& what,
                   const std::function<void(nlohmann::ordered_json&)>& edit);
    // The texture list's name for a texture of the build, adding it to
    // layers.json when it is not there yet. Empty: it could not be added.
    std::string ensureLayer(const AssetItem& texture);
    void browse(AssetBrowser::Request request);

    // A choice in progress: the panel shows only its list until it is made.
    struct Choice {
        std::string title;
        std::vector<std::string> options;
        int current = -1;
        std::function<void(int)> apply;
    };
    void choose(std::string title, std::vector<std::string> options, int current, std::function<void(int)> apply);
    void drawChoice(ui::Ui& ui, float x, float& y, float w);

    void drawPresets(ui::Ui& ui, float x, float& y, float w);
    void drawTextures(ui::Ui& ui, float x, float& y, float w);
    void drawMaterials(ui::Ui& ui, float x, float& y, float w);
    void drawCategory(ui::Ui& ui, float x, float& y, float w);
    void drawSoil(ui::Ui& ui, float x, float& y, float w);
    void drawRock(ui::Ui& ui, float x, float& y, float w);
    void drawDecorations(ui::Ui& ui, float x, float& y, float w);

    std::optional<Choice> choice_;
    AssetCatalog catalog_;
    AssetBrowser browser_;
    File categories_, soils_, ground_, layers_, rocks_, plants_, props_, decals_;
    game::TerrainLook look_;
    bool lookLoaded_ = false;
    std::filesystem::file_time_type lookStamp_{};
    std::string category_ = "fertile";
    std::string soil_;
    std::string rock_;
    AssetKind decorKind_ = AssetKind::Plant;
    std::string decor_;
    int material_ = 0;
    int preset_ = -1;
    std::string presetName_;
    std::vector<std::string> presetList_;
    bool presetsListed_ = false;
    bool confirmLoad_ = false, confirmRemove_ = false;
    std::string status_;
    // Open sections.
    bool showTextures_ = true, showMaterials_ = false, showCategory_ = true, showSoil_ = false;
    bool showRock_ = false, showDecor_ = false;
};

} // namespace client

