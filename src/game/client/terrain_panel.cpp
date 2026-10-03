#include "game/client/terrain_panel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>

#include "engine/biomes/registry.hpp"

namespace client {
namespace fs = std::filesystem;
namespace eb = engine::biomes;
using Json = nlohmann::ordered_json;

namespace {

std::uint64_t id(const char* name, int index = 0) { return ui::widgetId(name, index); }

fs::path terrainDirectory() { return eb::defaultDirectory(); }
fs::path configDirectory() { return terrainDirectory().parent_path(); }

std::string format(const char* pattern, double value) {
    char text[48];
    std::snprintf(text, sizeof text, pattern, value);
    return text;
}

double rounded(double v, double step = 0.001) { return std::round(v / step) * step; }

// A heading over a group; with `open`, a click on it folds the group.
void heading(ui::Ui& ui, float x, float& y, float w, const char* title, int index, bool* open = nullptr) {
    const auto& t = ui.theme();
    const float h = ui.textHeight(0.8f);
    if (open) {
        const ui::Rect r{x - 4, y - 3, w + 8, h + 6};
        if (ui.selectable(id("ground.heading", index), r, false)) *open = !*open;
    }
    ui.text(x, y, std::string(open ? (*open ? "- " : "+ ") : "") + title, t.accent, 0.8f, true);
    y += h + 5;
    ui.rect({x, y, w, 1}, t.barEdge.withAlpha(0.55f));
    y += 9;
}

bool sliderRow(ui::Ui& ui, std::uint64_t which, float x, float& y, float w, const std::string& label,
               const std::string& value, float& v, float low, float high, bool logarithmic = false) {
    const auto& t = ui.theme();
    ui.text(x, y, ui.fit(label, w * 0.62f, 0.88f), t.label, 0.88f);
    ui.textRight(x + w, y, value, t.accent, 0.88f);
    y += ui.textHeight(0.88f) + 4;
    const bool moved = ui.slider(which, {x, y, w, 18}, v, low, high, logarithmic);
    y += 25;
    return moved;
}

// A labelled button that opens a choice.
bool pickRow(ui::Ui& ui, std::uint64_t which, float x, float& y, float w, const std::string& label,
             const std::string& value, bool inherited = false) {
    const auto& t = ui.theme();
    const float lw = std::floor(w * 0.36f);
    ui.text(x, y + 7, ui.fit(label, lw - 6, 0.88f), inherited ? t.labelSoft : t.label, 0.88f);
    const bool pressed = ui.button(which, {x + lw, y, w - lw, 28}, ui.fit(value, w - lw - 14, 0.85f));
    y += 33;
    return pressed;
}

void note(ui::Ui& ui, float x, float& y, float w, const std::string& text) {
    y += ui.paragraph(x, y, w, text, ui.theme().labelSoft, 0.76f) + 8;
}

// The member at a path of an object, or null.
const Json* at(const Json& j, std::initializer_list<const char*> path) {
    const Json* here = &j;
    for (const char* key : path) {
        if (!here->is_object() || !here->contains(key)) return nullptr;
        here = &(*here)[key];
    }
    return here;
}

std::string text(const Json* j) { return j && j->is_string() ? j->get<std::string>() : std::string(); }
double number(const Json* j, double otherwise) { return j && j->is_number() ? j->get<double>() : otherwise; }

void eraseMember(Json& object, const char* group, const char* key) {
    if (!object.contains(group) || !object[group].is_object()) return;
    object[group].erase(key);
    if (object[group].empty()) object.erase(group);
}

std::string cleanName(const std::string& name) {
    // Letters of any script (UTF-8 bytes pass whole), digits and a few marks;
    // nothing that is a path or a control character.
    std::string out;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x80 || std::isalnum(u) || c == '-' || c == '_' || c == ' ' || c == '.') out += c;
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

bool copyWhole(const fs::path& from, const fs::path& to, std::string* why) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    const auto partial = fs::path(to.string() + ".partial");
    fs::copy_file(from, partial, fs::copy_options::overwrite_existing, ec);
    if (!ec) fs::rename(partial, to, ec);
    if (ec) {
        if (why) *why = from.filename().string() + ": " + ec.message();
        fs::remove(partial, ec);
        return false;
    }
    return true;
}

// Every file a preset holds: (the live file, its place in the preset).
std::vector<std::pair<fs::path, fs::path>> presetFiles(const fs::path& preset) {
    std::vector<std::pair<fs::path, fs::path>> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(terrainDirectory(), ec))
        if (e.path().extension() == ".json") files.emplace_back(e.path(), preset / "terrain" / e.path().filename());
    std::sort(files.begin(), files.end());
    files.emplace_back(configDirectory() / "ground.json", preset / "ground.json");
    files.emplace_back(game::terrainLookFile(), preset / "terrain_look.json");
    return files;
}

constexpr const char* kClassLabels[eb::kClasses] = {"Grass", "Dirt", "Sand", "Rock", "Marsh", "Snow"};
constexpr const char* kZones[] = {"steppe", "taiga", "temperate_forest", "tropical_forest", "mediterranean",
                                  "savanna", "tundra", "alpine", "desert", "ice"};
constexpr const char* kKeepBefore = "_before-load";

} // namespace

// --- files ----------------------------------------------------------------

Json& TerrainPanel::File::data() {
    std::error_code ec;
    const auto now = fs::last_write_time(path, ec);
    if (!loaded || (!ec && now != stamp)) {
        std::ifstream in(path);
        auto fresh = Json::parse(in, nullptr, false);
        // A file being written is empty for an instant: keep what was read.
        if (!fresh.is_discarded()) {
            doc = std::move(fresh);
            stamp = now;
            loaded = true;
        }
    }
    return doc;
}

bool TerrainPanel::File::save(std::string* why) {
    std::error_code ec;
    const auto partial = fs::path(path.string() + ".partial");
    {
        std::ofstream out(partial, std::ios::trunc);
        out << doc.dump(1, ' ', false) << "\n";
        if (!out) {
            if (why) *why = "cannot write " + partial.string();
            return false;
        }
    }
    fs::rename(partial, path, ec);
    if (ec) {
        if (why) *why = ec.message();
        return false;
    }
    stamp = fs::last_write_time(path, ec);
    return true;
}

Json* TerrainPanel::entry(File& file, const char* list, const std::string& name) {
    auto& d = file.data();
    Json* items = nullptr;
    if (d.is_array()) items = &d;
    else if (d.is_object() && d.contains(list) && d[list].is_array()) items = &d[list];
    if (!items) return nullptr;
    for (auto& e : *items)
        if (e.is_object() && e.value("name", std::string()) == name) return &e;
    return nullptr;
}

