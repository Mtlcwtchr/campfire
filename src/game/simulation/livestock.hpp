#pragma once
// The flock (GDD 7). Animals age, graze, give wool and milk, breed and die. They
// are bodies in the world: a shepherd walks to one to shear it, and an animal
// with no pasture under it loses condition whatever the herd size says.

#include "game/simulation/world.hpp"

namespace sim {

// Days between one wolf making a kill, how far it looks for one, and how close
// a dog or a shepherd has to be to make stock not worth the risk.
// One wolf takes something about once a month. At one in five it was two kills
// a day across a pack and the whole country was eaten out inside a year.
inline constexpr std::int32_t kWolfKillsOneDayIn = 30;
inline constexpr std::int32_t kWolfReach = 12;
inline constexpr std::int32_t kDogGuardReach = 6;
// How wide a sweep a shepherd with a dog brings in at once.
inline constexpr std::int32_t kDogDrivesWithin = 10;
// Days between one unwatched animal wandering off on its own.
inline constexpr std::int32_t kStrayOneDayIn = 6;
// The hour a shepherd starts bringing the flock in, and before which it is let
// out again. At dusk, not in the dark: offered after nightfall this work
// competed with sleep and lost every time.
inline constexpr std::int32_t kPenningHour = 17;

void tickLivestock(World& w);

// What this animal is ready to give, or JobKind::None.
JobKind nextAnimalAction(const World& w, const Animal& a);

} // namespace sim
