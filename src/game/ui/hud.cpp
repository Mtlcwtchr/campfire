#include "game/ui/hud.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "game/simulation/inventory.hpp"
#include "game/ui/info_panel.hpp"

namespace ui {
namespace {

// The shape of the thing, in one place: change these and the whole HUD moves
// together. Taken off the reference art rather than invented.
constexpr float kTopBarHeight = 46.0f;
constexpr float kDeckBarHeight = 34.0f;
constexpr float kCardRowHeight = 108.0f;
constexpr float kCardWidth = 92.0f;
constexpr float kInspectorWidth = 300.0f;

std::string percent(double v) {
    char out[16];
    std::snprintf(out, sizeof out, "%d%%", static_cast<int>(std::lround(v * 100.0)));
    return out;
}

std::string number(std::int64_t v) { return std::to_string(v); }

// The community's own state, averaged over the people in it: what the top bar
// reports. Read once a frame rather than by each gauge.
struct Vitals {
    std::int32_t people = 0;
    std::int32_t growth = 0;      // born minus died, over this community's life
    double hunger = 1.0;          // 1 is fed
    double rest = 1.0;
    double health = 1.0;
    std::int32_t flock = 0;
    std::int32_t ailing = 0;
};

Vitals readVitals(const sim::World& w) {
    Vitals v;
    double hunger = 0, rest = 0, health = 0;
    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        ++v.people;
        hunger += p.satiety.toDouble();
        rest += p.rest.toDouble();
        health += p.health.toDouble();
    }
    if (v.people > 0) {
        hunger /= v.people;
        rest /= v.people;
        health /= v.people;
        v.hunger = hunger;
        v.rest = rest;
        v.health = health;
    }
    for (const auto& a : w.animals())
        if (a.alive && a.owner.valid()) ++v.flock;
    v.ailing = w.ailing();
    v.growth = w.report().births - w.report().deaths;
    return v;
}

Colour gaugeColour(const Ui& ui, double value) {
    if (value > 0.6) return ui.theme().good;
    if (value > 0.3) return ui.theme().warn;
    return ui.theme().bad;
}

// What the Build deck offers: every building this community knows how to raise,
// in the order the content lists them. Nothing here is a special case - a new
// building in a JSON file appears on the bar.
std::vector<core::DefId> buildableHere(const sim::World& w) {
    std::vector<core::DefId> out;
    if (w.settlements().empty()) return out;
    for (const auto& def : w.db().buildings()) {
        if (def.kind == content::BuildingKind::Other && def.irrigationRadius <= 0) continue;
        out.push_back(def.id);
    }
    return out;
}

} // namespace

const char* deckName(Deck d) {
    switch (d) {
        case Deck::Buildings: return "Buildings";
        case Deck::Production: return "Production";
        case Deck::People: return "People";
        case Deck::Zones: return "Zones";
        case Deck::Logistics: return "Logistics";
        case Deck::Ritual: return "Ritual";
        default: return "?";
    }
}

