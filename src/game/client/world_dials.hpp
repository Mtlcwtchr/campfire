#pragma once
// The generator's dials, with names on them.
//
// One table, read by every screen that lets somebody turn them: the world-
// choosing screen before a game and the explorer's own menu. Each dial is a
// name, a way to read the value out of the parameters and a way to move it, so
// a screen knows nothing about what any of them mean and a new dial costs one
// line here rather than one line in each screen.
//
// No SDL and no drawing: this is what the dials *are*, not how they look.

#include <string>
#include <vector>

#include "game/generation/world_map_gen.hpp"

namespace client {

struct Dial {
    const char* label;
    const char* note;
    std::string (*show)(const generation::WorldMapParams&);
    void (*move)(generation::WorldMapParams&, int);
};

const std::vector<Dial>& dials();

} // namespace client