void TerrainPanel::saved(File& file, const std::string& what, bool validate) {
    std::string why;
    if (!file.save(&why)) {
        status_ = "Could not save: " + why;
        return;
    }
    if (!validate) {
        status_ = "Saved " + what + ": the picture follows.";
        return;
    }
    std::vector<eb::Problem> problems;
    const auto registry = eb::Registry::load(terrainDirectory(), &problems);
    if (registry)
        for (const auto& p : registry->validate()) problems.push_back(p);
    status_ = problems.empty() ? "Saved " + what + ": the picture follows within a second."
                               : "Saved " + what + ", but the ground will not take it yet - " +
                                         fs::path(problems.front().file).filename().string() + ": " +
                                         problems.front().what;
}

void TerrainPanel::editCategory(const std::function<void(Json&)>& edit) {
    if (Json* e = entry(categories_, "categories", category_)) {
        edit(*e);
        saved(categories_, category_, true);
    }
}

void TerrainPanel::editSoil(const std::function<void(Json&)>& edit) {
    if (Json* e = entry(soils_, "soils", soil_)) {
        edit(*e);
        saved(soils_, "the soil " + soil_, true);
    }
}

void TerrainPanel::editEntry(File& file, const char* list, const std::string& name, const std::string& what,
                             const std::function<void(Json&)>& edit) {
    if (Json* e = entry(file, list, name)) {
        edit(*e);
        saved(file, what, true);
    } else {
        status_ = "\"" + name + "\" is not in " + file.path.filename().string() + ".";
    }
}

std::string TerrainPanel::ensureLayer(const AssetItem& texture) {
    if (texture.kind != AssetKind::Texture) return {};
    auto& d = layers_.data();
    if (!d.is_object() || !d.contains("layers") || !d["layers"].is_array()) {
        status_ = "layers.json did not read: the texture was not added.";
        return {};
    }
    auto& list = d["layers"];
    // Already listed - by this panel a moment ago, before the renderer read it
    // again, or under the stem the catalogue saw.
    for (const auto& e : list) {
        if (!e.is_object()) continue;
        const auto name = e.value("name", std::string());
        const auto path = e.value("path", std::string());
        if ((!path.empty() && path == texture.stem) ||
            (path.empty() && "terrain/ph/" + name + "/ph_" + name == texture.stem))
            return name;
    }
    if (texture.registered) return texture.name;
    if (list.size() >= 64) {
        status_ = "The ground's texture list is full (64): the texture was not added.";
        return {};
    }
    std::string name = texture.name;
    const auto taken = [&](const std::string& n) {
        return std::any_of(list.begin(), list.end(), [&](const Json& e) { return e.value("name", std::string()) == n; });
    };
    for (int n = 2; taken(name); ++n) name = texture.name + "_" + std::to_string(n);
    Json e = Json::object();
    e["name"] = name;
    e["metres"] = rounded(texture.metres, 0.01);
    e["role"] = "added in the ground panel";
    e["path"] = texture.stem;
    list.push_back(e);
    std::string why;
    if (!layers_.save(&why)) {
        status_ = "Could not add the texture: " + why;
        return {};
    }
    return name;
}

void TerrainPanel::browse(AssetBrowser::Request request) {
    catalog_.refresh();
    browser_.open(std::move(request), catalog_);
}

void TerrainPanel::openCatalogue() {
    AssetBrowser::Request request;
    request.title = "Everything the build has for the ground";
    for (std::size_t k = 0; k < kAssetKinds; ++k) request.kinds.push_back(AssetKind(k));
    browse(std::move(request));
}

void TerrainPanel::drawWindows(ui::Ui& ui, const ui::Rect& area) { browser_.draw(ui, area); }

// --- presets --------------------------------------------------------------

fs::path TerrainPanel::presetsDirectory() { return configDirectory().parent_path() / "presets" / "terrain"; }

std::vector<std::string> TerrainPanel::presets() {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(presetsDirectory(), ec))
        if (e.is_directory() && e.path().extension() != ".partial") names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

bool TerrainPanel::savePreset(const std::string& requested, std::string* why) {
    const auto name = cleanName(requested);
    if (name.empty()) {
        if (why) *why = "a preset needs a name (letters, digits, - _ . and spaces)";
        return false;
    }
    const auto target = presetsDirectory() / name;
    const auto building = fs::path(target.string() + ".partial");
    std::error_code ec;
    fs::remove_all(building, ec);
    Json manifest;
    manifest["_comment"] = "A ground preset: every file the ground panel edits (TerrainPanel). Loaded by "
                           "copying them back over content/config.";
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char when[32];
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    manifest["saved"] = when;
    manifest["files"] = Json::array();
    for (const auto& [live, kept] : presetFiles(building)) {
        if (!fs::exists(live, ec)) continue;
        if (!copyWhole(live, kept, why)) return false;
        manifest["files"].push_back(fs::relative(kept, building, ec).generic_string());
    }
    {
        std::ofstream out(building / "preset.json", std::ios::trunc);
        out << manifest.dump(1, ' ', false) << "\n";
        if (!out) {
            if (why) *why = "cannot write the preset's manifest";
            return false;
        }
    }
    fs::remove_all(target, ec);
    fs::rename(building, target, ec);
    if (ec) {
        if (why) *why = ec.message();
        return false;
    }
    return true;
}

bool TerrainPanel::loadPreset(const std::string& name, std::string* why) {
    const auto source = presetsDirectory() / name;
    std::error_code ec;
    if (name.empty() || !fs::is_directory(source, ec)) {
        if (why) *why = "no preset \"" + name + "\"";
        return false;
    }
    // What is there now, kept before it is replaced.
    if (name != kKeepBefore && !savePreset(kKeepBefore, why)) return false;
    int copied = 0;
    for (const auto& [live, kept] : presetFiles(source)) {
        if (!fs::exists(kept, ec)) continue;
        if (!copyWhole(kept, live, why)) return false;
        ++copied;
    }
    // A preset may hold library files the live directory no longer has.
    for (const auto& e : fs::directory_iterator(source / "terrain", ec)) {
        const auto live = terrainDirectory() / e.path().filename();
        if (e.path().extension() == ".json" && !fs::exists(live, ec)) {
            if (!copyWhole(e.path(), live, why)) return false;
            ++copied;
        }
    }
    if (copied == 0) {
        if (why) *why = "the preset holds no files";
        return false;
    }
    return true;
}

bool TerrainPanel::removePreset(const std::string& name, std::string* why) {
    std::error_code ec;
    const auto target = presetsDirectory() / name;
    if (name.empty() || !fs::is_directory(target, ec)) {
        if (why) *why = "no preset \"" + name + "\"";
        return false;
    }
    fs::remove_all(target, ec);
    if (ec) {
        if (why) *why = ec.message();
        return false;
    }
    return true;
}

