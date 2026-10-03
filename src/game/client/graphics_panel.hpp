#pragma once
// The graphics settings window of the explorer: tabs, dropdowns, checkboxes and
// sliders over one GraphicsSettings value. Immediate-mode on ui::Ui, drawn by
// MenuPass into a software surface; the explorer forwards the pointer.
#include <cstdint>
#include <string>

#include "engine/ui/ui.hpp"
#include "game/render/graphics_settings.hpp"

namespace client {
class GraphicsPanel {
public:
    static constexpr int kWide = 380, kHigh = 590;
    // Draws the panel and applies what the pointer did. Returns true when a
    // setting changed this frame.
    bool draw(ui::Ui& ui, game::GraphicsSettings& settings);
    void showTab(int tab) { tab_ = tab < 0 ? 0 : tab > 4 ? 4 : tab; }
    // What the panel asks of the explorer that is not a graphics value.
    bool sceneView = false;       // freeze the cull camera and fly
    bool sceneViewToggled = false; // set by the panel, consumed by the explorer
    bool saveRequested = false, resetRequested = false;
    std::string status;
    // The developer's views of the world (the Views tab): which map is on the
    // ground, how the ground's mesh is drawn, which generation stage. The
    // explorer writes the current values in every frame and applies a click
    // (viewsChanged) back to the menu and the renderer.
    struct Views {
        int map = 0;            // world::MapView
        int stage = 7;          // generation::TerrainStage
        int grid = 0;           // 0 off, 1 source step, 2 triangle edges
        bool objectWire = false;
        bool stages = false;    // the world has saved generation stages
        bool weatherOn = true;  // weather at all
        int weather = 0;        // world::weather::kPresets: auto, clear, rain, storm, drought, fog
    } views;
    bool viewsChanged = false;
private:
    bool slider(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, float& value,
                float low, float high, const char* format, bool logarithmic = false);
    bool toggle(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, bool& value);
    int tab_ = 0;
    std::uint64_t dragging_ = 0;
};
} // namespace client

