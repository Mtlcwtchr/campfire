#include "game/ui/editor.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <SDL3_image/SDL_image.h>

namespace ui {
namespace {

// Every colour in the theme, by name, so the editor can list them without
// knowing what any of them is for. One line each: adding a colour to the theme
// adds it to the editor.
struct ColourField {
    const char* name;
    Colour Theme::*member;
};

const std::vector<ColourField>& colourFields() {
    static const std::vector<ColourField> kFields = {
            {"bar top", &Theme::barTop},         {"bar bottom", &Theme::barBottom},
            {"bar edge", &Theme::barEdge},       {"slot", &Theme::slot},
            {"slot hot", &Theme::slotHot},       {"slot down", &Theme::slotDown},
            {"slot edge", &Theme::slotEdge},     {"paper", &Theme::paper},
            {"paper edge", &Theme::paperEdge},   {"paper shade", &Theme::paperShade},
            {"ink", &Theme::ink},                {"ink soft", &Theme::inkSoft},
            {"label", &Theme::label},            {"label soft", &Theme::labelSoft},
            {"accent", &Theme::accent},          {"danger", &Theme::danger},
            {"good", &Theme::good},              {"warn", &Theme::warn},
            {"bad", &Theme::bad},                {"gauge", &Theme::gauge},
            {"selection", &Theme::selection},
    };
    return kFields;
}

struct MetricField {
    const char* name;
    float Theme::*member;
    float step, low, high;
};

const std::vector<MetricField>& metricFields() {
    static const std::vector<MetricField> kFields = {
            {"corner radius", &Theme::radius, 1.0f, 0.0f, 16.0f},
            {"row height", &Theme::rowHeight, 1.0f, 10.0f, 40.0f},
            {"padding", &Theme::pad, 1.0f, 0.0f, 24.0f},
            {"text size", &Theme::textScale, 0.05f, 0.6f, 2.0f},
    };
    return kFields;
}

std::string twoPlaces(double v) {
    char out[32];
    std::snprintf(out, sizeof out, "%.2f", v);
    return out;
}

} // namespace

bool saveTheme(const Theme& theme, const std::filesystem::path& file) {
    nlohmann::json j;
    for (const auto& field : colourFields()) {
        const Colour& c = theme.*(field.member);
        j["colours"][field.name] = {c.r, c.g, c.b, c.a};
    }
    for (const auto& field : metricFields()) j["metrics"][field.name] = theme.*(field.member);
    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return true;
}

bool loadTheme(Theme& theme, const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return false;
    }
    if (j.contains("colours")) {
        for (const auto& field : colourFields()) {
            if (!j["colours"].contains(field.name)) continue;
            const auto& v = j["colours"][field.name];
            if (!v.is_array() || v.size() < 4) continue;
            theme.*(field.member) = Colour{v[0].get<float>(), v[1].get<float>(), v[2].get<float>(),
                                           v[3].get<float>()};
        }
    }
    if (j.contains("metrics")) {
        for (const auto& field : metricFields())
            if (j["metrics"].contains(field.name))
                theme.*(field.member) = j["metrics"][field.name].get<float>();
    }
    return true;
}

