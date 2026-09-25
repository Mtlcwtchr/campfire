#pragma once
// Sown fields (GDD 14: the grain chain). A tile inside a farm area can be broken,
// sown and reaped. Unlike a wild stand, a field is something the community made:
// it costs seed it could otherwise have eaten, and it pays back weeks later.

#include "game/simulation/world.hpp"

namespace sim {

// A ripe crop is not safe. It ages in the field and is lost if nobody reaps it,
// which is what stops a settlement leaving thirteen ripe fields standing.
inline constexpr std::int32_t kDaysRipeBeforeLost = 14;
inline const Fixed kOverripe = Fixed::ratio(2, 1);

// Ripens what is growing. Runs once a game day; nothing here changes per tick.
void tickFarming(World& w);

// Can this tile be worked as a field right now, and what is the next thing to do
// on it? Returns JobKind::None when the tile is not a field, is not ready, or the
// season is wrong.
JobKind nextFieldAction(const World& w, SettlementId s, TilePos tile, DefId& outCrop);

// How far a sower will carry seed to a furrow. A field sits outside the
// settlement core by design, so requiring the seed to be lying next to it had
// every sowing job do its work and then fail for want of grain.
inline constexpr std::int32_t kSeedCarryRadius = 45;

// What the settlement sows. The first crop in the ethnos list whose seed the
// community actually has, and whose season it is.
// What a tile will actually grow: what the river left there, plus what a channel
// beside it holds. Nothing else reads Tile::fertility directly.
Fixed effectiveFertility(const World& w, TilePos at);

DefId chooseCropToSow(const World& w, SettlementId s);

} // namespace sim
