#pragma once
// The world editor: the explorer's Edit mode.
//
// Explore looks at a world; Edit makes one. A world is painted in layers, the
// coarsest first.
//
// The REGIONS (generation/world_layout.hpp) are the coarsest: a grid of 131 km
// squares, each generated with a preset, a seed and the dials, or left empty as
// open sea. They are painted by selecting them - the brush is a whole region,
// because a region is this layer's resolution - and generated or cleared.
//
// Under them are the LAYERS (generation/world_layers.hpp): continents, ranges,
// hills, sea share, weathering, rain - each a map at its own resolution, and
// each painted with a brush that is never finer than one of its texels nor
// wider than a fixed number of them (generation/world_brush.hpp). The brushes
// are the manual ones - paint a value, raise, lower, smooth, wipe back to what
// the generator makes - and the procedural ones, which are the generator's own
// passes run over the brush's footprint with dials of their own. Strokes go on
// the ground or on the map of the layer in the panel, one undo step a stroke.
//
// It holds the layout, a selection and the brushes, draws its panel with
// ui::Ui (MenuPass puts it on the screen, like the graphics panel) and says
// when the world wants building; the explorer builds it. It knows nothing about
// rendering except the overlay numbers it hands the terrain and water shaders.
#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/render/frame.hpp"
#include "engine/ui/ui.hpp"
#include "game/generation/world_brush.hpp"
#include "game/generation/world_import.hpp"
#include "game/generation/world_layout.hpp"

namespace client {

class WorldEditor {
public:
    static constexpr int kWide = 384, kHigh = 780;

    // The presets the panel offers, the file Save and Load use, and the world
    // that is showing - described as a layout.
    void open(std::vector<generation::WorldPreset> presets, std::filesystem::path file,
              generation::WorldLayout showing);
    // The world was rebuilt by something else (the world menu): describe it.
    void showing(generation::WorldLayout layout);
    // The world as it came out of the generator, for the map in the panel to
    // be drawn over.
    void builtWorld(const generation::WorldMapData& world);

    bool active() const { return active_; }
    void toggle();
    // The details tool's "remove": the derived instance nearest a point of
    // the ground, asked of whoever holds the world (the client).
    struct PickedObject { std::uint64_t id = 0; double x = 0, y = 0; };
    void objectPicker(std::function<std::optional<PickedObject>(double, double)> picker) { picker_ = std::move(picker); }
    // Paint a layer by its file name ("continents", "rain" ...; "regions" for
    // the region grid). False if there is no such layer.
    bool chooseLayerNamed(const std::string& name);

    // A key while editing. True if the editor took it.
    bool handle(const SDL_Event& event);
    // Where the pointer is on the ground, in world metres, and what it is doing
    // there: painting (selecting regions, or the layer's brush), or erasing.
    // `valid` false when it is off the ground or over the interface.
    void pointer(double worldX, double worldY, bool valid, bool paint, bool erase);

    // The panel. Draws it and applies what was pressed; true when anything
    // changed, so the picture is painted again.
    bool draw(ui::Ui& ui);
    ui::Input& panelInput() { return panelInput_; }
    bool panelDirty() const { return dirty_; }
    void panelPainted() { dirty_ = false; }

    // Set when the world should be (re)built from layout(); the caller builds
    // it and calls built().
    bool wanted() const { return wanted_; }
    const generation::WorldLayout& layout() const { return layout_; }
    // The caller began building layout(): strokes from now on are for the
    // next build, which is wanted again as soon as one is painted.
    void started();
    void built(double seconds);
    // Build after every stroke without being asked (the client, whose builds
    // swap in without the world leaving the screen), and whether the panel
    // offers its own Save and Load (not when something else saves the world).
    void autoBuild(bool on) { autoBuild_ = on; dirty_ = true; }
    void ownFile(bool on) { ownFile_ = on; dirty_ = true; }

