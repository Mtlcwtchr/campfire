#pragma once

#include <string>

namespace client {

// Which sides of a wall tile carry more wall. Cardinals only: two walls meeting
// corner to corner are two runs, not one.
constexpr int kWallNorth = 1;
constexpr int kWallEast = 2;
constexpr int kWallSouth = 4;
constexpr int kWallWest = 8;

// A wall tile is drawn for the wall beside it. The sheet gives a segment drawn
// across the view and a segment drawn receding, which between them cover the
// four sides a square grid has; it also gives a bend, a T-junction and a
// crossing, but those were drawn for a hex grid (D44) and each covers two or
// three tiles of wall, so squeezed into one tile they come out thinner than the
// run they join and a corner reads as a notch in the wall. A turn therefore
// keeps the run going instead: the receding card, with the straight from the
// next tile butting into its side.
//
// Returns the suffix on the building's sprite name; empty is the straight card,
// which a wall tile on its own takes as well - the sheet's end cap is drawn two
// tiles long and cut down to one it kept a piece of its neighbour.
inline std::string wallVariant(int cardinals) {
    const bool alongY = (cardinals & (kWallNorth | kWallSouth)) != 0;
    // A run north to south, and every turn out of one, takes the receding card;
    // everything else - a run east to west, a lone tile - takes the straight.
    return alongY ? "_side" : "";
}

} // namespace client
