#include "game/client/graphics_panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

namespace client {
namespace {
constexpr float kRow = 26, kLabelW = 150;
}

bool GraphicsPanel::slider(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, float& value,
                           float low, float high, const char* format, bool logarithmic) {
    const auto& theme = ui.theme();
    const auto& input = ui.input();
    const ui::Rect track{x + kLabelW, y + 9, kWide - kLabelW - x - 70, 6};
    const ui::Rect grab{track.x - 6, y, track.w + 12, kRow - 4};
    ui.text(x, y + 6, label, theme.label, 0.9f);
    const auto toT = [&](float v) {
        return logarithmic ? std::log(v / low) / std::log(high / low) : (v - low) / (high - low);
    };
    const auto fromT = [&](float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return logarithmic ? low * std::pow(high / low, t) : low + (high - low) * t;
    };
    bool changed = false;
    if (input.pressed && ui.hovered(grab)) dragging_ = id;
    if (!input.down && dragging_ == id) dragging_ = 0;
    if (dragging_ == id) {
        const float next = fromT((input.mouseX - track.x) / track.w);
        if (next != value) { value = next; changed = true; }
    }
    const float t = std::clamp(toT(value), 0.0f, 1.0f);
    ui.roundRect(track, theme.slot, 3);
    ui.roundRect({track.x, track.y, track.w * t, track.h}, theme.accent.withAlpha(0.85f), 3);
    const ui::Rect knob{track.x + track.w * t - 5, y + 4, 10, kRow - 12};
    ui.roundRect(knob, dragging_ == id ? theme.accent : theme.paper, 3);
    char text[48];
    std::snprintf(text, sizeof(text), format, value);
    ui.textRight(kWide - 10, y + 6, text, theme.accent, 0.9f);
    ui.claim(grab);
    y += kRow;
    return changed;
}

bool GraphicsPanel::toggle(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, bool& value) {
    const bool before = value;
    ui.checkbox(id, {x, y + 2, kWide - x - 12, kRow - 6}, label, value);
    y += kRow;
    return before != value;
}

bool GraphicsPanel::draw(ui::Ui& ui, game::GraphicsSettings& s) {
    const auto& theme = ui.theme();
    const game::GraphicsSettings before = s;
    const float high = float(ui.height());
    ui.panel({0, 0, float(kWide), high});
    ui.text(12, 10, "GRAPHICS", theme.accent, 1.2f);
    ui.textRight(kWide - 12, 12, "O close", theme.labelSoft, 0.8f);
    const char* tabs[] = {"Quality", "Lighting", "Sky & fog", "Scene"};
    for (int i = 0; i < 4; ++i)
        if (ui.tab(ui::widgetId("graphics.tab", i), {10.0f + i * 90.0f, 34, 86, 24}, tabs[i], tab_ == i)) tab_ = i;
    // Everything under the tabs scrolls when the window is shorter than it.
    const ui::Rect frame{0, 64, float(kWide), high - 64 - 44};
    const ui::Rect content = ui.beginScroll(ui::widgetId("graphics.scroll", tab_), frame);
    float y = content.y + 4;
    const float x = 14;
    // Dropdowns draw last so an open list lies over the rows below it.
    struct Choice { std::uint64_t id; ui::Rect rect; std::vector<std::string> options; int* value; };
    std::vector<Choice> choices;
    const auto choose = [&](const char* label, const char* name, std::vector<std::string> options, int& value) {
        ui.text(x, y + 6, label, theme.label, 0.9f);
        choices.push_back({ui::widgetId(name), {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4},
                           std::move(options), &value});
        y += kRow;
    };
    int preset = s.quality;
    switch (tab_) {
    case 0:
        choose("Preset", "graphics.preset", {"Low", "Medium", "High", "Ultra"}, preset);
        {
            std::vector<std::string> names;
            for (const auto& r : game::kResolutions) names.emplace_back(r.name);
            choose("Resolution", "graphics.resolution", std::move(names), s.resolution);
        }
        choose("Anti-aliasing", "graphics.aa", {"Off", "MSAA 4x", "FXAA"}, s.antialiasing);
        choose("Terrain textures", "graphics.textures", {"1024 (half)", "2048 (UE source)"}, s.terrainTextures);
        slider(ui, ui::widgetId("graphics.draw"), x, y, "Draw distance", s.drawDistanceKm, 2, 60, "%.1f km", true);
        slider(ui, ui::widgetId("graphics.blend"), x, y, "Terrain blend", s.terrainBlend, 0.5f, 12, "%.1f m", true);
        toggle(ui, ui::widgetId("graphics.shadows"), x, y, "Shadows (distance field clipmap)", s.shadows);
        slider(ui, ui::widgetId("graphics.soft"), x, y, "Shadow softness", s.shadowSoftness, 0.5f, 3, "%.2f");
        toggle(ui, ui::widgetId("graphics.proxies"), x, y, "Forest proxies (32 m)", s.forestProxies);
        toggle(ui, ui::widgetId("graphics.far"), x, y, "Far forest hierarchy", s.farForest);
        toggle(ui, ui::widgetId("graphics.mass"), x, y, "Mass clusters (128 m merged)", s.massClusters);
        slider(ui, ui::widgetId("graphics.objerr"), x, y, "Object LOD error", s.objectError, 1, 16, "x%.1f", true);
        slider(ui, ui::widgetId("graphics.vegmesh"), x, y, "Tree mesh from", s.vegetationMeshPixels, 20, 8000, "%.0f px", true);
        slider(ui, ui::widgetId("graphics.vegbudget"), x, y, "Tree mesh budget", s.vegetationMeshKiloTriangles, 5, 8000, "%.0fk tris", true);
        toggle(ui, ui::widgetId("graphics.fartrees"), x, y, "Trees to the horizon (GPU)", s.farTrees);
        slider(ui, ui::widgetId("graphics.fartreestart"), x, y, "Placed objects reach", s.farTreesStart, 400, 6000, "%.0f m", true);
        slider(ui, ui::widgetId("graphics.foliagereach"), x, y, "Grass reach", s.foliageDistance, 150, 8000, "%.0f m", true);
        y += 6;
        ui.text(x, y, "LOD allowance x" + [&] { char b[16]; std::snprintf(b, sizeof b, "%.2f", s.lodScale()); return std::string(b); }(),
                theme.labelSoft, 0.85f);
        y += 16;
        ui.text(x, y, "Anti-aliasing rebuilds the renderer.", theme.labelSoft, 0.8f);
        break;
    case 1:
        slider(ui, ui::widgetId("graphics.elev"), x, y, "Sun elevation", s.sunElevation, 2, 89, "%.0f deg");
        slider(ui, ui::widgetId("graphics.azim"), x, y, "Sun azimuth", s.sunAzimuth, 0, 360, "%.0f deg");
        slider(ui, ui::widgetId("graphics.sun"), x, y, "Sun intensity", s.sunIntensity, 0, 3, "%.2f");
        slider(ui, ui::widgetId("graphics.amb"), x, y, "Sky / ambient", s.ambient, 0, 3, "%.2f");
        slider(ui, ui::widgetId("graphics.exp"), x, y, "Exposure", s.exposure, 0.25f, 3, "%.2f", true);
        toggle(ui, ui::widgetId("graphics.grade"), x, y, "Warm grade (bloom, grain)", s.grade);
        slider(ui, ui::widgetId("graphics.gradeamt"), x, y, "Grade strength", s.gradeStrength, 0, 1, "%.2f");
        break;
    case 2:
        toggle(ui, ui::widgetId("graphics.fog"), x, y, "Distance fog", s.fog);
        slider(ui, ui::widgetId("graphics.fogstart"), x, y, "Fog start", s.fogStart, 0, 0.9f, "%.2f");
        slider(ui, ui::widgetId("graphics.draw2"), x, y, "Fog / draw end", s.drawDistanceKm, 2, 60, "%.1f km", true);
        toggle(ui, ui::widgetId("graphics.skybox"), x, y, "Cirrus dome (UE GoodSky)", s.skybox);
        slider(ui, ui::widgetId("graphics.skyrot"), x, y, "Sky rotation", s.skyRotation, 0, 360, "%.0f deg");
        toggle(ui, ui::widgetId("graphics.clouds"), x, y, "Volumetric clouds", s.clouds);
        choose("Cloud quality", "graphics.cloudq", {"Low (12)", "Medium (24)", "High (48)"}, s.cloudQuality);
        slider(ui, ui::widgetId("graphics.cover"), x, y, "Cloud coverage", s.cloudCoverage, 0, 1, "%.2f");
        slider(ui, ui::widgetId("graphics.dens"), x, y, "Cloud density", s.cloudDensity, 0, 4, "%.2f");
        slider(ui, ui::widgetId("graphics.alt"), x, y, "Cloud base", s.cloudAltitude, 300, 6000, "%.0f m", true);
        break;
    case 3: {
        bool view = sceneView;
        if (toggle(ui, ui::widgetId("graphics.sceneview"), x, y, "Scene view: freeze culling (F)", view))
            sceneViewToggled = true;
        const char* lines[] = {
            "Freezes the camera every decision is made from:",
            "culling, LOD, impostors, streaming, placement,",
            "shadows. The live camera becomes a free fly",
            "camera that only draws; the frozen frustum is",
            "drawn in yellow. Nothing is recomputed.",
            "Fly: RMB look, WASD, Q/E, Shift faster.",
            "F again returns to the frozen camera."};
        for (const char* line : lines) { ui.text(x, y + 4, line, theme.labelSoft, 0.8f); y += 15; }
        break;
    }
    }
    ui.endScroll(y + 8);
    const float by = high - 36;
    if (ui.button(ui::widgetId("graphics.save"), {12, by, 110, 24}, "Save")) saveRequested = true;
    if (ui.button(ui::widgetId("graphics.reset"), {130, by, 110, 24}, "Defaults")) resetRequested = true;
    if (!status.empty()) ui.text(250, by + 7, status, theme.labelSoft, 0.8f);
    for (auto& choice : choices) {
        // A box scrolled out of the frame is not there to open.
        if (choice.rect.y < frame.y || choice.rect.bottom() > frame.bottom()) continue;
        const int picked = ui.dropdown(choice.id, choice.rect, choice.options, *choice.value);
        if (picked >= 0) *choice.value = picked;
    }
    if (preset != s.quality) s = game::graphicsPreset(preset, s);
    s.clampAll();
    return !(s == before);
}
} // namespace client

