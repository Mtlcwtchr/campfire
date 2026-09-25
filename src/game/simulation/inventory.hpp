#pragma once
// Queries and mutations over physical goods. Everything here works in terms of
// batches at places - there is no abstract settlement stockpile to add to.

#include <vector>

#include "game/simulation/world.hpp"

namespace sim {

// ECS-facing inventory accessors. The legacy Person fields are kept as a
// compatibility mirror while the remaining job code is migrated.
ItemStackId carriedStack(const World& w, PersonId person);
ItemStackId equippedToolStack(const World& w, PersonId person);
void setCarriedStack(World& w, PersonId person, ItemStackId stack);
void setEquippedToolStack(World& w, PersonId person, ItemStackId stack);

const ecs::ItemStackState* ecsStackState(const World& w, ItemStackId stack);
bool stackAlive(const World& w, ItemStackId stack);
std::int32_t stackCount(const World& w, ItemStackId stack);
DefId stackDefinition(const World& w, ItemStackId stack);
StackWhere stackWhere(const World& w, ItemStackId stack);
BuildingId stackBuilding(const World& w, ItemStackId stack);
TilePos stackTile(const World& w, ItemStackId stack);
Fixed stackFreshness(const World& w, ItemStackId stack);
std::int32_t stackDurability(const World& w, ItemStackId stack);
bool isAvailable(const World& w, ItemStackId stack);
void setStackLocation(World& w, ItemStackId stack, StackWhere where, PersonId holder,
                      BuildingId building, TilePos tile);
void setStackCount(World& w, ItemStackId stack, std::int32_t count);

// Is this batch sitting somewhere a hauler could pick it up from?
bool isLoose(const ItemStack& s);
// Available to the settlement's economy: on the ground or in a store, not in
// somebody's hands and not equipped.
bool isAvailable(const ItemStack& s);

std::int32_t countAvailable(const World& w, DefId item);
std::int32_t countAvailable(const World& w, ItemCategory category);

// Nearest available batch of a specific item, skipping anything another pawn has
// already claimed. Distance is Chebyshev on the grid: a cheap pre-filter, since
// the real cost is only known after pathfinding.
ItemStackId findNearestAvailable(const World& w, TilePos from, DefId item, PersonId forWho);

// Nearest batch a pawn would actually be willing to eat right now: edible without
// cooking, not spoiled, and not unsafe raw unless the pawn is starving.
ItemStackId findNearestEdible(const World& w, const Person& p, BuildingId preferredBuilding = {});

// What felling this node yields, if felling it is a thing anybody would do. A
// fruit tree names it separately from its picking; a timber tree's only harvest
// IS the felling, and is recognised by being woodcutting that consumes the tree.
const content::HarvestSpec& fellSpecOf(const content::ContentDb& db,
                                       const content::ResourceNodeDef& def);
// Whether the node is one that gets picked rather than felled.
bool isGathered(const content::ResourceNodeDef& def);
// One pass of the search above. `spendSeed` false skips items the settlement has
// set aside to sow; the caller runs it again with true only if nothing else is
// edible anywhere.
ItemStackId findEdibleStack(const World& w, const Person& p, BuildingId preferredBuilding,
                            bool starving, bool preferHome, bool spendSeed);

// Nearest garment nobody is wearing, warmest first at equal distance.
ItemStackId findNearestWearable(const World& w, const Person& p);

// Nearest usable tool of a class, preferring higher efficiency at equal distance.
ItemStackId findNearestTool(const World& w, TilePos from, ToolClass cls, PersonId forWho);

// A completed storage building with a free slot that accepts this item.
BuildingId findStorageFor(const World& w, DefId item, TilePos near);
// A completed building of any of these defs that nobody is working at.
BuildingId findFreeWorkplace(const World& w, const std::vector<DefId>& buildingDefs, TilePos near,
                             PersonId forWho);
// Nearest completed building that provides an open fire (cooking, boiling).
BuildingId findFireSource(const World& w, TilePos near);
// Nearest natural water source tile a pawn can drink from or draw water at.
bool findWaterTile(const World& w, TilePos from, TilePos& out);

std::int32_t stacksInBuilding(const World& w, BuildingId b);

// Move `count` units out of a batch into a new batch at a new place. Returns the
// new batch, or an invalid id if the source could not supply that many.
ItemStackId splitStack(World& w, ItemStackId from, std::int32_t count);
// Merge a batch into a compatible one at the same place, if there is one.
void tryMergeAtDestination(World& w, ItemStackId id);

void consume(World& w, ItemStackId id, std::int32_t count);

// Takes `count` units of an item out of whatever is available within `radius`
// of a tile. Returns false and consumes nothing when there is not enough.
bool consumeFromStores(World& w, DefId item, std::int32_t count, TilePos near, std::int32_t radius);

// Spend one unit of tool durability; destroys the tool when it runs out.
void wearTool(World& w, ItemStackId tool, std::int32_t amount);

// Perishables lose freshness once per game day; a storage building slows it.
void tickSpoilage(World& w);

} // namespace sim