// --- the panel ------------------------------------------------------------

void TerrainPanel::choose(std::string title, std::vector<std::string> options, int current,
                          std::function<void(int)> apply) {
    choice_ = Choice{std::move(title), std::move(options), current, std::move(apply)};
}

void TerrainPanel::drawChoice(ui::Ui& ui, float x, float& y, float w) {
    const auto& t = ui.theme();
    ui.text(x, y, ui.fit(choice_->title, w, 0.92f, true), t.accent, 0.92f, true);
    y += ui.textHeight(0.92f) + 8;
    if (ui.button(id("ground.choice.cancel"), {x, y, w, 28}, "Cancel")) {
        choice_.reset();
        return;
    }
    y += 36;
    const float rowH = 26;
    for (int i = 0; i < int(choice_->options.size()); ++i) {
        const ui::Rect r{x, y, w, rowH};
        if (ui.selectable(id("ground.choice.row", i), r, i == choice_->current)) {
            auto apply = std::move(choice_->apply);
            choice_.reset();
            apply(i);
            return;
        }
        ui.text(r.x + 8, r.y + (rowH - ui.textHeight(0.88f)) * 0.5f, ui.fit(choice_->options[std::size_t(i)], w - 16, 0.88f),
                i == choice_->current ? t.accent : t.label, 0.88f);
        y += rowH + 2;
    }
}

void TerrainPanel::draw(ui::Ui& ui, float x, float& y, float w) {
    if (categories_.path.empty()) {
        categories_.path = terrainDirectory() / "categories.json";
        soils_.path = terrainDirectory() / "soils.json";
        ground_.path = configDirectory() / "ground.json";
        layers_.path = terrainDirectory() / "layers.json";
        rocks_.path = terrainDirectory() / "rocks.json";
        plants_.path = terrainDirectory() / "plants.json";
        props_.path = terrainDirectory() / "props.json";
        decals_.path = terrainDirectory() / "decals.json";
    }
    {
        std::error_code ec;
        const auto stamp = fs::last_write_time(game::terrainLookFile(), ec);
        if (!lookLoaded_ || (!ec && stamp != lookStamp_)) {
            look_.load(game::terrainLookFile());
            lookStamp_ = stamp;
            lookLoaded_ = true;
        }
    }
    if (choice_) {
        drawChoice(ui, x, y, w);
        return;
    }
    if (browser_.isOpen()) {
        const auto& t = ui.theme();
        ui.text(x, y, ui.fit(browser_.title(), w, 0.92f, true), t.accent, 0.92f, true);
        y += ui.textHeight(0.92f) + 8;
        note(ui, x, y, w, "Choose in the window beside this panel: everything the build has, with pictures. Click a "
                          "picture to read about it, click it again or press Choose to take it.");
        if (ui.button(id("ground.browse.cancel"), {x, y, w, 28}, "Cancel")) browser_.close();
        y += 36;
        return;
    }
    note(ui, x, y, w, "What the control maps' categories paint, the soils and materials under them and how the "
                      "textures are laid. Every change is saved at once; the picture follows within a second.");
    drawPresets(ui, x, y, w);
    if (ui.button(id("ground.catalogue"), {x, y, w, 28}, "See everything the build has")) openCatalogue();
    y += 38;
    drawTextures(ui, x, y, w);
    drawCategory(ui, x, y, w);
    drawSoil(ui, x, y, w);
    drawRock(ui, x, y, w);
    drawDecorations(ui, x, y, w);
    drawMaterials(ui, x, y, w);
    if (!status_.empty()) {
        y += 2;
        y += ui.paragraph(x, y, w, status_, ui.theme().label, 0.8f) + 6;
    }
}

void TerrainPanel::drawPresets(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "PRESETS", 0);
    if (!presetsListed_) {
        const std::string was = preset_ >= 0 && preset_ < int(presetList_.size()) ? presetList_[std::size_t(preset_)] : "";
        presetList_ = presets();
        presetsListed_ = true;
        const auto found = std::find(presetList_.begin(), presetList_.end(), was);
        preset_ = found == presetList_.end() ? -1 : int(found - presetList_.begin());
    }
    const bool have = preset_ >= 0 && preset_ < int(presetList_.size());
    const std::string shown = have ? presetList_[std::size_t(preset_)]
                                   : presetList_.empty() ? std::string("none saved yet") : std::string("choose one");
    if (pickRow(ui, id("ground.preset.pick"), x, y, w, "Preset", shown) && !presetList_.empty()) {
        choose("Preset", presetList_, preset_, [this](int i) {
            preset_ = i;
            presetName_ = presetList_[std::size_t(i)];
            confirmLoad_ = confirmRemove_ = false;
        });
    }
    const float half = std::floor((w - 8) * 0.5f);
    if (ui.button(id("ground.preset.load"), {x, y, half, 28}, confirmLoad_ ? "Load - sure?" : "Load", have)) {
        if (!confirmLoad_) {
            confirmLoad_ = true;
            confirmRemove_ = false;
        } else {
            std::string why;
            const auto name = presetList_[std::size_t(preset_)];
            if (loadPreset(name, &why)) {
                status_ = "Loaded \"" + name + "\". What was there is kept as \"" + kKeepBefore + "\".";
                categories_.loaded = soils_.loaded = ground_.loaded = false;
                lookLoaded_ = false;
            } else {
                status_ = "Could not load: " + why;
            }
            confirmLoad_ = false;
            presetsListed_ = false;
        }
    }
    if (ui.button(id("ground.preset.remove"), {x + half + 8, y, w - half - 8, 28},
                  confirmRemove_ ? "Delete - sure?" : "Delete", have)) {
        if (!confirmRemove_) {
            confirmRemove_ = true;
            confirmLoad_ = false;
        } else {
            std::string why;
            const auto name = presetList_[std::size_t(preset_)];
            status_ = removePreset(name, &why) ? "Deleted the preset \"" + name + "\"." : "Could not delete: " + why;
            confirmRemove_ = false;
            presetsListed_ = false;
            preset_ = -1;
        }
    }
    y += 36;
    ui.textField(id("ground.preset.name"), {x, y, w - 92, 28}, presetName_, "name for a new preset", 48);
    if (ui.button(id("ground.preset.save"), {x + w - 84, y, 84, 28}, "Save", !cleanName(presetName_).empty())) {
        std::string why;
        const auto name = cleanName(presetName_);
        if (savePreset(name, &why)) {
            status_ = "Saved the preset \"" + name + "\".";
            presetList_ = presets();
            const auto found = std::find(presetList_.begin(), presetList_.end(), name);
            preset_ = found == presetList_.end() ? -1 : int(found - presetList_.begin());
        } else {
            status_ = "Could not save: " + why;
        }
    }
    y += 36;
    note(ui, x, y, w, "A preset is every file of the ground at once: categories, soils, rocks, ground cover, decals, "
                      "forest, water and decor biomes, the materials and the texture laying. content/presets/terrain.");
}