namespace {

void drawThemeTab(Ui& ui, Editor& editor, const Rect& body) {
    Theme& theme = ui.theme();
    const float rowHeight = 22;
    float y = body.y;

    ui.text(body.x, y, "COLOURS", theme.labelSoft, 0.9f);
    y += 18;
    // One block per colour: swatch, name, and that colour's own controls, so a
    // row's buttons cannot be mistaken for the next column's. Blocks are laid
    // across the width and wrapped, which is what makes the list fit whatever
    // window it is given.
    const float blockWidth = 300;
    const int columns = std::max(1, static_cast<int>(body.w / blockWidth));
    for (std::size_t i = 0; i < colourFields().size(); ++i) {
        const auto& field = colourFields()[i];
        Colour& colour = theme.*(field.member);
        const float column = static_cast<float>(i % columns) * blockWidth;
        const float row = y + static_cast<float>(i / columns) * rowHeight;
        const Rect swatch{body.x + column, row + 3, 22, 14};
        ui.rect(swatch, colour);
        ui.border(swatch, theme.labelSoft.withAlpha(0.5f));
        ui.text(swatch.right() + 8, row + 5, field.name, theme.label, 0.85f);

        // Three channels, each nudged by a pair of small buttons. Crude on
        // purpose: a colour wheel is a week of work and this is a tuning tool.
        const char* names[3] = {"r", "g", "b"};
        float* channels[3] = {&colour.r, &colour.g, &colour.b};
        for (int c = 0; c < 3; ++c) {
            const float x = body.x + column + 150 + c * 46;
            const Rect down{x, row + 2, 20, 16};
            const Rect up{x + 21, row + 2, 20, 16};
            if (ui.button(widgetId("editor.colour.down", static_cast<int>(i) * 4 + c), down,
                          names[c]))
                *channels[c] = std::clamp(*channels[c] - 0.05f, 0.0f, 1.0f);
            if (ui.button(widgetId("editor.colour.up", static_cast<int>(i) * 4 + c), up, "+"))
                *channels[c] = std::clamp(*channels[c] + 0.05f, 0.0f, 1.0f);
        }
    }
    y += ((colourFields().size() + columns - 1) / columns) * rowHeight + 14;

    ui.text(body.x, y, "MEASURES", theme.labelSoft, 0.9f);
    y += 18;
    for (std::size_t i = 0; i < metricFields().size(); ++i) {
        const auto& field = metricFields()[i];
        float& value = theme.*(field.member);
        ui.text(body.x, y + 4, field.name, theme.label, 0.85f);
        ui.text(body.x + 160, y + 4, twoPlaces(value), theme.accent, 0.85f);
        const Rect less{body.x + 230, y, 20, 18};
        const Rect more{body.x + 254, y, 20, 18};
        if (ui.button(widgetId("editor.metric.less", static_cast<int>(i)), less, "-"))
            value = std::clamp(value - field.step, field.low, field.high);
        if (ui.button(widgetId("editor.metric.more", static_cast<int>(i)), more, "+"))
            value = std::clamp(value + field.step, field.low, field.high);
        y += 22;
    }

    y += 8;
    if (ui.button(widgetId("editor.theme.save"), {body.x, y, 120, 24}, "Save theme"))
        editor.message = saveTheme(theme, "content/config/ui_theme.json")
                                 ? "theme written to content/config/ui_theme.json"
                                 : "could not write the theme";
    if (ui.button(widgetId("editor.theme.load"), {body.x + 130, y, 120, 24}, "Reload"))
        editor.message = loadTheme(theme, "content/config/ui_theme.json") ? "theme reloaded"
                                                                         : "no theme file to read";
    if (ui.button(widgetId("editor.theme.reset"), {body.x + 260, y, 120, 24}, "Defaults")) {
        theme = Theme{};
        editor.message = "theme back to what the code says";
    }
}

// --- what the ground is made of ------------------------------------------
//
// The same table the renderer reads every frame and the bakery reads when it
// bakes, so a row here is a row there. Which half a number belongs to is
// written next to it: moving the scale of a material shows in the next frame of
// a game that is already open, and moving its tint shows after the textures are
// baked again. Saying so beside the button is cheaper than a person wondering
// why nothing happened.
struct GroundKnob {
    const char* name;
    float step, low, high;
    bool live;                                    // shows in the next frame, or after baking
    float& (*of)(content::GroundMaterial&);
};

const GroundKnob kGroundKnobs[] = {
        {"metres to a turn", 0.1f, 0.5f, 200.0f, true,
         [](content::GroundMaterial& m) -> float& { return m.metresPerTurn; }},
        {"border width", 0.02f, 0.0f, 1.0f, true,
         [](content::GroundMaterial& m) -> float& { return m.blendWidth; }},
        {"tearing", 0.02f, 0.0f, 1.5f, true,
         [](content::GroundMaterial& m) -> float& { return m.tear; }},
        {"tear size, m", 0.2f, 0.2f, 40.0f, true,
         [](content::GroundMaterial& m) -> float& { return m.tearMetres; }},
        {"tint red", 0.02f, 0.0f, 2.0f, false,
         [](content::GroundMaterial& m) -> float& { return m.tint[0]; }},
        {"tint green", 0.02f, 0.0f, 2.0f, false,
         [](content::GroundMaterial& m) -> float& { return m.tint[1]; }},
        {"tint blue", 0.02f, 0.0f, 2.0f, false,
         [](content::GroundMaterial& m) -> float& { return m.tint[2]; }},
        {"keeps its colour", 0.02f, 0.0f, 1.0f, false,
         [](content::GroundMaterial& m) -> float& { return m.keepColour; }},
};
constexpr int kGroundKnobCount = static_cast<int>(sizeof(kGroundKnobs) / sizeof(kGroundKnobs[0]));

// Where the file is, if nobody has said. content/ is found by walking up the
// way everything else in this project finds it, so the editor started from the
// build directory and the editor started from the source tree edit the same
// file - which matters here more than elsewhere, because the game is reading it.
std::filesystem::path defaultGroundFile(const std::filesystem::path& contentDir) {
    return contentDir / "config" / "ground.json";
}

std::filesystem::path defaultMaterialFile(const std::filesystem::path& contentDir) {
    return contentDir / "config" / "terrain_materials.json";
}

const char* materialIdFor(const std::string& groundName) {
    if (groundName == "grass") return "grass_lush";
    if (groundName == "dirt") return "soil_base";
    if (groundName == "sand") return "sand_dry";
    if (groundName == "rock") return "rock_ground";
    if (groundName == "marsh") return "mud_wet";
    if (groundName == "snow") return "snow_clean";
    return nullptr;
}

float jsonFloat(const nlohmann::json& j, const char* key, float fallback) {
    return j.contains(key) && j[key].is_number() ? j[key].get<float>() : fallback;
}

bool loadMaterialLibrary(const std::filesystem::path& file, std::vector<EditorMaterial>& out) {
    std::ifstream in(file);
    if (!in) return false;
    nlohmann::json root;
    try { in >> root; } catch (const std::exception&) { return false; }
    if (!root.is_array()) return false;
    out.clear();
    for (const auto& j : root) {
        if (!j.contains("id") || !j.contains("group")) continue;
        EditorMaterial m;
        m.id = j["id"].get<std::string>();
        m.group = j["group"].get<std::string>();
        m.worldScale = jsonFloat(j, "world_scale", m.worldScale);
        m.saturation = jsonFloat(j, "saturation", m.saturation);
        m.normalStrength = jsonFloat(j, "normal_strength", m.normalStrength);
        m.aoStrength = jsonFloat(j, "ao_strength", m.aoStrength);
        m.roughnessMultiplier = jsonFloat(j, "roughness_multiplier", m.roughnessMultiplier);
        m.heightStrength = jsonFloat(j, "height_strength", m.heightStrength);
        m.slopePreference = jsonFloat(j, "slope_preference", m.slopePreference);
        m.moistureResponse = jsonFloat(j, "moisture_response", m.moistureResponse);
        m.snowCompatibility = jsonFloat(j, "snow_compatibility", m.snowCompatibility);
        if (j.contains("tint") && j["tint"].is_array() && j["tint"].size() >= 3)
            for (int i = 0; i < 3; ++i) m.tint[i] = j["tint"][i].get<float>();
        out.push_back(std::move(m));
    }
    return true;
}

bool saveMaterialLibrary(const std::filesystem::path& file, const std::vector<EditorMaterial>& values) {
    std::ifstream in(file);
    if (!in) return false;
    nlohmann::json root;
    try { in >> root; } catch (const std::exception&) { return false; }
    if (!root.is_array()) return false;
    for (auto& j : root) {
        if (!j.contains("id")) continue;
        const auto it = std::find_if(values.begin(), values.end(), [&](const EditorMaterial& m) {
            return m.id == j["id"].get<std::string>();
        });
        if (it == values.end()) continue;
        j["group"] = it->group;
        j["world_scale"] = it->worldScale;
        j["tint"] = {it->tint[0], it->tint[1], it->tint[2]};
        j["saturation"] = it->saturation;
        j["normal_strength"] = it->normalStrength;
        j["ao_strength"] = it->aoStrength;
        j["roughness_multiplier"] = it->roughnessMultiplier;
        j["height_strength"] = it->heightStrength;
        j["slope_preference"] = it->slopePreference;
        j["moisture_response"] = it->moistureResponse;
        j["snow_compatibility"] = it->snowCompatibility;
    }
    const std::filesystem::path backup = file.string() + ".bak";
    std::error_code ec;
    std::filesystem::copy_file(file, backup, std::filesystem::copy_options::overwrite_existing, ec);
    std::ofstream out(file);
    if (!out) return false;
    out << root.dump(2) << "\n";
    return true;
}

EditorMaterial* selectedMaterial(Editor& editor) {
    if (editor.groundChosen < 0 || editor.groundChosen >= static_cast<int>(editor.ground.size()))
        return nullptr;
    const char* id = materialIdFor(editor.ground[editor.groundChosen].name);
    if (!id) return nullptr;
    auto it = std::find_if(editor.materials.begin(), editor.materials.end(),
                           [&](const EditorMaterial& m) { return m.id == id; });
    return it == editor.materials.end() ? nullptr : &*it;
}

const char* textureSuffix(int target) {
    static const char* suffixes[] = {"diffuse", "normal", "rough", "ao", "displacement", "mask"};
    return target >= 0 && target < 6 ? suffixes[target] : nullptr;
}

std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

void ensureMaterialLibrary(Editor& editor, const std::filesystem::path& contentDir) {
    if (!editor.materialFile.empty()) return;
    editor.materialFile = defaultMaterialFile(contentDir);
    editor.materials = editor.materialsAsRead = {};
    loadMaterialLibrary(editor.materialFile, editor.materials);
    editor.materialsAsRead = editor.materials;
}

SDL_Texture* materialPreview(Ui& ui, const std::filesystem::path& file) {
    static std::unordered_map<std::string, SDL_Texture*> cache;
    const std::string key = file.generic_string();
    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;
    if (!std::filesystem::exists(file)) return nullptr;
    SDL_Texture* texture = IMG_LoadTexture(ui.renderer(), file.c_str());
    if (texture != nullptr) cache.emplace(key, texture);
    return texture;
}

void drawGroundTab(Ui& ui, Editor& editor, const std::filesystem::path& contentDir,
                   const Rect& body) {
    const Theme& theme = ui.theme();
    ensureMaterialLibrary(editor, contentDir);
    if (editor.groundFile.empty()) editor.groundFile = defaultGroundFile(contentDir);
    if (editor.ground.empty()) {
        editor.ground = content::loadGroundMaterials(editor.groundFile);
        editor.groundAsRead = editor.ground;
    }

    float y = body.y;
    ui.text(body.x, y, ui.fit(editor.groundFile.generic_string(), body.w - 20, 0.85f), theme.accent,
            0.85f);
    y += 16;
    ui.text(body.x, y,
            "drop a ground.json on this window to edit that one instead", theme.labelSoft, 0.8f);
    y += 20;

    // The list of materials on the left. All of them, not only the six the
    // ground blends between: the rest are baked too, and a person tuning the
    // tint of a cliff face should not have to guess which file it lives in.
    // Both boxes are as tall as what is in them rather than as tall as the
    // window. A tuning page is a short page, and a short page in a tall empty
    // box reads as a page that failed to load something.
    const int count = static_cast<int>(editor.ground.size());
    const float wanted = 8 + std::max(count, 1) * 18.0f;
    const Rect list{body.x, y, 170, std::min(wanted, body.bottom() - y - 34)};
    ui.rect(list, theme.barBottom.withAlpha(0.5f));
    const int rows = static_cast<int>((list.h - 8) / 18);
    editor.groundScroll = std::clamp(editor.groundScroll, 0, std::max(0, count - rows));
    if (ui.hovered(list))
        editor.groundScroll = std::clamp(editor.groundScroll - static_cast<int>(ui.input().wheel),
                                         0, std::max(0, count - rows));
    for (int i = 0; i < rows && editor.groundScroll + i < count; ++i) {
        const int index = editor.groundScroll + i;
        const Rect row{list.x + 4, list.y + 4 + i * 18, list.w - 8, 17};
        const bool on = editor.groundChosen == index;
        if (on) ui.rect(row, theme.slotHot);
        if (ui.hovered(row) && ui.input().pressed) editor.groundChosen = index;
        const bool blended = index < static_cast<int>(content::kBlendedMaterials);
        ui.text(row.x + 6, row.y + 4, editor.ground[index].name,
                on ? theme.label : (blended ? theme.labelSoft : theme.labelSoft.withAlpha(0.7f)),
                0.9f);
        if (!blended) ui.textRight(row.right() - 6, row.y + 4, "baked", theme.labelSoft, 0.7f);
    }
    ui.claim(list);

    editor.groundChosen = std::clamp(editor.groundChosen, 0, std::max(0, count - 1));
    if (count == 0) {
        ui.text(list.right() + 16, list.y + 10, "no materials in that file", theme.bad, 0.9f);
        return;
    }
    content::GroundMaterial& m = editor.ground[editor.groundChosen];

    // The numbers of the chosen one, each with the pair of buttons that moves
    // it. The same crude pair as the theme tab, and for the same reason: this is
    // a tuning tool, and a slider that has to be dragged to a value is worse at
    // repeating a value than a button that steps to it.
    const Rect detail{list.right() + 12, list.y, body.w - list.w - 12,
                      body.bottom() - list.y - 34};
    ui.rect(detail, theme.barBottom.withAlpha(0.5f));
    float dy = detail.y + 10;
    ui.text(detail.x + 12, dy, m.name, theme.accent, 1.2f);
    ui.textRight(detail.right() - 12, dy + 6, m.set + "/" + m.variant + "  PBR source maps",
                 theme.labelSoft, 0.85f);
    dy += 28;

    for (int i = 0; i < kGroundKnobCount; ++i) {
        const GroundKnob& knob = kGroundKnobs[i];
        ui.text(detail.x + 12, dy + 4, knob.name, theme.label, 0.85f);
        float& value = knob.of(m);
        ui.text(detail.x + 170, dy + 4, twoPlaces(value), theme.accent, 0.85f);
        const Rect less{detail.x + 232, dy, 22, 18};
        const Rect more{detail.x + 258, dy, 22, 18};
        if (ui.button(widgetId("editor.ground.less", i), less, "-"))
            value = std::clamp(value - knob.step, knob.low, knob.high);
        if (ui.button(widgetId("editor.ground.more", i), more, "+"))
            value = std::clamp(value + knob.step, knob.low, knob.high);
        ui.text(detail.x + 292, dy + 4, knob.live ? "next frame" : "needs baking",
                knob.live ? theme.good : theme.warn, 0.8f);
        dy += 22;
    }

    EditorMaterial* pbr = selectedMaterial(editor);
    if (pbr != nullptr) {
        ui.text(detail.x + 12, dy + 4, "PBR / SHADER PROPERTIES", theme.accent, 0.95f);
        dy += 24;
        struct PbrKnob { const char* name; float* value; float step, low, high; };
        PbrKnob knobs[] = {
                {"world scale", &pbr->worldScale, 0.1f, 0.1f, 100.0f},
                {"normal strength", &pbr->normalStrength, 0.02f, 0.0f, 2.0f},
                {"AO strength", &pbr->aoStrength, 0.02f, 0.0f, 2.0f},
                {"roughness multiplier", &pbr->roughnessMultiplier, 0.02f, 0.0f, 2.0f},
                {"height weight", &pbr->heightStrength, 0.02f, 0.0f, 2.0f},
                {"slope weight", &pbr->slopePreference, 0.02f, 0.0f, 1.0f},
                {"moisture response", &pbr->moistureResponse, 0.02f, 0.0f, 1.0f},
                {"snow compatibility", &pbr->snowCompatibility, 0.02f, 0.0f, 1.0f},
        };
        for (int i = 0; i < static_cast<int>(std::size(knobs)); ++i) {
            auto& k = knobs[i];
            ui.text(detail.x + 12, dy + 4, k.name, theme.label, 0.82f);
            ui.text(detail.x + 170, dy + 4, twoPlaces(*k.value), theme.accent, 0.82f);
            if (ui.button(widgetId("editor.pbr.less", i), {detail.x + 232, dy, 22, 18}, "-"))
                *k.value = std::clamp(*k.value - k.step, k.low, k.high);
            if (ui.button(widgetId("editor.pbr.more", i), {detail.x + 258, dy, 22, 18}, "+"))
                *k.value = std::clamp(*k.value + k.step, k.low, k.high);
            dy += 20;
        }

        ui.text(detail.x + 12, dy + 3, "TEXTURE CHANNELS — click a slot, then drop an image",
                theme.accent, 0.9f);
        dy += 22;
        static const char* labels[] = {"albedo", "normal", "roughness", "AO", "height", "mask"};
        for (int i = 0; i < 6; ++i) {
            const Rect slot{detail.x + 12, dy, detail.w - 24, 18};
            if (editor.textureTarget == i) ui.rect(slot, theme.slotHot);
            if (ui.hovered(slot) && ui.input().pressed) editor.textureTarget = i;
            ui.text(slot.x + 5, slot.y + 3, labels[i], theme.label, 0.78f);
            const std::string expected = pbr->id + "_" + textureSuffix(i) + ".png";
            const std::filesystem::path source = contentDir.parent_path() / "assets" / "terrain" /
                                                  pbr->group / "src" / expected;
            if (SDL_Texture* preview = materialPreview(ui, source))
                ui.image({slot.x + 72, slot.y + 1, 16, 16}, preview);
            ui.text(slot.x + 94, slot.y + 3, ui.fit(expected, slot.w - 102, 0.75f),
                    editor.textureTarget == i ? theme.accent : theme.labelSoft, 0.75f);
            dy += 20;
        }
        ui.text(detail.x + 12, dy + 2, "source: assets/terrain/" + pbr->group + "/src/" + pbr->id + "_*",
                theme.labelSoft, 0.72f);
    } else {
        ui.text(detail.x + 12, dy + 8, "This material has no active PBR slot.", theme.labelSoft, 0.85f);
    }

    ui.claim(detail);

    // Saving, and going back. Two ways back, because they are different
    // questions: undo what I have done since the file was read, and undo
    // everything that was ever done to the file.
    const float by = body.bottom() - 26;
    if (ui.button(widgetId("editor.ground.save"), {body.x, by, 130, 24}, "Save ground")) {
        if (content::saveGroundMaterials(editor.groundFile, editor.ground)) {
            editor.groundAsRead = editor.ground;
            editor.message = "written to " + editor.groundFile.generic_string() +
                             " - a running game picks it up within the second";
        } else {
            editor.message = "could not write " + editor.groundFile.generic_string();
        }
    }
    if (ui.button(widgetId("editor.ground.revert"), {body.x + 140, by, 130, 24}, "Revert")) {
        editor.ground = editor.groundAsRead;
        editor.message = "back to what was read";
    }
    if (ui.button(widgetId("editor.ground.reload"), {body.x + 280, by, 130, 24}, "Reread file")) {
        std::vector<content::GroundMaterial> fresh;
        if (content::readGroundMaterials(editor.groundFile, fresh)) {
            editor.ground = fresh;
            editor.groundAsRead = fresh;
            editor.message = "read again from " + editor.groundFile.filename().generic_string();
        } else {
            editor.message = "could not read " + editor.groundFile.generic_string();
        }
    }
    const std::filesystem::path backup = editor.groundFile.string() + ".bak";
    std::error_code ec;
    const bool haveBackup = std::filesystem::exists(backup, ec);
    if (ui.button(widgetId("editor.ground.backup"), {body.x + 420, by, 130, 24}, "Restore",
                  haveBackup)) {
        std::vector<content::GroundMaterial> fresh;
        if (content::readGroundMaterials(backup, fresh)) {
            editor.ground = fresh;
            editor.groundAsRead = fresh;
            editor.message = "ground restored from .bak";
        } else {
            editor.message = "the .bak beside that file is not a table";
        }
    }
    if (!haveBackup)
        ui.text(body.x + 558, by + 6, "no .bak yet - the first save makes one", theme.labelSoft,
                0.8f);

    const float by2 = by - 30;
    const std::filesystem::path materialBackup = editor.materialFile.string() + ".bak";
    const bool haveMaterialBackup = std::filesystem::exists(materialBackup, ec);
    if (ui.button(widgetId("editor.material.save"), {body.x, by2, 130, 24}, "Save material")) {
        if (saveMaterialLibrary(editor.materialFile, editor.materials)) {
            editor.materialsAsRead = editor.materials;
            editor.message = "material properties saved (backup updated)";
        } else editor.message = "could not write terrain_materials.json";
    }
    if (ui.button(widgetId("editor.material.backup"), {body.x + 140, by2, 130, 24}, "Backup all")) {
        std::error_code copyError;
        std::filesystem::copy_file(editor.materialFile, materialBackup,
                                   std::filesystem::copy_options::overwrite_existing, copyError);
        std::filesystem::copy_file(editor.groundFile, backup,
                                   std::filesystem::copy_options::overwrite_existing, copyError);
        editor.message = copyError ? "could not create backup" : "ground + material backup saved";
    }
    if (ui.button(widgetId("editor.material.restore"), {body.x + 280, by2, 130, 24}, "Restore all",
                  haveMaterialBackup)) {
        std::error_code restoreError;
        std::filesystem::copy_file(materialBackup, editor.materialFile,
                                   std::filesystem::copy_options::overwrite_existing, restoreError);
        if (!restoreError && loadMaterialLibrary(editor.materialFile, editor.materials)) {
            editor.materialsAsRead = editor.materials;
            editor.message = "material properties restored";
        } else editor.message = "could not restore terrain_materials.json";
    }
    if (ui.button(widgetId("editor.material.bake"), {body.x + 420, by2, 130, 24}, "Bake")) {
        EditorMaterial* current = selectedMaterial(editor);
        if (current == nullptr) {
            editor.message = "select one of the six active materials before baking";
        } else if (!saveMaterialLibrary(editor.materialFile, editor.materials)) {
            editor.message = "save failed; bake was not started";
        } else {
            const std::filesystem::path repo = contentDir.parent_path();
            const std::string command = "cd " + shellQuote(repo.generic_string()) +
                                        " && python3 tools/pack_terrain.py --only " + current->id;
            const int result = std::system(command.c_str());
            editor.message = result == 0 ? "bake complete: packed PBR maps refreshed"
                                         : "bake failed; see the terminal output";
        }
    }
    if (!haveMaterialBackup)
        ui.text(body.x + 558, by2 + 6, "no material backup yet", theme.labelSoft, 0.8f);
}

// A file dropped on the window. Which tab it is for is decided by what is in
// it rather than by which tab is showing: a person dragging a ground.json means
// the ground whichever page they were looking at.
void takeDropped(Ui&, Editor& editor, const std::filesystem::path& contentDir) {
    const std::filesystem::path file = editor.dropped;
    editor.dropped.clear();
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        editor.message = "that is not a file: " + file.generic_string();
        return;
    }

