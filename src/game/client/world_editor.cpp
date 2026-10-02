#include "game/client/world_editor.hpp"

#include "engine/biomes/registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <initializer_list>
#include <iostream>

#include "engine/core/progress.hpp"
#include "engine/core/rng.hpp"
#include "engine/world_source/transfer.hpp"

namespace client {
namespace {
constexpr float kRow = 26, kLabelW = 132;
constexpr std::size_t kUndoSteps = 40;

std::string number(const char* format, double value) {
    char text[64];
    std::snprintf(text, sizeof(text), format, value);
    return text;
}

std::string distance(double metres) {
    if (metres >= 10000.0) return number("%.0f km", metres / 1000.0);
    if (metres >= 1000.0) return number("%.1f km", metres / 1000.0);
    return number("%.0f m", metres);
}

void section(ui::Ui& ui, float x, float& y, const char* title) {
    const auto& theme = ui.theme();
    y += 4;
    ui.text(x, y, title, theme.accent, 0.95f);
    ui.line(x, y + 14, WorldEditor::kWide - 14, y + 14, theme.labelSoft.withAlpha(0.4f));
    y += 20;
}

// A number with a minus and a plus beside it, and what it means after them.
bool stepper(ui::Ui& ui, float x, float& y, const char* id, const char* label, std::int32_t& value,
             std::int32_t low, std::int32_t high, const std::string& after = {}) {
    const auto& theme = ui.theme();
    bool changed = false;
    ui.text(x, y + 6, label, theme.label, 0.9f);
    if (ui.button(ui::widgetId(id, 0), {x + kLabelW, y + 1, 26, kRow - 4}, "-", value > low)) {
        value = std::max(low, value - 1);
        changed = true;
    }
    ui.textCentred({x + kLabelW + 28, y + 1, 40, kRow - 4}, std::to_string(value), theme.accent, 0.95f);
    if (ui.button(ui::widgetId(id, 1), {x + kLabelW + 70, y + 1, 26, kRow - 4}, "+", value < high)) {
        value = std::min(high, value + 1);
        changed = true;
    }
    if (!after.empty()) ui.text(x + kLabelW + 104, y + 6, after, theme.labelSoft, 0.8f);
    y += kRow;
    return changed;
}

struct Stop {
    float at;
    float r, g, b;
};
std::array<float, 3> ramp(std::initializer_list<Stop> stops, float t) {
    const Stop* previous = stops.begin();
    for (const Stop& s : stops) {
        if (t <= s.at) {
            if (&s == previous) return {s.r, s.g, s.b};
            const float k = (t - previous->at) / std::max(1e-6f, s.at - previous->at);
            return {previous->r + (s.r - previous->r) * k, previous->g + (s.g - previous->g) * k,
                    previous->b + (s.b - previous->b) * k};
        }
        previous = &s;
    }
    return {previous->r, previous->g, previous->b};
}

// The colour a layer's value is drawn in, on the map in the panel.
std::array<float, 3> layerColour(generation::LayerId id, float value) {
    const generation::LayerDef& def = generation::layerDef(id);
    const float t = std::clamp((value - def.low) / (def.high - def.low), 0.0f, 1.0f);
    switch (id) {
        case generation::LayerId::Continents:
            return ramp({{0.0f, 18, 38, 92}, {0.46f, 64, 120, 186}, {0.52f, 196, 186, 120},
                         {0.62f, 104, 150, 72}, {0.8f, 140, 110, 70}, {1.0f, 240, 240, 236}}, t);
        case generation::LayerId::Hills:
            return ramp({{0.0f, 60, 72, 96}, {0.5f, 128, 128, 112}, {1.0f, 206, 150, 88}}, t);
        case generation::LayerId::Sea:
            return ramp({{0.0f, 214, 196, 140}, {1.0f, 30, 70, 160}}, t);
        case generation::LayerId::Weathering:
            return ramp({{0.0f, 236, 226, 204}, {1.0f, 104, 64, 36}}, t);
        case generation::LayerId::Rain:
            return ramp({{0.0f, 214, 180, 84}, {0.4f, 120, 170, 80}, {1.0f, 50, 100, 200}}, t);
        case generation::LayerId::Ranges:
            return value >= 0 ? std::array<float, 3>{236, 132, 48} : std::array<float, 3>{128, 90, 210};
        case generation::LayerId::Water:
            return ramp({{0.0f, 214, 180, 84}, {0.5f, 70, 140, 220}, {1.0f, 20, 60, 170}}, t);
        case generation::LayerId::Count:
            break;
    }
    return {128, 128, 128};
}

std::string layerName(int layer) {
    if (layer < 0) return "Regions  " + distance(double(generation::kRegionMetres));
    if (layer == int(generation::kLayerCount)) return "Terrain categories  256 m";
    if (layer == int(generation::kLayerCount) + 1) return "Details";
    const auto& def = generation::layerDef(generation::LayerId(layer));
    return std::string(def.label) + "  " + distance(def.texelMetres);
}

ui::Rect listOf(const ui::Rect& box, std::size_t options) {
    return {box.x, box.bottom() + 2, box.w, box.h * float(options)};
}
} // namespace

void WorldEditor::open(std::vector<generation::WorldPreset> presets, std::filesystem::path file,
                       generation::WorldLayout showing) {
    presets_ = std::move(presets);
    file_ = std::move(file);
    layout_ = std::move(showing);
    selected_.assign(layout_.regions.size(), false);
    wantX_ = layout_.regionsX;
    wantY_ = layout_.regionsY;
    plates_ = float(layout_.plates);
    blendKm_ = float(layout_.blendMetres) / 1000.0f;
    syncLatitude();
    seed_ = layout_.seed;
    brushSeed_ = layout_.seed;
    preset_ = 0;
    if (!layout_.regions.empty()) {
        const auto& first = layout_.regions.front().settings;
        for (std::size_t i = 0; i < presets_.size(); ++i)
            if (presets_[i].name == first.preset) preset_ = int(i);
        sea_ = float(first.seaPercent);
        erosion_ = float(first.erosionPasses);
        rain_ = float(first.rainfallPercent);
    }
    for (std::size_t i = 0; i < generation::kLayerCount; ++i) {
        brushes_[i] = LayerBrush{};
        brushes_[i].value = generation::layerDef(generation::LayerId(i)).paint;
    }
    undo_.clear();
    unbuilt_ = false;
    ++sketchRevision_;
    if (!layout_.imported) reopenSource();
    regionsChanged();
}

void WorldEditor::showing(generation::WorldLayout layout) {
    endStroke();
    layout_ = std::move(layout);
    selected_.assign(layout_.regions.size(), false);
    wantX_ = layout_.regionsX;
    wantY_ = layout_.regionsY;
    plates_ = float(layout_.plates);
    blendKm_ = float(layout_.blendMetres) / 1000.0f;
    syncLatitude();
    hoverX_ = hoverY_ = -1;
    undo_.clear();
    unbuilt_ = false;
    ++sketchRevision_;
    regionsChanged();
}

void WorldEditor::builtWorld(const generation::WorldMapData& world) {
    // A picture of the country as it came out, at most 512 across: land by
    // height, the sea, the rivers. The layer is drawn over it, so a person
    // painting sees where they are painting.
    if (world.width <= 0 || world.height <= 0) return;
    const int longest = std::max(world.width, world.height);
    const int step = std::max(1, (longest + 511) / 512);
    groundW_ = std::max(1, world.width / step);
    groundH_ = std::max(1, world.height / step);
    ground_.assign(std::size_t(groundW_) * std::size_t(groundH_) * 3, 0);
    for (int gy = 0; gy < groundH_; ++gy)
        for (int gx = 0; gx < groundW_; ++gx) {
            const core::TilePos p{std::min(world.width - 1, gx * step + step / 2),
                                  std::min(world.height - 1, gy * step + step / 2)};
            const generation::WorldCell& c = world.at(p);
            std::array<float, 3> rgb{};
            if (c.sea) rgb = {24, 46, 82};
            else if (c.river) rgb = {70, 118, 196};
            else rgb = ramp({{0.0f, 92, 124, 70}, {0.35f, 140, 138, 88}, {0.7f, 150, 120, 90},
                             {1.0f, 236, 234, 228}}, float(c.elevation) / 255.0f);
            std::uint8_t* out = &ground_[(std::size_t(gy) * std::size_t(groundW_) + std::size_t(gx)) * 3];
            for (int k = 0; k < 3; ++k) out[k] = std::uint8_t(std::clamp(rgb[std::size_t(k)], 0.0f, 255.0f));
        }
    mapStale_ = true;
    dirty_ = true;
}

void WorldEditor::toggle() {
    endStroke();
    active_ = !active_;
    pointerValid_ = false;
    hoverX_ = hoverY_ = -1;
    dirty_ = true;
}

void WorldEditor::paintLayer(generation::LayerId id, const LayerStroke& stroke) {
    if (layer_ != int(id)) chooseLayer(int(id));
    LayerBrush& b = brushes_[std::size_t(id)];
    const auto tools = generation::toolsFor(id);
    const auto found = std::find(tools.begin(), tools.end(), stroke.tool);
    b.tool = found == tools.end() ? 0 : std::size_t(found - tools.begin());
    const auto limits = generation::brushLimits(id, layout_);
    const auto widest = std::max(1, std::int32_t(std::lround(limits.maxRadius / limits.texel)));
    b.texels = std::clamp(std::int32_t(std::lround(stroke.radiusMetres / limits.texel)), 1, widest);
    b.value = stroke.value;
    strength_ = std::clamp(stroke.strength, 0.0f, 1.0f) * 100.0f;
    hardness_ = std::clamp(stroke.hardness, 0.0f, 1.0f) * 100.0f;
}

void WorldEditor::paintNothing() {
    if (layer_ >= 0) chooseLayer(-1);
    // Regions are not selected by pointing either: the stages paint land, and
    // the regions it touches join the world by themselves.
    brush_ = -1;
}

bool WorldEditor::reshape(std::int32_t west, std::int32_t east, std::int32_t north, std::int32_t south) {
    endStroke();
    if (importing()) { status("wait for the import to finish"); return false; }
    if (!generation::reshapeLayout(layout_, west, east, north, south)) {
        status("a world is 1 to " + std::to_string(generation::kMaxRegionsPerSide) + " regions a side");
        return false;
    }
    // The imported ground moves with the regions it is in: a region is four
    // source chunks a side.
    if (engine::world_source::WorldSource::exists(sourceRoot())) {
        constexpr std::int64_t kChunksPerRegion = generation::kRegionMetres / 32768;
        const engine::world_source::WorldExtent extent{double(layout_.widthMetres()), double(layout_.heightMetres()), 256,
                                                       32768};
        std::string why;
        if (!engine::world_source::reshapeSource(sourceRoot(), west * kChunksPerRegion, north * kChunksPerRegion, extent,
                                                 &why))
            std::cerr << "world source: " << why << "\n";
        reopenSource();
    }
    selected_.assign(layout_.regions.size(), false);
    wantX_ = layout_.regionsX;
    wantY_ = layout_.regionsY;
    syncLatitude();
    undo_.clear();
    ++sketchRevision_;
    regionsChanged();
    wanted_ = true;
    status(std::to_string(layout_.regionsX) + " x " + std::to_string(layout_.regionsY) + " regions");
    return true;
}

void WorldEditor::setAuthoring(const generation::AuthoringDials& dials) {
    if (layout_.authoring == dials) return;
    layout_.authoring = dials;
    // Worked-out land made by hand is made again with the new dials; a
    // sketch is not worked out, and nothing else reads them.
    const auto counts = stageCounts();
    unbuilt_ = true;
    if (counts[1] + counts[2] + counts[3] > 0) {
        wanted_ = true;
        status("working the land out again ...");
    }
}

void WorldEditor::setLatitude(double northDegrees, double kmPerDegree) {
    generation::Latitude next = layout_.latitude;
    next.fixed = true;
    next.northDegrees = std::clamp(northDegrees, -90.0, 90.0);
    next.kmPerDegree = std::clamp(kmPerDegree, 0.5, 200.0);
    next.legacyFrom = next.legacySpan = next.legacyRows = 0;
    if (next == layout_.latitude) return;
    layout_.latitude = next;
    syncLatitude();
    unbuilt_ = true;
    wanted_ = true;
    status("the climate follows the new latitude ...");
}

void WorldEditor::regionsChanged() {
    for (auto& b : bases_) b.reset();
    mapStale_ = true;
    dirty_ = true;
}

void WorldEditor::applyPreset(int index) {
    if (presets_.empty()) return;
    preset_ = std::clamp(index, 0, int(presets_.size()) - 1);
    const auto& p = presets_[std::size_t(preset_)].params;
    sea_ = float(std::clamp(p.seaPercent, 0, 100));
    erosion_ = float(std::clamp(p.erosionPasses, 0, 24));
    rain_ = float(std::clamp(p.rainfallPercent, 20, 250));
    dirty_ = true;
}

bool WorldEditor::handle(const SDL_Event& event) {
    if (!active_ || event.type != SDL_EVENT_KEY_DOWN) return false;
    const SDL_Keycode key = event.key.key;
    const bool command = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET) {
        const bool up = key == SDLK_RIGHTBRACKET;
        if (biomesMode()) {
            float& r = layer_ == kCategoriesLayer ? biomeRadiusKm_ : detailRadius_;
            const float low = layer_ == kCategoriesLayer ? 0.25f : 8.0f, high = layer_ == kCategoriesLayer ? 60.0f : 2000.0f;
            r = std::clamp(up ? r * 1.25f : r / 1.25f, low, high);
        } else if (painting()) {
            // A quarter at a time, and never less than a texel: the brush is
            // sized in the layer's own texels.
            LayerBrush& b = brushes_[std::size_t(layer_)];
            const auto limits = generation::brushLimits(layerId(), layout_);
            const auto widest = std::max(1, std::int32_t(std::lround(limits.maxRadius / limits.texel)));
            const auto next = up ? std::max(b.texels + 1, std::int32_t(std::lround(b.texels * 1.25)))
                                 : std::min(b.texels - 1, std::int32_t(std::lround(b.texels / 1.25)));
            b.texels = std::clamp(next, 1, widest);
        } else {
            brush_ = std::clamp(brush_ + (up ? 1 : -1), 0, std::max(layout_.regionsX, layout_.regionsY));
        }
        dirty_ = true;
        return true;
    }
    if (event.key.repeat) return false;
    if (command && key == SDLK_Z) { undo(); return true; }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        if (painting()) {
            endStroke();
            wanted_ = true;
            status("building ...");
        } else {
            generateSelected();
        }
        return true;
    }
    if (command && key == SDLK_S) {
        status(generation::saveWorldLayout(layout_, file_) ? "saved " + file_.filename().string()
                                                           : "could not save " + file_.string());
        return true;
    }
    if (painting()) return false;
    if (key == SDLK_DELETE || key == SDLK_BACKSPACE) {
        std::fill(selected_.begin(), selected_.end(), false);
        dirty_ = true;
        return true;
    }
    if (command && key == SDLK_A) {
        std::fill(selected_.begin(), selected_.end(), true);
        dirty_ = true;
        return true;
    }
    return false;
}