    // The numbers the terrain and water shaders draw the overlay from.
    std::array<std::array<float, 4>, engine::kSceneEditorVectors> overlay() const;
    // Changes whenever the coast sketch would draw differently (a stroke on
    // the continents, a stage moved, undo, load): the caller rebuilds the
    // sketch mesh (generation::sketchMesh) when it does.
    std::uint64_t sketchRevision() const { return sketchRevision_; }

    // --- the staged editor, for a client that draws its own panels --------
    // (client_ui.hpp: the Size, Land, Coast & relief, Mountains and Climate &
    // water tabs). The model stays here - the layout, strokes, undo, stages;
    // the client says what the pointer paints and when to build.
    struct LayerStroke {
        generation::BrushTool tool = generation::BrushTool::Paint;
        double radiusMetres = 8000;
        float value = 0;             // what Paint puts down, in the layer's units
        float strength = 0.6f;       // 0..1
        float hardness = 0.4f;       // 0..1
    };
    // The pointer paints this layer with this brush from now on.
    void paintLayer(generation::LayerId id, const LayerStroke& stroke);
    // The pointer paints nothing (it still says where it is).
    void paintNothing();
    // Sketched land worked out (-> Primary); the climate and the water worked
    // out for the land that has its relief (-> Water); the changes built.
    void pin() { pinSketches(); }
    void water() { generateWater(); }
    void build() { endStroke(); wanted_ = true; }
    void undoStroke() { undo(); }
    // Regions added or taken away at each side (generation::reshapeLayout).
    bool reshape(std::int32_t west, std::int32_t east, std::int32_t north, std::int32_t south);
    // How painted land is worked out; rebuilds what is already worked out.
    void setAuthoring(const generation::AuthoringDials& dials);
    // Where the world lies on its planet; rebuilds.
    void setLatitude(double northDegrees, double kmPerDegree);
    generation::Latitude latitudeShown() const { return shownLatitude(); }
    // Regions made by hand at each stage: Sketch, Primary, Relief, Water (and
    // Full, which is not by hand).
    std::array<std::int32_t, 5> stages() const { return stageCounts(); }
    bool unbuilt() const { return unbuilt_; }
    bool building() const { return building_; }
    const std::string& statusLine() const { return status_; }
    bool canUndo() const { return !undo_.empty(); }
    // --- the authored maps (engine/world_source, D158: the Import tab) ----
    // The world's source lives in its history root, `<root>/source`: the
    // height skeleton and control maps imported into it, 256 m a sample,
    // only where there is land. A region that holds height there is built
    // from it (generation/world_import.hpp).
    //
    // The pointer selects regions from now on (a click toggles one, Alt
    // takes it away), and the selection is what an import goes into.
    void selectRegions();
    void selectAll(bool on);
    std::int32_t selectedRegions() const { return selectedCount(); }
    std::int32_t importedRegions() const;
    std::filesystem::path sourceRoot() const;
    struct ImportRequest {
        // Loose pictures (a grey height map, an RGBA control map), or a
        // package directory with its world.json. The pictures win. Either
        // picture may come alone: a control map on its own goes over the
        // heights already imported and leaves them (and the regions' stages)
        // as they are.
        std::filesystem::path height, control, package;
        double lowMetres = -100, highMetres = 3000;   // what black and white stand for
        double seaGrey = -1;                           // the coast's grey, 0..255; negative: found in the picture
        double featherKm = 4;                          // the selection's edge blended over this
    };
    // Into the selected regions - the pictures stretched over the rectangle
    // around them, only the selected squares taking them - or into the whole
    // world when none are selected. Runs on a thread of its own; the regions
    // become imported ones and are built when it is done (update()).
    //
    // An import is heights and masks and nothing more: a region new heights
    // went into is at the Primary stage - the skeleton as drawn, no drainage,
    // no climate, no water - until it is taken further (stageImported).
    void importMaps(const ImportRequest& request);
    // The same maps made by the generator instead of read from pictures
    // (generation/world_procedural.hpp), at any time, a layer at a time, into
    // the selected regions or the whole world when none are selected. Each
    // runs on a thread of its own, like an import, and is built when done.
    struct GenerateRequest {
        generation::RegionSettings settings;   // what a region is generated with
        std::uint64_t seed = 1;
        bool ownSeeds = true;                  // each region its own seed, derived from `seed`
        double featherKm = 4;                  // the selection's edge blended over this
        float variation = 1.0f;                // how far the control maps wander, 0..2
    };
    // Phase 1: heights. The generator over the rectangle around the selection
    // (one planet over the whole world), taken to its primary stage and laid
    // into the source as an imported height map would be: the regions become
    // imported ones, at their heights alone (Primary).
    void generateHeights(const GenerateRequest& request);
    // Phase 2: control maps (moisture, forest, mountains, erosion) worked out
    // from the heights the source holds - generated or imported - under the
    // selection (or everywhere it holds any). Stages stay as they are.
    void generateControls(const GenerateRequest& request);
    // The selected imported regions (or every imported one when none are
    // selected) taken to a stage: Relief is the drainage (the height's
    // hollows breached - baked beside the source first when it is not - the
    // valleys and the climate worked out, no water), Water the rivers and
    // lakes on top, and Primary back to the heights alone.
    void stageImported(generation::RegionStage to);
    // Imported regions at the heights alone, with drainage, with water.
    std::array<std::int32_t, 3> importedStages() const;
    // What the source holds under the selection (or all of it), as a package
    // a person can edit and import again.
    void exportMaps(const std::filesystem::path& directory);
    // The selected regions' imported ground taken away: sea again, or
    // whatever is painted there.
    void clearImport();
    // Called every frame: takes in what a finished import did.
    void update();
    bool importing() const { return job_.valid(); }
    const std::string& importLine() const { return importLine_; }

