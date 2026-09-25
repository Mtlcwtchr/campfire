#pragma once
// The inspector. Everything the player clicks gets a panel explaining what it is,
// what state it is in, and - where the simulation knows - why.
//
// The text is produced separately from the drawing so the same description can be
// dumped to a log or a test without a window.

#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"

namespace ui {

struct InfoText {
    std::string title;
    std::string subtitle;
    // An empty line is a blank separator; a line starting with '#' is a heading.
    std::vector<std::string> lines;
    // The sprite that shows what this is, by the name its content definition
    // carries ("buildings/granary"). Empty when there is no picture of it.
    std::string picture;
};

InfoText describe(const sim::World& w, const client::Selection& sel);

// Who does what: a line per trade with how many live by it, and under a trade
// the player has opened, a line per person saying what they are doing now.
// `expanded` is a bit per work category.
InfoText describeTrades(const sim::World& w, sim::SettlementId settlement, std::uint32_t expanded);

// What the settlement has: every item it holds, and where - the stores, the
// workshops, the houses. Sorted heaviest first, because that is the question
// being asked.
InfoText describeStores(const sim::World& w, sim::SettlementId settlement);

// Draws the inspector down the right-hand side. Returns the width it used, so
// the rest of the HUD can keep out of the way.
float drawInfoPanel(SDL_Renderer* r, const sim::World& w, const client::Selection& sel);

} // namespace ui