void WorldEditor::select(std::int32_t x, std::int32_t y, bool on) {
    if (!layout_.inBounds(x, y)) return;
    const std::size_t i = layout_.indexOf(x, y);
    if (selected_[i] != on) { selected_[i] = on; dirty_ = true; }
}

void WorldEditor::pointer(double worldX, double worldY, bool valid, bool paint, bool erase) {
    const double r = double(generation::kRegionMetres);
    const std::int32_t hx = valid ? std::int32_t(std::floor(worldX / r)) : -1;
    const std::int32_t hy = valid ? std::int32_t(std::floor(worldY / r)) : -1;
    const bool inside = layout_.inBounds(hx, hy);
    if ((inside ? hx : -1) != hoverX_ || (inside ? hy : -1) != hoverY_) {
        hoverX_ = inside ? hx : -1;
        hoverY_ = inside ? hy : -1;
        dirty_ = true;
    }
    // The map in the panel draws the brush where the pointer is, so it is
    // drawn again when the pointer has moved by a pixel of it.
    const double mapPixel = double(layout_.widthMetres()) / std::max(1, mapW_);
    if (painting() && valid != pointerValid_) dirty_ = true;
    if (painting() && valid && std::hypot(worldX - pointerX_, worldY - pointerY_) > mapPixel) dirty_ = true;
    pointerValid_ = valid;
    pointerX_ = worldX;
    pointerY_ = worldY;
    erasing_ = erase;
    if (biomesMode()) {
        biomePointer(worldX, worldY, valid, paint, erase);
        return;
    }

    if (painting()) {
        if (stroke_ == Stroke::Ground && (!valid || !(paint || erase))) endStroke();
        if (valid && (paint || erase)) {
            if (stroke_ == Stroke::None) beginStroke(false, worldX, worldY, erase);
            else if (stroke_ == Stroke::Ground) strokeTo(worldX, worldY);
        }
        if (valid) readoutAt(worldX, worldY);
        return;
    }

    if (!valid || !(paint || erase)) return;
    // The brush is round and measured in regions; nought is the one under the
    // pointer. A region is in it when its centre is.
    if (brush_ == 0) { select(hx, hy, !erase); return; }
    const double reach = double(brush_) * r;
    for (std::int32_t y = 0; y < layout_.regionsY; ++y)
        for (std::int32_t x = 0; x < layout_.regionsX; ++x) {
            const double cx = (x + 0.5) * r, cy = (y + 0.5) * r;
            if (std::hypot(cx - worldX, cy - worldY) <= reach) select(x, y, !erase);
        }
}

std::int32_t WorldEditor::selectedCount() const {
    return std::int32_t(std::count(selected_.begin(), selected_.end(), true));
}

generation::Latitude WorldEditor::shownLatitude() const {
    // A world under the old rule shows the strip that rule gives it now.
    return layout_.latitude.fixed ? layout_.latitude
                                  : generation::legacyLatitude(layout_.widthCells(), layout_.heightCells(), layout_.seed);
}

void WorldEditor::syncLatitude() {
    const auto shown = shownLatitude();
    north_ = float(shown.northDegrees);
    kmPerDegree_ = float(shown.kmPerDegree);
}

generation::RegionSettings WorldEditor::panelSettings() const {
    generation::RegionSettings settings;
    settings.preset = presets_.empty() ? std::string{} : presets_[std::size_t(preset_)].name;
    settings.seed = seed_;
    settings.seaPercent = std::clamp(int(std::lround(sea_)), 0, 100);
    settings.erosionPasses = std::clamp(int(std::lround(erosion_)), 0, 24);
    settings.rainfallPercent = std::clamp(int(std::lround(rain_)), 20, 250);
    return settings;
}

std::vector<std::pair<std::int32_t, std::int32_t>> WorldEditor::selection() const {
    std::vector<std::pair<std::int32_t, std::int32_t>> which;
    for (std::int32_t y = 0; y < layout_.regionsY; ++y)
        for (std::int32_t x = 0; x < layout_.regionsX; ++x)
            if (selected_[layout_.indexOf(x, y)]) which.emplace_back(x, y);
    return which;
}

void WorldEditor::generateSelected() {
    if (selectedCount() == 0) { status("select regions first (LMB on the map)"); return; }
    // A generation of their own, the size of the rectangle around them: the
    // rest of the world is not run again and does not change.
    generation::generateRegions(layout_, selection(), panelSettings(), seed_, ownSeeds_);
    wanted_ = true;
    regionsChanged();
    status("building " + std::to_string(selectedCount()) + " region(s) as a world of their own ...");
}

void WorldEditor::generateWorld() {
    // The one option that makes everything again: every region, one planet.
    generation::generateWholeWorld(layout_, panelSettings(), seed_, ownSeeds_);
    wanted_ = true;
    regionsChanged();
    status("building the whole world as one ...");
}

