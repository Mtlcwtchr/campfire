#include "game/ui/overlay.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "game/simulation/inventory.hpp"
#include "game/work/planner.hpp"
#include "game/simulation/zones.hpp"
#include "game/ui/info_panel.hpp"

namespace ui {
namespace {

constexpr float kLine = 12.0f;

void text(SDL_Renderer* r, float x, float y, const std::string& s, Uint8 alpha = 255) {
    SDL_SetRenderDrawColor(r, 236, 232, 224, alpha);
    SDL_RenderDebugText(r, x, y, s.c_str());
}

void panel(SDL_Renderer* r, float x, float y, float w, float h) {
    SDL_SetRenderDrawColor(r, 16, 18, 22, 210);
    const SDL_FRect rect{x, y, w, h};
    SDL_RenderFillRect(r, &rect);
    SDL_SetRenderDrawColor(r, 90, 96, 104, 220);
    SDL_RenderRect(r, &rect);
}

std::string fixed1(core::Fixed v) { return core::toString(v, 1); }

} // namespace

bool hitPriorityControl(const HudState& hud, const HudLayout& layout, float x, float y,
                        int& outCategory, int& outDelta) {
    if (!hud.showPriorities || layout.priorities.w <= 0) return false;
    if (x < layout.priorities.x || x > layout.priorities.x + layout.priorities.w) return false;
    if (y < layout.priorityFirstRow) return false;

    const int row = static_cast<int>((y - layout.priorityFirstRow) / layout.priorityRowHeight);
    if (row < 0 || row >= static_cast<int>(content::kWorkCategoryCount)) return false;

    const float right = layout.priorities.x + layout.priorities.w;
    if (x >= right - 46 && x < right - 26) { outCategory = row; outDelta = -1; return true; }
    if (x >= right - 26) { outCategory = row; outDelta = +1; return true; }
    return false;
}

const char* speedLabel(int index) {
    static const char* kLabels[] = {"paused", "1x", "2x", "5x", "10x", "100x"};
    static_assert(sizeof(kLabels) / sizeof(kLabels[0]) == kSpeedCount,
                  "speed labels must match the ladder");
    return (index >= 0 && index < kSpeedCount) ? kLabels[index] : "?";
}

bool hitTradeHeading(const sim::World& w, const HudState& hud, const HudLayout& layout, float x,
                     float y, int& outCategory) {
    if (!hud.showTrades || layout.trades.w <= 0) return false;
    if (x < layout.trades.x || x > layout.trades.x + layout.trades.w) return false;
    if (y < layout.tradeFirstRow) return false;

    const int row = static_cast<int>((y - layout.tradeFirstRow) / layout.tradeRowHeight);
    if (row < 0) return false;

    // The panel's own text decides what a row is: a heading is a trade, an
    // indented line is a person. Reading it back is how the click and the
    // drawing cannot disagree about which row is which.
    const sim::SettlementId home = w.settlements().empty() ? sim::SettlementId{}
                                                           : w.settlements().front().id;
    const InfoText info = describeTrades(w, home, hud.expandedTrades);
    if (row >= static_cast<int>(info.lines.size())) return false;
    const std::string& line = info.lines[static_cast<std::size_t>(row)];
    if (line.rfind("    ", 0) == 0) return false;

    const std::string name = line.substr(2, line.find(':') - 2);
    for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i)
        if (name == content::workCategoryName(static_cast<content::WorkCategory>(i))) {
            outCategory = static_cast<int>(i);
            return true;
        }
    return false;
}

