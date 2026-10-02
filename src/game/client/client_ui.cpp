#include "game/client/client_ui.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

namespace client {
namespace {

constexpr float kBar = 44;      // the top bar
constexpr float kStatus = 30;   // the status bar in Edit
constexpr float kPanelWidth = 324;

using ui::Colour;
using ui::Rect;

std::uint64_t id(const char* name, int index = 0) { return ui::widgetId(name, index); }

std::string grouped(double value) {
    // Thousands apart by a thin space, the way a map writes distances.
    const long long whole = std::llround(value);
    std::string digits = std::to_string(whole < 0 ? -whole : whole);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += "\u2009";
        out += digits[i];
    }
    return (whole < 0 ? "\u2212" : "") + out;
}

std::string km(double value) {
    char text[32];
    std::snprintf(text, sizeof text, value >= 100 ? "%.0f km" : "%.1f km", value);
    return text;
}

std::string ago(std::filesystem::file_time_type when) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::filesystem::file_time_type::clock::now() - when).count();
    if (age < 60) return "just now";
    if (age < 3600) return std::to_string(age / 60) + " min ago";
    if (age < 86400) return std::to_string(age / 3600) + " h ago";
    if (age < 2 * 86400) return "yesterday";
    return std::to_string(age / 86400) + " days ago";
}

std::string megabytes(std::uintmax_t bytes) {
    char text[32];
    if (bytes < 1024 * 1024) std::snprintf(text, sizeof text, "%.0f KB", double(bytes) / 1024.0);
    else std::snprintf(text, sizeof text, "%.1f MB", double(bytes) / (1024.0 * 1024.0));
    return text;
}

// How long a world of this many regions takes to raise, as a person would say it.
std::string buildTime(std::int32_t regions) {
    const std::int32_t area = regions * regions;
    if (area <= 1) return "about 10 seconds";
    if (area <= 4) return "about half a minute";
    if (area <= 9) return "about a minute";
    if (area <= 16) return "two or three minutes";
    return "several minutes";
}

void dim(ui::Ui& ui, float alpha) {
    ui.rect({0, 0, float(ui.width()), float(ui.height())}, Colour(0.02f, 0.02f, 0.03f, alpha));
}

// A small heading over a group of controls, with a rule under it.
void section(ui::Ui& ui, float x, float& y, float w, const char* title) {
    const auto& t = ui.theme();
    ui.text(x, y, title, t.accent, 0.78f, true);
    y += ui.textHeight(0.78f) + 5;
    ui.rect({x, y, w, 1}, t.barEdge.withAlpha(0.55f));
    y += 9;
}

// A labelled slider: the name and the value on one line, the track under it.
bool sliderRow(ui::Ui& ui, std::uint64_t which, float x, float& y, float w, const std::string& label,
               const std::string& value, float& v, float low, float high, bool logarithmic) {
    const auto& t = ui.theme();
    ui.text(x, y, label, t.label, 0.93f);
    ui.textRight(x + w, y, value, t.accent, 0.93f);
    y += ui.textHeight(0.93f) + 5;
    const bool moved = ui.slider(which, {x, y, w, 20}, v, low, high, logarithmic);
    y += 28;
    return moved;
}

std::string metres(float value) {
    char text[32];
    std::snprintf(text, sizeof text, value < 10 ? "%.1f m" : "%.0f m", double(value));
    return text;
}

Rect centred(const ui::Ui& ui, float w, float h) {
    w = std::min(w, float(ui.width()) - 24);
    h = std::min(h, float(ui.height()) - 24);
    return {std::round((float(ui.width()) - w) * 0.5f), std::round((float(ui.height()) - h) * 0.5f), w, h};
}

} // namespace

void ClientUi::toast(std::string text, double now, bool bad) {
    toast_ = std::move(text);
    toastBad_ = bad;
    toastUntil_ = now + (bad ? 5.0 : 2.5);
}

void ClientUi::alert(std::string title, std::string text) {
    alertTitle_ = std::move(title);
    alertText_ = std::move(text);
}

const world::saves::SavedWorld* ClientUi::selected(const ClientView& view, std::size_t* index) const {
    if (!view.worlds) return nullptr;
    for (std::size_t i = 0; i < view.worlds->size(); ++i)
        if ((*view.worlds)[i].name == selectedName_) {
            if (index) *index = i;
            return &(*view.worlds)[i];
        }
    return nullptr;
}

void ClientUi::resetForm(const ClientView& view) {
    (void)view;
    formName_.clear();
    std::random_device random;
    formSeed_ = std::to_string(1 + random() % 999999999u);
    formPreset_ = 0;
    formRegions_ = 1;
    formRegionsDown_ = 1;
    formError_.clear();
}

void ClientUi::build(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    lastNow_ = view.now;
    switch (screen) {
        case Screen::Menu: menu(ui, view, out); break;
        case Screen::Host: host(ui, view, out); break;
        case Screen::Loading: loading(ui, view); break;
        case Screen::World: world(ui, view, out); break;
    }
    if (view.settingsOpen) settingsDone(ui, view, out);
    toastLine(ui, view);
    alertBox(ui);
}

// --- the main menu ----------------------------------------------------------

void ClientUi::menu(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    const float W = float(ui.width()), H = float(ui.height());
    // A darker band behind the words, fading into the world, so they read over
    // any sky or sea.
    const float band = std::min(W, 520.0f);
    ui.rect({0, 0, band, H}, Colour(0.03f, 0.025f, 0.02f, 0.62f));
    for (int i = 0; i < 6; ++i)
        ui.rect({band + float(i) * 16, 0, 16, H}, Colour(0.03f, 0.025f, 0.02f, 0.5f * (1.0f - float(i + 1) / 7.0f)));

    const float x = std::min(72.0f, W * 0.08f);
    float y = std::max(48.0f, H * 0.2f);
    ui.text(x, y, "CAMPFIRE", t.accent, 3.2f, true);
    y += ui.textHeight(3.2f) + 14;
    ui.text(x, y, "Explore worlds. Shape them.", t.labelSoft, 1.1f);
    y += 56;

    const float w = std::min(340.0f, band - x - 24), h = 46, gap = 12;
    const bool any = view.worlds && !view.worlds->empty();
    if (any) {
        const auto& last = view.worlds->front();
        if (ui.button(id("menu.continue"), {x, y, w, h}, "Continue: " + last.name, t.accent, t.ink, last.readable)) {
            out.load = 0;
            out.loadAs = WorldMode::Explore;
        }
        y += h + gap;
    }
    if (ui.button(id("menu.play"), {x, y, w, h}, any ? "Worlds" : "Play")) {
        screen = Screen::Host;
        creating_ = !any;
        if (creating_) resetForm(view);
        out.refreshWorlds = true;
    }
    y += h + gap;
    if (ui.button(id("menu.settings"), {x, y, w, h}, "Settings")) out.toggleSettings = true;
    y += h + gap;
    if (ui.button(id("menu.quit"), {x, y, w, h}, "Quit")) out.quit = true;

    ui.text(x, H - 36, "Your worlds are kept in the worlds folder beside the game.", t.labelSoft, 0.8f);
}

// --- the host screen: the saved worlds -----------------------------------------

void ClientUi::host(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    dim(ui, 0.5f);
    const Rect card = centred(ui, 1000, 660);
    ui.panel(card);
    ui.text(card.x + 28, card.y + 24, "Worlds", t.label, 1.7f, true);
    ui.text(card.x + 28, card.y + 24 + ui.textHeight(1.7f) + 10,
            "Choose a world to explore or edit, or make a new one.", t.labelSoft, 0.9f);
    if (ui.button(id("host.back"), {card.right() - 124, card.y + 24, 96, 34}, "Back")) {
        screen = Screen::Menu;
        creating_ = false;
    }

    const float top = card.y + 100, bottom = card.bottom() - 24;
    const float listW = std::floor((card.w - 28 * 3) * 0.54f);
    const Rect list{card.x + 28, top, listW, bottom - top - 50};
    hostList(ui, view, list, out);
    if (ui.toggle(id("host.new"), {list.x, list.bottom() + 12, 190, 38}, "+  New world", creating_)) {
        if (!creating_) resetForm(view);
        creating_ = !creating_;
    }
    ui.rect({list.right() + 27, top, 1, bottom - top}, t.barEdge.withAlpha(0.6f));
    const Rect side{list.right() + 56, top, card.right() - 28 - (list.right() + 56), bottom - top};
    if (creating_) hostCreate(ui, view, side, out);
    else hostDetails(ui, view, side, out);
    if (confirmingDelete_) confirmDelete(ui, view, out);
}

