#pragma once
// The explorer's own world menu.
//
// What it is for: the explorer shows a world, and the first thing anybody wants
// after looking at one is a different one - more plates, less rain, an
// archipelago instead of a continent - without leaving, restarting and losing
// where they were standing. So the dials are here, on the screen, and Enter
// builds what they say.
//
// It draws itself into an SDL_Renderer and knows nothing else about drawing.
// That renderer is a software one over a plain surface (see MenuPass), which is
// what lets this reuse SDL's own debug font and the same look as the other
// screens while the world behind it is drawn on the card. Text on the card
// needs a font, an atlas and a pass of its own; that is worth doing when the
// game moves over, and not worth blocking a menu on.

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/client/graphics_panel.hpp"
#include "game/client/world_editor.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/content/ground_materials.hpp"
#include "game/world/environment.hpp"
#include "game/world/weather.hpp"

namespace client {

class ExploreMenu {
public:
    // The presets come off the same file the new-game screen reads.
    // Finds content/ for itself, walking up from wherever it was started, the
    // same way everything else in this project does.
    // The ground is borrowed from the view, which reads it into the scene every
    // frame - that is what makes a number moved here show in the next frame
    // rather than on the next run.
    void open(const generation::WorldMapParams& showing, std::vector<content::GroundMaterial>* ground,
              std::filesystem::path groundFile);

    // Which of the two things it is showing.
    enum class Page { World, Ground };

    bool visible() const { return open_; }
    void toggle() { open_ = !open_; dirty_ = true; }
    // For a picture of it taken by a script: --explore --menu --at ground.
    void showGround() { page_ = Page::Ground; dirty_ = true; }

    // The file was written by the editor in the other window and the numbers on
    // this page are now the file's. Told rather than found out, because the
    // ground is the view's and the view is what watches the file.
    void groundReread();

    // A key while the menu is up. Returns true if the menu took it, so the
    // camera does not also act on it.
    bool handle(const SDL_Event& event);

    // Set when Enter has been pressed: the caller builds this world and then
    // calls built().
    bool wanted() const { return wanted_; }
    const generation::WorldMapParams& params() const { return params_; }
    void built(double seconds);