    // The explorer's own window, or none (a client with its own panels).
    void showWindow(bool on) { window_ = on; dirty_ = true; }
    bool windowShown() const { return active_ && window_; }

    // For the explorer: where the panel sits on the screen.
    static constexpr float kPanelX = 16, kPanelY = 128;

private:
    // --- the panel -------------------------------------------------------
    bool slider(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, float& value,
                float low, float high, const char* format);
    void drawRegions(ui::Ui& ui, float x, float& y, bool& changed);
    void drawLayer(ui::Ui& ui, float x, float& y, bool& changed);
    void drawFile(ui::Ui& ui, float x, float y, bool& changed);
    void status(std::string text) { status_ = std::move(text); dirty_ = true; }

    // --- the regions -----------------------------------------------------
    void applyPreset(int index);
    void select(std::int32_t x, std::int32_t y, bool on);
    void generateSelected();
    void generateWorld();
    generation::RegionSettings panelSettings() const;
    generation::Latitude shownLatitude() const;
    void syncLatitude();
    std::vector<std::pair<std::int32_t, std::int32_t>> selection() const;
    void clearSelected();
    std::int32_t selectedCount() const;
    // The regions' seeds or dials changed, or the world's size: what the
    // generator makes under the layers is something else now.
    void regionsChanged();

    // --- the authoring stages (world_layout.hpp, RegionStage) ---------------
    // Every sketch with paint on it pinned: its primary ground worked out.
    void pinSketches();
    // The water asked for on every pinned region.
    void generateWater();
    // How many regions made by hand are at each stage, Sketch..Water.
    std::array<std::int32_t, 5> stageCounts() const;
    // The regions a dab at (x, y) reaches, remembered for the stroke.
    void touchRegions(double x, double y, double radius);