void WorldEditor::clearSelected() {
    if (selectedCount() == 0) { status("select regions first (LMB on the map)"); return; }
    generation::clearRegions(layout_, selection());
    wanted_ = true;
    regionsChanged();
    status("clearing " + std::to_string(selectedCount()) + " region(s) ...");
}

void WorldEditor::started() {
    // What is being built is the layout as it stands now; anything painted
    // from here on is for the build after this one.
    wanted_ = false;
    unbuilt_ = false;
    building_ = true;
}

void WorldEditor::built(double seconds) {
    // Without started() (the explorer builds at once, on its own thread of
    // thought): what was wanted is what was built.
    if (!building_) {
        wanted_ = false;
        if (seconds >= 0) unbuilt_ = false;
    }
    building_ = false;
    if (seconds < 0) unbuilt_ = true;
    status(seconds < 0 ? std::string("build failed - see the console")
           : unbuilt_ ? number("built in %.1f s; building what was painted since ...", seconds)
                      : number("built in %.1f s", seconds));
}

// --- the layers --------------------------------------------------------------

generation::BrushTool WorldEditor::tool() const {
    const auto tools = generation::toolsFor(layerId());
    return tools[std::min(brushes_[std::size_t(layer_)].tool, tools.size() - 1)];
}

generation::Brush WorldEditor::brush() const {
    const generation::LayerId id = layerId();
    const LayerBrush& b = brushes_[std::size_t(layer_)];
    generation::Brush out;
    out.tool = tool();
    out.radius = generation::legalRadius(id, layout_, double(b.texels) * generation::layerDef(id).texelMetres);
    out.strength = strength_ / 100.0f;
    out.hardness = hardness_ / 100.0f;
    out.value = b.value;
    out.seed = brushSeed_;
    out.sizeKm = sizeKm_[std::size_t(out.tool)];
    out.amount = amount_ / 100.0f;
    return out;
}

const generation::LayerBase& WorldEditor::base(generation::LayerId id) {
    auto& slot = bases_[std::size_t(id)];
    if (!slot) slot = generation::layerBase(layout_, id);
    return *slot;
}

bool WorldEditor::chooseLayerNamed(const std::string& name) {
    if (name == "regions") { chooseLayer(-1); return true; }
    const auto id = generation::layerNamed(name);
    if (!id) return false;
    chooseLayer(int(*id));
    return true;
}

void WorldEditor::chooseLayer(int layer) {
    endStroke();
    layer_ = std::clamp(layer, -1, kDetailsLayer);
    if (painting()) {
        // The brush the layer had, made legal for the world as it is now.
        LayerBrush& b = brushes_[std::size_t(layer_)];
        const auto limits = generation::brushLimits(layerId(), layout_);
        b.texels = std::clamp(b.texels, 1, std::max(1, std::int32_t(std::lround(limits.maxRadius / limits.texel))));
        const generation::BrushTool t = tool();
        if (generation::brushToolDef(t).procedural && sizeKm_[std::size_t(t)] <= 0.0f)
            sizeKm_[std::size_t(t)] = generation::generatorSizeKm(t, layout_.widthCells());
    }
    readout_.clear();
    mapStale_ = true;
    dirty_ = true;
}

void WorldEditor::beginStroke(bool onMap, double x, double y, bool erase) {
    if (!painting()) return;
    // One undo step a stroke: the layer as it was. A copy shares every tile
    // until the stroke paints on one, so this costs nothing.
    undo_.emplace_back(layerId(), layout_.layer(layerId()));
    if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
    stroke_ = onMap ? Stroke::Map : Stroke::Ground;
    strokeErase_ = erase;
    lastX_ = x;
    lastY_ = y;
    carry_ = 0.0;
    strokeRegions_.clear();
    strokeChanged_ = false;
    dabAt(x, y);
}

void WorldEditor::strokeTo(double x, double y) {
    if (stroke_ == Stroke::None) return;
    const generation::Brush b = brush();
    for (const auto& [dx, dy] :
         generation::strokeDabs(lastX_, lastY_, x, y, generation::dabSpacing(layerId(), b), carry_))
        dabAt(dx, dy);
    lastX_ = x;
    lastY_ = y;
}

void WorldEditor::endStroke() {
    if (stroke_ == Stroke::None) return;
    stroke_ = Stroke::None;
    // A stroke that changed nothing is not a step to undo.
    if (!undo_.empty() && undo_.back().second == layout_.layer(undo_.back().first)) undo_.pop_back();
    const std::vector<std::size_t> touched = std::move(strokeRegions_);
    strokeRegions_.clear();
    if (!strokeChanged_) return;
    const generation::LayerId id = layerId();
    std::vector<std::pair<std::int32_t, std::int32_t>> which;
    bool computed = false;   // any region reached that is worked out, not a sketch
    for (const std::size_t r : touched) {
        const std::int32_t rx = std::int32_t(r % std::size_t(layout_.regionsX));
        const std::int32_t ry = std::int32_t(r / std::size_t(layout_.regionsX));
        which.emplace_back(rx, ry);
        const auto& region = layout_.regions[r];
        computed |= region.generated || region.source >= 0 ||
                    (region.stage != generation::RegionStage::Sketch && generation::paintedIn(layout_, rx, ry));
    }
    // The coast is a sketch until it is pinned: a stroke on it builds
    // nothing, and the sketch is drawn as it is (sketchRevision). On ground
    // already worked out it waits for Build, like any change of its shape.
    if (id == generation::LayerId::Continents) {
        status(computed ? "coast changed on worked-out ground: Build (Enter) redoes those regions"
                        : "sketched - Pin sketch works it out");
        return;
    }
    // Mountains painted on a pinned region (or a sketch: painting a range on
    // it pins it) are what the relief stage is for: the generator grows them
    // from the stroke, and only that region is made again.
    if (id == generation::LayerId::Ranges && !strokeErase_) {
        if (generation::raiseStage(layout_, which, generation::RegionStage::Relief) > 0) {
            ++sketchRevision_;
            regionsChanged();
        }
    }
    if (autoBuild_ && unbuilt_ && computed) {
        wanted_ = true;
        status("building the regions that changed ...");
    }
}

void WorldEditor::touchRegions(double x, double y, double radius) {
    const double r = double(generation::kRegionMetres);
    const auto x0 = std::int32_t(std::floor((x - radius) / r)), x1 = std::int32_t(std::floor((x + radius) / r));
    const auto y0 = std::int32_t(std::floor((y - radius) / r)), y1 = std::int32_t(std::floor((y + radius) / r));
    for (std::int32_t ry = std::max(0, y0); ry <= std::min(layout_.regionsY - 1, y1); ++ry)
        for (std::int32_t rx = std::max(0, x0); rx <= std::min(layout_.regionsX - 1, x1); ++rx) {
            const std::size_t index = layout_.indexOf(rx, ry);
            if (std::find(strokeRegions_.begin(), strokeRegions_.end(), index) != strokeRegions_.end()) continue;
            strokeRegions_.push_back(index);
            // Land painted into an empty region makes it a sketch: drawn, and
            // not computed until it is pinned.
            if (layerId() == generation::LayerId::Continents && !strokeErase_) generation::beginSketch(layout_, rx, ry);
        }
}

std::array<std::int32_t, 5> WorldEditor::stageCounts() const {
    std::array<std::int32_t, 5> counts{};
    // No world opened yet: a layout with no regions in it.
    if (layout_.regions.size() != std::size_t(layout_.regionsX) * std::size_t(layout_.regionsY)) return counts;
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx) {
            if (!generation::authored(layout_, rx, ry)) continue;
            const auto stage = layout_.at(rx, ry).stage;
            if (stage == generation::RegionStage::Sketch && !generation::paintedIn(layout_, rx, ry)) continue;
            ++counts[std::size_t(stage) - 1];
        }
    return counts;
}

void WorldEditor::pinSketches() {
    endStroke();
    std::vector<std::pair<std::int32_t, std::int32_t>> which;
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx)
            if (generation::authored(layout_, rx, ry) && layout_.at(rx, ry).stage == generation::RegionStage::Sketch &&
                generation::paintedIn(layout_, rx, ry))
                which.emplace_back(rx, ry);
    if (which.empty()) { status("no sketch to pin: paint land on the Continents layer first"); return; }
    generation::raiseStage(layout_, which, generation::RegionStage::Primary);
    ++sketchRevision_;
    regionsChanged();
    wanted_ = true;
    status("pinned " + std::to_string(which.size()) + " region(s): primary ground, coast and slopes ...");
}

void WorldEditor::generateWater() {
    endStroke();
    std::vector<std::pair<std::int32_t, std::int32_t>> which;
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx) {
            if (!generation::authored(layout_, rx, ry)) continue;
            const auto stage = layout_.at(rx, ry).stage;
            if (stage == generation::RegionStage::Primary || stage == generation::RegionStage::Relief)
                which.emplace_back(rx, ry);
        }
    if (which.empty()) { status("nothing pinned to drain: pin a sketch first"); return; }
    // Imported ground among them waits for its drained height (raiseRegions).
    raiseRegions(std::move(which), generation::RegionStage::Water, "rivers and lakes");
}

void WorldEditor::dabAt(double x, double y) {
    generation::Brush b = brush();
    if (strokeErase_) b.tool = generation::BrushTool::Erase;
    // Before the paint: a region is made a sketch only by the first land put
    // into it.
    touchRegions(x, y, b.radius);
    const generation::TexelRect changed = generation::dab(layout_, layerId(), b, x, y, &base(layerId()));
    if (changed.empty()) return;
    strokeChanged_ = true;
    if (layerId() == generation::LayerId::Continents) ++sketchRevision_;
    unbuilt_ = true;
    mapStale_ = true;
    dirty_ = true;
}

