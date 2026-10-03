#pragma once
// The game client's interface: every screen it has, drawn with ui::Ui.
//
// Main menu -> Host (the saved worlds: explore one, edit one, make a new one,
// delete one) -> Loading -> the world, in one of two modes. EXPLORE is the
// world and nothing else: a thin bar along the top, a line of hints that fades.
// EDIT is the world with its tools: a panel of terrain brushes, object brushes
// and the world-shape editor (regions and generator layers, the explorer's
// world editor), a status bar that says what is under the pointer, undo and
// redo. Esc pauses.
//
// It holds what only the interface cares about (which screen, which tab, the
// form being filled in, the tools' settings) and says what was asked for in a
// ClientActions; the application does it. What it shows and cannot know by
// itself - the saved worlds, loading progress, whether there is anything
// unsaved - comes in a ClientView each frame. It is built twice on a frame
// when the picture changed (see ui.hpp: the first pass acts, the second
// paints), so building must not act by itself.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "engine/ui/ui.hpp"
#include "game/client/terrain_panel.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/world_layout.hpp"
#include "game/world/terrain_brush.hpp"
#include "game/world/world_saves.hpp"
#include "game/world/world_tools.hpp"

namespace client {

enum class Screen { Menu, Host, Loading, World };
enum class WorldMode { Explore, Edit };
// The tabs of Edit are the stages a world is made in, in the order it is made
// (doc/authoring_pipeline_2026-09-30.md): its size, all sea; where the land is,
// a flat sketch; the coast torn and the ground raised off it; the mountains
// where plates meet; the climate and the water; then the ground by hand at any
// scale, and what stands on it. Each shows what the ones before made.
// Import is the other way to the same shape: height and control maps made
// elsewhere, put into the regions a person selects (D158) - or made by the
// generator there and then, a layer at a time (D182).
enum class EditTab { Size, Land, Coast, Mountains, Water, Terrain, Objects, Import, Ground };
inline constexpr int kEditTabs = 9;
// The tabs that paint the world's shape (its layers), rather than the ground
// itself or what stands on it.
inline bool shapeTab(EditTab tab) { return tab <= EditTab::Water || tab == EditTab::Import; }
enum class ObjectMode { Remove, Plant };

struct TerrainTool {
    world::BrushKind kind = world::BrushKind::Raise;
    float radius = 40;       // metres
    float strength = 6;      // metres a second, before the falloff
    float softness = 65;     // per cent
    float scale = 60;        // metres: the noise's wavelength, the channel's width
};

struct ObjectTool {
    ObjectMode mode = ObjectMode::Remove;
    std::uint32_t kinds = world::tools::kTrees | world::tools::kBushes;   // what Remove takes
    float radius = 16;       // metres
    std::uint32_t model = 0; // what Plant plants
    float spacing = 10;      // metres between plantings along a drag
};

// The stage tabs' brushes.
struct LandTool {
    float radiusKm = 12;     // the brush, as a radius
    float hardness = 60;     // per cent
};
struct MountainTool {
    float radiusKm = 8;
    float height = 600;      // the uplift a stroke asks for, in the ranges layer's units
    float strength = 70;     // per cent
};
struct ClimateTool {
    float radiusKm = 24;
    float rain = 160;        // per cent of what the climate would give
};

// The Import tab's form: what to import, how to read it, where to export.
struct ImportForm {
    std::string height, control, package, exportTo;   // paths, UTF-8
    float lowMetres = -100, highMetres = 3000;        // black and white of the height map
    // Where the picture's coast is: darker than this grey is sea. Auto finds
    // it in the picture (the top of its dark peak).
    bool autoSea = true;
    float seaGrey = 12;
    float featherKm = 4;                              // the selection's edge blended over this
};

// The Import tab's other way in: the same maps made by the generator instead
// of read from pictures (generation/world_procedural.hpp), a layer at a time -
// heights, then control maps - into the selection or the whole world.
struct GenerateForm {
    int preset = -1;                                  // into presets; -1 until first shown
    std::string seed = "1";                           // a number, or any text (hashed)
    bool ownSeeds = true;                             // each region its seed derived from it
    float seaPercent = 71, erosionPasses = 3, rainPercent = 100;
    float variation = 100;                            // per cent: how far the control maps wander
    float featherKm = 4;                              // the import form's, when asked
};

// What the stage tabs show, from the world editor.
struct ShapeView {
    std::int32_t regionsX = 1, regionsY = 1;
    // Regions made by hand at each stage: sketched, worked out, with mountains, with water.
    std::array<std::int32_t, 4> stages{};
    std::int32_t generatedRegions = 0;   // made from presets, the old way
    bool unbuilt = false, building = false, canUndo = false;
    // Nothing has been dug or planted yet: the world may grow to the west and north.
    bool historyEmpty = true;
    std::string status;
    double northDegrees = 50, kmPerDegree = 6;
    generation::AuthoringDials dials;
    // The Import tab.
    std::int32_t selectedRegions = 0, importedRegions = 0;
    // Imported regions at their heights alone, with drainage, with water.
    std::array<std::int32_t, 3> importedStages{};
    bool importing = false;
    std::string importLine, sourcePath;
};
// What the stage tabs ask of it.
struct ShapeActions {
    std::optional<std::array<std::int32_t, 4>> reshape;   // west, east, north, south
    bool pin = false, water = false, build = false, undo = false;
    std::optional<generation::AuthoringDials> dials;
    std::optional<std::pair<double, double>> latitude;    // north degrees, km per degree
    // The Import tab.
    std::optional<ImportForm> import;                     // import this into the selection
    // Generate instead of importing: heights (phase 1) or control maps over
    // the heights there are (phase 2), into the selection or everywhere.
    std::optional<GenerateForm> generateHeights, generateControls;
    std::optional<std::string> exportTo;                  // export the selection (or all) here
    bool clearImport = false;
    // The selected imported regions (or all of them) taken to a stage:
    // Primary the heights alone, Relief the drainage, Water rivers and lakes.
    std::optional<generation::RegionStage> importStage;
    std::optional<bool> selectAll;                        // everything, or nothing
    // A file dialog for a field of the form: 0 height, 1 control, 2 package, 3 export.
    std::optional<int> browse;
};

// What the interface shows and cannot know itself.
struct ClientView {
    const std::vector<world::saves::SavedWorld>* worlds = nullptr;
    const std::vector<generation::WorldPreset>* presets = nullptr;
    double now = 0;                 // seconds since start, for anything that moves
    // Loading.
    std::string loadingTitle, loadingDetail;
    double loadingSeconds = 0;
    // The world.
    std::string worldName;
    bool unsaved = false, saving = false;
    std::string cameraName;
    std::string undoLabel, redoLabel;
    bool pointerOnGround = false;
    double pointerX = 0, pointerY = 0, pointerHeight = 0;
    bool rebuilding = false;        // the world's shape is being built again
    // The two windows the renderer draws over this, where they are, in points.
    bool settingsOpen = false, shapeEditorOpen = false;
    ui::Rect settingsRect{}, shapeEditorRect{};
    ShapeView shape;
};

// What was asked for this frame.
struct ClientActions {
    bool quit = false;
    bool toMenu = false;            // leave the world (saving) for the main menu
    std::optional<std::size_t> load;          // index into the worlds
    WorldMode loadAs = WorldMode::Explore;
    std::optional<world::saves::NewWorld> create;
    WorldMode createAs = WorldMode::Explore;
    std::optional<std::size_t> remove;
    bool refreshWorlds = false;
    bool toggleSettings = false;
    bool save = false, undo = false, redo = false;
    bool cycleCamera = false;
    std::optional<WorldMode> mode;
    std::optional<EditTab> tab;
    ShapeActions shape;
};

class ClientUi {
public:
    // Builds the screen the interface is on.
    void build(ui::Ui& ui, const ClientView& view, ClientActions& out);
    // A key the interface may want before the world does. True if it took it.
    bool key(SDL_Keycode key, SDL_Keymod mod, ClientActions& out, bool typing);