namespace {

// --- the top bar ---------------------------------------------------------
void drawTopBar(Ui& ui, Hud& hud, const sim::World& w, const Vitals& vitals) {
    const Theme& theme = ui.theme();
    const Rect bar{0, 0, static_cast<float>(ui.width()), kTopBarHeight};
    ui.rect(bar, theme.barTop);
    ui.line(0, bar.bottom(), bar.right(), bar.bottom(), theme.barEdge);
    ui.claim(bar);

    // A slab of the bar, with a rule between one reading and the next: the top
    // bar is a row of separate facts, and without the rules it reads as one.
    float x = 14;
    const auto divider = [&](float at) {
        ui.line(at, 8, at, kTopBarHeight - 8, theme.barEdge.withAlpha(0.5f));
    };

    ui.text(x, 7, "PEOPLE", theme.labelSoft, 0.8f);
    ui.text(x, 20, number(vitals.people), theme.label, 1.7f);
    const std::string growth = (vitals.growth >= 0 ? "+" : "") + number(vitals.growth);
    ui.text(x + ui.textWidth(number(vitals.people), 1.7f) + 8, 26, growth,
            vitals.growth >= 0 ? theme.good : theme.bad, 0.9f);
    x += 104;
    divider(x - 12);

    // The three things that kill a community, as bars.
    const struct { const char* label; double value; } gauges[] = {
            {"Fed", vitals.hunger}, {"Rested", vitals.rest}, {"Health", vitals.health}};
    for (const auto& g : gauges) {
        const Rect slot{x, 9, 120, kTopBarHeight - 18};
        ui.text(slot.x, slot.y, g.label, theme.labelSoft, 0.85f);
        ui.textRight(slot.right(), slot.y, percent(g.value), theme.label, 0.85f);
        ui.gauge({slot.x, slot.y + 16, slot.w, 7}, static_cast<float>(g.value),
                 gaugeColour(ui, g.value), theme.gauge);
        x += slot.w + 22;
    }
    divider(x - 14);

    // The flock and the sick: the two other numbers a player watches.
    ui.text(x, 7, "FLOCK", theme.labelSoft, 0.8f);
    ui.text(x, 21, number(vitals.flock), theme.label, 1.3f);
    x += 76;
    if (vitals.ailing > 0) {
        ui.text(x, 7, "AILING", theme.labelSoft, 0.8f);
        ui.text(x, 21, number(vitals.ailing), theme.bad, 1.3f);
    }

    // The clock, and the speed it runs at.
    const auto date = w.now();
    const std::string season = std::string(core::seasonName(date.season));
    char when[64];
    std::snprintf(when, sizeof when, "Year %d, Day %d", date.year + 1, date.dayOfSeason + 1);
    char oclock[16];
    std::snprintf(oclock, sizeof oclock, "%02d:00", date.hour);

    const float speedWidth = 4 * 32.0f + 10;
    const Rect clock{bar.right() - speedWidth - 210, 5, 196, kTopBarHeight - 10};
    ui.line(clock.x - 14, 8, clock.x - 14, kTopBarHeight - 8, theme.barEdge.withAlpha(0.5f));
    std::string titled = season;
    if (!titled.empty()) titled[0] = static_cast<char>(std::toupper(titled[0]));
    ui.text(clock.x, clock.y + 1, titled, theme.accent, 1.0f);
    ui.text(clock.x, clock.y + 17, when, theme.label, 0.85f);
    ui.textRight(clock.right(), clock.y + 1, oclock, theme.labelSoft, 0.9f);

    // Pause, play, fast, faster - the ladder the simulation actually has.
    static const char* kSpeedFaces[] = {"||", ">", ">>", ">>>"};
    static const int kSpeedIndex[] = {0, 1, 3, 5};
    for (int i = 0; i < 4; ++i) {
        const Rect key{bar.right() - speedWidth + i * 32.0f, 8, 28, kTopBarHeight - 16};
        const bool on = hud.speed == kSpeedIndex[i];
        if (ui.iconButton(widgetId("hud.speed", i), key, nullptr, kSpeedFaces[i], on)) {
            hud.speed = kSpeedIndex[i];
            if (hud.speed > 0) hud.speedBeforePause = hud.speed;
        }
    }
    if (hud.speedLimited)
        ui.textRight(bar.right() - speedWidth - 200, 26, "(slow)", theme.warn, 0.8f);
}

// --- the deck along the bottom -------------------------------------------
void drawDeck(Ui& ui, Hud& hud, const sim::World& w, const client::Renderer& art) {
    const Theme& theme = ui.theme();
    const float top = ui.height() - kDeckBarHeight - kCardRowHeight;
    const Rect tabs{0, top, static_cast<float>(ui.width()), kDeckBarHeight};
    const Rect cards{0, tabs.bottom(), static_cast<float>(ui.width()), kCardRowHeight};
    ui.rect(tabs, theme.barBottom);
    ui.rect(cards, theme.barBottom.withAlpha(0.99f));
    ui.line(0, tabs.y, tabs.right(), tabs.y, theme.barEdge);
    ui.claim(tabs);
    ui.claim(cards);

    float x = 8;
    for (int i = 0; i < static_cast<int>(Deck::Count); ++i) {
        const auto deck = static_cast<Deck>(i);
        const std::string label = deckName(deck);
        const float width = ui.textWidth(label) + 34;
        if (ui.tab(widgetId("hud.deck", i), {x, tabs.y + 2, width, tabs.h - 2}, label,
                   hud.deck == deck)) {
            hud.deck = deck;
            hud.drawingZones = deck == Deck::Zones;
        }
        x += width;
    }
    // What the player is actually doing, said plainly on the right of the bar.
    // There is no build button and no demolish button: the community decides
    // what to raise and what to pull down (D110).
    const std::string doing = hud.drawingZones ? "drawing areas" : "watching";
    ui.textRight(tabs.right() - 12, tabs.y + 11, doing, theme.labelSoft, 0.9f);

    // What the chosen deck offers.
    float cardX = 10;
    const Rect row = cards.inset(8, 6, 8, 6);
    switch (hud.deck) {
        case Deck::Buildings: {
            // What this community knows how to raise, and how much of it stands.
            // A reading, not a control: nothing here can be clicked into the
            // world, because where a building goes is the community's business.
            for (core::DefId id : buildableHere(w)) {
                if (cardX + kCardWidth > row.right()) break;
                const auto& def = w.db().building(id);
                std::int32_t standing = 0, building = 0;
                for (const auto& b : w.buildings()) {
                    if (!b.alive || b.def != id) continue;
                    if (b.state == sim::BuildState::Complete) ++standing;
                    else ++building;
                }
                const Rect slot{cardX, row.y, kCardWidth, row.h};
                ui.roundRect(slot, standing > 0 ? theme.slotHot : theme.slot, 4.0f);
                ui.border(slot, theme.slotEdge);
                if (SDL_Texture* picture = art.pictureOf("buildings/" + def.name))
                    ui.image({slot.x + 4, slot.y + 4, slot.w - 8, slot.h - 30}, picture,
                             standing > 0 ? Colour{} : Colour{0.6f, 0.6f, 0.6f, 0.75f});
                ui.textCentred({slot.x + 3, slot.bottom() - 20, slot.w - 6, 16},
                               ui.fit(def.label, slot.w - 6, 0.85f),
                               standing > 0 ? theme.label : theme.labelSoft, 0.85f);
                if (standing > 0 || building > 0) {
                    const std::string count =
                            number(standing) + (building > 0 ? "+" + number(building) : "");
                    ui.text(slot.x + 6, slot.y + 4, count, theme.accent, 0.9f);
                }
                cardX += kCardWidth + 6;
            }
            break;
        }
        case Deck::Zones: {
            static const struct { const char* label; sim::ZoneKind kind; } kBrushes[] = {
                    {"Fields", sim::ZoneKind::Farm},      {"Pasture", sim::ZoneKind::Pasture},
                    {"Timber", sim::ZoneKind::Timber},    {"Quarry", sim::ZoneKind::Extraction},
                    {"Hunting", sim::ZoneKind::Hunting},  {"Fishing", sim::ZoneKind::Fishing},
                    {"Houses", sim::ZoneKind::Residential}, {"Craft", sim::ZoneKind::Craft},
                    {"Common", sim::ZoneKind::Civic},     {"Stores", sim::ZoneKind::Storage},
            };
            for (const auto& brush : kBrushes) {
                if (cardX + kCardWidth > row.right()) break;
                const Rect slot{cardX, row.y, kCardWidth, row.h};
                const bool on = hud.drawingZones && hud.brushKind == brush.kind;
                if (ui.card(widgetId("hud.zone", static_cast<int>(brush.kind)), slot, nullptr,
                            brush.label, on)) {
                    hud.brushKind = brush.kind;
                    hud.drawingZones = true;
                }
                cardX += kCardWidth + 6;
            }
            break;
        }
        case Deck::People: {
            // Who does what, as a row of trades with the count on each.
            std::vector<std::pair<std::string, int>> trades;
            for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
                int n = 0;
                for (const auto& p : w.people())
                    if (p.alive && static_cast<std::size_t>(p.profession) == i) ++n;
                if (n > 0)
                    trades.emplace_back(content::workCategoryName(static_cast<content::WorkCategory>(i)), n);
            }
            for (const auto& [name, count] : trades) {
                if (cardX + kCardWidth > row.right()) break;
                const Rect slot{cardX, row.y, kCardWidth, row.h};
                ui.roundRect(slot, theme.slot, 4.0f);
                ui.border(slot, theme.slotEdge);
                ui.textCentred({slot.x, slot.y + 18, slot.w, 24}, number(count), theme.label, 1.6f);
                ui.textCentred({slot.x, slot.bottom() - 22, slot.w, 16}, name, theme.labelSoft, 0.85f);
                cardX += kCardWidth + 6;
            }
            break;
        }
        default: {
            const std::string note = std::string(deckName(hud.deck)) + ": nothing here yet";
            ui.text(row.x + 4, row.y + 8, note, theme.labelSoft, 0.9f);
            break;
        }
    }
}

