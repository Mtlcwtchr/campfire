#pragma once
// Bodily states. GDD 7: these are separate axes, not one wellbeing bar, and an
// ignored one produces consequences rather than a mood penalty.

#include "game/simulation/world.hpp"

namespace sim {

struct NeedsSnapshot {
    Fixed satiety;
    Fixed hydration;
    Fixed rest;
    Fixed health;
    bool asleep;
    Person::Ailment ailment;
    Fixed ailmentSeverity;
};

NeedsSnapshot needsSnapshot(const World& w, const Person& person);
bool canWork(const World& w, const Person& person);

void tickNeeds(World& w);

// Diet quality from the trailing window of food groups eaten (GDD 7). 1.0 means
// a varied diet; a pawn living on one product sits near the floor.
Fixed dietVariety(const Person& p);

// How much of a person's work capacity survives their current bodily state.
// Hunger, thirst, exhaustion, injury and cold all bite here rather than each
// system inventing its own penalty.
Fixed workCapacity(const World& w, const Person& p);

// Same for movement.
Fixed moveCapacity(const World& w, const Person& p);

// Feeds one meal from a batch into a person and records the food groups.
std::int32_t mealPortions(const World& w, const Person& p, ItemStackId stack);
void eatFrom(World& w, Person& p, ItemStackId stack);

} // namespace sim