    bool selectMap(const std::string& name) {
        for (std::size_t i = 0; i < world::kMapNames.size(); ++i)
            if (name.empty() || name == world::kMapNames[i]) {
                mapView_ = static_cast<world::MapView>(name.empty() ? 0 : i);
                if (mapView_ == world::MapView::Flood) floodRequested_ = true;
                dirty_ = true;
                return true;
            }
        return false;
    }
    world::MapView mapView() const { return mapView_; }
    void setMapView(int map) {
        const auto next = static_cast<world::MapView>(std::clamp(map, 0, int(world::MapView::Count) - 1));
        if (next == mapView_) return;
        mapView_ = next;
        if (mapView_ == world::MapView::Flood) floodRequested_ = true;
        dirty_ = true;
    }
    void setTerrainStage(int stage) {
        if (!stagesAvailable_) return;
        const auto next = generation::TerrainStage(std::clamp(stage, 0, 7));
        if (next == terrainStage_) return;
        terrainStage_ = next;
        dirty_ = true;
    }
    generation::TerrainStage terrainStage() const { return terrainStage_; }
    void viewReadout(const std::string& name) {
        if (viewName_ == name) return;
        viewName_ = name;
        dirty_ = true;
    }
    void stagesAvailable(bool value) {
        if (stagesAvailable_ != value) dirty_ = true;
        stagesAvailable_=value;
        if (!value) terrainStage_=generation::TerrainStage::Final;
    }
    bool inspecting() const {
        return mapView_ == world::MapView::Fertility || mapView_ == world::MapView::Travel ||
               mapView_ == world::MapView::Wind || mapView_ == world::MapView::Flood ||
               mapView_ == world::MapView::Foundation;
    }
    bool showingAnything() const {
        return visible() || mapView_ != world::MapView::Natural || !iceVisible_ || terrainStage_!=generation::TerrainStage::Final;
    }
    bool potentialOnly() const { return potentialOnly_; }
    void iceVisible(bool visible) { iceVisible_ = visible; }
    bool iceVisible() const { return iceVisible_; }
    const world::SoilState& soil() const { return soil_; }
    void applySoilRequest(core::WorldPos centre) {
        if (!soilTreatment_) return;
        soil_.treat(centre, 64, *soilTreatment_, core::Fixed::ratio(1, 3));
        soilTreatment_.reset();
    }
    bool takeFloodRequest() { return std::exchange(floodRequested_, false); }
    void resetEnvironment() {
        soil_.clear();
        soilTreatment_.reset();
        floodRequested_ = false;
        potentialOnly_ = false;
        weatherPreset_ = 0;
        conditions_ = {};
    }
    void configureWeather(const core::TimeConfig& calendar, const std::array<float, 4>& seasons,
                          double day, int preset) {
        calendar_ = calendar;
        seasons_ = seasons;
        weatherDay_ = day;
        weatherPreset_ = preset;
    }
    void advanceWeather(double seconds) {
        if (!weatherPaused_) weatherDay_ += std::max(0.0, seconds) * weatherSpeed_ / 10.0;
    }
    world::weather::Snapshot weatherSnapshot() const {
        return world::weather::snapshot(params_.seed, weatherDay_, calendar_.daysPerSeason,
                                        seasons_, weatherPreset_);
    }
    // The weather window of the graphics panel: off (a still clear day, no
    // rain, no cloud forcing) or a chosen kind in place of the weather of the day.
    bool weatherEnabled() const { return weatherEnabled_; }
    void setWeatherEnabled(bool on) {
        if (weatherEnabled_ == on) return;
        weatherEnabled_ = on;
        dirty_ = true;
    }
    int weatherPreset() const { return weatherPreset_; }
    void setWeatherPreset(int preset) {
        const int next = std::clamp(preset, 0, int(world::weather::kPresets.size()) - 1);
        if (next == weatherPreset_) return;
        weatherPreset_ = next;
        conditions_.rainfall = next == 3 ? core::Fixed::ratio(3, 2)
                               : next == 4 ? core::Fixed::ratio(1, 4) : core::Fixed::ratio(3, 4);
        dirty_ = true;
    }
    double weatherDay() const { return weatherDay_; }
    const world::EnvironmentalConditions& conditions() const { return conditions_; }
    const world::weather::Sample& localWeather() const { return localWeather_; }
    void weatherReadout(const world::weather::Sample& sample) { localWeather_ = sample; }
    void inspectionReadout(std::vector<std::string> lines) { inspectionLines_ = std::move(lines); }

    // Whether the picture of the menu needs drawing again. Cleared by draw().
    bool dirty() const { return dirty_; }
    void draw(SDL_Renderer* into, int width, int height);

    // The draw distance slider on the status strip: how far the perspective
    // view draws, and where the distance fog becomes opaque. Logarithmic, so
    // a pixel of the track is the same fraction of the distance at both ends.
    static constexpr double kMinDrawDistance = 2000, kMaxDrawDistance = 60000;
    double drawDistance() const { return double(graphics_.drawDistanceKm) * 1000.0; }
    void drawDistance(double metres) {
        const double clamped = std::clamp(metres, kMinDrawDistance, kMaxDrawDistance);
        if (clamped != drawDistance()) { dirty_ = true; panelDirty_ = true; }
        graphics_.drawDistanceKm = float(clamped / 1000.0);
    }
    // All graphics options (the draw distance above is one of them), edited
    // by the F2 panel and handed to the renderer every frame.
    game::GraphicsSettings& graphics() { return graphics_; }
    const game::GraphicsSettings& graphics() const { return graphics_; }
    GraphicsPanel& panel() { return panel_; }
    bool panelVisible() const { return panelOpen_; }
    void togglePanel() { panelOpen_ = !panelOpen_; panelDirty_ = true; dirty_ = true; }
    // Explore / Edit: the world editor (` or the Edit button on the strip).
    WorldEditor& editor() { return editor_; }
    const WorldEditor& editor() const { return editor_; }
    void toggleEditor() {
        editor_.toggle();
        // The world menu and the editor would sit on the same corner.
        if (editor_.active()) open_ = false;
        dirty_ = true;
    }
    // The pointer in panel pixels, filled by the explorer each frame; MenuPass
    // repaints the panel when it changed or something else did.
    ui::Input& panelInput() { return panelInput_; }
    bool panelDirty() const { return panelDirty_; }
    void panelPainted() { panelDirty_ = false; }
    void panelChanged() { panelDirty_ = true; dirty_ = true; }
    // The pointer over the menu picture, in its own pixels (the picture's top
    // left is 0,0). True while the slider has it, so the camera does not.
    bool pointer(float x, float y, bool down);
    // Where the slider track is in the picture, for the pointer and a test.
    static constexpr float kTrackLeft = 10, kTrackRight = 450, kTrackY = 94;
    // Toolbar buttons on the status strip (top right of it).
    static constexpr float kButtonY = 6, kButtonW = 108, kGraphicsButtonX = 226, kSceneButtonX = 340;
    static constexpr float kEditButtonX = 340, kEditButtonY = 24;
    static double distanceAt(float x);
    static float trackAt(double metres);

