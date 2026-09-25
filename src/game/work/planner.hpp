#pragma once
// Autonomous work selection (GDD 2.1, 2.3, 6).
//
// The player never hands a job to a pawn. Each idle pawn looks at what the
// community currently lacks, at what it is physically able to do right now, and
// picks the best feasible thing. If its preferred work is impossible it picks
// another useful job rather than miming an impossible one.

#include <array>
#include <vector>

#include "game/simulation/world.hpp"

namespace sim {
namespace work {

// What the settlement currently wants, in units, per item. Demand is seeded from
// direct needs (food to eat, materials the standing blueprints still lack, tools
// the workers do not have) and then propagated backwards along known recipes, so
// "we need bread" turns into "we need grain" turns into "we need to reap" without
// anyone authoring a production plan.
struct Demand {
    // Two quantities per item, because "how badly do we want one more" and "how
    // many more do we want at all" are different questions. Value alone made the
    // community reap seven thousand ears of einkorn it had no way to thresh;
    // quantity alone would not tell a hauler that grain beats branches.
    std::vector<Fixed> item;                                       // value of one more unit
    std::vector<Fixed> wanted;                                     // units still worth acquiring
    std::array<Fixed, static_cast<std::size_t>(ToolClass::Count)> tool{};
    // Days of food actually in store, at two units of satiety a head a day.
    // A settlement is usually short of its seasonal target and perfectly fed at
    // the same time, so this - not the target gap - is what tells the community
    // it is in trouble.
    // Days of food a person could sit down to now. Kept apart from the stock in
    // the granary, which is days of food only after somebody threshes, grinds
    // and bakes it (D81).
    Fixed foodDays = core::kZero;
    Fixed larderDays = core::kZero;

    Fixed forItem(DefId id) const { return id.valid() && id.value < item.size() ? item[id.value] : core::kZero; }
};

Demand computeDemand(const World& w, SettlementId s);

// Fills in the job of every pawn that has none.
void assignJobs(World& w);

// In unrestricted mode the community decides for itself what to build and where,
// inside the areas the player allows (GDD 8). This places at most a few
// blueprints at a time so projects actually finish instead of all starting.
void planSettlementProjects(World& w);

// Called when a building is finished: the plans that were waiting behind it get
// their attempts forgiven.
void notePlanSucceeded(World& w, SettlementId settlement);

// How much land a scout takes in when it stops and looks.
inline constexpr std::int32_t kScoutRevealRadius = 6;

// How many garments this person would like on right now. One always; a second
// once the cold starts to bite.
std::int32_t desiredGarments(const World& w, const Person& p);

// The completed housing assigned to this pawn's household by deterministic
// capacity/comfort ordering. A house is the unit; there are no bed entities.
BuildingId homeForPerson(const World& w, const Person& p);

// How much of a sleeping place somebody takes up, in quarters: a child a half,
// an elder three quarters, an adult the whole of one. A house of four beds holds
// two parents and four children.
inline constexpr std::int32_t kBedQuarters = 4;
std::int32_t bedQuartersFor(const Person& p);

// Exposed for tests and for the UI's "why is this pawn idle" panel.
bool hasToolEquipped(const World& w, const Person& p, ToolClass cls);
bool knowsMethod(const World& w, const Person& p, DefId knowledgeDef);
// A level demanded before the thing may be attempted at all. Nothing ordinary
// demands one; see content::SkillRequirement.
bool skilledEnough(const Person& p, const content::SkillRequirement& required);
Fixed workRate(const World& w, const Person& p, WorkCategory category, Fixed toolEfficiency);

} // namespace work
} // namespace sim
