#pragma once
// The head-up display: what the player reads and touches while the game runs.
//
// Laid out after the reference art rather than after what is easy to draw: a bar
// along the top carrying the state of the community and the clock, a bar along
// the bottom carrying what can be built and done, and a panel on the right
// carrying whatever is selected. Everything in it is a widget from ui.hpp, so
// the look lives in one theme and the editor can change it while the game runs.

#include <string>
#include <vector>

#include "game/client/renderer.hpp"
#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"
#include "engine/ui/ui.hpp"

namespace ui {

// The speeds the game runs at. One table, read by the key that selects a speed
// and by the button that shows it, so the two cannot drift apart.
inline constexpr int kSpeedLadder[] = {0, 1, 2, 5, 10, 100};
inline constexpr int kSpeedCount = static_cast<int>(sizeof(kSpeedLadder) / sizeof(kSpeedLadder[0]));

// The tabs along the bottom. What each one offers is decided from content, so a
// new building appears in the list without anybody editing this file.
//
// Note what is not here: a way to place a building. The player directs by
// drawing areas; where a granary goes, and whether one is worth raising at all,
// is the community's own decision (GDD 9, D110). The Buildings deck says what
// they know how to raise and what stands - it is a reading, not a control.
enum class Deck : std::uint8_t { Buildings, Production, People, Zones, Logistics, Ritual, Count };
const char* deckName(Deck d);

// What the HUD is showing and what the player has picked up. This is interface
// state - never simulation state - and the client owns it.
struct Hud {
    Deck deck = Deck::Buildings;
    client::Selection selected;
    // What is drawn over the ground: areas, job lines, or nothing.
    client::Overlay overlay = client::Overlay::None;
    // Which community the player is looking at, for the world view's sake.
    bool worldView = false;

    int speed = 1;
    int speedBeforePause = 1;
    bool speedLimited = false;

    // Drawing areas is the other half of GDD 9's technical direction; the Zones
    // deck turns it on and picks the brush.
    bool drawingZones = false;
    sim::ZoneKind brushKind = sim::ZoneKind::Farm;
    sim::ZoneMode brushMode = sim::ZoneMode::Preferred;
    std::int32_t brushRadius = 2;
    bool erasing = false;

    // Set by the HUD every frame: whether the pointer is over interface rather
    // than over the world.
    bool overInterface = false;
    // Filled by the HUD when the player asks for something the client must do.
    bool wantsSave = false;
    bool wantsLoad = false;
    bool wantsEditor = false;
};

// Draws the whole interface for this frame and answers whatever the player did.
// The renderer is handed in for the pictures: a building's card shows the
// building's own sprite.
void drawHud(Ui& ui, Hud& hud, const sim::World& w, const client::Renderer& art,
             double simMsPerTick);

} // namespace ui
