// The world editor's terrain categories and details (engine/biomes,
// doc/plan_ground_types_2026-10-01.md, phase 9): a brush of ids into the
// world source, an inspector of a category's numbers, and the hand edits to
// the details the categories place.
#include "game/client/world_editor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

#include "engine/biomes/detail_edits.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/core/rng.hpp"
#include "engine/world_source/transfer.hpp"

namespace client {
namespace {
namespace ws = engine::world_source;
namespace eb = engine::biomes;

constexpr float kRow = 26, kLabelW = 132;

void heading(ui::Ui& ui, float x, float& y, const char* title) {
    y += 6;
    ui.text(x, y, title, ui.theme().labelSoft, 0.8f);
    y += 16;
}

// A number of a category in content/config/terrain/categories.json, written
// where the category itself says it (its parent's stays the parent's).
bool writeCategoryNumber(const std::filesystem::path& dir, const std::string& category,
                         const std::vector<std::string>& path, const nlohmann::ordered_json& value, std::string* why) {
    const auto file = dir / "categories.json";
    std::ifstream in(file);
    auto j = nlohmann::ordered_json::parse(in, nullptr, false);
    in.close();
    if (j.is_discarded() || !j.contains("categories")) { if (why) *why = file.string() + " is not readable"; return false; }
    for (auto& c : j["categories"]) {
        if (c.value("name", std::string()) != category) continue;
        nlohmann::ordered_json* at = &c;
        for (std::size_t k = 0; k + 1 < path.size(); ++k) {
            if (!at->contains(path[k]) || !(*at)[path[k]].is_object()) (*at)[path[k]] = nlohmann::ordered_json::object();
            at = &(*at)[path[k]];
        }
        (*at)[path.back()] = value;
        const auto partial = std::filesystem::path(file.string() + ".partial");
        {
            std::ofstream out(partial, std::ios::trunc);
            out << j.dump(1, ' ', false) << "\n";
            if (!out) { if (why) *why = "cannot write " + partial.string(); return false; }
        }
        std::error_code ec;
        std::filesystem::rename(partial, file, ec);
        if (ec) { if (why) *why = ec.message(); return false; }
        return true;
    }
    if (why) *why = "no category \"" + category + "\" in " + file.string();
    return false;
}

} // namespace

void WorldEditor::drawBiomes(ui::Ui& ui, float x, float& y, bool& changed) {
    const auto& theme = ui.theme();
    const auto registry = eb::active();
    ui.text(x, y + 2, ui.fit("Ids into the imported source: a category, or a forest, water or decor biome.",
                             kWide - 2 * x, 0.8f),
            theme.labelSoft, 0.8f);
    y += 18;
    if (!layout_.imported) {
        ui.text(x, y + 2, ui.fit("Nothing imported yet: the brush paints over an import.", kWide - 2 * x, 0.8f),
                theme.label, 0.8f);
        y += 20;
    }
    heading(ui, x, y, "BRUSH");
    ui.text(x, y + 6, "Layer", theme.label, 0.9f);
    biomeLayerBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
    y += kRow;
    ui.text(x, y + 6, biomeLayer_ == 0 ? "Category" : "Biome", theme.label, 0.9f);
    biomeIdBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
    y += kRow;
    float radius = std::max(1.0f, std::round(biomeRadiusKm_));
    if (slider(ui, ui::widgetId("editor.biome.radius"), x, y, "Radius, km", radius, 1, 60, "%.0f km")) {
        biomeRadiusKm_ = radius;
        changed = true;
    }
    ui.text(x, y + 2, ui.fit("Paint on the ground; Alt paints 0 back. [ ] resize. The world is raised again.",
                             kWide - 2 * x, 0.75f),
            theme.labelSoft, 0.75f);
    y += 20;
    if (registry && biomeLayer_ == 0) changed |= inspectCategory(ui, x, y);
    if (!registry) {
        ui.text(x, y + 2, "content/config/terrain did not load.", theme.label, 0.8f);
        y += 18;
    }
}

bool WorldEditor::inspectCategory(ui::Ui& ui, float x, float& y) {
    const auto& theme = ui.theme();
    const auto registry = eb::active();
    const eb::Category* c = registry ? registry->category(biomeIds_[0]) : nullptr;
    if (!c) return false;
    heading(ui, x, y, "CATEGORY");
    ui.text(x, y + 2, ui.fit(c->name + (c->inherit.empty() ? "" : "  (from " + c->inherit + ")"), kWide - 2 * x, 0.9f),
            theme.accent, 0.9f);
    y += 20;
    std::string soils;
    for (std::size_t k = 0; k < eb::kClasses; ++k)
        if (!c->soils[k].empty()) soils += (soils.empty() ? "" : ", ") + std::string(eb::kClassNames[k]) + " " + c->soils[k];
    ui.text(x, y + 2, ui.fit("Soils: " + (soils.empty() ? std::string("the engine's") : soils), kWide - 2 * x, 0.75f),
            theme.labelSoft, 0.75f);
    y += 16;
    ui.text(x, y + 2, ui.fit("Slopes: " + (c->slopeRock.empty() ? std::string("the engine's") : c->slopeRock) +
                             "; cover: " + (c->groundFoliage.empty() ? std::string("the engine's") : c->groundFoliage),
                             kWide - 2 * x, 0.75f),
            theme.labelSoft, 0.75f);
    y += 18;
    // Numbers only - a table refill, no recompile. Saved as they are moved;
    // the renderer reads the file again within a second.
    bool changed = false;
    std::string why;
    const auto dir = registry->directory();
    const auto save = [&](const std::vector<std::string>& path, const nlohmann::ordered_json& value) {
        if (writeCategoryNumber(dir, c->name, path, value, &why)) status("saved " + c->name + ": the picture follows");
        else status("could not save: " + why);
        changed = true;
    };
    float r = std::round(float(c->tint[0] * 100)), g = std::round(float(c->tint[1] * 100)), b = std::round(float(c->tint[2] * 100));
    const float r0 = r, g0 = g, b0 = b;
    slider(ui, ui::widgetId("editor.cat.r", int(c->id)), x, y, "Tint red", r, 50, 150, "%.0f %%");
    slider(ui, ui::widgetId("editor.cat.g", int(c->id)), x, y, "Tint green", g, 50, 150, "%.0f %%");
    slider(ui, ui::widgetId("editor.cat.b", int(c->id)), x, y, "Tint blue", b, 50, 150, "%.0f %%");
    if (r != r0 || g != g0 || b != b0) save({"look", "tint"}, nlohmann::ordered_json::array({r / 100.0, g / 100.0, b / 100.0}));
    float sat = std::round(float(c->saturation * 100));
    if (slider(ui, ui::widgetId("editor.cat.sat", int(c->id)), x, y, "Saturation", sat, 0, 150, "%.0f %%"))
        save({"look", "saturation"}, sat / 100.0);
    float steep = std::round(float(c->steepFrom * 100));
    if (slider(ui, ui::widgetId("editor.cat.steep", int(c->id)), x, y, "Rock from", steep, 10, 90, "%.0f %%"))
        save({"slopes", "steep_from"}, steep / 100.0);
    const auto control = [&](const char* id, const char* label, const char* key, const std::optional<double>& v) {
        float value = std::round(float(v.value_or(0.5) * 100));
        if (slider(ui, ui::widgetId(id, int(c->id)), x, y, label, value, 0, 100, "%.0f %%"))
            save({"controls", key}, value / 100.0);
    };
    control("editor.cat.moist", "Moisture", "moisture", c->controls.moisture);
    control("editor.cat.forest", "Forest", "forest", c->controls.forest);
    control("editor.cat.erosion", "Erosion", "erosion", c->controls.erosion);
    ui.text(x, y + 2, ui.fit("Controls are what assemble_controls.py writes where a map does not say.", kWide - 2 * x, 0.72f),
            theme.labelSoft, 0.72f);
    y += 18;
    return changed;
}

void WorldEditor::drawDetails(ui::Ui& ui, float x, float& y, bool& changed) {
    const auto& theme = ui.theme();
    ui.text(x, y + 2, ui.fit("Hand edits to what the categories place, kept apart from the maps.", kWide - 2 * x, 0.8f),
            theme.labelSoft, 0.8f);
    y += 18;
    heading(ui, x, y, "TOOL");
    ui.text(x, y + 6, "Tool", theme.label, 0.9f);
    detailToolBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
    y += kRow;
    if (detailTool_ == 2) {
        ui.text(x, y + 6, "Prop", theme.label, 0.9f);
        detailPropBox_ = {x + kLabelW, y + 1, kWide - kLabelW - x - 12, kRow - 4};
        y += kRow;
    }
    float radius = std::round(detailRadius_);
    if (slider(ui, ui::widgetId("editor.detail.radius"), x, y, "Radius, m", radius, 8, 2000, "%.0f m")) {
        detailRadius_ = radius;
        changed = true;
    }
    static const char* kHints[] = {
            "Brush the derived density up (x1.5 a stroke); Alt resets it.",
            "Brush the derived density down (x0.67 a stroke); Alt resets it.",
            "Click to pin the prop; it stays whatever the ids become. Alt unpins the nearest.",
            "Click to remove the derived instance nearest the pointer; Alt brings it back."};
    ui.text(x, y + 2, ui.fit(kHints[std::clamp(detailTool_, 0, 3)], kWide - 2 * x, 0.75f), theme.labelSoft, 0.75f);
    y += 20;
    if (!layout_.imported) {
        ui.text(x, y + 2, ui.fit("Nothing imported yet: details live beside an import's source.", kWide - 2 * x, 0.8f),
                theme.label, 0.8f);
        y += 20;
    }
}

void WorldEditor::biomePointer(double x, double y, bool valid, bool paint, bool erase) {
    if (!valid || !(paint || erase)) {
        if (biomeStroke_) {
            biomeStroke_ = false;
            if (layer_ == kCategoriesLayer) applyBiomeStroke();
            else applyDetailStroke();
        }
        return;
    }
    const double radius = layer_ == kCategoriesLayer ? double(biomeRadiusKm_) * 1000.0 : double(detailRadius_);
    if (!biomeStroke_) {
        biomeStroke_ = true;
        biomeErase_ = erase;
        biomeDabs_.clear();
        biomeDabs_.push_back({x, y, radius});
        biomeLastX_ = x;
        biomeLastY_ = y;
        dirty_ = true;
        return;
    }
    // Pins and removals are clicks; brushes are strokes.
    if (layer_ == kDetailsLayer && detailTool_ >= 2) return;
    if (std::hypot(x - biomeLastX_, y - biomeLastY_) > radius * 0.35) {
        biomeDabs_.push_back({x, y, radius});
        biomeLastX_ = x;
        biomeLastY_ = y;
    }
}

void WorldEditor::applyBiomeStroke() {
    const auto registry = eb::active();
    if (!registry || biomeDabs_.empty()) return;
    if (!layout_.imported) { status("nothing imported: the category brush paints over an import"); return; }
    const auto layer = eb::Layer(std::clamp(biomeLayer_, 0, 3));
    const std::uint32_t id = biomeErase_ ? 0 : biomeIds_[std::size_t(layer)];
    std::vector<ws::CategoricalDab> dabs;
    for (const auto& d : biomeDabs_) dabs.push_back({d[0], d[1], d[2]});
    std::string why;
    const ws::WorldExtent world{double(layout_.widthMetres()), double(layout_.heightMetres()), 256, 32768};
    const auto report = ws::paintCategorical(sourceRoot(), world, eb::kLayerNames[std::size_t(layer)], id, dabs,
                                             registry->legend(layer), true, &why);
    biomeDabs_.clear();
    if (!report) { status("could not paint: " + why); return; }
    reopenSource();
    status(std::string(eb::kLayerNames[std::size_t(layer)]) + " " + std::to_string(id) + ": " +
           std::to_string(report->chunksWritten) + " chunk(s) painted" + (autoBuild_ ? ", building ..." : "; Build (Enter) shows it"));
    if (autoBuild_ && report->chunksWritten) wanted_ = true;
    dirty_ = true;
}

void WorldEditor::applyDetailStroke() {
    if (biomeDabs_.empty()) return;
    if (!layout_.imported) { status("nothing imported: details live beside an import's source"); biomeDabs_.clear(); return; }
    std::string why;
    auto edits = eb::DetailEdits::load(sourceRoot(), &why);
    const auto registry = eb::active();
    const auto& first = biomeDabs_.front();
    std::string done;
    switch (detailTool_) {
        case 0:
        case 1:
            for (const auto& d : biomeDabs_) {
                if (biomeErase_) {
                    for (double v = d[1] - d[2]; v <= d[1] + d[2]; v += eb::DetailEdits::kDensityCell)
                        for (double u = d[0] - d[2]; u <= d[0] + d[2]; u += eb::DetailEdits::kDensityCell)
                            if (std::hypot(u - d[0], v - d[1]) <= d[2]) edits.setDensity(u, v, 1.0);
                } else {
                    edits.brushDensity(d[0], d[1], d[2], detailTool_ == 0 ? 1.5 : 1.0 / 1.5);
                }
            }
            done = biomeErase_ ? "density reset" : detailTool_ == 0 ? "thicker" : "thinner";
            break;
        case 2:
            if (biomeErase_) {
                const auto near = edits.pinnedIn(first[0] - first[2], first[1] - first[2], first[0] + first[2], first[1] + first[2]);
                const auto best = std::min_element(near.begin(), near.end(), [&](const auto& a, const auto& b) {
                    return std::hypot(a.x - first[0], a.y - first[1]) < std::hypot(b.x - first[0], b.y - first[1]);
                });
                done = best != near.end() && edits.unpin(best->id) ? "unpinned " + best->name : "no pin there";
            } else if (registry && !registry->props().empty()) {
                const auto& prop = registry->props()[std::size_t(std::clamp<int>(detailProp_, 0, int(registry->props().size()) - 1))];
                const double yaw = double(core::splitmix64(std::uint64_t(first[0] * 7.0) ^ std::uint64_t(first[1] * 13.0)) >> 11) *
                                   0x1p-53 * 6.283185307179586;
                edits.pin({"", "prop", prop.name, first[0], first[1], yaw, 1.0});
                done = "pinned " + prop.name;
            }
            break;
        default:
            if (!picker_) { done = "nothing to pick from"; break; }
            if (const auto picked = picker_(first[0], first[1])) {
                if (biomeErase_) edits.restore(picked->id, picked->x, picked->y);
                else edits.remove(picked->id, picked->x, picked->y);
                done = biomeErase_ ? "brought back" : "removed";
            } else {
                done = "nothing there";
            }
            break;
    }
    biomeDabs_.clear();
    if (!edits.save(sourceRoot(), &why)) { status("could not save the details: " + why); return; }
    reopenSource();
    status("details: " + done + (autoBuild_ ? ", building ..." : "; Build (Enter) shows it"));
    if (autoBuild_) wanted_ = true;
    dirty_ = true;
}

} // namespace client