void WorldEditor::undo() {
    endStroke();
    if (undo_.empty()) { status("nothing to undo"); return; }
    auto [id, map] = std::move(undo_.back());
    undo_.pop_back();
    layout_.layer(id) = std::move(map);
    unbuilt_ = true;
    ++sketchRevision_;
    mapStale_ = true;
    status(std::string("undone on ") + generation::layerDef(id).label);
}

void WorldEditor::clearLayer() {
    if (!painting() || layout_.layer(layerId()).empty()) return;
    endStroke();
    undo_.emplace_back(layerId(), layout_.layer(layerId()));
    if (undo_.size() > kUndoSteps) undo_.erase(undo_.begin());
    layout_.layer(layerId()).clear();
    unbuilt_ = true;
    ++sketchRevision_;
    mapStale_ = true;
    status(std::string(generation::layerDef(layerId()).label) + " wiped: the generator's own again");
}

void WorldEditor::readoutAt(double x, double y) {
    if (!painting()) return;
    const generation::LayerId id = layerId();
    const generation::LayerDef& def = generation::layerDef(id);
    const generation::LayerMap& map = layout_.layer(id);
    const auto tx = std::int32_t(std::floor(x / def.texelMetres));
    const auto ty = std::int32_t(std::floor(y / def.texelMetres));
    std::string text;
    if (map.inBounds(tx, ty)) {
        const float here = generation::effectiveAt(map, &base(id), tx, ty);
        if (def.kind == generation::LayerKind::Delta) {
            text = "Here " + number("%+.0f ", here) + def.unit + " on what the plates raised";
        } else {
            const float cover = map.cover(tx, ty);
            text = "Here " + number("%.0f ", here) + def.unit;
            text += cover > 0.0f ? number(", %.0f%% painted", cover * 100.0) +
                                           number(" over %.0f", base(id).at(tx, ty))
                                 : ", as generated";
        }
    }
    if (text != readout_) {
        readout_ = std::move(text);
        dirty_ = true;
    }
}

// --- the panel ---------------------------------------------------------------

bool WorldEditor::slider(ui::Ui& ui, std::uint64_t id, float x, float& y, const char* label, float& value,
                         float low, float high, const char* format) {
    const auto& theme = ui.theme();
    const auto& input = ui.input();
    const ui::Rect track{x + kLabelW, y + 9, kWide - kLabelW - x - 74, 6};
    const ui::Rect grab{track.x - 6, y, track.w + 12, kRow - 4};
    ui.text(x, y + 6, label, theme.label, 0.9f);
    bool changed = false;
    if (input.pressed && ui.hovered(grab)) dragging_ = id;
    if (!input.down && dragging_ == id) dragging_ = 0;
    if (dragging_ == id) {
        const float t = std::clamp((input.mouseX - track.x) / track.w, 0.0f, 1.0f);
        const float next = std::round(low + (high - low) * t);
        if (next != value) { value = next; changed = true; }
    }
    const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    ui.roundRect(track, theme.slot, 3);
    ui.roundRect({track.x, track.y, track.w * t, track.h}, theme.accent.withAlpha(0.85f), 3);
    ui.roundRect({track.x + track.w * t - 5, y + 4, 10, kRow - 12}, dragging_ == id ? theme.accent : theme.paper, 3);
    ui.textRight(kWide - 10, y + 6, number(format, value), theme.accent, 0.9f);
    ui.claim(grab);
    y += kRow;
    return changed;
}

bool WorldEditor::draw(ui::Ui& ui) {
    const auto& theme = ui.theme();
    const float x = 14;
    bool changed = false;

    // A dropdown's list lies over the rows below it, and they are drawn - and
    // take their clicks - before it. While the pointer is on an open list the
    // rows under it are shown a pointer somewhere else.
    const ui::Input real = panelInput_;
    const bool onList = openList_ != 0 && openRect_.contains(real.mouseX, real.mouseY);
    if (onList) {
        panelInput_.mouseX = panelInput_.mouseY = -10000.0f;
        panelInput_.pressed = panelInput_.released = false;
    }

    const float high = float(ui.height());
    ui.panel({0, 0, float(kWide), high});
    ui.text(12, 10, "WORLD EDITOR", theme.accent, 1.2f);
    ui.textRight(kWide - 12, 12, "` explore", theme.labelSoft, 0.8f);
    // Everything between the title and the foot scrolls when the window is
    // shorter than the panel is.
    const ui::Rect frame{0, 30, float(kWide), high - 30 - 36};
    scrollFrame_ = frame;
    const ui::Rect content = ui.beginScroll(ui::widgetId("editor.scroll", layer_ + 1), frame);
    float y = content.y + 4;

    // --- which layer ---------------------------------------------------------
    section(ui, x, y, "LAYER");
    const ui::Rect layerBox{x, y + 1, kWide - 2 * x, kRow - 4};
    y += kRow;
    if (biomesMode()) {
        if (layer_ == kCategoriesLayer) drawBiomes(ui, x, y, changed);
        else drawDetails(ui, x, y, changed);
    } else if (painting()) {
        const auto& def = generation::layerDef(layerId());
        const auto& map = layout_.layer(layerId());
        const auto limits = generation::brushLimits(layerId(), layout_);
        ui.text(x, y + 2, ui.fit(std::string(def.pass) + ", " + std::to_string(map.texelsX()) + " x " +
                                 std::to_string(map.texelsY()) + " texels", kWide - 2 * x, 0.8f),
                theme.labelSoft, 0.8f);
        ui.text(x, y + 16, ui.fit("Brush " + distance(limits.minRadius) + " to " + distance(limits.maxRadius) +
                                  (map.empty() ? ", nothing painted" : ", " + std::to_string(map.tileCount()) +
                                                                           " tile(s) painted"),
                                  kWide - 2 * x, 0.8f),
                theme.labelSoft, 0.8f);
        y += 34;
        drawLayer(ui, x, y, changed);
    } else {
        ui.text(x, y + 2, "Whole regions: generate with a preset, or leave as sea.", theme.labelSoft, 0.8f);
        y += 18;
        drawRegions(ui, x, y, changed);
    }
    ui.endScroll(y + 8);
    if (ownFile_) drawFile(ui, x, high - 32, changed);
    else if (!status_.empty())
        ui.text(x, high - 25, ui.fit(status_, kWide - 2 * x, 0.8f), ui.theme().labelSoft, 0.8f);

    // The dropdowns last, so their open lists lie over the rows below them.
    panelInput_ = real;
    const auto dropdown = [&](int which, const ui::Rect& box, const std::vector<std::string>& names, int chosen) {
        // A box scrolled out of the frame is not there to open.
        if (box.y < frame.y || box.bottom() > frame.bottom()) {
            if (openList_ == which) openList_ = 0;
            return -1;
        }
        const bool pressedBox = real.pressed && box.contains(real.mouseX, real.mouseY);
        const int picked = ui.dropdown(ui::widgetId("editor.list", which), box, names, chosen);
        if (pressedBox) openList_ = openList_ == which ? 0 : which;
        if (picked >= 0) openList_ = 0;
        if (openList_ == which) openRect_ = listOf(box, names.size());
        return picked;
    };
    if (painting()) {
        std::vector<std::string> names;
        for (const auto t : generation::toolsFor(layerId())) names.emplace_back(generation::brushToolDef(t).label);
        const int picked = dropdown(2, toolBox_, names, int(brushes_[std::size_t(layer_)].tool));
        if (picked >= 0 && std::size_t(picked) != brushes_[std::size_t(layer_)].tool) {
            brushes_[std::size_t(layer_)].tool = std::size_t(picked);
            chooseLayer(layer_);        // sets the procedural brush's size up
            changed = true;
        }
    } else if (!biomesMode() && !presets_.empty()) {
        std::vector<std::string> names;
        for (const auto& p : presets_) names.push_back(p.label);
        const int picked = dropdown(3, presetBox_, names, preset_);
        if (picked >= 0 && picked != preset_) { applyPreset(picked); changed = true; }
    }
    if (biomesMode()) {
        // The categories' own lists: which layer, which id; the details' tool and prop.
        const auto registry = engine::biomes::active();
        if (layer_ == kCategoriesLayer) {
            std::vector<std::string> layers{"Ground category", "Forest biome", "Water type", "Decor biome"};
            const int picked = dropdown(4, biomeLayerBox_, layers, biomeLayer_);
            if (picked >= 0 && picked != biomeLayer_) { biomeLayer_ = picked; changed = true; }
            if (registry) {
                std::vector<std::string> names;
                std::vector<std::uint32_t> ids;
                if (biomeLayer_ != 0) { names.push_back("as the category says (0)"); ids.push_back(0); }
                for (const auto& [name, id] : registry->legend(engine::biomes::Layer(biomeLayer_))) {
                    (void)name;
                    ids.push_back(id);
                }
                std::sort(ids.begin() + (biomeLayer_ != 0 ? 1 : 0), ids.end());
                for (std::size_t k = biomeLayer_ != 0 ? 1 : 0; k < ids.size(); ++k)
                    for (const auto& [name, id] : registry->legend(engine::biomes::Layer(biomeLayer_)))
                        if (id == ids[k]) names.push_back(std::to_string(id) + "  " + name);
                const auto at = std::find(ids.begin(), ids.end(), biomeIds_[std::size_t(biomeLayer_)]);
                const int chosen = at == ids.end() ? 0 : int(at - ids.begin());
                const int pickedId = dropdown(5, biomeIdBox_, names, chosen);
                if (pickedId >= 0 && pickedId != chosen) { biomeIds_[std::size_t(biomeLayer_)] = ids[std::size_t(pickedId)]; changed = true; }
            }
        } else {
            std::vector<std::string> tools{"Thicker", "Thinner", "Pin a prop", "Remove nearest"};
            const int picked = dropdown(6, detailToolBox_, tools, detailTool_);
            if (picked >= 0 && picked != detailTool_) { detailTool_ = picked; changed = true; }
            if (registry && detailTool_ == 2 && !registry->props().empty()) {
                std::vector<std::string> props;
                for (const auto& p : registry->props()) props.push_back(p.name);
                const int pickedProp = dropdown(7, detailPropBox_, props, std::min<int>(detailProp_, int(props.size()) - 1));
                if (pickedProp >= 0 && pickedProp != detailProp_) { detailProp_ = pickedProp; changed = true; }
            }
        }
    }
    {
        std::vector<std::string> names;
        names.push_back(layerName(-1));
        for (std::size_t i = 0; i < generation::kLayerCount; ++i) names.push_back(layerName(int(i)));
        names.push_back(layerName(kCategoriesLayer));
        names.push_back(layerName(kDetailsLayer));
        const int picked = dropdown(1, layerBox, names, layer_ + 1);
        if (picked >= 0 && picked - 1 != layer_) {
            chooseLayer(picked - 1);
            changed = true;
        }
    }
    // A list whose dropdown is not on the panel any more (the view changed
    // under it) has nothing to protect.
    if ((openList_ == 2 && !painting()) || (openList_ == 3 && (painting() || biomesMode()))) openList_ = 0;
    if (openList_ >= 4 && !biomesMode()) openList_ = 0;

    // A stroke on the map, taken after the lists so that picking from one
    // never paints the map under it.
    if (painting()) {
        const auto& in = ui.input();
        const bool over = mapRect_.contains(in.mouseX, in.mouseY) && frame.contains(in.mouseX, in.mouseY) && !onList;
        if (over != overMap_) dirty_ = true;
        overMap_ = over;
        if (over) {
            const auto [wx, wy] = mapToWorld(mapRect_, in.mouseX, in.mouseY);
            if (std::hypot(wx - mapPointerX_, wy - mapPointerY_) > 1.0) dirty_ = true;
            mapPointerX_ = wx;
            mapPointerY_ = wy;
            readoutAt(wx, wy);
            const bool erase = (SDL_GetModState() & SDL_KMOD_ALT) != 0;
            if (in.pressed && !in.takenByUi && stroke_ == Stroke::None) beginStroke(true, wx, wy, erase);
        }
        if (stroke_ == Stroke::Map) {
            if (!in.down) {
                endStroke();
            } else {
                const float px = std::clamp(in.mouseX, mapRect_.x, mapRect_.right() - 0.01f);
                const float py = std::clamp(in.mouseY, mapRect_.y, mapRect_.bottom() - 0.01f);
                const auto [wx, wy] = mapToWorld(mapRect_, px, py);
                strokeTo(wx, wy);
            }
        }
    } else {
        overMap_ = false;
    }
    if (changed) dirty_ = true;
    return changed;
}