void TerrainPanel::drawTextures(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "TEXTURES", 1, &showTextures_);
    if (!showTextures_) return;
    bool changed = false;
    changed |= sliderRow(ui, id("ground.look.tile"), x, y, w, "Tile size", format("x %.2f", look_.tileScale),
                         look_.tileScale, 0.25f, 4.0f, true);
    changed |= sliderRow(ui, id("ground.look.octaves"), x, y, w, "Grows with distance",
                         look_.octaves < 0.05f ? std::string("off") : format("up to x%.0f", std::exp2(double(look_.octaves))),
                         look_.octaves, 0.0f, 4.0f);
    changed |= sliderRow(ui, id("ground.look.start"), x, y, w, "Next size from",
                         format("%.0f texels a pixel", look_.octaveStart), look_.octaveStart, 2.0f, 64.0f, true);
    changed |= sliderRow(ui, id("ground.look.macro"), x, y, w, "Broad light and dark",
                         format("%.0f %%", double(look_.macroVariation) * 100.0), look_.macroVariation, 0.0f, 3.0f);
    note(ui, x, y, w, "The scan lies at its own size close up and doubles each time a pixel covers that many more of "
                      "its texels, so a hillside does not repeat one small photograph. Fewer texels a pixel: the "
                      "next size sooner, less repetition, softer middle distance.");
    if (changed) {
        look_.clampAll();
        std::string why;
        if (look_.save(game::terrainLookFile(), &why)) {
            std::error_code ec;
            lookStamp_ = fs::last_write_time(game::terrainLookFile(), ec);
            status_ = "Saved the texture laying: the picture follows.";
        } else {
            status_ = "Could not save: " + why;
        }
    }
}

void TerrainPanel::drawMaterials(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "MATERIALS", 2, &showMaterials_);
    if (!showMaterials_) return;
    auto& list = ground_.data();
    if (!list.is_array() || list.empty()) {
        note(ui, x, y, w, "content/config/ground.json did not read.");
        return;
    }
    std::vector<std::string> names;
    for (const auto& e : list) names.push_back(e.value("name", std::string("?")));
    material_ = std::clamp(material_, 0, int(names.size()) - 1);
    if (pickRow(ui, id("ground.material.pick"), x, y, w, "Material", names[std::size_t(material_)]))
        choose("Material", names, material_, [this](int i) { material_ = i; });
    Json& m = list[std::size_t(material_)];
    const auto field = [&](const char* key, const char* label, const char* pattern, float low, float high, bool lg,
                           double otherwise, int index) {
        float v = float(number(at(m, {key}), otherwise));
        if (sliderRow(ui, id("ground.material.field", index), x, y, w, label, format(pattern, v), v, low, high, lg)) {
            m[key] = rounded(v);
            saved(ground_, "the material " + names[std::size_t(material_)], false);
        }
    };
    field("metres_per_turn", "Texture size", "%.2f m", 0.5f, 32.0f, true, 2.0, 0);
    field("blend_width", "Border width", "%.2f", 0.02f, 0.6f, false, 0.1, 1);
    field("tear", "Border tear", "%.2f", 0.0f, 1.0f, false, 0.3, 2);
    field("tear_metres", "Tear size", "%.1f m", 0.2f, 30.0f, true, 2.0, 3);
    note(ui, x, y, w, "The six classes the world blends by slope, wetness and height: how big their texture is and how "
                      "they meet their neighbours. content/config/ground.json.");
}

