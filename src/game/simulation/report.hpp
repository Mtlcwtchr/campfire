#pragma once
// Turns a finished headless run into the plain-text report GDD 14 asks for: why
// people went hungry, why they stood idle, and what tools were missing.

#include <string>

#include "game/simulation/world.hpp"

namespace sim {

std::string formatReport(const World& w);

// A short one-line status, printed periodically during a run.
std::string formatStatusLine(const World& w);

// Why the community is doing what it is doing right now: the demand table its
// planner is reading, and what every person is currently on. This is the view an
// agent needs to tell "they cannot" from "they will not".
std::string formatDecisionDump(const World& w);

// Invariants that must hold at every tick. A violated one is a bug in the
// simulation, not a bad outcome for the community.
std::vector<std::string> checkInvariants(const World& w);

} // namespace sim