    // --- the layers ------------------------------------------------------
    // Which layer is being painted: -1 the regions, else a LayerId - or, past
    // them, the terrain categories and the details (biomesMode).
    bool painting() const { return layer_ >= 0 && layer_ < kCategoriesLayer; }
    generation::LayerId layerId() const { return generation::LayerId(layer_); }
    void chooseLayer(int layer);
    // The brush the current layer paints with, from its own settings.
    generation::Brush brush() const;
    generation::BrushTool tool() const;
    const generation::LayerBase& base(generation::LayerId id);
    void beginStroke(bool onMap, double x, double y, bool erase);
    void strokeTo(double x, double y);
    void endStroke();
    void dabAt(double x, double y);
    void undo();
    void clearLayer();
    void readoutAt(double x, double y);

    // --- the map of the layer in the panel --------------------------------
    void drawMap(ui::Ui& ui, const ui::Rect& area);
    void paintMap();
    // A point of the map's rectangle as world metres.
    std::pair<double, double> mapToWorld(const ui::Rect& area, float px, float py) const;

    // --- the terrain categories (engine/biomes) ---------------------------
    // Two more entries at the end of the layer list. Categories: a brush of
    // ids - the ground's category, or a forest, water or decor biome - into
    // the world source, the same chunks an import writes, and an inspector
    // of the chosen category's numbers that saves content/config/terrain
    // and reaches the picture live. Details: the hand edits to what the
    // categories place - density brushed thicker or thinner, an instance
    // pinned, a derived one removed (source/details).
    static constexpr int kCategoriesLayer = int(generation::kLayerCount);
    static constexpr int kDetailsLayer = kCategoriesLayer + 1;
    bool biomesMode() const { return layer_ >= kCategoriesLayer; }
    void drawBiomes(ui::Ui& ui, float x, float& y, bool& changed);
    void drawDetails(ui::Ui& ui, float x, float& y, bool& changed);
    bool inspectCategory(ui::Ui& ui, float x, float& y);
    void biomePointer(double x, double y, bool valid, bool paint, bool erase);
    void applyBiomeStroke();
    void applyDetailStroke();
    int biomeLayer_ = 0;                 // engine::biomes::Layer
    std::array<std::uint32_t, 5> biomeIds_{1, 1, 1, 1, 1};   // the id each layer paints
    static constexpr int kPaintedFeatures = 4;              // the fifth: the environment's painted layer
    float biomeRadiusKm_ = 3;
    int detailTool_ = 0;                 // thicker, thinner, pin, remove
    int detailProp_ = 0;
    float detailRadius_ = 60;
    bool biomeStroke_ = false, biomeErase_ = false;
    double biomeLastX_ = 0, biomeLastY_ = 0;
    std::vector<std::array<double, 3>> biomeDabs_;   // x, y, radius
    std::function<std::optional<PickedObject>(double, double)> picker_;
    ui::Rect biomeLayerBox_{}, biomeIdBox_{}, detailToolBox_{}, detailPropBox_{};

    std::vector<generation::WorldPreset> presets_;
    std::filesystem::path file_;
    generation::WorldLayout layout_;
    std::vector<bool> selected_;
    bool active_ = false;
    bool wanted_ = false;
    bool dirty_ = true;

    // What the next "generate" puts in a region.
    int preset_ = 0;
    std::uint64_t seed_ = 1;
    bool ownSeeds_ = true;          // each region its own seed, derived from seed_
    float sea_ = 71, erosion_ = 3, rain_ = 100;
    // The size the world will be when "resize" is pressed.
    std::int32_t wantX_ = 1, wantY_ = 1;
    float plates_ = 0, blendKm_ = 12;
    // The latitude the world will have when "apply" is pressed.
    float north_ = 50, kmPerDegree_ = 6;

    // The region brush, in regions: nought is the one region under the pointer.
    std::int32_t brush_ = 0;
    double pointerX_ = 0, pointerY_ = 0;
    bool pointerValid_ = false;
    std::int32_t hoverX_ = -1, hoverY_ = -1;
    bool erasing_ = false;