    // A selected channel turns an ordinary desktop drop into an asset import.
    // The baker has a strict source naming convention, so the user never has
    // to rename a downloaded image by hand.
    if (editor.textureTarget >= 0 && editor.textureTarget < 6) {
        EditorMaterial* material = selectedMaterial(editor);
        const std::string extension = file.extension().string();
        if (material != nullptr && (extension == ".png" || extension == ".jpg" ||
                                    extension == ".jpeg" || extension == ".tga" || extension == ".exr")) {
            const std::filesystem::path repo = contentDir.parent_path();
            const std::filesystem::path destination = repo / "assets" / "terrain" / material->group /
                                                      "src" / (material->id + "_" + textureSuffix(editor.textureTarget) + ".png");
            std::filesystem::create_directories(destination.parent_path(), ec);
            std::filesystem::copy_file(file, destination,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) {
                editor.message = "imported " + file.filename().generic_string() + " as " +
                                 material->id + "_" + textureSuffix(editor.textureTarget) +
                                 ".png — press Bake";
                return;
            }
            editor.message = "could not import texture: " + ec.message();
            return;
        }
        if (material == nullptr) {
            editor.message = "select one of the six active materials first";
            return;
        }
        editor.message = "drop an image (PNG, JPG, TGA or EXR) into the selected channel";
        return;
    }
    std::vector<content::GroundMaterial> fresh;
    if (content::readGroundMaterials(file, fresh)) {
        editor.ground = std::move(fresh);
        editor.groundAsRead = editor.ground;
        editor.groundFile = file;
        editor.groundChosen = 0;
        editor.groundScroll = 0;
        editor.tab = Editor::Tab::Ground;
        editor.message = "editing " + file.generic_string() + " - Save writes it back there";
        return;
    }
    if (file.extension() == ".json") {
        // Not a material table, but a file this game is made of: show it. It
        // does not have to be under content/ - a maker comparing two versions of
        // a file keeps the other one somewhere else.
        editor.files.clear();
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                editor.files.push_back(entry.path());
        std::sort(editor.files.begin(), editor.files.end());
        editor.files.insert(editor.files.begin(), file);
        editor.fileChosen = 0;
        editor.fileScroll = 0;
        editor.openFileLines.clear();
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) editor.openFileLines.push_back(line);
        editor.tab = Editor::Tab::Data;
        editor.message = "showing " + file.generic_string();
        return;
    }
    editor.message = "nothing here reads a " + file.extension().generic_string() + " file";
}