    // How big a picture it wants, in pixels.
    static constexpr int kWide = 460;
    static constexpr int kHigh = 404;
    static constexpr int kStatusHigh = 104;

    // How the explorer's pieces are put on the screen. The explorer keeps the
    // defaults. The game client hides the developer's strip and the terrain
    // progress panel, lays its own whole-window interface under everything
    // (`canvas`), and shows the two windows - graphics, world editor - where
    // its layout wants them, at its interface's scale and in its face.
    struct Presentation {
        bool developer = true;                  // the status strip and the progress panel
        const ui::Canvas* canvas = nullptr;     // under the windows, the size of the view
        float panelScale = 1.0f;                // screen pixels per window pixel
        float graphicsX = -1, graphicsY = -1;   // top left, screen pixels; negative: the explorer's place
        float editorX = -1, editorY = -1;
        // How tall the two windows may be, in window points: less than their
        // full height, and what does not fit scrolls. Negative: all of it.
        float graphicsHigh = -1, editorHigh = -1;
        std::shared_ptr<ui::FontFace> font, bold;
        float fontPoints = 11.0f;
    };
    Presentation& presentation() { return presentation_; }
    const Presentation& presentation() const { return presentation_; }

private:
    void applyPreset(std::size_t index);
    bool handleGround(SDL_Keycode key);
    void drawWorld(SDL_Renderer* into, int width, float& y);
    void drawGround(SDL_Renderer* into, int width, float& y);

    std::vector<generation::WorldPreset> presets_;
    generation::WorldMapParams params_;
    std::size_t preset_ = 0;
    std::size_t field_ = 0;
    Page page_ = Page::World;
    // Borrowed, not owned: the view owns the ground and hands the scene its
    // numbers each frame.
    std::vector<content::GroundMaterial>* ground_ = nullptr;
    std::vector<content::GroundMaterial> asLoaded_;   // what to go back to
    std::filesystem::path groundFile_;
    std::size_t material_ = 0, number_ = 0;
    bool open_ = false;
    bool dirty_ = true;
    bool wanted_ = false;
    world::MapView mapView_ = world::MapView::Natural;
    generation::TerrainStage terrainStage_=generation::TerrainStage::Final;
    bool stagesAvailable_=false;
    bool potentialOnly_ = false;
    bool iceVisible_ = true;
    bool floodRequested_ = false;
    world::SoilState soil_;
    std::optional<world::SoilState::Treatment> soilTreatment_;
    world::EnvironmentalConditions conditions_{};
    core::TimeConfig calendar_{};
    std::array<float, 4> seasons_{18.0f, 32.0f, 21.0f, 9.0f};
    double weatherDay_ = 0;
    int weatherPreset_ = 0;
    bool weatherEnabled_ = true;
    bool weatherPaused_ = false;
    double weatherSpeed_ = 1.0;
    world::weather::Sample localWeather_{};
    std::vector<std::string> inspectionLines_;
    std::string viewName_ = "map / ortho";
    std::string note_;
    game::GraphicsSettings graphics_;
    GraphicsPanel panel_;
    WorldEditor editor_;
    ui::Input panelInput_;
    bool panelOpen_ = false, panelDirty_ = true;
    bool pointerWasDown_ = false;
    bool dragging_ = false;
    Presentation presentation_;
};

} // namespace client