// --- the inspector on the right ------------------------------------------
void drawInspector(Ui& ui, Hud& hud, const sim::World& w, const client::Renderer& art) {
    if (!hud.selected.valid()) return;
    const Theme& theme = ui.theme();
    const InfoText info = describe(w, hud.selected);

    const float top = kTopBarHeight + 10;
    const float bottom = ui.height() - kDeckBarHeight - kCardRowHeight - 10;
    const Rect panel{ui.width() - kInspectorWidth - 10, top, kInspectorWidth, bottom - top};
    ui.panel(panel, true);

    const Rect body = panel.inset(12);
    float y = body.y;

    ui.text(body.x, y, info.title, theme.ink, 1.5f);
    const Rect close{panel.right() - 26, panel.y + 8, 18, 18};
    if (ui.button(widgetId("hud.close"), close, "x")) hud.selected = {};
    y += ui.textHeight(1.5f) + 4;

    if (!info.subtitle.empty()) {
        ui.text(body.x, y, info.subtitle, theme.inkSoft, 0.9f);
        y += ui.textHeight(0.9f) + 6;
    }

    // A picture of the thing, where there is one.
    if (SDL_Texture* portrait = art.pictureOf(info.picture); portrait != nullptr) {
        const Rect frame{body.x, y, body.w, 92};
        ui.roundRect(frame, theme.paperShade, 4.0f);
        ui.image(frame.inset(6), portrait);
        ui.border(frame, theme.paperEdge);
        y += frame.h + 8;
    }

    // The bars a player actually watches, for the two kinds of thing that have
    // them. Everything else falls through to the writing below.
    if (hud.selected.kind == client::SelectionKind::Person &&
        hud.selected.person.value < w.people().size()) {
        const sim::Person& p = w.people()[hud.selected.person.value];
        const struct { const char* label; float value; Colour colour; } bars[] = {
                {"Fed", static_cast<float>(p.satiety.toDouble()), theme.good},
                {"Water", static_cast<float>(p.hydration.toDouble()), theme.good},
                {"Rested", static_cast<float>(p.rest.toDouble()), theme.warn},
                {"Health", static_cast<float>(p.health.toDouble()), theme.bad}};
        for (const auto& bar : bars) {
            ui.text(body.x, y, bar.label, theme.inkSoft, 0.9f);
            ui.textRight(body.right(), y, percent(bar.value), theme.ink, 0.9f);
            ui.gauge({body.x, y + 12, body.w, 7}, bar.value, bar.colour, theme.paperShade);
            y += 26;
        }
        y += 4;
    } else if (hud.selected.kind == client::SelectionKind::Building &&
               hud.selected.building.value < w.buildings().size()) {
        const sim::Building& b = w.buildings()[hud.selected.building.value];
        const auto& def = w.db().building(b.def);
        if (b.state != sim::BuildState::Complete && def.workAmount > core::kZero) {
            const float done = static_cast<float>(b.workDone.toDouble() / def.workAmount.toDouble());
            ui.text(body.x, y, "Built", theme.inkSoft, 0.9f);
            ui.textRight(body.right(), y, percent(done), theme.ink, 0.9f);
            ui.gauge({body.x, y + 12, body.w, 7}, done, theme.warn, theme.paperShade);
            y += 28;
        }
    }

    // Whatever the inspector has to say, line by line. The lines come from
    // info_panel, which is the one place that knows how to describe a thing: a
    // line beginning with '#' is a heading, an empty one is a break.
    for (const auto& line : info.lines) {
        if (y > body.bottom() - 18) break;
        if (line.empty()) {
            y += 6;
            continue;
        }
        if (line[0] == '#') {
            y += 4;
            const std::string heading = line.substr(1);
            ui.text(body.x, y, heading, theme.inkSoft, 1.0f);
            y += ui.textHeight(1.0f) + 2;
            ui.line(body.x, y, body.right(), y, theme.paperEdge.withAlpha(0.6f));
            y += 5;
            continue;
        }
        // A line of the form "label: value" is set as two columns, which is what
        // makes a column of numbers read as a column.
        const auto colon = line.find(": ");
        const std::string label = colon == std::string::npos ? std::string() : line.substr(0, colon);
        const std::string value = colon == std::string::npos ? std::string() : line.substr(colon + 2);
        // Two columns only when the two of them actually fit side by side. A
        // right-aligned value that is longer than the space left over lands on
        // top of its own label, which is how "fire or cooking and boiling" came
        // out written over itself.
        const bool twoColumns = colon != std::string::npos && colon < 24 &&
                                ui.textWidth(label, 0.9f) + ui.textWidth(value, 0.9f) + 12 < body.w;
        if (twoColumns) {
            ui.text(body.x, y, label, theme.inkSoft, 0.9f);
            ui.textRight(body.right(), y, value, theme.ink, 0.9f);
        } else {
            ui.text(body.x, y, ui.fit(line, body.w, 0.9f), theme.ink, 0.9f);
        }
        y += ui.textHeight(0.9f) + 6;
    }
}

} // namespace

void drawHud(Ui& ui, Hud& hud, const sim::World& w, const client::Renderer& art,
             double simMsPerTick) {
    const Vitals vitals = readVitals(w);
    drawTopBar(ui, hud, w, vitals);
    drawDeck(ui, hud, w, art);
    drawInspector(ui, hud, w, art);

    // The one line of engine state a player ever needs, kept out of the way.
    char rate[64];
    std::snprintf(rate, sizeof rate, "%.2f ms/tick", simMsPerTick);
    ui.text(8, ui.height() - kDeckBarHeight - kCardRowHeight - 16, rate,
            ui.theme().labelSoft.withAlpha(0.7f), 0.8f);

    hud.overInterface = ui.input().overUi;
}

} // namespace ui