void drawContentTab(Ui& ui, Editor& editor, const content::ContentDb& db, const Rect& body) {
    const Theme& theme = ui.theme();
    static const char* kKinds[] = {"Items", "Buildings", "Recipes", "Resources", "Animals"};
    constexpr int kKindCount = static_cast<int>(sizeof(kKinds) / sizeof(kKinds[0]));
    float x = body.x;
    for (int i = 0; i < kKindCount; ++i) {
        const float width = ui.textWidth(kKinds[i]) + 24;
        if (ui.tab(widgetId("editor.kind", i), {x, body.y, width, 22}, kKinds[i],
                   editor.contentKind == i)) {
            editor.contentKind = i;
            editor.scroll = 0;
            editor.chosen = -1;
        }
        x += width;
    }

    // The list on the left, what is in the chosen row on the right.
    const Rect list{body.x, body.y + 28, body.w * 0.38f, body.h - 34};
    const Rect detail{list.right() + 12, list.y, body.w - list.w - 12,
                      std::min(kGroundKnobCount * 22.0f + 78.0f, body.bottom() - list.y - 34)};
    ui.rect(list, theme.barBottom.withAlpha(0.5f));
    ui.rect(detail, theme.barBottom.withAlpha(0.5f));

    std::vector<std::string> names;
    switch (editor.contentKind) {
        case 0: for (const auto& d : db.items()) names.push_back(d.name); break;
        case 1: for (const auto& d : db.buildings()) names.push_back(d.name); break;
        case 2: for (const auto& d : db.recipes()) names.push_back(d.name); break;
        case 4: for (const auto& d : db.animals()) names.push_back(d.name); break;
        default: for (const auto& d : db.resourceNodes()) names.push_back(d.name); break;
    }

    const int rows = static_cast<int>((list.h - 8) / 16);
    editor.scroll = std::clamp(editor.scroll, 0,
                               std::max(0, static_cast<int>(names.size()) - rows));
    if (ui.hovered(list)) editor.scroll = std::clamp(editor.scroll - static_cast<int>(ui.input().wheel) * 3, 0,
                                                     std::max(0, static_cast<int>(names.size()) - rows));
    for (int i = 0; i < rows && editor.scroll + i < static_cast<int>(names.size()); ++i) {
        const int index = editor.scroll + i;
        const Rect row{list.x + 4, list.y + 4 + i * 16, list.w - 8, 15};
        const bool on = editor.chosen == index;
        if (on) ui.rect(row, theme.slotHot);
        if (ui.hovered(row) && ui.input().pressed) editor.chosen = index;
        ui.text(row.x + 4, row.y + 3, ui.fit(names[index], row.w - 8, 0.85f),
                on ? theme.label : theme.labelSoft, 0.85f);
    }
    ui.claim(list);

    // What that thing is, in the terms the simulation reads it.
    float y = detail.y + 8;
    const auto line = [&](const std::string& label, const std::string& value) {
        ui.text(detail.x + 10, y, label, theme.labelSoft, 0.85f);
        ui.text(detail.x + 150, y, value, theme.label, 0.85f);
        y += 15;
    };
    if (editor.chosen >= 0 && editor.chosen < static_cast<int>(names.size())) {
        ui.text(detail.x + 10, y, names[editor.chosen], theme.accent, 1.2f);
        y += 24;
        switch (editor.contentKind) {
            case 0: {
                const auto& d = db.items()[editor.chosen];
                line("label", d.label);
                line("category", std::string(content::itemCategoryName(d.category)));
                line("nutrition", twoPlaces(d.nutrition.toDouble()));
                line("mass, kg", twoPlaces(d.massPerUnit.toDouble()));
                line("spoils in", std::to_string(d.spoilDays) + " days");
                line("insulation", twoPlaces(d.insulation.toDouble()));
                break;
            }
            case 1: {
                const auto& d = db.buildings()[editor.chosen];
                line("label", d.label);
                line("kind", std::string(content::buildingKindName(d.kind)));
                line("footprint", std::to_string(d.footprintWidth) + " x " +
                                          std::to_string(d.footprintDepth));
                line("work", twoPlaces(d.workAmount.toDouble()));
                line("storage", std::to_string(d.storageSlots));
                line("knowledge", d.requiredKnowledge);
                y += 6;
                ui.text(detail.x + 10, y, "made of", theme.labelSoft, 0.85f);
                y += 15;
                for (const auto& m : d.materials)
                    line("  " + db.item(m.item).name, std::to_string(m.count));
                break;
            }
            case 2: {
                const auto& d = db.recipes()[editor.chosen];
                line("work", twoPlaces(d.workAmount.toDouble()));
                line("category", std::string(content::workCategoryName(d.category)));
                line("knowledge", d.requiredKnowledge);
                y += 6;
                ui.text(detail.x + 10, y, "takes", theme.labelSoft, 0.85f);
                y += 15;
                for (const auto& i : d.inputs) line("  " + db.item(i.item).name, std::to_string(i.count));
                ui.text(detail.x + 10, y, "gives", theme.labelSoft, 0.85f);
                y += 15;
                for (const auto& o : d.outputs) line("  " + db.item(o.item).name, std::to_string(o.count));
                break;
            }
            case 4: {
                const auto& d = db.animals()[editor.chosen];
                line("label", d.label);
                line("kept or wild", d.wild ? (d.predator ? "wild, a predator" : "wild") : "herded");
                line("grown at", std::to_string(d.adultAgeDays) + " days");
                line("lives", std::to_string(d.maxAgeDays) + " days");
                line("size, m", twoPlaces(d.sizeMetres.toDouble()));
                if (d.breedIntervalDays > 0)
                    line("breeds every", std::to_string(d.breedIntervalDays) + " days");
                if (d.shearIntervalDays > 0)
                    line("sheared every", std::to_string(d.shearIntervalDays) + " days");
                if (d.milkIntervalDays > 0)
                    line("milked every", std::to_string(d.milkIntervalDays) + " days");
                if (d.wildCarryingCapacity > 0)
                    line("the land holds", std::to_string(d.wildCarryingCapacity));
                if (!d.tamesInto.empty()) line("tamed becomes", d.tamesInto);
                y += 6;
                const auto yields = [&](const char* what, const std::vector<content::IngredientSpec>& list) {
                    if (list.empty()) return;
                    ui.text(detail.x + 10, y, what, theme.labelSoft, 0.85f);
                    y += 15;
                    for (const auto& i : list)
                        line("  " + db.item(i.item).name, std::to_string(i.count));
                };
                yields("shearing gives", d.shearYields);
                yields("milking gives", d.milkYields);
                yields("slaughter gives", d.slaughterYields);
                yields("hunting gives", d.huntYields);
                break;
            }
            default: {
                const auto& d = db.resourceNodes()[editor.chosen];
                line("label", d.label);
                line("kind", std::string(content::resourceKindName(d.kind)));
                line("consumed", d.consumedOnHarvest ? "yes" : "no");
                line("regrows in", std::to_string(d.regrowDays) + " days");
                line("spreads", d.spreadOneIn > 0 ? "1 in " + std::to_string(d.spreadOneIn) : "never");
                line("keep standing", std::to_string(d.keepStandingPercent) + "%");
                break;
            }
        }
    } else {
        ui.text(detail.x + 10, y, "choose something on the left", theme.labelSoft, 0.9f);
    }
    ui.claim(detail);
}