void TerrainPanel::drawCategory(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "CATEGORY", 3, &showCategory_);
    if (!showCategory_) return;
    const auto registry = eb::active();
    if (!registry || registry->categories().empty()) {
        note(ui, x, y, w, "content/config/terrain did not load: every ground is the engine's own.");
        return;
    }
    std::vector<std::string> names, labels;
    for (const auto& c : registry->categories()) {
        names.push_back(c.name);
        labels.push_back(c.name + "  (id " + std::to_string(c.id) + ")");
    }
    const eb::Category* resolved = registry->category(category_);
    if (!resolved) {
        category_ = names.size() > 1 ? names[1] : names[0];
        resolved = registry->category(category_);
    }
    const int chosen = int(std::find(names.begin(), names.end(), category_) - names.begin());
    if (pickRow(ui, id("ground.category.pick"), x, y, w, "Category", labels[std::size_t(chosen)]))
        choose("Category (an id of the ground control map)", labels, chosen, [this, names](int i) {
            category_ = names[std::size_t(i)];
        });
    Json* found = entry(categories_, "categories", category_);
    if (!resolved || !found) {
        note(ui, x, y, w, "This category is not in categories.json.");
        return;
    }
    const Json own = *found;   // a copy: an edit below rewrites the file
    note(ui, x, y, w, "Grey labels are inherited; choose \"(inherit)\" to give a value back to the parent.");

    // Inheritance.
    {
        const auto parent = text(at(own, {"inherit"}));
        if (pickRow(ui, id("ground.category.inherit"), x, y, w, "Inherits", parent.empty() ? "nothing" : parent)) {
            std::vector<std::string> options{"(nothing)"};
            for (const auto& n : names)
                if (n != category_) options.push_back(n);
            const int current = parent.empty() ? 0 : int(std::find(options.begin(), options.end(), parent) - options.begin());
            choose("Inherits from", options, current, [this, options](int i) {
                editCategory([&](Json& c) {
                    if (i == 0) c.erase("inherit");
                    else c["inherit"] = options[std::size_t(i)];
                });
            });
        }
    }

    // A library choice: (inherit), maybe none, or an entry of a library,
    // chosen in the window; written at group/key.
    const auto library = [&](int index, const char* label, const char* group, const char* key,
                             const std::string& effective, AssetKind kind, const std::string& title,
                             bool allowNone) {
        const auto* mine = group ? at(own, {group, key}) : at(own, {key});
        const bool inherited = mine == nullptr;
        const std::string shown = inherited ? (effective.empty() ? std::string("engine's own") : effective) + "  (inherited)"
                                            : text(mine);
        if (!pickRow(ui, id("ground.category.library", index), x, y, w, label, shown, inherited)) return;
        AssetBrowser::Request request;
        request.title = title + " - " + category_;
        request.kinds = {kind};
        request.current = inherited ? effective : text(mine);
        request.extras = {"(inherit)"};
        if (allowNone) request.extras.push_back("none");
        const std::string g = group ? group : "", k = key;
        request.apply = [this, g, k](const AssetItem* item, int extra) {
            const bool inherit = !item && extra == 0;
            const std::string value = item ? item->name : std::string("none");
            editCategory([&](Json& c) {
                if (inherit) {
                    if (g.empty()) c.erase(k);
                    else eraseMember(c, g.c_str(), k.c_str());
                } else if (g.empty()) {
                    c[k] = value;
                } else {
                    if (!c.contains(g) || !c[g].is_object()) c[g] = Json::object();
                    c[g][k] = value;
                }
            });
        };
        browse(std::move(request));
    };

    y += 4;
    heading(ui, x, y, w, "SOILS BY MATERIAL CLASS", 10);
    for (std::size_t k = 0; k < eb::kClasses; ++k)
        library(int(k), kClassLabels[k], "soils", eb::kClassNames[k], resolved->soils[k], AssetKind::Soil,
                std::string("Soil behind ") + kClassLabels[k], false);

    heading(ui, x, y, w, "SLOPES", 11);
    library(10, "Rock face", "slopes", "rock", resolved->slopeRock, AssetKind::Rock, "Rock on the slopes", false);
    library(11, "Scree", "slopes", "scree", resolved->scree, AssetKind::Soil, "Soil at the foot of the slopes", false);
    {
        float steep = float(number(at(own, {"slopes", "steep_from"}), resolved->steepFrom));
        if (sliderRow(ui, id("ground.category.steep"), x, y, w, "Rock from", format("%.0f %% slope", steep * 100.0),
                      steep, 0.1f, 0.9f))
            editCategory([&](Json& c) {
                if (!c.contains("slopes") || !c["slopes"].is_object()) c["slopes"] = Json::object();
                c["slopes"]["steep_from"] = rounded(steep, 0.01);
            });
    }

    heading(ui, x, y, w, "GROUND COVER AND LAYERS", 12);
    library(20, "Grass", nullptr, "ground_foliage", resolved->groundFoliage, AssetKind::Foliage, "Ground cover", true);
    library(21, "Forest", "defaults", "forest", resolved->forest, AssetKind::Forest,
            "Forest where the forest map says 0", false);
    library(22, "Water", "defaults", "water", resolved->water, AssetKind::Water, "Water where the water map says 0", false);
    library(23, "Decor", "defaults", "decor", resolved->decor, AssetKind::Decor, "Decor where the decor map says 0", false);

    // Decals: the category's own list, or the inherited one copied on the
    // first change.
    heading(ui, x, y, w, "DECALS", 13);
    {
        const auto* mine = at(own, {"decals"});
        eb::Weighted shown;
        if (mine && mine->is_array()) {
            for (const auto& pair : *mine)
                if (pair.is_array() && pair.size() == 2 && pair[0].is_string() && pair[1].is_number())
                    shown.emplace_back(pair[0].get<std::string>(), pair[1].get<double>());
        } else {
            shown = resolved->decals;
        }
        if (!mine) note(ui, x, y, w, shown.empty() ? "None (inherited)." : "Inherited: a change makes them this category's own.");
        const auto writeDecals = [this](eb::Weighted list) {
            editCategory([&](Json& c) {
                Json array = Json::array();
                for (const auto& [name, weight] : list) array.push_back(Json::array({name, rounded(weight, 0.01)}));
                c["decals"] = array;
            });
        };
        for (std::size_t i = 0; i < shown.size(); ++i) {
            float weight = float(shown[i].second);
            const auto& t = ui.theme();
            ui.text(x, y, ui.fit(shown[i].first, w - 90, 0.86f), mine ? t.label : t.labelSoft, 0.86f);
            ui.textRight(x + w - 36, y, format("x %.2f", weight), t.accent, 0.86f);
            y += ui.textHeight(0.86f) + 4;
            const bool moved = ui.slider(id("ground.decal.weight", int(i)), {x, y + 2, w - 36, 18}, weight, 0.0f, 3.0f);
            const bool gone = ui.button(id("ground.decal.remove", int(i)), {x + w - 28, y, 28, 24}, "x");
            y += 30;
            if (moved || gone) {
                auto list = shown;
                if (gone) list.erase(list.begin() + std::ptrdiff_t(i));
                else list[i].second = weight;
                writeDecals(list);
                break;
            }
        }
        if (ui.button(id("ground.decal.add"), {x, y, w, 28}, "Add a decal")) {
            AssetBrowser::Request request;
            request.title = "Decal to add - " + category_;
            request.kinds = {AssetKind::Decal};
            request.apply = [writeDecals, shown](const AssetItem* item, int) {
                if (!item) return;
                auto list = shown;
                if (std::any_of(list.begin(), list.end(), [&](const auto& p) { return p.first == item->name; })) return;
                list.emplace_back(item->name, 1.0);
                writeDecals(list);
            };
            browse(std::move(request));
        }
        y += 36;
    }

    heading(ui, x, y, w, "LOOK", 14);
    {
        const auto* tint = at(own, {"look", "tint"});
        float rgb[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] = float(tint && tint->is_array() && tint->size() == 3 ? (*tint)[std::size_t(i)].get<double>()
                                                                       : resolved->tint[std::size_t(i)]);
        const char* labels3[] = {"Tint red", "Tint green", "Tint blue"};
        bool moved = false;
        for (int i = 0; i < 3; ++i)
            moved |= sliderRow(ui, id("ground.category.tint", i), x, y, w, labels3[i],
                               format("%.0f %%", double(rgb[i]) * 100.0), rgb[i], 0.5f, 1.5f);
        if (moved)
            editCategory([&](Json& c) {
                if (!c.contains("look") || !c["look"].is_object()) c["look"] = Json::object();
                c["look"]["tint"] = Json::array({rounded(rgb[0]), rounded(rgb[1]), rounded(rgb[2])});
            });
        float saturation = float(number(at(own, {"look", "saturation"}), resolved->saturation));
        if (sliderRow(ui, id("ground.category.saturation"), x, y, w, "Saturation",
                      format("%.0f %%", double(saturation) * 100.0), saturation, 0.0f, 1.5f))
            editCategory([&](Json& c) {
                if (!c.contains("look") || !c["look"].is_object()) c["look"] = Json::object();
                c["look"]["saturation"] = rounded(saturation, 0.01);
            });
    }

    heading(ui, x, y, w, "CONTROLS WHERE A MAP DOES NOT SAY", 15);
    {
        const struct { const char* key; const char* label; std::optional<double> value; } controls[] = {
                {"moisture", "Moisture", resolved->controls.moisture},
                {"forest", "Forest", resolved->controls.forest},
                {"erosion", "Erosion", resolved->controls.erosion},
                {"mountain", "Mountain", resolved->controls.mountain}};
        int index = 0;
        for (const auto& control : controls) {
            float v = float(number(at(own, {"controls", control.key}), control.value.value_or(0.5)));
            if (sliderRow(ui, id("ground.category.control", index++), x, y, w, control.label,
                          format("%.0f %%", double(v) * 100.0), v, 0.0f, 1.0f)) {
                const std::string key = control.key;
                editCategory([&](Json& c) {
                    if (!c.contains("controls") || !c["controls"].is_object()) c["controls"] = Json::object();
                    c["controls"][key] = rounded(v, 0.01);
                });
            }
        }
        note(ui, x, y, w, "What tools/assemble_controls.py writes into a control map where it has no picture.");
    }

    heading(ui, x, y, w, "CLIMATE BEFORE THE WORLD HAS ONE", 16);
    {
        std::vector<std::string> zones(std::begin(kZones), std::end(kZones));
        const auto* zone = at(own, {"climate", "zone"});
        const std::string shown = zone ? text(zone)
                                       : (resolved->climateZone.empty() ? std::string("none") : resolved->climateZone) +
                                                 "  (inherited)";
        if (pickRow(ui, id("ground.category.zone"), x, y, w, "Zone", shown, zone == nullptr)) {
            std::vector<std::string> options{"(inherit)"};
            options.insert(options.end(), zones.begin(), zones.end());
            const int current = zone ? int(std::find(options.begin(), options.end(), text(zone)) - options.begin()) : 0;
            choose("Climate zone", options, current, [this, options](int i) {
                editCategory([&](Json& c) {
                    if (i == 0) eraseMember(c, "climate", "zone");
                    else {
                        if (!c.contains("climate") || !c["climate"].is_object()) c["climate"] = Json::object();
                        c["climate"]["zone"] = options[std::size_t(i)];
                    }
                });
            });
        }
        const struct { const char* key; const char* label; std::optional<double> value; } numbers[] = {
                {"warmth", "Warmth", resolved->warmth}, {"fertility", "Fertility", resolved->fertility}};
        int index = 0;
        for (const auto& n : numbers) {
            float v = float(number(at(own, {"climate", n.key}), n.value.value_or(0.5)));
            if (sliderRow(ui, id("ground.category.climate", index++), x, y, w, n.label,
                          format("%.0f %%", double(v) * 100.0), v, 0.0f, 1.0f)) {
                const std::string key = n.key;
                editCategory([&](Json& c) {
                    if (!c.contains("climate") || !c["climate"].is_object()) c["climate"] = Json::object();
                    c["climate"][key] = rounded(v, 0.01);
                });
            }
        }
    }
    y += 4;
}