void WorldEditor::drawRegions(ui::Ui& ui, float x, float& y, bool& changed) {
    const auto& theme = ui.theme();

    // --- the world -----------------------------------------------------------
    section(ui, x, y, "WORLD");
    changed |= stepper(ui, x, y, "editor.across", "Regions across", wantX_, 1, generation::kMaxRegionsPerSide);
    changed |= stepper(ui, x, y, "editor.down", "Regions down", wantY_, 1, generation::kMaxRegionsPerSide);
    ui.text(x, y + 4, number("%.0f", double(wantX_) * generation::kRegionMetres / 1000.0) + " x " +
            number("%.0f km", double(wantY_) * generation::kRegionMetres / 1000.0) +
            "   (a region is 131 km, 256 x 512 m pages)", theme.labelSoft, 0.8f);
    y += 18;
    ui.text(x, y + 6, "World seed", theme.label, 0.9f);
    ui.text(x + kLabelW, y + 6, std::to_string(layout_.seed), theme.accent, 0.9f);
    if (ui.button(ui::widgetId("editor.worldseed"), {kWide - 70, y + 1, 56, kRow - 4}, "New")) {
        layout_.seed = core::splitmix64(layout_.seed ^ SDL_GetTicks()) % 1000000u;
        changed = true;
    }
    y += kRow;
    changed |= slider(ui, ui::widgetId("editor.plates"), x, y, "Plates (0 auto)", plates_, 0, 64, "%.0f");
    changed |= slider(ui, ui::widgetId("editor.band"), x, y, "Border band", blendKm_,
                      float(generation::kMinRegionBlendMetres) / 1000.0f,
                      float(generation::kMaxRegionBlendMetres) / 1000.0f, "%.0f km");
    // Where the world is on its planet: the latitude of its top edge and how
    // much map a degree is. Moving it moves the climate of everything, once,
    // when the world is rebuilt.
    changed |= slider(ui, ui::widgetId("editor.north"), x, y, "Latitude at top", north_, -90, 90, "%.0f deg");
    changed |= slider(ui, ui::widgetId("editor.kmdeg"), x, y, "Km per degree", kmPerDegree_, 1, 120, "%.1f km");
    {
        const auto shown = shownLatitude();
        const bool latitudeMoved = std::abs(north_ - float(shown.northDegrees)) > 0.05f ||
                                   std::abs(kmPerDegree_ - float(shown.kmPerDegree)) > 0.05f;
        const bool pending = wantX_ != layout_.regionsX || wantY_ != layout_.regionsY ||
                             int(plates_) != layout_.plates ||
                             int(std::lround(blendKm_ * 1000.0f)) != layout_.blendMetres || latitudeMoved;
        if (ui.button(ui::widgetId("editor.resize"), {x, y + 2, kWide - 2 * x, 24},
                      pending ? "Apply world settings and rebuild" : "Rebuild world", true)) {
            // The layers go with the world: what was painted stays where it
            // was, on the regions that are still there.
            generation::resizeLayout(layout_, wantX_, wantY_);
            if (latitudeMoved) {
                // Degrees from here on, not the rule the world was made under.
                layout_.latitude.fixed = true;
                layout_.latitude.northDegrees = north_;
                layout_.latitude.kmPerDegree = kmPerDegree_;
                layout_.latitude.legacyFrom = layout_.latitude.legacySpan = layout_.latitude.legacyRows = 0;
            }
            syncLatitude();
            layout_.plates = int(plates_);
            layout_.blendMetres = std::clamp(int(std::lround(blendKm_ * 1000.0f)), 0,
                                             generation::kMaxRegionBlendMetres);
            selected_.assign(layout_.regions.size(), false);
            undo_.clear();
            regionsChanged();
            wanted_ = true;
            status("rebuilding the world ...");
            changed = true;
        }
        y += 32;
    }

    // --- the selection -------------------------------------------------------
    section(ui, x, y, "SELECTION");
    ui.text(x, y + 4, std::to_string(selectedCount()) + " of " + std::to_string(layout_.regions.size()) +
            " regions selected", theme.label, 0.9f);
    if (ui.button(ui::widgetId("editor.all"), {kWide - 124, y, 52, 20}, "All")) {
        std::fill(selected_.begin(), selected_.end(), true); changed = true;
    }
    if (ui.button(ui::widgetId("editor.none"), {kWide - 66, y, 52, 20}, "None")) {
        std::fill(selected_.begin(), selected_.end(), false); changed = true;
    }
    y += 24;
    changed |= stepper(ui, x, y, "editor.brush", "Brush, regions", brush_, 0,
                       std::max(layout_.regionsX, layout_.regionsY));
    if (hoverX_ >= 0) {
        const auto& region = layout_.at(hoverX_, hoverY_);
        std::string what = "Region " + std::to_string(hoverX_) + "," + std::to_string(hoverY_) + ": ";
        if (!region.generated)
            what += !generation::paintedIn(layout_, hoverX_, hoverY_) ? std::string("empty (open sea)")
                    : generation::authored(layout_, hoverX_, hoverY_)
                    ? std::string("made by hand, ") + generation::stageName(region.stage)
                    : std::string("made by hand (its paint)");
        else {
            std::string label = region.settings.preset;
            for (const auto& p : presets_) if (p.name == region.settings.preset) label = p.label;
            what += label + ", sea " + std::to_string(region.settings.seaPercent) + "%";
            if (region.source >= 0 && std::size_t(region.source) < layout_.generations.size()) {
                const auto& g = layout_.generations[std::size_t(region.source)];
                what += g.w == layout_.regionsX && g.h == layout_.regionsY && g.x == 0 && g.y == 0
                        ? ", whole world"
                        : ", own run " + std::to_string(g.w) + "x" + std::to_string(g.h);
            }
        }
        ui.text(x, y + 2, ui.fit(what, kWide - 2 * x, 0.85f), theme.label, 0.85f);
        if (region.generated)
            ui.text(x, y + 16, ui.fit("seed " + std::to_string(region.settings.seed) + ", erosion " +
                    std::to_string(region.settings.erosionPasses) + ", rain " +
                    std::to_string(region.settings.rainfallPercent) + "%", kWide - 2 * x, 0.8f),
                    theme.labelSoft, 0.8f);
    } else {
        ui.text(x, y + 2, "Point at the map to see a region.", theme.labelSoft, 0.85f);
    }
    y += 32;
    ui.text(x, y, "LMB paint  Alt+LMB unpaint  [ ] brush  Del none", theme.labelSoft, 0.8f);
    y += 16;

    // --- what to generate ----------------------------------------------------
    section(ui, x, y, "GENERATE WITH");
    presetBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
    ui.text(x, y + 6, "Preset", theme.label, 0.9f);
    y += kRow;
    ui.text(x, y + 6, "Seed", theme.label, 0.9f);
    ui.text(x + kLabelW, y + 6, std::to_string(seed_), theme.accent, 0.9f);
    if (ui.button(ui::widgetId("editor.seed"), {kWide - 70, y + 1, 56, kRow - 4}, "New")) {
        seed_ = core::splitmix64(seed_ ^ (SDL_GetTicks() * 2654435761u)) % 1000000u;
        changed = true;
    }
    y += kRow;
    {
        const bool before = ownSeeds_;
        ui.checkbox(ui::widgetId("editor.ownseeds"), {x, y + 2, kWide - 2 * x, kRow - 6},
                    "Own seed per region (from the seed above)", ownSeeds_);
        changed |= before != ownSeeds_;
        y += kRow;
    }
    changed |= slider(ui, ui::widgetId("editor.sea"), x, y, "Sea", sea_, 0, 100, "%.0f %%");
    changed |= slider(ui, ui::widgetId("editor.erosion"), x, y, "Erosion passes", erosion_, 0, 24, "%.0f");
    changed |= slider(ui, ui::widgetId("editor.rain"), x, y, "Rainfall", rain_, 20, 250, "%.0f %%");
    y += 4;
    const float half = (kWide - 2 * x - 8) / 2;
    if (ui.button(ui::widgetId("editor.generate"), {x, y, half, 26}, "Generate selected", selectedCount() > 0)) {
        generateSelected(); changed = true;
    }
    if (ui.button(ui::widgetId("editor.clear"), {x + half + 8, y, half, 26}, "Clear selected", selectedCount() > 0)) {
        clearSelected(); changed = true;
    }
    y += 32;
    if (ui.button(ui::widgetId("editor.world"), {x, y, kWide - 2 * x, 26}, "Generate the whole world as one")) {
        generateWorld();
        changed = true;
    }
    y += 34;
}

