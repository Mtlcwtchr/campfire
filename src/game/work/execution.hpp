#pragma once
// Advances whatever each pawn is currently doing by one tick: walking a path,
// putting work into a target, or delivering what it carries.

#include "game/simulation/world.hpp"

namespace sim {
namespace work {

void executeJobs(World& w);

// Exposed for the tests: move one pawn one tick along its path. Returns true when
// it has arrived at the end of the path.
bool advanceAlongPath(World& w, Person& p);

} // namespace work
} // namespace sim