    Screen screen = Screen::Menu;
    WorldMode mode = WorldMode::Explore;
    EditTab tab = EditTab::Land;
    bool paused = false;
    TerrainTool terrain;
    ObjectTool objects;
    LandTool land;
    MountainTool mountains;
    ClimateTool climate;
    ImportForm importForm;
    GenerateForm generateForm;
    // The Ground tab: the control maps' categories, soils, materials and the
    // texture laying, edited live, with presets (terrain_panel.hpp).
    TerrainPanel ground;

    // Shows a line for a few seconds under the top bar ("Saved", an error).
    void toast(std::string text, double now, bool bad = false);
    // A message in front of everything until dismissed.
    void alert(std::string title, std::string text);
    // The explore hints are shown again, from now.
    void showHints(double now) { hintsFrom_ = now; hintsVisible_ = true; }
    // The host screen's selection, kept when the list is read again.
    void selectWorld(const std::string& name) { selectedName_ = name; }
    // Where the tools panel is, in points, so the world is not painted under it.
    const ui::Rect& toolsPanel() const { return toolsRect_; }

private:
    void menu(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void host(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void hostList(ui::Ui& ui, const ClientView& view, const ui::Rect& area, ClientActions& out);
    void hostDetails(ui::Ui& ui, const ClientView& view, const ui::Rect& area, ClientActions& out);
    void hostCreate(ui::Ui& ui, const ClientView& view, const ui::Rect& area, ClientActions& out);
    void loading(ui::Ui& ui, const ClientView& view);
    void world(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void topBar(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void editPanel(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void terrainTools(ui::Ui& ui, float x, float& y, float w);
    void sizeTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void landTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void coastTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void mountainsTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void waterTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void importTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out);
    void objectTools(ui::Ui& ui, float x, float& y, float w);
    void statusBar(ui::Ui& ui, const ClientView& view);
    void hints(ui::Ui& ui, const ClientView& view);
    void pauseMenu(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void settingsDone(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void confirmDelete(ui::Ui& ui, const ClientView& view, ClientActions& out);
    void alertBox(ui::Ui& ui);
    void toastLine(ui::Ui& ui, const ClientView& view);
    const world::saves::SavedWorld* selected(const ClientView& view, std::size_t* index = nullptr) const;
    void resetForm(const ClientView& view);

    // Host screen.
    std::string selectedName_;
    bool creating_ = false;
    std::string formName_, formSeed_;
    int formPreset_ = 0;
public:
    // For the scripted tour: the new-world form, open, filled in.
    void showCreateForm(bool empty, std::int32_t across, std::int32_t down) {
        creating_ = true;
        formPreset_ = empty ? -1 : 0;
        formRegions_ = across;
        formRegionsDown_ = down;
    }
private:
    std::int32_t formRegions_ = 1;      // across
    std::int32_t formRegionsDown_ = 1;
    std::string formError_;
    bool confirmingDelete_ = false;
    std::optional<std::string> pendingDelete_;
    // World.
    double hintsFrom_ = 0;
    bool hintsVisible_ = true;
    ui::Rect toolsRect_{};
    // What the Coast & relief and Size tabs are set to before they are applied.
    std::optional<generation::AuthoringDials> dials_;
    std::optional<std::pair<float, float>> latitude_;
    // Messages.
    std::string toast_;
    bool toastBad_ = false;
    double toastUntil_ = 0;
    std::string alertTitle_, alertText_;
    double lastNow_ = 0;
};

} // namespace client