void TerrainPanel::drawSoil(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "SOIL", 4, &showSoil_);
    if (!showSoil_) return;
    const auto registry = eb::active();
    if (!registry || registry->soils().empty()) {
        note(ui, x, y, w, "No soils loaded.");
        return;
    }
    std::vector<std::string> names;
    for (const auto& s : registry->soils()) names.push_back(s.name);
    if (std::find(names.begin(), names.end(), soil_) == names.end()) {
        // The chosen category's grass soil, else the first.
        const auto* c = registry->category(category_);
        soil_ = c && !c->soils[0].empty() ? c->soils[0] : names.front();
    }
    if (pickRow(ui, id("ground.soil.pick"), x, y, w, "Soil", soil_)) {
        AssetBrowser::Request request;
        request.title = "Soil to edit";
        request.kinds = {AssetKind::Soil};
        request.current = soil_;
        request.apply = [this](const AssetItem* item, int) { if (item) soil_ = item->name; };
        browse(std::move(request));
    }
    const auto index = registry->soilIndex(soil_);
    Json* found = entry(soils_, "soils", soil_);
    if (!index || !found) return;
    const eb::Soil& resolved = registry->soils()[*index];
    const Json own = *found;

    // Texture layers and their shares: moving one share rescales the others
    // so they still make one.
    std::vector<std::pair<std::string, double>> layers = resolved.layers;
    if (const auto* mine = at(own, {"layers"}); mine && mine->is_array()) {
        layers.clear();
        for (const auto& pair : *mine)
            if (pair.is_array() && pair.size() == 2) layers.emplace_back(pair[0].get<std::string>(), pair[1].get<double>());
    }
    const auto writeLayers = [this](std::vector<std::pair<std::string, double>> list) {
        // Shares that make exactly one, to the thousandth.
        double sum = 0;
        for (const auto& l : list) sum += l.second;
        for (auto& l : list) l.second = sum > 1e-9 ? l.second / sum : 1.0 / double(list.size());
        sum = 0;
        for (auto& l : list) sum += (l.second = rounded(l.second, 0.001));
        if (!list.empty()) list.back().second = rounded(list.back().second + (1.0 - sum), 0.001);
        editSoil([&](Json& s) {
            Json array = Json::array();
            for (const auto& [name, share] : list) array.push_back(Json::array({name, rounded(share, 0.001)}));
            s["layers"] = array;
        });
    };
    // A texture of the build for layer `i` (or a new one at the end).
    const auto pickTexture = [this, writeLayers, layers](std::size_t i) {
        AssetBrowser::Request request;
        request.title = (i < layers.size() ? "Texture " + std::to_string(i + 1) : std::string("Texture to add")) +
                        " of the soil " + soil_;
        request.kinds = {AssetKind::Texture};
        if (i < layers.size()) request.current = layers[i].first;
        request.apply = [this, writeLayers, layers, i](const AssetItem* item, int) {
            if (!item) return;
            const auto name = ensureLayer(*item);
            if (name.empty()) return;
            auto list = layers;
            if (i < list.size()) {
                list[i].first = name;
            } else {
                const double share = 1.0 / double(list.size() + 1);
                for (auto& l : list) l.second *= 1.0 - share;
                list.emplace_back(name, share);
            }
            writeLayers(list);
        };
        browse(std::move(request));
    };
    for (std::size_t i = 0; i < layers.size(); ++i) {
        const float rowW = layers.size() > 1 ? w - 36 : w;
        const float rowY = y;
        if (pickRow(ui, id("ground.soil.layer", int(i)), x, y, rowW, "Texture " + std::to_string(i + 1), layers[i].first))
            pickTexture(i);
        if (layers.size() > 1 && ui.button(id("ground.soil.layer.remove", int(i)), {x + w - 28, rowY, 28, 28}, "x")) {
            auto list = layers;
            list.erase(list.begin() + std::ptrdiff_t(i));
            writeLayers(list);
            break;
        }
        if (layers.size() > 1) {
            float share = float(layers[i].second);
            if (sliderRow(ui, id("ground.soil.share", int(i)), x, y, w, "Share", format("%.0f %%", double(share) * 100.0),
                          share, 0.02f, 0.98f)) {
                auto list = layers;
                double rest = 0;
                for (std::size_t k = 0; k < list.size(); ++k)
                    if (k != i) rest += list[k].second;
                list[i].second = share;
                for (std::size_t k = 0; k < list.size(); ++k)
                    if (k != i) list[k].second = rest > 1e-6 ? list[k].second / rest * (1.0 - share)
                                                             : (1.0 - share) / double(list.size() - 1);
                // Rounded shares must still make exactly one.
                double sum = 0;
                for (auto& l : list) sum += (l.second = rounded(l.second, 0.001));
                list.back().second = rounded(list.back().second + (1.0 - sum), 0.001);
                writeLayers(list);
                break;
            }
        }
    }
    if (layers.size() < 3) {
        if (ui.button(id("ground.soil.layer.add"), {x, y, w, 28}, "Add a texture")) pickTexture(layers.size());
        y += 36;
    }

    // Tint.
    {
        const auto* tint = at(own, {"tint"});
        float rgb[3];
        for (int i = 0; i < 3; ++i)
            rgb[i] = float(tint && tint->is_array() && tint->size() == 3 ? (*tint)[std::size_t(i)].get<double>()
                                                                       : resolved.tint[std::size_t(i)]);
        const char* labels3[] = {"Tint red", "Tint green", "Tint blue"};
        bool moved = false;
        for (int i = 0; i < 3; ++i)
            moved |= sliderRow(ui, id("ground.soil.tint", i), x, y, w, labels3[i], format("%.0f %%", double(rgb[i]) * 100.0),
                               rgb[i], 0.5f, 1.5f);
        if (moved)
            editSoil([&](Json& s) { s["tint"] = Json::array({rounded(rgb[0]), rounded(rgb[1]), rounded(rgb[2])}); });
    }

    // The noise that mixes the layers. A named, shared noise becomes this
    // soil's own on the first change, so other soils keep theirs.
    {
        const eb::Noise& n = resolved.noise;
        const auto* mine = at(own, {"noise"});
        if (mine && mine->is_string())
            note(ui, x, y, w, "Mixed by the shared noise \"" + mine->get<std::string>() + "\": a change makes it this soil's own.");
        const auto writeNoise = [this, n](const std::function<void(Json&)>& edit) {
            editSoil([&](Json& s) {
                if (!s.contains("noise") || !s["noise"].is_object()) {
                    Json inline_ = Json::object();
                    inline_["kind"] = eb::kNoiseKindNames[std::size_t(n.kind)];
                    inline_["metres"] = n.metres;
                    inline_["contrast"] = n.contrast;
                    if (n.warp != 0.0) inline_["warp"] = n.warp;
                    if (n.angle != 0.0) inline_["angle"] = n.angle;
                    s["noise"] = inline_;
                }
                edit(s["noise"]);
            });
        };
        if (pickRow(ui, id("ground.soil.noise.kind"), x, y, w, "Mixed by", eb::kNoiseKindNames[std::size_t(n.kind)])) {
            std::vector<std::string> kinds(std::begin(eb::kNoiseKindNames), std::end(eb::kNoiseKindNames));
            choose("Noise that mixes the layers", kinds, int(n.kind), [writeNoise, kinds](int i) {
                writeNoise([&](Json& noise) { noise["kind"] = kinds[std::size_t(i)]; });
            });
        }
        float metres = float(n.metres), contrast = float(n.contrast), warp = float(n.warp), angle = float(n.angle);
        if (sliderRow(ui, id("ground.soil.noise.metres"), x, y, w, "Patch size", format("%.1f m", metres), metres, 0.5f,
                      96.0f, true))
            writeNoise([&](Json& noise) { noise["metres"] = rounded(metres, 0.1); });
        if (sliderRow(ui, id("ground.soil.noise.contrast"), x, y, w, "Edge hardness", format("%.0f %%", contrast * 100.0),
                      contrast, 0.0f, 1.0f))
            writeNoise([&](Json& noise) { noise["contrast"] = rounded(contrast, 0.01); });
        if (sliderRow(ui, id("ground.soil.noise.warp"), x, y, w, "Warp", format("%.2f", warp), warp, 0.0f, 2.0f))
            writeNoise([&](Json& noise) { noise["warp"] = rounded(warp, 0.01); });
        if (n.kind == eb::NoiseKind::Streaks &&
            sliderRow(ui, id("ground.soil.noise.angle"), x, y, w, "Streak angle", format("%.0f\u00b0", angle), angle, 0.0f,
                      180.0f))
            writeNoise([&](Json& noise) { noise["angle"] = rounded(angle, 1.0); });
    }
    note(ui, x, y, w, "Soils are shared: every category that names this one changes with it. A new texture is new "
                      "shader code, built once when it is chosen.");
}