    int layer_ = -1;
    // Each layer remembers its own brush: which of its tools, how many of its
    // texels across, and what Paint puts down.
    struct LayerBrush {
        std::size_t tool = 0;
        std::int32_t texels = 4;
        float value = 0;
    };
    std::array<LayerBrush, generation::kLayerCount> brushes_{};
    // Shared by all of them, in per cent.
    float strength_ = 60, hardness_ = 40;
    // The procedural brushes' dials: a seed, the pass's scale (per tool; nought
    // until first chosen, then the generator's own) and the Hills amplitude.
    std::uint64_t brushSeed_ = 1;
    std::array<float, generation::kBrushToolCount> sizeKm_{};
    float amount_ = 100;

    // A stroke in progress: where its last dab went and how far the pointer
    // has gone since.
    enum class Stroke { None, Ground, Map };
    Stroke stroke_ = Stroke::None;
    bool strokeErase_ = false;
    double lastX_ = 0, lastY_ = 0, carry_ = 0;
    // The regions the stroke in progress has reached, and whether it changed
    // anything: what it asks of the stages when it ends.
    std::vector<std::size_t> strokeRegions_;
    bool strokeChanged_ = false;
    std::uint64_t sketchRevision_ = 1;
    std::vector<std::pair<generation::LayerId, generation::LayerMap>> undo_;
    // What each layer is where nothing was painted, worked out when first
    // wanted and forgotten when the regions change.
    std::array<std::optional<generation::LayerBase>, generation::kLayerCount> bases_;
    bool unbuilt_ = false;          // painted since the world was last built
    bool autoBuild_ = false;        // build after every stroke
    bool building_ = false;         // started() and not yet built()
    bool ownFile_ = true;           // the Save and Load row
    bool window_ = true;            // the panel is drawn (the explorer), or the client draws its own
    std::string readout_;

    // The map: the world as last built, and the layer over it.
    std::vector<std::uint8_t> ground_;     // RGB
    int groundW_ = 0, groundH_ = 0;
    SDL_Renderer* mapRenderer_ = nullptr;
    SDL_Texture* mapTexture_ = nullptr;
    int mapW_ = 0, mapH_ = 0;
    std::vector<std::uint8_t> mapPixels_;
    bool mapStale_ = true;
    bool overMap_ = false;
    double mapPointerX_ = 0, mapPointerY_ = 0;

    // An import, a clear or a drainage bake in flight, and what it said last.
    struct Job {
        bool ok = false;
        std::string line;
        std::shared_ptr<const generation::ImportedSource> opened;
        std::vector<std::pair<std::int32_t, std::int32_t>> regions;   // what it went into
        bool clearing = false;
        bool heights = false;                                          // new heights went in
        std::optional<generation::RegionStage> raiseTo;                // the regions taken this far after
        std::string packageStage;                                      // what the package asked for
    };
    std::future<Job> job_;
    std::string importLine_;
    std::string jobLabel_ = "Importing";
    void reopenSource();
    // The imported regions a stage change is for: the selected ones that
    // hold imported ground, or all of them when none are selected.
    std::vector<std::pair<std::int32_t, std::int32_t>> importTargets() const;
    // Whether any imported region outside `except` is at its drainage or
    // further: an import that changes the height makes its bake stale.
    bool drainedElsewhere(const std::vector<std::pair<std::int32_t, std::int32_t>>& except) const;
    // Regions taken to `to`; imported ones among them wait for the drained
    // height to be baked first when it is not current.
    void raiseRegions(std::vector<std::pair<std::int32_t, std::int32_t>> which, generation::RegionStage to,
                      const std::string& doing);

    ui::Input panelInput_;
    std::uint64_t dragging_ = 0;
    std::string status_;
    // Where the panel's boxes and the map were last drawn, and which of the
    // dropdowns has its list open (0 none, 1 layer, 2 tool, 3 preset) and
    // where: the rows under an open list must not take its clicks.
    ui::Rect toolBox_{}, presetBox_{}, mapRect_{}, openRect_{}, scrollFrame_{};
    int openList_ = 0;
};

} // namespace client