void WorldEditor::drawLayer(ui::Ui& ui, float x, float& y, bool& changed) {
    const auto& theme = ui.theme();
    const generation::LayerId id = layerId();
    const generation::LayerDef& def = generation::layerDef(id);
    LayerBrush& b = brushes_[std::size_t(layer_)];
    const generation::BrushTool t = tool();
    const auto limits = generation::brushLimits(id, layout_);
    const auto widest = std::max(1, std::int32_t(std::lround(limits.maxRadius / limits.texel)));

    // --- the brush -----------------------------------------------------------
    section(ui, x, y, "BRUSH");
    ui.text(x, y + 6, "Tool", theme.label, 0.9f);
    toolBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
    y += kRow;
    {
        // Sized in the layer's own texels: one is the finest it can paint.
        std::int32_t texels = std::clamp(b.texels, 1, widest);
        const double radius = generation::legalRadius(id, layout_, double(texels) * limits.texel);
        if (stepper(ui, x, y, "editor.size", "Size, texels", texels, 1, widest, distance(radius))) {
            b.texels = texels;
            changed = true;
        }
    }
    changed |= slider(ui, ui::widgetId("editor.strength"), x, y,
                      t == generation::BrushTool::Ranges ? "Height" : "Strength", strength_, 1, 100, "%.0f %%");
    changed |= slider(ui, ui::widgetId("editor.hardness"), x, y, "Hardness", hardness_, 0, 100, "%.0f %%");
    if (t == generation::BrushTool::Paint) {
        const std::string label = std::string("Value, ") + def.unit;
        changed |= slider(ui, ui::widgetId("editor.value", layer_), x, y, label.c_str(), b.value, def.low,
                          def.high, "%.0f");
    }
    if (t == generation::BrushTool::Continents || t == generation::BrushTool::Hills) {
        ui.text(x, y + 6, "Seed", theme.label, 0.9f);
        ui.text(x + kLabelW, y + 6, std::to_string(brushSeed_), theme.accent, 0.9f);
        if (ui.button(ui::widgetId("editor.brushseed"), {kWide - 132, y + 1, 56, kRow - 4}, "New")) {
            brushSeed_ = core::splitmix64(brushSeed_ ^ (SDL_GetTicks() * 2654435761u)) % 1000000u;
            changed = true;
        }
        // The seed of the region under the pointer, at the generator's own
        // size: with those the brush paints what the pass itself made.
        if (ui.button(ui::widgetId("editor.regionseed"), {kWide - 70, y + 1, 56, kRow - 4}, "Region")) {
            const std::int32_t rx = hoverX_ >= 0 ? hoverX_ : 0, ry = hoverY_ >= 0 ? hoverY_ : 0;
            brushSeed_ = layout_.at(rx, ry).settings.seed;
            sizeKm_[std::size_t(t)] = generation::generatorSizeKm(t, layout_.widthCells());
            changed = true;
        }
        y += kRow;
        changed |= slider(ui, ui::widgetId("editor.scale", int(t)), x, y, "Scale", sizeKm_[std::size_t(t)], 2, 400,
                          "%.0f km");
        if (t == generation::BrushTool::Hills)
            changed |= slider(ui, ui::widgetId("editor.amount"), x, y, "Amplitude", amount_, 0, 300, "%.0f %%");
    }

    // --- the map -------------------------------------------------------------
    y += 4;
    const float wide = kWide - 2 * x;
    const float aspect = float(layout_.regionsY) / float(std::max(1, layout_.regionsX));
    const float high = std::clamp(wide * aspect, 60.0f, 220.0f);
    const float across = std::min(wide, high / aspect);
    mapRect_ = {x + (wide - across) * 0.5f, y, across, high};
    drawMap(ui, mapRect_);
    y += high + 4;
    ui.text(x, y, ui.fit(readout_.empty() ? std::string("Point at the ground or the map.") : readout_,
                         kWide - 2 * x, 0.8f),
            theme.label, 0.8f);
    y += 15;
    ui.text(x, y, "LMB paint  Alt+LMB erase  [ ] size  Ctrl+Z undo", theme.labelSoft, 0.8f);
    y += 18;

    // --- what to do with it --------------------------------------------------
    const float half = (kWide - 2 * x - 8) / 2;
    if (ui.button(ui::widgetId("editor.undo"), {x, y, half, 24}, "Undo  (Ctrl+Z)", !undo_.empty())) {
        undo();
        changed = true;
    }
    if (ui.button(ui::widgetId("editor.wipe"), {x + half + 8, y, half, 24}, "Wipe the layer",
                  !layout_.layer(id).empty())) {
        clearLayer();
        changed = true;
    }
    y += 30;
    if (ui.button(ui::widgetId("editor.build"), {x, y, half, 24}, unbuilt_ ? "Build  (Enter) *" : "Build  (Enter)")) {
        endStroke();
        wanted_ = true;
        status("building ...");
        changed = true;
    }
    {
        const bool before = autoBuild_;
        ui.checkbox(ui::widgetId("editor.autobuild"), {x + half + 8, y + 2, half, 20}, "after each stroke",
                    autoBuild_);
        changed |= before != autoBuild_;
    }
    y += 30;

    // --- the stages ----------------------------------------------------------
    // A region made by hand is worked out only as far as it has been decided:
    // a sketch is drawn, a pinned coast gets its primary ground, a range
    // painted on it grows mountains, and the water comes last.
    section(ui, x, y, "STAGES");
    const auto counts = stageCounts();
    ui.text(x, y + 2, ui.fit(std::to_string(counts[0]) + " sketch, " + std::to_string(counts[1]) + " pinned, " +
                                 std::to_string(counts[2]) + " relief, " + std::to_string(counts[3]) + " water",
                             kWide - 2 * x, 0.85f),
            theme.label, 0.85f);
    y += 20;
    if (ui.button(ui::widgetId("editor.pin"), {x, y, half, 24}, "Pin sketch", counts[0] > 0)) {
        pinSketches();
        changed = true;
    }
    if (ui.button(ui::widgetId("editor.water"), {x + half + 8, y, half, 24}, "Generate water",
                  counts[1] + counts[2] > 0)) {
        generateWater();
        changed = true;
    }
    y += 30;
    ui.text(x, y, ui.fit("Continents: a sketch, nothing is built. Pin: relief, ragged coast, slopes.",
                         kWide - 2 * x, 0.75f), theme.labelSoft, 0.75f);
    y += 14;
    ui.text(x, y, ui.fit("Ranges on pinned ground: mountains from the strokes. Then the water.",
                         kWide - 2 * x, 0.75f), theme.labelSoft, 0.75f);
    y += 18;
}

void WorldEditor::drawFile(ui::Ui& ui, float x, float y, bool& changed) {
    const auto& theme = ui.theme();
    if (ui.button(ui::widgetId("editor.save"), {x, y, 80, 24}, "Save")) {
        status(generation::saveWorldLayout(layout_, file_) ? "saved " + file_.filename().string()
                                                           : "could not save " + file_.string());
        changed = true;
    }
    if (ui.button(ui::widgetId("editor.load"), {x + 88, y, 80, 24}, "Load")) {
        endStroke();
        if (auto loaded = generation::loadWorldLayout(file_)) {
            layout_ = std::move(*loaded);
            selected_.assign(layout_.regions.size(), false);
            wantX_ = layout_.regionsX; wantY_ = layout_.regionsY;
            plates_ = float(layout_.plates);
            blendKm_ = float(layout_.blendMetres) / 1000.0f;
            syncLatitude();
            undo_.clear();
            ++sketchRevision_;
            regionsChanged();
            wanted_ = true;
            status("loaded, building ...");
        } else {
            status("no world at " + file_.string());
        }
        changed = true;
    }
    if (!status_.empty()) ui.text(x + 178, y + 7, ui.fit(status_, kWide - x - 190, 0.8f), theme.labelSoft, 0.8f);
}

// --- the map -----------------------------------------------------------------

std::pair<double, double> WorldEditor::mapToWorld(const ui::Rect& area, float px, float py) const {
    return {double(px - area.x) / double(std::max(1.0f, area.w)) * double(layout_.widthMetres()),
            double(py - area.y) / double(std::max(1.0f, area.h)) * double(layout_.heightMetres())};
}