void ClientUi::hostList(ui::Ui& ui, const ClientView& view, const Rect& area, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& worlds = view.worlds ? *view.worlds : std::vector<world::saves::SavedWorld>{};
    if (worlds.empty()) {
        ui.roundRect(area, t.slot.withAlpha(0.35f), 6);
        ui.paragraph(area.x + 20, area.y + 20, area.w - 40,
                     "No worlds yet. Make one with New world - it takes seconds for a small one.", t.labelSoft, 0.95f);
        ui.claim(area);
        return;
    }
    constexpr float row = 64, gap = 6;
    const ui::Rect content = ui.beginScroll(id("host.list"), area);
    for (std::size_t i = 0; i < worlds.size(); ++i) {
        const Rect r{content.x, content.y + float(i) * (row + gap), content.w, row};
        const auto& w = worlds[i];
        const bool chosen = w.name == selectedName_;
        if (ui.selectable(id("host.row", int(i)), r, chosen)) {
            selectedName_ = w.name;
            creating_ = false;
        }
        // A double click on a world is Explore.
        if (chosen && ui.input().doubleClick && ui.hovered(r) && w.readable) {
            out.load = i;
            out.loadAs = WorldMode::Explore;
        }
        const float right = 150;
        ui.text(r.x + 16, r.y + 12, ui.fit(w.name, r.w - right - 24, 1.05f, true), t.label, 1.05f, true);
        const std::string facts = !w.readable ? "Cannot be read"
            : std::to_string(w.regionsX) + " \u00d7 " + std::to_string(w.regionsY) + " regions  \u00b7  " +
                  km(w.widthKm()) + "  \u00b7  seed " + std::to_string(w.seed);
        ui.text(r.x + 16, r.y + 38, ui.fit(facts, r.w - right - 24, 0.85f), w.readable ? t.labelSoft : t.danger, 0.85f);
        ui.textRight(r.right() - 16, r.y + 13, ago(w.modified), t.labelSoft, 0.85f);
        ui.textRight(r.right() - 16, r.y + 38, w.hasHistory ? "edited  \u00b7  " + megabytes(w.historyBytes) : "untouched",
                     w.hasHistory ? t.accent.withAlpha(0.85f) : t.labelSoft.withAlpha(0.7f), 0.8f);
    }
    ui.endScroll(content.y + float(worlds.size()) * (row + gap) - gap);
}

void ClientUi::hostDetails(ui::Ui& ui, const ClientView& view, const Rect& area, ClientActions& out) {
    const auto& t = ui.theme();
    std::size_t index = 0;
    const auto* w = selected(view, &index);
    if (!w) {
        ui.paragraph(area.x, area.y + 6, area.w,
                     view.worlds && !view.worlds->empty()
                         ? "Select a world on the left - or make a new one."
                         : "Nothing here yet. Make your first world with New world.",
                     t.labelSoft, 0.95f);
        return;
    }
    const float bh = 44;
    float by = area.bottom() - bh * 2 - 12;
    // The facts scroll above the buttons when the window is short.
    const ui::Rect page = ui.beginScroll(id("host.details"), {area.x, area.y, area.w, by - 12 - area.y});
    float y = page.y;
    const float pw = page.w - (page.w < area.w ? 6 : 0);
    ui.text(page.x, y, ui.fit(w->name, pw, 1.5f, true), t.label, 1.5f, true);
    y += ui.textHeight(1.5f) + 22;
    const auto fact = [&](const char* key, const std::string& value, const Colour& colour) {
        ui.text(page.x, y, key, t.labelSoft, 0.9f);
        ui.text(page.x + 104, y, ui.fit(value, pw - 104, 0.9f), colour, 0.9f);
        y += 28;
    };
    if (w->readable) {
        fact("Size", std::to_string(w->regionsX) + " \u00d7 " + std::to_string(w->regionsY) + " regions, " +
                         km(w->widthKm()) + " across", t.label);
        fact("Land", std::to_string(w->generatedRegions) + " of " + std::to_string(w->regionsX * w->regionsY) +
                         " regions made, the rest open sea", t.label);
        fact("Seed", std::to_string(w->seed), t.label);
        fact("History", w->hasHistory ? "edited, " + megabytes(w->historyBytes) + " of changes" : "untouched",
             w->hasHistory ? t.accent : t.label);
        fact("Changed", ago(w->modified), t.label);
        y += 6;
        y += ui.paragraph(page.x, y, pw, "Opening it raises the world from its layout: " +
                                              (w->generatedRegions == 0 ? std::string("at once, it is all sea")
                                                                        : buildTime(std::max(w->regionsX, w->regionsY))) +
                                              ".",
                          t.labelSoft, 0.85f);
    } else {
        y += ui.paragraph(page.x, y, pw,
                          "This world's file could not be read. It may have been written by a newer version, or damaged.",
                          t.danger, 0.95f);
    }
    ui.endScroll(y);
    if (ui.button(id("host.explore"), {area.x, by, area.w, bh}, "Explore", t.accent, t.ink, w->readable)) {
        out.load = index;
        out.loadAs = WorldMode::Explore;
    }
    by += bh + 12;
    const float half = std::floor((area.w - 12) * 0.5f);
    if (ui.button(id("host.edit"), {area.x, by, half, bh}, "Edit", w->readable)) {
        out.load = index;
        out.loadAs = WorldMode::Edit;
    }
    if (ui.button(id("host.delete"), {area.x + half + 12, by, area.w - half - 12, bh}, "Delete...",
                  t.danger.withAlpha(0.85f), t.selection)) {
        confirmingDelete_ = true;
        pendingDelete_ = w->name;
    }
}