// --- the files the game is made of ---------------------------------------
void drawDataTab(Ui& ui, Editor& editor, const content::ContentDb& db,
                 const std::filesystem::path& contentDir, const Rect& body) {
    const Theme& theme = ui.theme();
    if (editor.files.empty()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(contentDir, ec))
            if (entry.is_regular_file() && entry.path().extension() == ".json")
                editor.files.push_back(entry.path());
        std::sort(editor.files.begin(), editor.files.end());
    }

    float y = body.y;
    if (ui.button(widgetId("editor.data.reload"), {body.x, y, 150, 24}, "Reload content")) {
        editor.wantsReload = true;
        editor.files.clear();
        editor.openFileLines.clear();
        editor.fileChosen = -1;
    }
    ui.text(body.x + 170, y + 7,
            std::to_string(db.items().size()) + " items, " +
                    std::to_string(db.buildings().size()) + " buildings, " +
                    std::to_string(db.recipes().size()) + " recipes, " +
                    std::to_string(db.resourceNodes().size()) + " resources",
            theme.labelSoft, 0.85f);
    y += 30;

    // What the loader said about them. A file that parses and a file that means
    // what it says are different things, and this is where the difference shows.
    if (!db.errors().empty()) {
        ui.text(body.x, y, std::to_string(db.errors().size()) + " errors", theme.bad, 0.9f);
        y += 14;
        for (std::size_t i = 0; i < db.errors().size() && i < 4; ++i) {
            ui.text(body.x + 12, y, ui.fit(db.errors()[i], body.w - 24, 0.85f), theme.bad, 0.85f);
            y += 13;
        }
    }
    if (!db.warnings().empty()) {
        ui.text(body.x, y, std::to_string(db.warnings().size()) + " warnings", theme.warn, 0.9f);
        y += 14;
        for (std::size_t i = 0; i < db.warnings().size() && i < 4; ++i) {
            ui.text(body.x + 12, y, ui.fit(db.warnings()[i], body.w - 24, 0.85f), theme.warn, 0.85f);
            y += 13;
        }
    }
    if (db.errors().empty() && db.warnings().empty()) {
        ui.text(body.x, y, "content loads clean", theme.good, 0.9f);
        y += 16;
    }
    y += 6;

    const Rect list{body.x, y, body.w * 0.34f, body.bottom() - y};
    const Rect view{list.right() + 12, y, body.w - list.w - 12, list.h};
    ui.rect(list, theme.barBottom.withAlpha(0.5f));
    ui.rect(view, theme.barBottom.withAlpha(0.5f));

    const int rows = static_cast<int>((list.h - 8) / 15);
    for (int i = 0; i < rows && i < static_cast<int>(editor.files.size()); ++i) {
        const Rect row{list.x + 4, list.y + 4 + i * 15, list.w - 8, 14};
        const bool on = editor.fileChosen == i;
        if (on) ui.rect(row, theme.slotHot);
        if (ui.hovered(row) && ui.input().pressed) {
            editor.fileChosen = i;
            editor.fileScroll = 0;
            editor.openFileLines.clear();
            std::ifstream in(editor.files[i]);
            std::string line;
            while (std::getline(in, line)) editor.openFileLines.push_back(line);
        }
        const std::string name =
                std::filesystem::relative(editor.files[i], contentDir).generic_string();
        ui.text(row.x + 4, row.y + 3, ui.fit(name, row.w - 8, 0.85f),
                on ? theme.label : theme.labelSoft, 0.85f);
    }
    ui.claim(list);

    // The file itself. Read-only on purpose: this is a window onto the data, and
    // the data is authored in a text editor where a mistake can be undone.
    if (!editor.openFileLines.empty()) {
        const int shown = static_cast<int>((view.h - 10) / 13);
        if (ui.hovered(view))
            editor.fileScroll = std::clamp(
                    editor.fileScroll - static_cast<int>(ui.input().wheel) * 3, 0,
                    std::max(0, static_cast<int>(editor.openFileLines.size()) - shown));
        for (int i = 0; i < shown; ++i) {
            const int index = editor.fileScroll + i;
            if (index >= static_cast<int>(editor.openFileLines.size())) break;
            ui.text(view.x + 8, view.y + 5 + i * 13,
                    ui.fit(editor.openFileLines[index], view.w - 16, 0.85f), theme.label, 0.85f);
        }
        ui.textRight(view.right() - 8, view.bottom() - 14,
                     std::to_string(editor.openFileLines.size()) + " lines", theme.labelSoft, 0.8f);
    } else {
        ui.text(view.x + 10, view.y + 10, "choose a file on the left", theme.labelSoft, 0.9f);
    }
    ui.claim(view);
}

