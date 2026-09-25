#pragma once
// The HUD. GDD 9 requires the interface to keep two things visibly apart: the
// technical direction the player gives the planner (zones, priorities) and the
// political acts of the ruler inside the world. This panel labels them as such
// even while only the technical loop exists.

#include <SDL3/SDL.h>

#include "game/client/renderer.hpp"
#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"

namespace ui {

// Speed ladder. Index 0 is paused; the rest are multipliers on the simulation
// rate. One table, read by both the input handler and the HUD, so the key that
// selects a speed and the label that reports it cannot drift apart.
inline constexpr int kSpeedLadder[] = {0, 1, 2, 5, 10, 100};
inline constexpr int kSpeedCount = static_cast<int>(sizeof(kSpeedLadder) / sizeof(kSpeedLadder[0]));

const char* speedLabel(int index);

struct HudState {
    int speed = 1;                 // index into kSpeedLadder; 0 = paused
    int speedBeforePause = 1;      // so unpausing returns to the speed you were at
    client::Overlay overlay = client::Overlay::None;
    client::Selection selected;
    bool showHelp = true;
    // True when the simulation could not deliver the requested number of ticks
    // inside the frame's time budget, so the HUD can say so instead of quietly
    // running slower than it claims.
    bool speedLimited = false;

    // --- the two control loops of GDD 9, kept visibly apart ---------------
    // Drawing an area is technical direction: it takes effect at once and cannot
    // be disobeyed. Nothing here is a law.
    bool drawingZones = false;
    sim::ZoneKind brushKind = sim::ZoneKind::Farm;
    sim::ZoneMode brushMode = sim::ZoneMode::Preferred;
    std::int32_t brushRadius = 2;
    bool erasing = false;

    // Category priorities are the other half of the same loop: a weight on the
    // planner's score, not an order to anybody.
    bool showPriorities = false;

    // --- the two summaries -----------------------------------------------
    // Who does what, and what the settlement has. Neither is a control: they
    // answer the two questions a player asks constantly and could otherwise
    // only answer by clicking every person and every store in turn.
    bool showTrades = false;
    bool showStores = false;
    // A bit per work category, for the trades the player has opened.
    std::uint32_t expandedTrades = 0;
    // How far down the stores list the player has scrolled.
    int storeScroll = 0;
};

// Where the HUD put things this frame, so input can tell a click on a panel from
// a click on the world behind it.
struct HudLayout {
    float inspectorWidth = 0.0f;
    SDL_FRect priorities{0, 0, 0, 0};
    float priorityRowHeight = 14.0f;
    float priorityFirstRow = 0.0f;
    // The trades panel, and where its rows begin, so a click can open one.
    SDL_FRect trades{0, 0, 0, 0};
    float tradeRowHeight = 12.0f;
    float tradeFirstRow = 0.0f;
    SDL_FRect stores{0, 0, 0, 0};
};

// Did this click land on a priority control? Returns the work category and which
// way to nudge it.
bool hitPriorityControl(const HudState& hud, const HudLayout& layout, float x, float y,
                        int& outCategory, int& outDelta);

// Did this click land on a trade heading? Returns which work category, so the
// caller can open or close it.
bool hitTradeHeading(const sim::World& w, const HudState& hud, const HudLayout& layout, float x,
                     float y, int& outCategory);

HudLayout drawHud(SDL_Renderer* r, const sim::World& w, const HudState& hud, double simMsPerTick);

} // namespace ui