void ClientUi::hostCreate(ui::Ui& ui, const ClientView& view, const Rect& area, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& presets = view.presets ? *view.presets : std::vector<generation::WorldPreset>{};
    // The form scrolls when the window is short; what it is for - the
    // buttons - stays at its foot.
    const float bh = 44;
    const float by = area.bottom() - bh * 2 - 12;
    const ui::Rect page = ui.beginScroll(id("host.create"), {area.x, area.y, area.w, by - 12 - area.y});
    const float x = page.x, w = page.w - (page.w < area.w ? 6 : 0);
    float y = page.y;
    ui.text(x, y, "New world", t.label, 1.5f, true);
    y += ui.textHeight(1.5f) + 20;

    ui.text(x, y, "Name", t.labelSoft, 0.85f);
    y += ui.textHeight(0.85f) + 6;
    if (ui.textField(id("host.name"), {x, y, w, 36}, formName_, "My world", 60)) formError_.clear();
    y += 36 + 16;

    // What the world starts as: one of the presets, generated over all of
    // it, or nothing - open sea of that size, ready at once, to be filled
    // region by region or by hand in Edit.
    ui.text(x, y, "Kind of world", t.labelSoft, 0.85f);
    y += ui.textHeight(0.85f) + 6;
    const float cw = std::floor((w - 8) * 0.5f), ch = 32;
    const int kinds = int(presets.size()) + 1;   // the presets, then Empty
    formPreset_ = std::clamp(formPreset_, -1, int(presets.size()) - 1);
    for (int i = 0; i < kinds; ++i) {
        const Rect r{x + float(i % 2) * (cw + 8), y + float(i / 2) * (ch + 8), cw, ch};
        const bool empty = i == int(presets.size());
        const std::string label = empty ? "Empty (open sea)"
                                  : presets[std::size_t(i)].label.empty() ? presets[std::size_t(i)].name
                                                                          : presets[std::size_t(i)].label;
        const int value = empty ? -1 : i;
        if (ui.toggle(id("host.preset", i), r, label, formPreset_ == value)) formPreset_ = value;
    }
    y += float((kinds + 1) / 2) * (ch + 8) + 2;
    const bool empty = formPreset_ < 0;
    const std::string note = empty ? "Nothing generated: the sea, and a place for regions you make, one by one, "
                                     "in Edit (World shape). Ready in a moment at any size."
                                   : presets[std::size_t(formPreset_)].note;
    if (!note.empty()) y += ui.paragraph(x, y, w, note, t.labelSoft, 0.82f) + 6;
    y += 8;

    // Regions across and down, and the sizes most asked for; an empty world
    // may be as big as the reference one (and bigger), a generated one is
    // kept to what raises in minutes - grow it and fill it region by region.
    ui.text(x, y, "Size, in regions of 131 km", t.labelSoft, 0.85f);
    y += ui.textHeight(0.85f) + 6;
    const std::int32_t most = empty ? world::saves::kMaxEmptyRegions : world::saves::kMaxGeneratedRegions;
    formRegions_ = std::clamp(formRegions_, 1, most);
    formRegionsDown_ = std::clamp(formRegionsDown_, 1, most);
    const float label = 70, step = std::min(150.0f, w - label - 8);
    ui.text(x, y + 9, "Across", t.label, 0.9f);
    ui.stepper(id("host.across"), {x + label, y, step, 32}, formRegions_, 1, most);
    y += 38;
    ui.text(x, y + 9, "Down", t.label, 0.9f);
    ui.stepper(id("host.down"), {x + label, y, step, 32}, formRegionsDown_, 1, most);
    y += 38;
    struct Quick { const char* label; std::int32_t across, down; };
    static constexpr Quick quick[] = {{"1 \u00d7 1", 1, 1}, {"2 \u00d7 2", 2, 2}, {"4 \u00d7 4", 4, 4},
                                      {"800 \u00d7 2000 km", world::saves::kReferenceRegionsX,
                                       world::saves::kReferenceRegionsY}};
    const float qw = std::floor((w - 24) / 4.0f);
    int q = 0;
    for (const auto& pick : quick) {
        const bool fits = pick.across <= most && pick.down <= most;
        const Rect r{x + float(q) * (qw + 8), y, qw, 30};
        if (ui.toggle(id("host.quick", q), r, pick.label, formRegions_ == pick.across && formRegionsDown_ == pick.down,
                      fits)) {
            formRegions_ = pick.across;
            formRegionsDown_ = pick.down;
        }
        ++q;
    }
    y += 30 + 6;
    const double acrossKm = double(formRegions_) * double(generation::kRegionMetres) / 1000.0;
    const double downKm = double(formRegionsDown_) * double(generation::kRegionMetres) / 1000.0;
    const std::int32_t regions = formRegions_ * formRegionsDown_;
    ui.text(x, y, ui.fit(km(acrossKm) + " \u00d7 " + km(downKm) + ", " + std::to_string(regions) +
                             (regions == 1 ? " region" : " regions") + ", ready " +
                             (empty ? std::string("at once") : "in " + buildTime(std::max(formRegions_, formRegionsDown_))),
                         w, 0.82f),
            t.labelSoft, 0.82f);
    y += ui.textHeight(0.82f) + 16;

    ui.text(x, y, "Seed", t.labelSoft, 0.85f);
    y += ui.textHeight(0.85f) + 6;
    if (ui.textField(id("host.seed"), {x, y, w - 112, 36}, formSeed_, "random", 12)) {
        std::erase_if(formSeed_, [](char c) { return c < '0' || c > '9'; });
        formError_.clear();
    }
    if (ui.button(id("host.random"), {x + w - 100, y, 100, 36}, "Random")) {
        std::random_device random;
        formSeed_ = std::to_string(1 + random() % 999999999u);
    }
    y += 36 + 12;
    if (!formError_.empty()) y += ui.paragraph(x, y, w, formError_, t.danger, 0.9f) + 8;
    ui.endScroll(y);

    const auto make = [&](WorldMode as) {
        world::saves::NewWorld spec;
        spec.name = formName_;
        spec.empty = empty;
        if (!empty && !presets.empty()) spec.preset = presets[std::size_t(formPreset_)].name;
        spec.regions = formRegions_;
        spec.regionsX = formRegions_;
        spec.regionsY = formRegionsDown_;
        if (formSeed_.empty()) {
            std::random_device random;
            spec.seed = 1 + random() % 999999999u;
        } else {
            spec.seed = std::strtoull(formSeed_.c_str(), nullptr, 10);
            if (spec.seed == 0) spec.seed = 1;
        }
        out.create = spec;
        out.createAs = as;
    };
    const bool can = empty || !presets.empty();
    float ay = by;
    if (ui.button(id("host.make"), {area.x, ay, area.w, bh}, "Create and explore", t.accent, t.ink, can))
        make(WorldMode::Explore);
    ay += bh + 12;
    const float half = std::floor((area.w - 12) * 0.5f);
    if (ui.button(id("host.makeEdit"), {area.x, ay, half, bh}, "Create and edit", can)) make(WorldMode::Edit);
    if (ui.button(id("host.cancel"), {area.x + half + 12, ay, area.w - half - 12, bh}, "Cancel")) creating_ = false;
}

void ClientUi::confirmDelete(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    dim(ui, 0.45f);
    const Rect box = centred(ui, 460, 210);
    ui.panel(box);
    const std::string name = pendingDelete_.value_or("");
    ui.text(box.x + 24, box.y + 24, ui.fit("Delete \u201c" + name + "\u201d?", box.w - 48, 1.3f, true), t.label, 1.3f,
            true);
    ui.paragraph(box.x + 24, box.y + 24 + ui.textHeight(1.3f) + 16, box.w - 48,
                 "Its layout and everything done to it will be removed from disk. This cannot be undone.",
                 t.labelSoft, 0.92f);
    const float bw = std::floor((box.w - 48 - 12) * 0.5f), by = box.bottom() - 24 - 40;
    if (ui.button(id("delete.yes"), {box.x + 24, by, bw, 40}, "Delete", t.danger, t.selection)) {
        if (view.worlds)
            for (std::size_t i = 0; i < view.worlds->size(); ++i)
                if ((*view.worlds)[i].name == name) out.remove = i;
        confirmingDelete_ = false;
        pendingDelete_.reset();
    }
    if (ui.button(id("delete.no"), {box.x + 24 + bw + 12, by, bw, 40}, "Cancel")) {
        confirmingDelete_ = false;
        pendingDelete_.reset();
    }
    ui.claim({0, 0, float(ui.width()), float(ui.height())});
}

// --- loading --------------------------------------------------------------------

void ClientUi::loading(ui::Ui& ui, const ClientView& view) {
    const auto& t = ui.theme();
    dim(ui, 0.6f);
    const Rect box = centred(ui, 480, 156);
    ui.panel(box);
    ui.text(box.x + 26, box.y + 24, ui.fit(view.loadingTitle, box.w - 52, 1.3f, true), t.label, 1.3f, true);
    ui.text(box.x + 26, box.y + 24 + ui.textHeight(1.3f) + 14, ui.fit(view.loadingDetail, box.w - 52 - 60, 0.92f),
            t.labelSoft, 0.92f);
    ui.textRight(box.right() - 26, box.y + 24 + ui.textHeight(1.3f) + 14,
                 std::to_string(int(view.loadingSeconds)) + " s", t.labelSoft, 0.92f);
    // No honest percentage exists for generation, so the bar says "working"
    // rather than a number it would have to invent.
    const Rect track{box.x + 26, box.bottom() - 40, box.w - 52, 6};
    ui.roundRect(track, t.gauge, 3);
    const float span = track.w * 0.28f;
    const float phase = float(std::fmod(view.now * 0.55, 1.0));
    const float from = track.x - span + (track.w + span) * phase;
    const float left = std::max(track.x, from), right = std::min(track.right(), from + span);
    if (right > left) ui.roundRect({left, track.y, right - left, track.h}, t.accent, 3);
    ui.claim({0, 0, float(ui.width()), float(ui.height())});
}