void TerrainPanel::drawRock(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "ROCK FACES", 5, &showRock_);
    if (!showRock_) return;
    const auto registry = eb::active();
    if (!registry || registry->rocks().empty()) {
        note(ui, x, y, w, "No rocks loaded.");
        return;
    }
    if (!registry->rockIndex(rock_)) {
        const auto* c = registry->category(category_);
        rock_ = c && registry->rockIndex(c->slopeRock) ? c->slopeRock : registry->rocks().front().name;
    }
    if (pickRow(ui, id("ground.rock.pick"), x, y, w, "Rock", rock_)) {
        AssetBrowser::Request request;
        request.title = "Rock to edit";
        request.kinds = {AssetKind::Rock};
        request.current = rock_;
        request.apply = [this](const AssetItem* item, int) { if (item) rock_ = item->name; };
        browse(std::move(request));
    }
    const eb::Rock& resolved = registry->rocks()[*registry->rockIndex(rock_)];
    const std::string rock = rock_;
    const auto edit = [this, rock](const std::function<void(Json&)>& change) {
        editEntry(rocks_, "rocks", rock, "the rock " + rock, change);
    };
    if (pickRow(ui, id("ground.rock.layer"), x, y, w, "Face texture", resolved.layer)) {
        AssetBrowser::Request request;
        request.title = "Face texture of the rock " + rock;
        request.kinds = {AssetKind::Texture};
        request.current = resolved.layer;
        request.apply = [this, edit](const AssetItem* item, int) {
            if (!item) return;
            const auto name = ensureLayer(*item);
            if (!name.empty()) edit([&](Json& r) { r["layer"] = name; });
        };
        browse(std::move(request));
    }
    if (pickRow(ui, id("ground.rock.scree"), x, y, w, "Scree", resolved.scree.empty() ? "none" : resolved.scree)) {
        AssetBrowser::Request request;
        request.title = "Soil at the foot of the rock " + rock;
        request.kinds = {AssetKind::Soil};
        request.current = resolved.scree;
        request.extras = {"none"};
        request.apply = [edit](const AssetItem* item, int) {
            edit([&](Json& r) {
                if (item) r["scree"] = item->name;
                else r.erase("scree");
            });
        };
        browse(std::move(request));
    }
    float metres = float(resolved.strataMetres), tilt = float(resolved.strataTilt);
    const bool movedMetres = sliderRow(ui, id("ground.rock.strata"), x, y, w, "Strata",
                                       metres < 0.05f ? std::string("none") : format("every %.1f m", metres), metres,
                                       0.0f, 12.0f);
    const bool movedTilt = sliderRow(ui, id("ground.rock.tilt"), x, y, w, "Strata lean", format("%.2f", tilt), tilt,
                                     -0.5f, 0.5f);
    if (movedMetres || movedTilt)
        edit([&](Json& r) {
            if (metres < 0.05f) {
                r.erase("strata");
                return;
            }
            if (!r.contains("strata") || !r["strata"].is_object()) r["strata"] = Json::object();
            r["strata"]["metres"] = rounded(metres, 0.1);
            r["strata"]["tilt"] = rounded(tilt, 0.01);
        });
    note(ui, x, y, w, "Rocks are shared: every category that names this one on its slopes changes with it.");
}

