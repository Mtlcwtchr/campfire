#pragma once
// Demography: ageing, life stages, pairing, conception, birth and natural death,
// plus the autonomous choice of a main profession and the fixing of a personal
// method into a settlement tradition (GDD 6).

#include "game/simulation/world.hpp"

namespace sim {

void tickPopulation(World& w);

// Builds the starting community of GDD 6: 8-12 people in several existing
// families with adults, children and elders, carrying no items and no stores.
class WorldBuilder {
public:
    static void createStartingCommunity(World& w, SettlementId s, DefId ethnos, std::int32_t size);
};

// A pawn re-picks its main specialisation from accumulated skill, traits and what
// the community currently needs. Never assigned by the player (GDD 6).
// Grown children who are not the heir of their house set up houses of their own,
// in the same trade. One per review.
void foundNewHouseholds(World& w);

// Returns whether the person actually changed trade.
bool reconsiderProfession(World& w, Person& p);

// The labour a settlement's life actually costs, in work units a day. These are
// the numbers the planner reasons with (D49); the abstract day (D103) needs the
// same ones, because a community nobody is watching has to cost what it would
// have cost if somebody were.
inline constexpr std::int32_t kWorkPerAdultDay = 300;
inline constexpr std::int32_t kFieldWorkPerHeadDay = 40;
inline constexpr std::int32_t kMealWorkPerHeadDay = 53;
// What one mouth needs eaten in a day, in units of satiety.
inline constexpr std::int32_t kSatietyPerMouthDay = 2;

// The ledger the community reads when somebody wonders whether to change trade:
// the work each trade wants doing in a day, and the hands already on it. Exposed
// because it explains the shape of a settlement better than any other number,
// and because a headless run has to be able to print it.
Fixed tradeNeed(const World& w, const Settlement& st, content::WorkCategory c);
Fixed tradeHands(const World& w, const Settlement& st, content::WorkCategory c);

} // namespace sim