// --- the world ------------------------------------------------------------------

void ClientUi::world(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    topBar(ui, view, out);
    if (mode == WorldMode::Edit) {
        editPanel(ui, view, out);
        statusBar(ui, view);
    } else {
        toolsRect_ = {};
        hints(ui, view);
    }
    if (view.rebuilding) {
        const auto& t = ui.theme();
        const std::string text = "Raising the world's new shape...";
        const float w = ui.textWidth(text, 0.92f) + 40;
        const Rect pill{std::round((float(ui.width()) - w) * 0.5f), kBar + 12, w, 32};
        ui.roundRect(pill, t.barTop, 16);
        ui.border(pill, t.accent.withAlpha(0.7f));
        ui.textCentred(pill, text, t.accent, 0.92f);
    }
    if (paused) pauseMenu(ui, view, out);
}

void ClientUi::topBar(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    const float W = float(ui.width());
    const Rect bar{0, 0, W, kBar};
    ui.rect(bar, t.barTop);
    ui.rect({0, kBar - 1, W, 1}, t.barEdge);
    ui.claim(bar);
    if (ui.button(id("bar.menu"), {10, 7, 78, 30}, "Menu")) paused = true;

    const float segment = 108;
    const float middle = std::round(W * 0.5f);
    if (ui.toggle(id("bar.explore"), {middle - segment, 7, segment, 30}, "Explore", mode == WorldMode::Explore))
        out.mode = WorldMode::Explore;
    if (ui.toggle(id("bar.edit"), {middle, 7, segment, 30}, "Edit", mode == WorldMode::Edit))
        out.mode = WorldMode::Edit;
    ui.text(102, 15, ui.fit(view.worldName, std::max(40.0f, middle - segment - 16 - 102), 1.0f, true), t.label, 1.0f,
            true);

    // The right side is laid from the edge inwards, and what does not fit
    // between it and the mode switch is left out rather than drawn over.
    const float floor = middle + segment + 16;
    float x = W - 10;
    const auto room = [&](float w) { return x - w >= floor; };
    x -= 76;
    if (ui.button(id("bar.save"), {x, 7, 76, 30}, "Save", view.unsaved && !view.saving)) out.save = true;
    x -= 12;
    const std::string status = view.saving ? "Saving..." : view.unsaved ? "Unsaved changes" : "All changes saved";
    const float statusW = ui.textWidth(status, 0.88f);
    if (room(statusW + 12 + (mode == WorldMode::Edit ? 142 : 0))) {
        ui.textRight(x, 15, status, view.unsaved ? t.warn : t.labelSoft, 0.88f);
        x -= statusW + 16;
    }
    if (mode == WorldMode::Edit && room(142)) {
        x -= 68;
        // On the stages of the world's shape Undo takes back the last stroke of
        // paint; on the ground, the last change to it.
        const bool shaping = shapeTab(tab);
        if (ui.button(id("bar.redo"), {x, 7, 68, 30}, "Redo", !shaping && !view.redoLabel.empty())) out.redo = true;
        x -= 74;
        if (ui.button(id("bar.undo"), {x, 7, 68, 30}, "Undo", shaping ? view.shape.canUndo : !view.undoLabel.empty())) {
            if (shaping) out.shape.undo = true;
            else out.undo = true;
        }
        x -= 10;
    }
    if (room(132)) {
        x -= 132;
        if (ui.button(id("bar.camera"), {x, 7, 132, 30}, "View: " + view.cameraName)) out.cycleCamera = true;
    }
}

void ClientUi::editPanel(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    const float H = float(ui.height());
    const float x = 12, y = kBar + 12, w = kPanelWidth;
    const float bottom = H - kStatus - 12;
    toolsRect_ = {x, y, w, bottom - y};
    ui.panel(toolsRect_);
    // The stages in the order a world is made, two rows of them: the shape of
    // the world first, then the ground by hand and what stands on it.
    static constexpr const char* names[kEditTabs] = {"1 Size", "2 Land", "3 Coast", "4 Mountains",
                                                     "5 Climate", "6 Sculpt", "7 Objects", "8 Import"};
    const float tw4 = std::floor((w - 12) / 4.0f);
    for (int i = 0; i < kEditTabs; ++i) {
        const bool top = i < 4;
        const Rect r{x + 6 + float(i % 4) * tw4, y + 6 + (top ? 0.0f : 34.0f), tw4, 30};
        if (ui.tab(id("edit.tab", i), r, names[i], int(tab) == i)) {
            tab = EditTab(i);
            out.tab = tab;
        }
    }
    // The tools scroll inside the panel when the window is too short for
    // them; at its foot, what the last change was (the world's shape: what
    // the editor says; the ground: what Undo would take back).
    const float foot = ui.textHeight(0.8f) + 24;
    const float top = y + 6 + 34 + 30 + 8;
    const ui::Rect frame{x + 4, top, w - 8, bottom - foot - top};
    const ui::Rect content = ui.beginScroll(id("edit.scroll", int(tab)), frame);
    float cy = content.y + 6;
    const float cx = content.x + 12, cw = content.w - 24;
    switch (tab) {
        case EditTab::Size: sizeTab(ui, view, cx, cy, cw, out); break;
        case EditTab::Land: landTab(ui, view, cx, cy, cw, out); break;
        case EditTab::Coast: coastTab(ui, view, cx, cy, cw, out); break;
        case EditTab::Mountains: mountainsTab(ui, view, cx, cy, cw, out); break;
        case EditTab::Water: waterTab(ui, view, cx, cy, cw, out); break;
        case EditTab::Terrain: terrainTools(ui, cx, cy, cw); break;
        case EditTab::Objects: objectTools(ui, cx, cy, cw); break;
        case EditTab::Import: importTab(ui, view, cx, cy, cw, out); break;
    }
    ui.endScroll(cy + 6);
    const std::string last = shapeTab(tab)
            ? (view.shape.status.empty() ? std::string("Nothing changed yet") : view.shape.status)
            : view.undoLabel.empty() ? "Nothing to undo yet" : "Last change: " + view.undoLabel;
    ui.text(x + 16, bottom - 14 - ui.textHeight(0.8f), ui.fit(last, w - 32, 0.8f), t.labelSoft, 0.8f);
}

namespace {
std::string kilometres(float km) {
    char text[32];
    std::snprintf(text, sizeof text, km < 10 ? "%.1f km" : "%.0f km", double(km));
    return text;
}
std::string percent(float v) { return std::to_string(int(std::lround(v))) + " %"; }

// How many regions are at each stage, in a line.
std::string stageLine(const ShapeView& shape) {
    std::string line;
    const char* names[] = {"sketched", "worked out", "with mountains", "with water"};
    for (std::size_t i = 0; i < shape.stages.size(); ++i) {
        if (shape.stages[i] == 0) continue;
        if (!line.empty()) line += ", ";
        line += std::to_string(shape.stages[i]) + " " + names[i];
    }
    if (shape.generatedRegions > 0) line += (line.empty() ? "" : ", ") + std::to_string(shape.generatedRegions) + " generated";
    return line.empty() ? "No land yet: the world is all sea." : "Regions: " + line + ".";
}
} // namespace