HudLayout drawHud(SDL_Renderer* r, const sim::World& w, const HudState& hud, double simMsPerTick) {
    HudLayout layout;
    int outW = 0, outH = 0;
    SDL_GetCurrentRenderOutputSize(r, &outW, &outH);

    const auto date = w.now();

    // --- top bar: the world's own clock and vital signs ------------------
    panel(r, 8, 8, 560, 62);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "year %d  %s  day %d  %02d:00   %s C   %s",
                  date.year, std::string(core::seasonName(date.season)).c_str(), date.dayOfSeason + 1,
                  date.hour, fixed1(w.outdoorTemperature()).c_str(),
                  date.isDaylight ? "daylight" : "night");
    text(r, 16, 16, buf);

    std::int32_t working = 0, alive = 0;
    core::Fixed food = core::kZero;
    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        ++alive;
        if (p.job.valid()) ++working;
    }
    for (const auto& s : w.stacks()) {
        if (!sim::isAvailable(s)) continue;
        const auto& def = w.db().item(s.def);
        if (def.category == content::ItemCategory::Food)
            food += def.nutrition * std::int64_t(s.count) * s.freshness;
    }
    std::int32_t flock = 0;
    for (const auto& a : w.animals()) if (a.alive) ++flock;
    std::int32_t sown = 0, ripe = 0;
    for (std::int32_t y = 0; y < w.map().height(); ++y)
        for (std::int32_t x = 0; x < w.map().width(); ++x) {
            const auto& t = w.map().at({x, y});
            if (!t.crop.valid()) continue;
            ++sown;
            if (t.cropGrowth >= core::kOne) ++ripe;
        }
    std::snprintf(buf, sizeof(buf), "people %d  working %d  food %s  flock %d  sown %d (%d ripe)  known %d",
                  alive, working, core::toString(food, 0).c_str(), flock, sown, ripe,
                  w.exploredCount());
    text(r, 16, 16 + kLine * 2, buf);

    std::snprintf(buf, sizeof(buf), "tick %lld   speed %s%s   overlay %s   %.2f ms/tick",
                  static_cast<long long>(w.tickCount()), speedLabel(hud.speed),
                  hud.speedLimited ? " (cpu-bound)" : "",
                  client::overlayName(hud.overlay), simMsPerTick);
    text(r, 16, 16 + kLine * 3.5f, buf, 190);

    // --- the inspector ----------------------------------------------------
    // Everything clickable explains itself here: person, building, standing tree,
    // pile of grain, or the bare tile.
    layout.inspectorWidth = drawInfoPanel(r, w, hud.selected);

    // --- priorities: a weight on the planner, not an order ------------------
    if (hud.showPriorities) {
        const float rows = static_cast<float>(content::kWorkCategoryCount);
        const float h = 26 + rows * layout.priorityRowHeight;
        layout.priorities = {8.0f, 82.0f, 260.0f, h};
        layout.priorityFirstRow = layout.priorities.y + 22.0f;
        panel(r, layout.priorities.x, layout.priorities.y, layout.priorities.w, layout.priorities.h);
        text(r, layout.priorities.x + 8, layout.priorities.y + 6, "WORK PRIORITIES  (click - / +)");

        const auto& st = w.settlements().front();
        for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
            const float y = layout.priorityFirstRow + layout.priorityRowHeight * i;
            const core::Fixed value = st.priorities[i] <= core::kZero ? core::kOne : st.priorities[i];
            char row[96];
            std::snprintf(row, sizeof(row), "%-14s %s",
                          std::string(content::workCategoryName(static_cast<content::WorkCategory>(i))).c_str(),
                          core::toString(value, 1).c_str());
            // Dim the ones the community is not set up for, so the list reads.
            text(r, layout.priorities.x + 8, y, row, value == core::kOne ? 170 : 255);
            text(r, layout.priorities.x + layout.priorities.w - 40, y, "-", 220);
            text(r, layout.priorities.x + layout.priorities.w - 20, y, "+", 220);
        }
    }

    // --- zone brush ---------------------------------------------------------
    if (hud.drawingZones) {
        const float w0 = 560.0f;
        const float x = (outW - w0) / 2.0f;
        const float y = outH - 116.0f;
        panel(r, x, y, w0, 52);
        std::snprintf(buf, sizeof(buf), "DRAWING AREAS: %s / %s   radius %d",
                      std::string(sim::zoneKindName(hud.brushKind)).c_str(),
                      hud.brushMode == sim::ZoneMode::Forbidden ? "forbidden"
                              : hud.brushMode == sim::ZoneMode::HighPriority ? "high priority"
                              : hud.brushMode == sim::ZoneMode::Preferred ? "preferred" : "allowed",
                      hud.brushRadius);
        text(r, x + 10, y + 8, buf);
        text(r, x + 10, y + 8 + kLine * 1.6f,
             "[ ] kind    m mode    , . size    drag to paint, right drag to erase    e done", 200);
    }

    // --- who does what, and what we have ----------------------------------
    // Two questions a player asks constantly, and without these could only
    // answer by clicking every person and every store in turn.
    const sim::SettlementId home = w.settlements().empty() ? sim::SettlementId{}
                                                           : w.settlements().front().id;
    float summaryY = 80.0f;
    if (hud.showTrades && home.valid()) {
        const InfoText info = describeTrades(w, home, hud.expandedTrades);
        const float width = 300.0f;
        const float height = kLine * 1.4f * (info.lines.size() + 3) + 10;
        panel(r, 8, summaryY, width, height);
        text(r, 16, summaryY + 6, info.title);
        text(r, 16, summaryY + 6 + kLine * 1.4f, info.subtitle, 190);
        layout.trades = {8, summaryY, width, height};
        layout.tradeRowHeight = kLine * 1.4f;
        layout.tradeFirstRow = summaryY + 6 + kLine * 1.4f * 2.4f;
        for (std::size_t i = 0; i < info.lines.size(); ++i)
            text(r, 16, layout.tradeFirstRow + kLine * 1.4f * i, info.lines[i],
                 info.lines[i].rfind("    ", 0) == 0 ? 190 : 255);
        summaryY += height + 8;
    }
    if (hud.showStores && home.valid()) {
        const InfoText info = describeStores(w, home);
        const int rows = 24;
        const int first = std::clamp(hud.storeScroll, 0,
                                     std::max(0, static_cast<int>(info.lines.size()) - rows));
        const float width = 300.0f;
        const float height = kLine * 1.4f * (std::min<int>(rows, static_cast<int>(info.lines.size())) + 3) + 10;
        panel(r, 8, summaryY, width, height);
        text(r, 16, summaryY + 6, info.title);
        text(r, 16, summaryY + 6 + kLine * 1.4f, info.subtitle, 190);
        layout.stores = {8, summaryY, width, height};
        float y = summaryY + 6 + kLine * 1.4f * 2.4f;
        for (int i = first; i < static_cast<int>(info.lines.size()) && i < first + rows; ++i) {
            const std::string& line = info.lines[static_cast<std::size_t>(i)];
            const bool heading = !line.empty() && line[0] == '#';
            text(r, 16, y, heading ? line.substr(1) : line, heading ? 255 : 200);
            y += kLine * 1.4f;
        }
    }

    // --- controls ----------------------------------------------------------
    if (hud.showHelp) {
        static const std::array<const char*, 12> lines{
            "TECHNICAL DIRECTION (settings, not law)",
            "  space pause    1..5 speed 1x 2x 5x 10x 100x    -/+ step speed",
            "  z zones    j jobs    o overlay off    h hide this panel",
            "  e draw areas    p work priorities    wasd pan    wheel zoom",
            "  t who does what (click a trade to open it)    y what we have",
            "  click anything to inspect it; click again to reach what is",
            "  underneath it; right click clears the selection",
            "  cyan HOME border; orange under pawn = outside HOME",
            "THE COUNTRY (zoom out past the map)",
            "  click a settlement to read it    enter to go and look",
            "  esc comes home",
            "",
        };
        const float h = kLine * 1.5f * lines.size() + 12;
        panel(r, 8, outH - h - 8, 520, h);
        for (std::size_t i = 0; i < lines.size(); ++i)
            text(r, 16, outH - h + kLine * 1.5f * i, lines[i], i == 0 || i == 8 ? 255 : 190);
    }
    return layout;
}

} // namespace ui
