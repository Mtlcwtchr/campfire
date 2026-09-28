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
    static constexpr int kWide = 380, kHigh = 560;
    // Draws the panel and applies what the pointer did. Returns true when a
    // setting changed this frame.
    bool draw(ui::Ui& ui, game::GraphicsSettings& settings);
    void showTab(int tab) { tab_ = tab < 0 ? 0 : tab > 3 ? 3 : tab; }
    // What the panel asks of the explorer that is not a graphics value.
    bool sceneView = false;       // freeze the cull camera and fly
    bool sceneViewToggled = false; // set by the panel, consumed by the explorer
    bool saveRequested = false, resetRequested = false;
    std::string status;
private:
    bool slider(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, float& value,
                float low, float high, const char* format, bool logarithmic = false);
    bool toggle(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, bool& value);
    int tab_ = 0;
    std::uint64_t dragging_ = 0;
};
} // namespace client