void WorldEditor::paintMap() {
    mapPixels_.assign(std::size_t(mapW_) * std::size_t(mapH_) * 4, 255);
    if (!painting()) return;
    const generation::LayerId id = layerId();
    const generation::LayerDef& def = generation::layerDef(id);
    const generation::LayerMap& map = layout_.layer(id);
    const generation::LayerBase& made = base(id);
    const bool delta = def.kind == generation::LayerKind::Delta;
    const double texel = def.texelMetres;
    for (int py = 0; py < mapH_; ++py)
        for (int px = 0; px < mapW_; ++px) {
            const double wx = (px + 0.5) / mapW_ * double(layout_.widthMetres());
            const double wy = (py + 0.5) / mapH_ * double(layout_.heightMetres());
            std::array<float, 3> under{40, 44, 52};
            if (!ground_.empty()) {
                const int gx = std::min(groundW_ - 1, int((px + 0.5) / mapW_ * groundW_));
                const int gy = std::min(groundH_ - 1, int((py + 0.5) / mapH_ * groundH_));
                const std::uint8_t* g = &ground_[(std::size_t(gy) * std::size_t(groundW_) + std::size_t(gx)) * 3];
                under = {float(g[0]), float(g[1]), float(g[2])};
            }
            // Texel by texel, not blended: the map shows the layer at the
            // resolution it has, which is the resolution it can be painted at.
            const auto tx = std::min(map.texelsX() - 1, std::int32_t(wx / texel));
            const auto ty = std::min(map.texelsY() - 1, std::int32_t(wy / texel));
            std::array<float, 3> over{};
            float alpha = 0.0f;
            if (delta) {
                const float v = map.value(tx, ty);
                over = layerColour(id, v);
                alpha = std::clamp(std::abs(v) / 450.0f, 0.0f, 0.85f);
            } else {
                const float cover = map.cover(tx, ty);
                over = layerColour(id, generation::effectiveAt(map, &made, tx, ty));
                // The generator's own showing through faintly, the paint firmly.
                alpha = 0.38f + 0.5f * cover;
            }
            std::uint8_t* out = &mapPixels_[(std::size_t(py) * std::size_t(mapW_) + std::size_t(px)) * 4];
            for (int k = 0; k < 3; ++k)
                out[k] = std::uint8_t(std::clamp(under[std::size_t(k)] * (1.0f - alpha) +
                                                         over[std::size_t(k)] * alpha, 0.0f, 255.0f));
            out[3] = 255;
        }
}

void WorldEditor::drawMap(ui::Ui& ui, const ui::Rect& area) {
    const auto& theme = ui.theme();
    const int w = std::max(1, int(area.w)), h = std::max(1, int(area.h));
    if (!mapTexture_ || mapRenderer_ != ui.renderer() || w != mapW_ || h != mapH_) {
        // Made on the panel's own renderer, which also frees it; only one of
        // ours that is still alive is ever destroyed here.
        if (mapTexture_ && mapRenderer_ == ui.renderer()) SDL_DestroyTexture(mapTexture_);
        mapTexture_ = SDL_CreateTexture(ui.renderer(), SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        if (mapTexture_) SDL_SetTextureScaleMode(mapTexture_, SDL_SCALEMODE_NEAREST);
        mapRenderer_ = ui.renderer();
        mapW_ = w;
        mapH_ = h;
        mapStale_ = true;
    }
    if (!mapTexture_) return;
    if (mapStale_) {
        paintMap();
        SDL_UpdateTexture(mapTexture_, nullptr, mapPixels_.data(), mapW_ * 4);
        mapStale_ = false;
    }
    ui.image(area, mapTexture_);
    // The regions, faintly: the coarsest layer is under every other one.
    const ui::Colour rule = theme.label.withAlpha(0.35f);
    for (std::int32_t rx = 1; rx < layout_.regionsX; ++rx) {
        const float px = area.x + area.w * float(rx) / float(layout_.regionsX);
        ui.line(px, area.y, px, area.bottom(), rule);
    }
    for (std::int32_t ry = 1; ry < layout_.regionsY; ++ry) {
        const float py = area.y + area.h * float(ry) / float(layout_.regionsY);
        ui.line(area.x, py, area.right(), py, rule);
    }
    ui.border(area, theme.barEdge);
    // The brush where the pointer is - on the map, or on the ground.
    if (overMap_ || pointerValid_) {
        const double cx = overMap_ ? mapPointerX_ : pointerX_, cy = overMap_ ? mapPointerY_ : pointerY_;
        const float sx = area.x + float(cx / double(layout_.widthMetres())) * area.w;
        const float sy = area.y + float(cy / double(layout_.heightMetres())) * area.h;
        const float radius = std::max(1.5f, float(brush().radius / double(layout_.widthMetres())) * area.w);
        const bool erase = overMap_ ? (SDL_GetModState() & SDL_KMOD_ALT) != 0 : erasing_;
        const ui::Colour ring = erase ? theme.bad : theme.accent;
        constexpr int kSegments = 40;
        for (int k = 0; k < kSegments; ++k) {
            const float a0 = float(k) / kSegments * 6.2831853f, a1 = float(k + 1) / kSegments * 6.2831853f;
            ui.line(sx + std::cos(a0) * radius, sy + std::sin(a0) * radius, sx + std::cos(a1) * radius,
                    sy + std::sin(a1) * radius, ring);
        }
    }
}

std::array<std::array<float, 4>, engine::kSceneEditorVectors> WorldEditor::overlay() const {
    std::array<std::array<float, 4>, engine::kSceneEditorVectors> o{};
    if (!active_) return o;
    // A layer being painted: the region grid stays, the selection does not,
    // and the brush carries the layer's texel so its grid shows inside it.
    const float texel = painting() ? float(generation::layerDef(layerId()).texelMetres) : 0.0f;
    o[0] = {float(generation::kRegionMetres), 1.0f, float(layout_.regionsX), float(layout_.regionsY)};
    o[1] = {painting() ? -1.0f : float(hoverX_), painting() ? -1.0f : float(hoverY_), float(layout_.blendMetres),
            texel};
    if (painting()) {
        if (pointerValid_) {
            const generation::Brush b = brush();
            const float mode = erasing_ ? 2.0f : generation::brushToolDef(b.tool).procedural ? 3.0f : 1.0f;
            o[2] = {float(pointerX_), float(pointerY_), float(b.radius), mode};
        }
    } else if (biomesMode()) {
        if (pointerValid_) {
            const double radius = layer_ == kCategoriesLayer ? double(biomeRadiusKm_) * 1000.0 : double(detailRadius_);
            o[2] = {float(pointerX_), float(pointerY_), float(radius), erasing_ ? 2.0f : 3.0f};
        }
    } else if (pointerValid_ && brush_ > 0) {
        o[2] = {float(pointerX_), float(pointerY_), float(double(brush_) * generation::kRegionMetres),
                erasing_ ? 2.0f : 1.0f};
    }
    // Thirty-two regions to a row (the most a world has) and sixteen bits to
    // a number: whole numbers a float holds exactly, never bit patterns it
    // might not. Selected from o[3], generated from o[19] (editor_overlay.hlsli).
    for (std::int32_t y = 0; y < layout_.regionsY; ++y)
        for (std::int32_t x = 0; x < layout_.regionsX; ++x) {
            const std::uint32_t index = std::uint32_t(y) * 32u + std::uint32_t(x);
            const std::uint32_t component = index / 16u;
            const float bit = float(1u << (index % 16u));
            if (!painting() && selected_[layout_.indexOf(x, y)]) o[3 + component / 4u][component % 4u] += bit;
            // "Made": anything with ground of its own - generated, worked out
            // by hand past its sketch, or imported. The rest is hatched as
            // empty, and an imported continent hatched over was a striped
            // field laid on the world.
            const auto& region = layout_.at(x, y);
            const bool made = region.generated || region.source >= 0 ||
                              (region.stage != generation::RegionStage::Full &&
                               region.stage != generation::RegionStage::Sketch && generation::paintedIn(layout_, x, y)) ||
                              generation::importedIn(layout_, x, y);
            if (made) o[19 + component / 4u][component % 4u] += bit;
        }
    return o;
}


// --- the authored maps ---------------------------------------------------------

namespace {
namespace ws = engine::world_source;

ws::WorldExtent extentOf(const generation::WorldLayout& layout) {
    return {double(layout.widthMetres()), double(layout.heightMetres()), 256, 32768};
}
std::array<double, 4> squareOf(std::int32_t rx, std::int32_t ry) {
    const double r = double(generation::kRegionMetres);
    return {rx * r, ry * r, (rx + 1) * r, (ry + 1) * r};
}
std::string reportLine(const ws::ImportReport& report, std::size_t regions, double seconds) {
    return std::to_string(regions) + (regions == 1 ? " region: " : " regions: ") +
           std::to_string(report.chunksWritten) + " chunks written, " + std::to_string(report.chunksRemoved) +
           " emptied, " + std::to_string(report.chunksUnchanged) + " unchanged (" + number("%.1f s)", seconds);
}
} // namespace

std::filesystem::path WorldEditor::sourceRoot() const {
    // Beside the layout: its history root (world_saves.hpp), then "source".
    return file_.parent_path() / file_.stem() / "source";
}

void WorldEditor::reopenSource() { layout_.imported = generation::openImported(sourceRoot(), layout_); }

void WorldEditor::selectRegions() {
    if (layer_ >= 0) chooseLayer(-1);
    brush_ = 0;
}

void WorldEditor::selectAll(bool on) {
    std::fill(selected_.begin(), selected_.end(), on);
    dirty_ = true;
}

std::int32_t WorldEditor::importedRegions() const {
    return layout_.imported ? layout_.imported->regionsHeld() : 0;
}

void WorldEditor::importMaps(const ImportRequest& request) {
    if (importing()) return;
    endStroke();
    auto regions = selection();
    // Nothing selected is the whole world, which needs no mask: nothing
    // beside it to blend into.
    const bool whole = regions.empty();
    if (whole)
        for (std::int32_t y = 0; y < layout_.regionsY; ++y)
            for (std::int32_t x = 0; x < layout_.regionsX; ++x) regions.emplace_back(x, y);
    ws::ImportTarget target;
    target.world = extentOf(layout_);
    target.rastersOnly = true;
    target.featherMetres = std::max(0.0, request.featherKm) * 1000.0;
    std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
    for (const auto& [rx, ry] : regions) {
        const auto sq = squareOf(rx, ry);
        if (!whole) target.mask.push_back(sq);
        box = {std::min(box[0], sq[0]), std::min(box[1], sq[1]), std::max(box[2], sq[2]), std::max(box[3], sq[3])};
    }
    target.rect = box;
    const auto root = sourceRoot();
    // New heights make the drained height stale; regions elsewhere that are
    // at their drainage or further need it made again before they are built.
    // The regions the heights go into drop back to the heights alone.
    const bool heights = !request.height.empty() || (request.control.empty() && !request.package.empty());
    const bool rebake = heights && drainedElsewhere(regions);
    status("importing into " + std::to_string(regions.size()) + " region(s) ...");
    importLine_ = "Importing ...";
    jobLabel_ = "Importing";
    job_ = std::async(std::launch::async, [request, target, root, regions, heights, rebake] {
        Job job;
        job.regions = regions;
        job.heights = heights;
        const auto began = std::chrono::steady_clock::now();
        std::string why;
        std::optional<ws::ImportReport> report;
        if (!request.height.empty() || !request.control.empty()) {
            ws::LooseImages images;
            images.height = request.height;
            images.control = request.control;
            images.lowMetres = request.lowMetres;
            images.seaGrey = request.seaGrey;
            images.highMetres = request.highMetres;
            report = ws::importImages(images, root, target, &why);
        } else if (!request.package.empty()) {
            report = ws::importPackage(request.package, root, target, &why);
        } else {
            why = "choose a height map, a control map or a package first";
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        job.ok = report.has_value();
        job.line = job.ok ? "Imported into " + reportLine(*report, regions.size(), seconds) : "Import failed: " + why;
        if (job.ok && report->seaGrey >= 0) job.line += "; coast at grey " + std::to_string(report->seaGrey);
        if (job.ok) job.opened = generation::ImportedSource::open(root, rebake);
        return job;
    });
}

void WorldEditor::clearImport() {
    if (importing()) return;
    const auto regions = selection();
    if (regions.empty()) { status("select the regions to clear first"); return; }
    if (!ws::WorldSource::exists(sourceRoot())) { status("nothing has been imported"); return; }
    std::vector<std::array<double, 4>> rects;
    for (const auto& [rx, ry] : regions) rects.push_back(squareOf(rx, ry));
    const auto root = sourceRoot();
    const bool rebake = drainedElsewhere(regions);
    importLine_ = "Clearing ...";
    jobLabel_ = "Clearing";
    job_ = std::async(std::launch::async, [rects, root, regions, rebake] {
        Job job;
        job.regions = regions;
        job.clearing = true;
        const auto began = std::chrono::steady_clock::now();
        std::string why;
        const auto report = ws::clearRasters(root, rects, &why);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        job.ok = report.has_value();
        job.line = job.ok ? "Cleared " + reportLine(*report, regions.size(), seconds) : "Clear failed: " + why;
        if (job.ok) job.opened = generation::ImportedSource::open(root, rebake);
        return job;
    });
}

std::vector<std::pair<std::int32_t, std::int32_t>> WorldEditor::importTargets() const {
    std::vector<std::pair<std::int32_t, std::int32_t>> out;
    if (!layout_.imported) return out;
    const auto chosen = selection();
    if (!chosen.empty()) {
        for (const auto& [rx, ry] : chosen)
            if (generation::importedIn(layout_, rx, ry)) out.emplace_back(rx, ry);
        return out;
    }
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx)
            if (generation::importedIn(layout_, rx, ry)) out.emplace_back(rx, ry);
    return out;
}

bool WorldEditor::drainedElsewhere(const std::vector<std::pair<std::int32_t, std::int32_t>>& except) const {
    if (!layout_.imported) return false;
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx) {
            if (!generation::importedIn(layout_, rx, ry)) continue;
            if (std::find(except.begin(), except.end(), std::pair{rx, ry}) != except.end()) continue;
            if (std::uint8_t(layout_.at(rx, ry).stage) >= std::uint8_t(generation::RegionStage::Relief)) return true;
        }
    return false;
}

