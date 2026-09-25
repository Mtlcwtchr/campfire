#pragma once
// Areas (GDD 9). Two kinds of area exist and they are not the same thing:
//
//   - what the community lays out for itself, because in unrestricted mode the
//     settlers choose where they live, fell, sow and graze (GDD 8);
//   - what the player draws, which is technical direction to the planner: not a
//     law, cannot be broken, needs no institution.
//
// Both are the same Zone; only `playerDrawn` differs, and the community never
// touches an area the player drew.

#include <vector>

#include "game/simulation/world.hpp"

namespace sim {

// False means the planner must not even offer this work here.
bool workAllowedAt(const World& w, SettlementId s, WorkCategory c, TilePos at);
bool workAllowedAt(const World& w, SettlementId s, WorkCategory c, WorldPos at);

// Multiplier applied to a candidate job's score from overlapping zones.
// Overlapping rules resolve by strictness: a forbid anywhere wins, otherwise the
// strongest positive weight applies (GDD 8, "the main open fork" - this is the
// provisional resolution recorded in DECISIONS.md D7).
Fixed zoneWeightAt(const World& w, SettlementId s, WorkCategory c, TilePos at);
Fixed zoneWeightAt(const World& w, SettlementId s, WorkCategory c, WorldPos at);

// Is the tile inside any of this settlement's areas at all? Used to keep
// autonomous building inside the settled ground.
bool insideSettlementZone(const World& w, SettlementId s, TilePos at);
bool insideSettlementZone(const World& w, SettlementId s, WorldPos at);

// Whether a batch lying at `at` counts as being to hand for work done at `site`.
// Inside its own settlement a community has its stores to hand: the walking was
// already paid for by the haulers who carried them in. Out in the field only
// what is close by counts.
//
// This is the rule that decides whether a village can use its own granary. With
// a flat radius of ten tiles it could not: a bakery on one side of the
// settlement and a granary on the other are fourteen apart, so a community
// starved to the last person with twelve hundred units of flour in store and
// "no materials for any job" as the reason its people stood idle.
bool materialsInReach(const World& w, SettlementId s, TilePos site, TilePos at);
inline constexpr std::int32_t kMaterialReach = 10;
bool insideZoneOfKind(const World& w, SettlementId s, ZoneKind kind, TilePos at);
bool insideZoneOfKind(const World& w, SettlementId s, ZoneKind kind, WorldPos at);
// Same question with no settlement filter, for code that only has a tile.
bool insideAnyZoneOfKind(const World& w, ZoneKind kind, TilePos at);
bool insideAnyZoneOfKind(const World& w, ZoneKind kind, WorldPos at);

// The nearest zone of a kind, or nullptr. Zones are few, so this is a plain scan.
const Zone* nearestZoneOfKind(const World& w, SettlementId s, ZoneKind kind, TilePos near);
const Zone* nearestZoneOfKind(const World& w, SettlementId s, ZoneKind kind, WorldPos near);
const Zone* nearestStorageZone(const World& w, SettlementId s, TilePos near);
const Zone* nearestStorageZone(const World& w, SettlementId s, WorldPos near);

// A tile inside a storage area (or a storage building) that still has room for one
// more batch. Storage areas are ground stockpiles: the community can pile goods up
// before it can build anything (GDD 7, zero production chain).
bool findStorageSpot(const World& w, DefId item, TilePos near, TilePos& outTile, BuildingId& outBuilding);

// The community reads the ground around its hearth and decides where it lives,
// where it fells timber and cuts stone, where it hunts, where it will sow, where
// the flock grazes, and where a wall would go. Areas the player drew are left
// alone. Called once when the settlement is founded and occasionally afterwards,
// because woods run out and fields wear.
void layOutSettlementZones(World& w, SettlementId s);

// Lays the areas out again when the community has grown enough to need more
// ground. Player-drawn areas are never touched (GDD 9).
void reconsiderZones(World& w);

// The area a trade is worked in, so a family that lives by it can live near it.
ZoneKind zoneOfTrade(content::WorkCategory trade);

// Gives every family a place of its own near the ground it works, so that the
// settlement grows as quarters rather than as a heap of huts round the fire.
void siteHouseholds(World& w);

// How much further from its own work a family's place may sit than the fire
// itself does. Not zero: the place cannot stand on the field or the stockpile,
// so it has to step off them.
inline constexpr std::int32_t kSeatSlack = 6;

// The furthest the community works from its fire.
inline constexpr std::int32_t kMaxWorkDistance = 26;

std::string_view zoneKindName(ZoneKind k);

} // namespace sim