void TerrainPanel::drawDecorations(ui::Ui& ui, float x, float& y, float w) {
    heading(ui, x, y, w, "DECORATIONS", 6, &showDecor_);
    if (!showDecor_) return;
    const auto registry = eb::active();
    if (!registry) {
        note(ui, x, y, w, "content/config/terrain did not load.");
        return;
    }
    const bool known = decorKind_ == AssetKind::Plant ? registry->plantIndex(decor_).has_value()
                     : decorKind_ == AssetKind::Prop  ? registry->propIndex(decor_).has_value()
                                                      : registry->decalIndex(decor_).has_value();
    if (!known) {
        if (!registry->props().empty()) { decorKind_ = AssetKind::Prop; decor_ = registry->props().front().name; }
        else if (!registry->plants().empty()) { decorKind_ = AssetKind::Plant; decor_ = registry->plants().front().name; }
        else {
            note(ui, x, y, w, "No plants, props or decals loaded.");
            return;
        }
    }
    const char* kindName = decorKind_ == AssetKind::Plant ? "plant" : decorKind_ == AssetKind::Prop ? "prop" : "decal";
    if (pickRow(ui, id("ground.decor.pick"), x, y, w, "Edit", decor_ + "  (" + kindName + ")")) {
        AssetBrowser::Request request;
        request.title = "Plant, prop or decal to edit";
        request.kinds = {AssetKind::Prop, AssetKind::Plant, AssetKind::Decal};
        request.current = decor_;
        request.apply = [this](const AssetItem* item, int) {
            if (!item) return;
            decorKind_ = item->kind;
            decor_ = item->name;
        };
        browse(std::move(request));
    }
    const std::string name = decor_;
    File* file = decorKind_ == AssetKind::Plant ? &plants_ : decorKind_ == AssetKind::Prop ? &props_ : &decals_;
    const char* list = decorKind_ == AssetKind::Plant ? "plants" : decorKind_ == AssetKind::Prop ? "props" : "decals";
    const auto edit = [this, file, list, name, kindName](const std::function<void(Json&)>& change) {
        editEntry(*file, list, name, std::string("the ") + kindName + " " + name, change);
    };
    // A scene model of the build, written as the entry's "model".
    const auto pickModel = [this, edit, name](const std::string& current) {
        AssetBrowser::Request request;
        request.title = "Model of " + name;
        request.kinds = {AssetKind::Model};
        request.current = current;
        request.apply = [edit](const AssetItem* item, int) {
            if (item) edit([&](Json& e) { e["model"] = item->name; });
        };
        browse(std::move(request));
    };
    // A [smallest, largest] pair, kept in order.
    const auto range = [&](const char* key, const char* label, double low0, double high0, int index) {
        float low = float(low0), high = float(high0);
        const bool a = sliderRow(ui, id("ground.decor.range", index), x, y, w, std::string(label) + ", smallest",
                                 format("x %.2f", low), low, 0.05f, 4.0f, true);
        const bool b = sliderRow(ui, id("ground.decor.range", index + 1), x, y, w, std::string(label) + ", largest",
                                 format("x %.2f", high), high, 0.05f, 4.0f, true);
        if (a || b) {
            if (a && low > high) high = low;
            if (b && high < low) low = high;
            const std::string k = key;
            edit([&](Json& e) { e[k] = Json::array({rounded(low, 0.01), rounded(high, 0.01)}); });
        }
    };
    if (decorKind_ == AssetKind::Plant) {
        const eb::Plant& p = registry->plants()[*registry->plantIndex(name)];
        if (pickRow(ui, id("ground.decor.model"), x, y, w, "Model", p.model)) pickModel(p.model);
        range("height", "Height", p.heightMin, p.heightMax, 0);
        note(ui, x, y, w, "Trees and shrubs the forest biomes name. Height against the model's own.");
    } else if (decorKind_ == AssetKind::Prop) {
        const eb::Prop& p = registry->props()[*registry->propIndex(name)];
        if (pickRow(ui, id("ground.decor.model"), x, y, w, "Model", p.model)) pickModel(p.model);
        range("scale", "Scale", p.scaleMin, p.scaleMax, 0);
        note(ui, x, y, w, "Logs, stumps, stones and the like the forest and decor biomes scatter, a hectare.");
    } else {
        const eb::Decal& d = registry->decals()[*registry->decalIndex(name)];
        ui.text(x, y, std::string("Kind: ") + eb::kDecalKindNames[std::size_t(d.kind)], ui.theme().labelSoft, 0.86f);
        y += ui.textHeight(0.86f) + 8;
        if (d.kind == eb::DecalKind::Texture &&
            pickRow(ui, id("ground.decor.texture"), x, y, w, "Texture", d.texture)) {
            AssetBrowser::Request request;
            request.title = "Texture of the decal " + name;
            request.kinds = {AssetKind::Texture};
            request.current = d.texture;
            request.apply = [this, edit](const AssetItem* item, int) {
                if (!item) return;
                const auto layer = ensureLayer(*item);
                if (!layer.empty()) edit([&](Json& e) { e["texture"] = layer; });
            };
            browse(std::move(request));
        }
        if (d.kind == eb::DecalKind::Instance &&
            pickRow(ui, id("ground.decor.model"), x, y, w, "Model", d.model))
            pickModel(d.model);
        float density = float(d.density);
        if (sliderRow(ui, id("ground.decor.density"), x, y, w, "Cells that hold one",
                      format("%.0f %%", double(density) * 100.0), density, 0.0f, 1.0f))
            edit([&](Json& e) { e["density"] = rounded(density, 0.01); });
        note(ui, x, y, w, "A decal set the categories and decor biomes name, with a weight on its density.");
    }
}

} // namespace client