void ClientUi::sizeTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& shape = view.shape;
    section(ui, x, y, w, "SIZE");
    const double km = double(generation::kRegionMetres) / 1000.0;
    ui.text(x, y, std::to_string(shape.regionsX) + " \u00d7 " + std::to_string(shape.regionsY) + " regions, " +
                      kilometres(float(shape.regionsX * km)) + " \u00d7 " + kilometres(float(shape.regionsY * km)),
            t.label, 0.95f, true);
    y += ui.textHeight(0.95f) + 10;
    // A region at a time, at any side. Sea is what is added; nothing is
    // computed for it. West and north move where everything already is, so
    // they wait until nothing has been dug or planted.
    struct Side { const char* name; int index; bool moves; };
    static constexpr Side sides[] = {{"West", 0, true}, {"East", 1, false}, {"North", 2, true}, {"South", 3, false}};
    const float bw = 40;
    for (const auto& side : sides) {
        ui.text(x, y + 8, side.name, t.label, 0.9f);
        const bool can = !side.moves || shape.historyEmpty;
        const bool across = side.index < 2;
        const std::int32_t now = across ? shape.regionsX : shape.regionsY;
        std::array<std::int32_t, 4> change{};
        if (ui.button(id("size.less", side.index), {x + w - 2 * bw - 8, y, bw, 30}, "\u2212", can && now > 1)) {
            change[std::size_t(side.index)] = -1;
            out.shape.reshape = change;
        }
        if (ui.button(id("size.more", side.index), {x + w - bw, y, bw, 30}, "+", can && now < generation::kMaxRegionsPerSide)) {
            change[std::size_t(side.index)] = 1;
            out.shape.reshape = change;
        }
        y += 36;
    }
    if (!shape.historyEmpty)
        y += ui.paragraph(x, y, w, "West and north wait: ground has been dug or planted, and it is held where it is.",
                          t.labelSoft, 0.8f) + 6;
    y += ui.paragraph(x, y, w, "A world is sea until land is painted. Nothing here is computed.", t.labelSoft, 0.82f) + 12;

    section(ui, x, y, w, "ON ITS PLANET");
    if (!latitude_) latitude_ = std::pair<float, float>{float(shape.northDegrees), float(shape.kmPerDegree)};
    char value[32];
    std::snprintf(value, sizeof value, "%.0f\u00b0", double(latitude_->first));
    sliderRow(ui, id("size.north"), x, y, w, "Latitude of the top edge", value, latitude_->first, -90, 90, false);
    std::snprintf(value, sizeof value, "%.1f km", double(latitude_->second));
    sliderRow(ui, id("size.kmdeg"), x, y, w, "Map per degree", value, latitude_->second, 1, 120, true);
    const double spanDegrees = shape.regionsY * km / std::max(0.1f, latitude_->second);
    std::snprintf(value, sizeof value, "%.0f\u00b0 to %.0f\u00b0", double(latitude_->first),
                  double(latitude_->first) - spanDegrees);
    ui.text(x, y, std::string("From ") + value, t.labelSoft, 0.82f);
    y += ui.textHeight(0.82f) + 8;
    const bool moved = std::abs(latitude_->first - float(shape.northDegrees)) > 0.05f ||
                       std::abs(latitude_->second - float(shape.kmPerDegree)) > 0.05f;
    if (ui.button(id("size.latitude"), {x, y, w, 32}, "Apply the latitude", moved)) out.shape.latitude = *latitude_;
    y += 38;
    if (!moved) latitude_.reset();   // follow the world when nothing is being changed
}

void ClientUi::landTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    section(ui, x, y, w, "LAND");
    y += ui.paragraph(x, y, w, "Paint where the land is. Every region the brush touches joins the world by itself. "
                               "It is drawn flat at once and nothing is computed; the coast and the ground come in "
                               "Coast & relief.",
                      t.labelSoft, 0.82f) + 10;
    sliderRow(ui, id("land.size"), x, y, w, "Brush (radius)", kilometres(land.radiusKm), land.radiusKm, 2, 128, true);
    sliderRow(ui, id("land.hard"), x, y, w, "Hard edge", percent(land.hardness), land.hardness, 0, 100, false);
    y += ui.paragraph(x, y, w, "Left mouse: land.  Alt: back to sea.  [ ]: size.", t.labelSoft, 0.82f) + 10;
    if (ui.button(id("land.undo"), {x, y, w, 30}, "Undo the last stroke", view.shape.canUndo)) out.shape.undo = true;
    y += 38;
    y += ui.paragraph(x, y, w, stageLine(view.shape), t.label, 0.85f) + 6;
}

void ClientUi::coastTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& shape = view.shape;
    section(ui, x, y, w, "COAST AND GROUND");
    y += ui.paragraph(x, y, w, "The painted land worked out: its coast torn the way coasts are, its ground raised "
                               "with the first relief - never lower than the floor below, so painted land stays land.",
                      t.labelSoft, 0.82f) + 10;
    if (!dials_) dials_ = shape.dials;
    auto& d = *dials_;
    float coast = d.coast * 100.0f, relief = d.relief * 100.0f;
    sliderRow(ui, id("coast.torn"), x, y, w, "Torn coast", percent(coast), coast, 0, 200, false);
    sliderRow(ui, id("coast.km"), x, y, w, "Bays and headlands", kilometres(d.coastKm), d.coastKm, 2, 80, true);
    sliderRow(ui, id("coast.relief"), x, y, w, "Relief", percent(relief), relief, 0, 200, false);
    sliderRow(ui, id("coast.floor"), x, y, w, "Land no lower than", metres(d.minLandMetres), d.minLandMetres, 0, 200, false);
    d.coast = coast / 100.0f;
    d.relief = relief / 100.0f;
    const bool changed = !(d == shape.dials);
    const std::int32_t sketched = shape.stages[0], worked = shape.stages[1] + shape.stages[2] + shape.stages[3];
    if (ui.button(id("coast.pin"), {x, y, w, 36}, sketched > 0 ? "Work out the sketched land" : "No sketched land",
                  t.accent, t.ink, sketched > 0)) {
        if (changed) out.shape.dials = d;
        out.shape.pin = true;
    }
    y += 42;
    if (ui.button(id("coast.apply"), {x, y, w, 30}, "Apply to the land worked out", changed && worked > 0)) {
        out.shape.dials = d;
    }
    y += 38;
    y += ui.paragraph(x, y, w, stageLine(shape), t.label, 0.85f) + 6;
    if (!changed) dials_.reset();
}

void ClientUi::mountainsTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    section(ui, x, y, w, "MOUNTAINS");
    y += ui.paragraph(x, y, w, "Paint where plates meet. The generator raises the range along the stroke - ridges, "
                               "peaks and passes - and weathers it; the land around keeps its ground.",
                      t.labelSoft, 0.82f) + 10;
    sliderRow(ui, id("mount.size"), x, y, w, "Brush (radius)", kilometres(mountains.radiusKm), mountains.radiusKm, 2, 64, true);
    sliderRow(ui, id("mount.height"), x, y, w, "How high", std::to_string(int(mountains.height)), mountains.height, 100, 900,
              false);
    sliderRow(ui, id("mount.strength"), x, y, w, "Strength", percent(mountains.strength), mountains.strength, 5, 100, false);
    y += ui.paragraph(x, y, w, "Left mouse: a range.  Alt: wear it back down.  [ ]: size.", t.labelSoft, 0.82f) + 10;
    const float half = std::floor((w - 8) * 0.5f);
    if (ui.button(id("mount.build"), {x, y, half, 30}, "Build (Enter)", view.shape.unbuilt && !view.shape.building))
        out.shape.build = true;
    if (ui.button(id("mount.undo"), {x + half + 8, y, w - half - 8, 30}, "Undo", view.shape.canUndo)) out.shape.undo = true;
    y += 38;
    y += ui.paragraph(x, y, w, stageLine(view.shape), t.label, 0.85f) + 6;
}