std::array<std::int32_t, 3> WorldEditor::importedStages() const {
    std::array<std::int32_t, 3> counts{};
    if (!layout_.imported || layout_.regions.size() != std::size_t(layout_.regionsX) * std::size_t(layout_.regionsY))
        return counts;
    for (std::int32_t ry = 0; ry < layout_.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout_.regionsX; ++rx) {
            if (!generation::importedIn(layout_, rx, ry)) continue;
            switch (layout_.at(rx, ry).stage) {
                case generation::RegionStage::Relief: ++counts[1]; break;
                case generation::RegionStage::Water: ++counts[2]; break;
                default: ++counts[0]; break;
            }
        }
    return counts;
}

void WorldEditor::stageImported(generation::RegionStage to) {
    if (importing()) return;
    endStroke();
    auto which = importTargets();
    if (which.empty()) {
        status(layout_.imported ? "none of the selected regions holds imported heights" : "nothing has been imported");
        return;
    }
    if (to == generation::RegionStage::Primary) {
        // Back to the heights alone: the drainage and the water dropped.
        std::int32_t moved = 0;
        for (const auto& [rx, ry] : which) {
            auto& r = layout_.at(rx, ry);
            if (r.stage == generation::RegionStage::Primary) continue;
            r.stage = generation::RegionStage::Primary;
            ++moved;
        }
        if (moved == 0) { status("those regions hold only their heights already"); return; }
        ++sketchRevision_;
        regionsChanged();
        unbuilt_ = true;
        wanted_ = true;
        status(std::to_string(moved) + " region(s) back to their heights alone: no drainage, no water");
        return;
    }
    raiseRegions(std::move(which), to,
                 to == generation::RegionStage::Water ? "rivers and lakes" : "drainage and climate, no water yet");
}

void WorldEditor::raiseRegions(std::vector<std::pair<std::int32_t, std::int32_t>> which, generation::RegionStage to,
                               const std::string& doing) {
    // Imported ground past its heights is built on the drained height: made
    // first, off the frame, when it is not current.
    bool needsBake = false;
    if (layout_.imported && !layout_.imported->drained() &&
        std::uint8_t(to) >= std::uint8_t(generation::RegionStage::Relief))
        for (const auto& [rx, ry] : which) needsBake |= generation::importedIn(layout_, rx, ry);
    if (!needsBake) {
        const std::int32_t moved = generation::raiseStage(layout_, which, to);
        if (moved == 0) { status("those regions are that far already"); return; }
        ++sketchRevision_;
        regionsChanged();
        unbuilt_ = true;
        wanted_ = true;
        status(std::to_string(moved) + " region(s): " + doing + " ...");
        return;
    }
    if (importing()) return;
    const auto root = sourceRoot();
    importLine_ = "Draining the heights ...";
    jobLabel_ = "Drainage";
    status("breaching the imported heights' hollows, then " + doing + " ...");
    job_ = std::async(std::launch::async, [root, which, to, doing] {
        Job job;
        job.regions = which;
        job.raiseTo = to;
        const auto began = std::chrono::steady_clock::now();
        std::string why;
        job.ok = generation::ImportedSource::bake(root, &why);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        job.line = job.ok ? "Heights drained (" + number("%.1f s): ", seconds) + std::to_string(which.size()) +
                                    " region(s) to " + doing
                          : "Drainage failed: " + why;
        if (job.ok) job.opened = generation::ImportedSource::open(root);
        return job;
    });
}

void WorldEditor::exportMaps(const std::filesystem::path& directory) {
    if (importing()) return;
    if (!ws::WorldSource::exists(sourceRoot())) { importLine_ = "Nothing to export: nothing has been imported"; dirty_ = true; return; }
    if (directory.empty()) { importLine_ = "Choose where to export to"; dirty_ = true; return; }
    ws::ExportOptions options;
    const auto regions = selection();
    if (!regions.empty()) {
        std::array<double, 4> box{1e300, 1e300, -1e300, -1e300};
        for (const auto& [rx, ry] : regions) {
            const auto sq = squareOf(rx, ry);
            box = {std::min(box[0], sq[0]), std::min(box[1], sq[1]), std::max(box[2], sq[2]), std::max(box[3], sq[3])};
        }
        options.rect = box;
    }
    std::string why;
    const auto began = std::chrono::steady_clock::now();
    const auto report = ws::exportPackage(sourceRoot(), directory, options, &why);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    importLine_ = report ? number("Exported %.0f", report->sizeX / 1000.0) + number(" x %.0f km to ", report->sizeY / 1000.0) +
                               directory.filename().string() + number(" (%.1f s)", seconds)
                         : "Export failed: " + why;
    dirty_ = true;
}

void WorldEditor::update() {
    if (!job_.valid()) return;
    if (job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        // Still at it: what it is doing, for the tab.
        const auto line = jobLabel_ + ": " + core::progressLine();
        if (line != importLine_) { importLine_ = line; dirty_ = true; }
        return;
    }
    Job job = job_.get();
    importLine_ = job.line;
    dirty_ = true;
    if (!job.ok) { status(job.line); return; }
    layout_.imported = job.opened;
    if (job.raiseTo) {
        // The drained height is there now: the regions go on to their stage.
        generation::raiseStage(layout_, job.regions, *job.raiseTo);
    } else if (!job.clearing && job.heights) {
        // The regions new heights went into are imported ones: made by hand,
        // their ground the skeleton's as drawn - the heights alone, whatever
        // stage they were at. Drainage and water are asked for again, each
        // on its own (stageImported). One generated from a preset gives up
        // that generation.
        std::vector<std::pair<std::int32_t, std::int32_t>> generated;
        for (const auto& [rx, ry] : job.regions)
            if (layout_.at(rx, ry).generated || layout_.at(rx, ry).source >= 0) generated.emplace_back(rx, ry);
        if (!generated.empty()) generation::clearRegions(layout_, generated);
        for (const auto& [rx, ry] : job.regions) layout_.at(rx, ry).stage = generation::RegionStage::Primary;
    }
    // A control map alone changes what the regions are built from and not
    // how far they are taken.
    ++sketchRevision_;
    regionsChanged();
    unbuilt_ = true;
    wanted_ = true;
    status(job.line);
}

} // namespace client