void drawGameTab(Ui& ui, Editor& editor, sim::Game& game, const Rect& body) {
    const Theme& theme = ui.theme();
    float y = body.y;
    ui.text(body.x, y, "COMMUNITIES ON THE MAP", theme.labelSoft, 0.9f);
    y += 20;
    for (std::size_t i = 0; i < game.communities.size(); ++i) {
        const sim::World& w = *game.communities[i];
        std::int32_t people = 0;
        for (const auto& p : w.people())
            if (p.alive) ++people;
        const bool watched = i == game.watching;
        const Rect row{body.x, y, body.w, 18};
        if (watched) ui.rect(row, theme.slotHot.withAlpha(0.5f));
        char line[160];
        std::snprintf(line, sizeof line, "%2zu  cell %4d,%-4d  %-10s  %3d people  tick %8lld  %s",
                      i, w.localCell().x, w.localCell().y,
                      w.settlements().empty() ? "-" : w.settlements().front().name.c_str(), people,
                      static_cast<long long>(w.tickCount()),
                      w.detail() == sim::World::Detail::Detailed ? "watched" : "counted");
        ui.text(row.x + 4, row.y + 4, line, watched ? theme.accent : theme.label, 0.85f);
        y += 18;
        if (y > body.bottom() - 40) break;
    }

    y += 10;
    if (ui.button(widgetId("editor.game.save"), {body.x, y, 130, 24}, "Save game")) {
        std::string error;
        editor.message = sim::saveGame(game, "saves/editor.save", &error)
                                 ? "written to saves/editor.save"
                                 : ("could not save: " + error);
    }
    ui.text(body.x + 150, y + 6,
            "checksum " + std::to_string(game.active().checksum()), theme.labelSoft, 0.85f);
}

} // namespace