void ClientUi::waterTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& shape = view.shape;
    section(ui, x, y, w, "CLIMATE AND WATER");
    y += ui.paragraph(x, y, w, "Worked out for the land first - winds, rain, rivers and lakes from the ground and the "
                               "latitude. Then made wetter or drier by hand where it should be.",
                      t.labelSoft, 0.82f) + 10;
    const std::int32_t ready = shape.stages[1] + shape.stages[2];
    if (ui.button(id("water.make"), {x, y, w, 36}, ready > 0 ? "Work out the climate and water" : "No land waiting for it",
                  t.accent, t.ink, ready > 0))
        out.shape.water = true;
    y += 44;
    section(ui, x, y, w, "RAIN BY HAND");
    sliderRow(ui, id("water.size"), x, y, w, "Brush (radius)", kilometres(climate.radiusKm), climate.radiusKm, 8, 256, true);
    sliderRow(ui, id("water.rain"), x, y, w, "Rain", percent(climate.rain), climate.rain, 20, 250, false);
    y += ui.paragraph(x, y, w, "Left mouse: this much rain.  Alt: back to the climate's own.", t.labelSoft, 0.82f) + 10;
    const float half = std::floor((w - 8) * 0.5f);
    if (ui.button(id("water.build"), {x, y, half, 30}, "Build (Enter)", shape.unbuilt && !shape.building))
        out.shape.build = true;
    if (ui.button(id("water.undo"), {x + half + 8, y, w - half - 8, 30}, "Undo", shape.canUndo)) out.shape.undo = true;
    y += 38;
    y += ui.paragraph(x, y, w, stageLine(shape), t.label, 0.85f) + 6;
}

void ClientUi::importTab(ui::Ui& ui, const ClientView& view, float x, float& y, float w, ClientActions& out) {
    const auto& t = ui.theme();
    const auto& shape = view.shape;
    auto& f = importForm;
    // A path to type or paste, and a button beside it that opens the
    // system's own file dialog for it.
    const auto pathRow = [&](const char* name, int field, const char* label, std::string& value, const char* hint) {
        ui.text(x, y, label, t.label, 0.9f);
        y += ui.textHeight(0.9f) + 4;
        const float bw = 34;
        ui.textField(id(name), {x, y, w - bw - 6, 28}, value, hint, 1024);
        if (ui.button(id(name, 1), {x + w - bw, y, bw, 28}, "...", !shape.importing)) out.shape.browse = field;
        y += 36;
    };
    section(ui, x, y, w, "IMPORT MAPS");
    y += ui.paragraph(x, y, w, "Select regions on the ground (left mouse; Alt takes one away). The maps are stretched "
                               "over the rectangle around them, only the selected regions take them, and their edge "
                               "blends into the ground beside. Nothing selected: the whole world.",
                      t.labelSoft, 0.82f) + 8;
    {
        const std::string selected = shape.selectedRegions == 0 ? std::string("No regions selected")
                                     : std::to_string(shape.selectedRegions) +
                                           (shape.selectedRegions == 1 ? " region selected" : " regions selected");
        ui.text(x, y + 6, selected, t.label, 0.9f, true);
        const float bw = 52;
        if (ui.button(id("import.all"), {x + w - 2 * bw - 6, y, bw, 26}, "All")) out.shape.selectAll = true;
        if (ui.button(id("import.none"), {x + w - bw, y, bw, 26}, "None", shape.selectedRegions > 0))
            out.shape.selectAll = false;
        y += 36;
    }
    pathRow("import.height", 0, "Height map (grey PNG, 8 or 16 bit)", f.height, "path to height.png");
    pathRow("import.control", 1, "Control map (RGBA PNG, optional; alone it goes over the heights already there)",
            f.control, "moisture, forest, mountains, erosion");
    // The sea of a picture is rarely black: it is the darkest grey there is a
    // lot of. Darker than the coast is open sea (kept as nothing); land rises
    // from the coast at a metre to white.
    ui.checkbox(id("import.sea.auto"), {x, y, w, 26}, "Find the coast in the picture", f.autoSea);
    y += 32;
    if (!f.autoSea)
        sliderRow(ui, id("import.sea"), x, y, w, "Sea up to grey", std::to_string(int(std::lround(f.seaGrey))), f.seaGrey,
                  0, 128, false);
    sliderRow(ui, id("import.high"), x, y, w, "White stands at", metres(f.highMetres), f.highMetres, 100, 8000, true);
    sliderRow(ui, id("import.feather"), x, y, w, "Blend the edge over", kilometres(f.featherKm), f.featherKm, 0, 32, false);
    pathRow("import.package", 2, "Or a package folder (world.json)", f.package, "used when no picture is given");
    const bool ready = !f.height.empty() || !f.control.empty() || !f.package.empty();
    const bool heightsIn = !f.height.empty() || (f.control.empty() && !f.package.empty());
    const std::string where = shape.selectedRegions > 0
                                      ? std::to_string(shape.selectedRegions) +
                                            (shape.selectedRegions == 1 ? " region" : " regions")
                                      : std::string("the whole world");
    const std::string into = (heightsIn ? "Import heights into " : "Import the control map into ") + where;
    if (ui.button(id("import.go"), {x, y, w, 36}, shape.importing ? "Working ..." : into, t.accent, t.ink,
                  ready && !shape.importing && !shape.building))
        out.shape.import = f;
    y += 42;
    if (ui.button(id("import.clear"), {x, y, w, 30}, "Clear the selected regions",
                  shape.selectedRegions > 0 && shape.importedRegions > 0 && !shape.importing))
        out.shape.clearImport = true;
    y += 38;
    if (!shape.importLine.empty()) y += ui.paragraph(x, y, w, shape.importLine, t.label, 0.85f) + 8;

    // An import is the heights and the masks. What is worked out from them
    // is asked for a stage at a time, so a big import is looked at before
    // anything is drained or any river is fitted to it.
    section(ui, x, y, w, "WORK OUT THE IMPORTED GROUND");
    y += ui.paragraph(x, y, w, "An import brings heights and control maps only. Take the selected imported regions "
                               "(or all of them) further one step at a time. Drainage breaches the heights' hollows "
                               "and works out valleys and climate; water adds rivers and lakes on top.",
                      t.labelSoft, 0.82f) + 8;
    {
        const auto& s = shape.importedStages;
        const std::string counts = std::to_string(s[0]) + " heights only, " + std::to_string(s[1]) + " drained, " +
                                   std::to_string(s[2]) + " with water";
        y += ui.paragraph(x, y, w, counts, t.label, 0.85f) + 6;
    }
    const bool canStage = shape.importedRegions > 0 && !shape.importing && !shape.building;
    const std::string scope = shape.selectedRegions > 0 ? " (selected)" : " (all imported)";
    if (ui.button(id("import.stage.drain"), {x, y, w, 30}, "Drainage & climate" + scope, canStage))
        out.shape.importStage = generation::RegionStage::Relief;
    y += 36;
    if (ui.button(id("import.stage.water"), {x, y, w, 30}, "Rivers & lakes" + scope, canStage))
        out.shape.importStage = generation::RegionStage::Water;
    y += 36;
    if (ui.button(id("import.stage.heights"), {x, y, w, 30}, "Back to heights only" + scope, canStage))
        out.shape.importStage = generation::RegionStage::Primary;
    y += 38;

    section(ui, x, y, w, "EXPORT");
    y += ui.paragraph(x, y, w, "What is imported under the selection (or all of it), as a package to edit and import "
                               "again: raster/height.png, raster/control_0.png and world.json.",
                      t.labelSoft, 0.82f) + 8;
    pathRow("import.export", 3, "Into the folder", f.exportTo, "path to a folder");
    if (ui.button(id("import.export.go"), {x, y, w, 30}, "Export", !f.exportTo.empty() && shape.importedRegions > 0 &&
                                                                     !shape.importing))
        out.shape.exportTo = f.exportTo;
    y += 38;
    const std::string held = shape.importedRegions == 0 ? std::string("Nothing imported yet.")
                             : std::to_string(shape.importedRegions) +
                                   (shape.importedRegions == 1 ? " region holds imported ground." : " regions hold imported ground.");
    y += ui.paragraph(x, y, w, held + " Imports are not undone by Undo: clear the regions instead.", t.labelSoft, 0.8f) + 4;
    if (!shape.sourcePath.empty()) y += ui.paragraph(x, y, w, shape.sourcePath, t.labelSoft, 0.72f) + 4;
    y += ui.paragraph(x, y, w, stageLine(shape), t.label, 0.85f) + 6;
}

void ClientUi::terrainTools(ui::Ui& ui, float x, float& y, float w) {
    const auto& t = ui.theme();
    // Two kinds of brush, and the world is made of both. By hand: the ground
    // goes where it is pushed, and Restore gives it back to the generator.
    // Like nature: the generator's own processes - ridged relief, the angle
    // of repose, running water, a river's bed - run inside the brush.
    const float cw = std::floor((w - 8) * 0.5f), ch = 30;
    const auto group = [&](const char* title, std::initializer_list<world::BrushKind> kinds) {
        section(ui, x, y, w, title);
        int n = 0;
        for (const auto kind : kinds) {
            const int k = int(kind);
            const Rect r{x + float(n % 2) * (cw + 8), y + float(n / 2) * (ch + 6), cw, ch};
            if (ui.toggle(id("terrain.kind", k), r, std::to_string(k + 1) + "  " + world::tools::brushName(kind),
                          terrain.kind == kind))
                terrain.kind = kind;
            ++n;
        }
        y += float((n + 1) / 2) * (ch + 6) + 6;
    };
    using K = world::BrushKind;
    group("BY HAND", {K::Raise, K::Lower, K::Smooth, K::Flatten, K::Restore});
    group("LIKE NATURE", {K::Noise, K::Thermal, K::Hydraulic, K::Carve});
    y += ui.paragraph(x, y, w, world::tools::brushHint(terrain.kind), t.labelSoft, 0.85f) + 12;

    section(ui, x, y, w, "SETTINGS");
    // Any size: a brush a few metres across works the ground at four metres
    // to a sample, one kilometres across at a quarter of a kilometre - the
    // detail a stroke keeps is the detail its size has (EditLayer::levelFor).
    const std::string size = terrain.radius >= 1000 ? [&] {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f km", double(terrain.radius) / 1000.0);
        return std::string(text);
    }() : metres(terrain.radius);
    sliderRow(ui, id("terrain.size"), x, y, w, "Size (radius)", size, terrain.radius, 4, 8000, true);
    {
        char step[64];
        std::snprintf(step, sizeof step, "Shapes the ground at %d m a sample.",
                      int(world::EditLayer::stepOf(world::EditLayer::levelFor(terrain.radius))));
        y += ui.paragraph(x, y - 4, w, step, t.labelSoft, 0.85f) + 6;
    }
    const bool rate = terrain.kind == world::BrushKind::Smooth || terrain.kind == world::BrushKind::Flatten ||
                      terrain.kind == world::BrushKind::Thermal || terrain.kind == world::BrushKind::Hydraulic;
    char value[32];
    std::snprintf(value, sizeof value, rate ? "%.1f" : "%.1f m/s", double(terrain.strength));
    sliderRow(ui, id("terrain.strength"), x, y, w, rate ? "Rate" : "Strength", value, terrain.strength, 0.5f, 40,
              true);
    sliderRow(ui, id("terrain.softness"), x, y, w, "Soft edge", std::to_string(int(std::lround(terrain.softness))) + " %",
              terrain.softness, 0, 100, false);
    if (terrain.kind == world::BrushKind::Noise || terrain.kind == world::BrushKind::Carve)
        sliderRow(ui, id("terrain.scale"), x, y, w, terrain.kind == world::BrushKind::Carve ? "Channel width" : "Scale",
                  metres(terrain.scale), terrain.scale, 8, 400, true);
}

void ClientUi::objectTools(ui::Ui& ui, float x, float& y, float w) {
    const auto& t = ui.theme();
    section(ui, x, y, w, "MODE");
    const float half = std::floor((w - 8) * 0.5f);
    if (ui.toggle(id("objects.remove"), {x, y, half, 32}, "Remove", objects.mode == ObjectMode::Remove))
        objects.mode = ObjectMode::Remove;
    if (ui.toggle(id("objects.plant"), {x + half + 8, y, w - half - 8, 32}, "Plant", objects.mode == ObjectMode::Plant))
        objects.mode = ObjectMode::Plant;
    y += 32 + 16;

    if (objects.mode == ObjectMode::Remove) {
        section(ui, x, y, w, "WHAT TO REMOVE");
        struct Kind { const char* label; std::uint32_t mask; };
        static constexpr Kind kinds[] = {{"Trees", world::tools::kTrees}, {"Bushes", world::tools::kBushes},
                                         {"Rocks", world::tools::kRocks}, {"Logs, stumps", world::tools::kSmall}};
        for (int i = 0; i < 4; ++i) {
            const Rect r{x + float(i % 2) * (half + 8), y + float(i / 2) * 40, i % 2 ? w - half - 8 : half, 32};
            const bool on = (objects.kinds & kinds[i].mask) == kinds[i].mask;
            if (ui.toggle(id("objects.kind", i), r, kinds[i].label, on))
                objects.kinds = on ? objects.kinds & ~kinds[i].mask : objects.kinds | kinds[i].mask;
        }
        y += 80 + 8;
        section(ui, x, y, w, "BRUSH");
        sliderRow(ui, id("objects.size"), x, y, w, "Size (radius)", metres(objects.radius), objects.radius, 2, 100,
                  true);
        y += ui.paragraph(x, y, w, "Hold the left button and sweep over what should go. Undo brings it back.",
                          t.labelSoft, 0.85f) + 8;
    } else {
        section(ui, x, y, w, "WHAT TO PLANT");
        for (int m = 0; m < 8; ++m) {
            const Rect r{x + float(m % 2) * (half + 8), y + float(m / 2) * 40, m % 2 ? w - half - 8 : half, 32};
            if (ui.toggle(id("objects.model", m), r, world::tools::modelName(std::uint32_t(m)), objects.model == std::uint32_t(m)))
                objects.model = std::uint32_t(m);
        }
        y += 160 + 8;
        section(ui, x, y, w, "PLANTING");
        sliderRow(ui, id("objects.spacing"), x, y, w, "Apart along a drag", metres(objects.spacing), objects.spacing, 2,
                  60, true);
        y += ui.paragraph(x, y, w, "Click to plant one; drag to plant a row. Each is turned and sized a little "
                                   "differently.",
                          t.labelSoft, 0.85f) + 8;
    }
}

void ClientUi::statusBar(ui::Ui& ui, const ClientView& view) {
    const auto& t = ui.theme();
    const float W = float(ui.width()), H = float(ui.height());
    const Rect bar{0, H - kStatus, W, kStatus};
    ui.rect(bar, t.barBottom);
    ui.rect({0, bar.y, W, 1}, t.barEdge);
    ui.claim(bar);
    const float ty = bar.y + (kStatus - ui.textHeight(0.85f)) * 0.5f;
    std::string where = "Point at the ground";
    if (view.pointerOnGround) {
        char h[32];
        std::snprintf(h, sizeof h, "%.1f m", view.pointerHeight);
        where = "x " + grouped(view.pointerX) + " m    y " + grouped(view.pointerY) + " m    height " + h;
    }
    ui.text(12, ty, where, t.label, 0.85f);
    const char* keys = tab == EditTab::Terrain   ? (terrain.kind == world::BrushKind::Raise || terrain.kind == world::BrushKind::Lower
                                                        ? "Left mouse: sculpt    Alt: the other way    [ ]: size    1-9: brush    Ctrl+Z: undo"
                                                        : "Left mouse: sculpt    [ ]: size    1-9: brush    Ctrl+Z: undo")
                       : tab == EditTab::Objects ? "Left mouse: apply    [ ]: size    Ctrl+Z: undo"
                       : tab == EditTab::Size    ? "Add or take away regions at any side: nothing is computed"
                       : tab == EditTab::Coast   ? "Set the coast and the ground, then work out the sketched land"
                       : tab == EditTab::Import  ? "Left mouse: select a region    Alt: take it away    Enter: build"
                                                 : "Left mouse: paint    Alt: take away    [ ]: size    Enter: build    Ctrl+Z: undo";
    const float room = W - ui.textWidth(where, 0.85f) - 48;
    ui.textRight(W - 12, ty, ui.fit(keys, room, 0.85f), t.labelSoft, 0.85f);
}