void drawEditor(Ui& ui, Editor& editor, const content::ContentDb& db,
                const std::filesystem::path& contentDir, sim::Game* game) {
    if (!editor.open) return;
    // Before anything is drawn, because a dropped file may change which tab is
    // showing and the tabs are drawn below.
    if (!editor.dropped.empty()) takeDropped(ui, editor, contentDir);
    const Theme& theme = ui.theme();
    const Rect screen{0, 0, static_cast<float>(ui.width()), static_cast<float>(ui.height())};
    ui.rect(screen, Colour::rgb(0, 0, 0, 0.55f));

    const Rect window{60, 60, screen.w - 120, screen.h - 120};
    ui.panel(window);
    const Rect head = window.inset(14, 12, 14, 0);

    ui.text(head.x, head.y, "EDITOR", theme.accent, 1.4f);
    static const char* kTabs[] = {"Interface", "Ground", "Content", "Data", "Game"};
    float x = head.x + 130;
    for (int i = 0; i < static_cast<int>(Editor::Tab::Count); ++i) {
        if (i == static_cast<int>(Editor::Tab::Game) && game == nullptr) continue;
        const float width = ui.textWidth(kTabs[i]) + 28;
        if (ui.tab(widgetId("editor.tab", i), {x, head.y - 2, width, 26}, kTabs[i],
                   editor.tab == static_cast<Editor::Tab>(i)))
            editor.tab = static_cast<Editor::Tab>(i);
        x += width;
    }
    if (ui.button(widgetId("editor.close"), {window.right() - 40, window.y + 10, 26, 22}, "x"))
        editor.open = false;

    const Rect body = window.inset(16, 52, 16, 34);
    switch (editor.tab) {
        case Editor::Tab::Theme: drawThemeTab(ui, editor, body); break;
        case Editor::Tab::Ground: drawGroundTab(ui, editor, contentDir, body); break;
        case Editor::Tab::Content: drawContentTab(ui, editor, db, body); break;
        case Editor::Tab::Data: drawDataTab(ui, editor, db, contentDir, body); break;
        default:
            if (game != nullptr) drawGameTab(ui, editor, *game, body);
            else ui.text(body.x, body.y, "no game is running", ui.theme().labelSoft, 0.9f);
            break;
    }

    if (!editor.message.empty())
        ui.text(window.x + 16, window.bottom() - 24, editor.message, theme.labelSoft, 0.85f);
    ui.claim(window);
}

} // namespace ui