void ClientUi::hints(ui::Ui& ui, const ClientView& view) {
    if (!hintsVisible_) return;
    const double shown = view.now - hintsFrom_;
    constexpr double kFor = 12, kFade = 1.5;
    if (shown > kFor) {
        hintsVisible_ = false;
        return;
    }
    const float alpha = float(std::clamp((kFor - shown) / kFade, 0.0, 1.0));
    const auto& t = ui.theme();
    const std::string text =
            "Right mouse: look    WASD: move    Q / E: down / up    Wheel: zoom    V: view    Tab: edit    Esc: menu";
    const float w = std::min(float(ui.width()) - 24, ui.textWidth(text, 0.9f) + 40);
    const Rect pill{std::round((float(ui.width()) - w) * 0.5f), float(ui.height()) - 64, w, 36};
    ui.roundRect(pill, t.barTop.withAlpha(0.88f * alpha), 18);
    ui.textCentred(pill, ui.fit(text, w - 24, 0.9f), t.label.withAlpha(alpha), 0.9f);
}

void ClientUi::pauseMenu(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    dim(ui, 0.55f);
    const Rect box = centred(ui, 360, 372);
    ui.panel(box);
    ui.text(box.x + 26, box.y + 24, "Paused", t.label, 1.5f, true);
    ui.text(box.x + 26, box.y + 24 + ui.textHeight(1.5f) + 10, ui.fit(view.worldName, box.w - 52, 0.9f), t.labelSoft,
            0.9f);
    const float w = box.w - 52, h = 42;
    float y = box.y + 104;
    if (ui.button(id("pause.resume"), {box.x + 26, y, w, h}, "Resume", t.accent, t.ink)) paused = false;
    y += h + 10;
    if (ui.button(id("pause.save"), {box.x + 26, y, w, h}, view.unsaved ? "Save" : "Saved", view.unsaved)) out.save = true;
    y += h + 10;
    if (ui.button(id("pause.settings"), {box.x + 26, y, w, h}, "Settings")) {
        paused = false;
        out.toggleSettings = true;
    }
    y += h + 10;
    if (ui.button(id("pause.menu"), {box.x + 26, y, w, h}, "Save and leave to menu")) {
        paused = false;
        out.toMenu = true;
    }
    y += h + 10;
    if (ui.button(id("pause.quit"), {box.x + 26, y, w, h}, "Save and quit")) out.quit = true;
    ui.claim({0, 0, float(ui.width()), float(ui.height())});
}

void ClientUi::settingsDone(ui::Ui& ui, const ClientView& view, ClientActions& out) {
    const auto& t = ui.theme();
    ui.claim(view.settingsRect);
    const Rect done{view.settingsRect.x, view.settingsRect.bottom() + 8, view.settingsRect.w, 38};
    if (ui.button(id("settings.done"), done, "Done", t.accent, t.ink)) out.toggleSettings = true;
}

void ClientUi::toastLine(ui::Ui& ui, const ClientView& view) {
    if (toast_.empty() || view.now > toastUntil_) return;
    const auto& t = ui.theme();
    const float w = std::min(float(ui.width()) - 24, ui.textWidth(toast_, 0.95f) + 44);
    // Under the "raising" pill when the world is being built again, not on it.
    const float top = screen == Screen::World ? kBar + 12 + (view.rebuilding ? 42.0f : 0.0f) : 20;
    const Rect pill{std::round((float(ui.width()) - w) * 0.5f), top, w, 34};
    ui.roundRect(pill, toastBad_ ? t.danger.withAlpha(0.95f) : t.barTop, 17);
    ui.border(pill, toastBad_ ? t.selection.withAlpha(0.6f) : t.accent.withAlpha(0.8f));
    ui.textCentred(pill, ui.fit(toast_, w - 24, 0.95f), toastBad_ ? t.selection : t.label, 0.95f);
}

void ClientUi::alertBox(ui::Ui& ui) {
    if (alertTitle_.empty()) return;
    const auto& t = ui.theme();
    dim(ui, 0.5f);
    const Rect box = centred(ui, 480, 220);
    ui.panel(box);
    ui.text(box.x + 24, box.y + 24, ui.fit(alertTitle_, box.w - 48, 1.3f, true), t.label, 1.3f, true);
    ui.paragraph(box.x + 24, box.y + 24 + ui.textHeight(1.3f) + 16, box.w - 48, alertText_, t.labelSoft, 0.92f);
    if (ui.button(id("alert.ok"), {box.right() - 24 - 120, box.bottom() - 24 - 40, 120, 40}, "OK", t.accent, t.ink)) {
        alertTitle_.clear();
        alertText_.clear();
    }
    ui.claim({0, 0, float(ui.width()), float(ui.height())});
}

bool ClientUi::key(SDL_Keycode key, SDL_Keymod mod, ClientActions& out, bool typing) {
    const bool command = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
    const bool shift = (mod & SDL_KMOD_SHIFT) != 0;
    if (!alertTitle_.empty()) {
        if (key == SDLK_ESCAPE || key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            alertTitle_.clear();
            alertText_.clear();
        }
        return true;
    }
    if (typing) return key == SDLK_ESCAPE;   // the field gives the keyboard back itself
    if (confirmingDelete_) {
        if (key == SDLK_ESCAPE) { confirmingDelete_ = false; pendingDelete_.reset(); }
        return true;
    }
    switch (screen) {
        case Screen::Menu:
            return false;
        case Screen::Host:
            if (key == SDLK_ESCAPE) {
                if (creating_) creating_ = false;
                else screen = Screen::Menu;
                return true;
            }
            return false;
        case Screen::Loading:
            return true;
        case Screen::World:
            break;
    }
    if (key == SDLK_ESCAPE) { paused = !paused; return true; }
    if (paused) return false;
    if (command && key == SDLK_S) { out.save = true; return true; }
    if (key == SDLK_TAB) {
        out.mode = mode == WorldMode::Explore ? WorldMode::Edit : WorldMode::Explore;
        return true;
    }
    if (key == SDLK_V) { out.cycleCamera = true; return true; }
    if (key == SDLK_F1 && mode == WorldMode::Explore) {
        hintsVisible_ = !hintsVisible_;
        hintsFrom_ = lastNow_;
        return true;
    }
    if (mode != WorldMode::Edit) return false;
    if (shapeTab(tab)) {
        // The world's shape: its own undo, its own build, the stage's brush.
        if (command && key == SDLK_Z) { out.shape.undo = true; return true; }
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) { out.shape.build = true; return true; }
        if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET) {
            const float factor = key == SDLK_RIGHTBRACKET ? 1.25f : 0.8f;
            if (tab == EditTab::Land) land.radiusKm = std::clamp(land.radiusKm * factor, 2.0f, 128.0f);
            if (tab == EditTab::Mountains) mountains.radiusKm = std::clamp(mountains.radiusKm * factor, 2.0f, 64.0f);
            if (tab == EditTab::Water) climate.radiusKm = std::clamp(climate.radiusKm * factor, 8.0f, 256.0f);
            return true;
        }
        return false;
    }
    if (command && key == SDLK_Z) { (shift ? out.redo : out.undo) = true; return true; }
    if (command && key == SDLK_Y) { out.redo = true; return true; }
    if (key == SDLK_LEFTBRACKET || key == SDLK_RIGHTBRACKET) {
        const float factor = key == SDLK_RIGHTBRACKET ? 1.25f : 0.8f;
        if (tab == EditTab::Terrain) terrain.radius = std::clamp(terrain.radius * factor, 4.0f, 8000.0f);
        else objects.radius = std::clamp(objects.radius * factor, 2.0f, 100.0f);
        return true;
    }
    if (tab == EditTab::Terrain && key >= SDLK_1 && key < SDLK_1 + int(world::BrushKind::Count)) {
        terrain.kind = world::BrushKind(int(key - SDLK_1));
        return true;
    }
    return false;
}

} // namespace client

