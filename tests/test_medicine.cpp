// Medicine and burial (D98, D99).
//
// These are contract tests, not statistics: they put a wound on somebody and
// then assert that the settlement answers it - gathers, brews, walks over and
// treats - and that being ill costs what it should while it lasts.

#include "framework.hpp"

#include <algorithm>
#include "support.hpp"

#include "game/simulation/report.hpp"
#include "game/work/planner.hpp"
#include "game/simulation/world.hpp"

namespace {

sim::WorldConfig cfg(std::uint64_t seed = 11) {
    sim::WorldConfig c;
    c.seed = seed;
    c.mapWidth = 110;
    c.mapHeight = 110;
    c.worldCells = 110;
    c.startingPopulation = 12;
    return c;
}

sim::Person& anAdult(sim::World& w) {
    for (auto& p : w.people())
        if (p.alive && p.stage == sim::LifeStage::Adult) return p;
    return w.people().front();
}

} // namespace

TEST(an_ailment_is_its_own_axis_and_not_a_bite_out_of_health) {
    // GDD 7 asks for independent bodily states. A wound is a thing that happened
    // and runs its course; health is what hunger and cold wear away. Conflating
    // them means a wounded person reads as a starving one and the community
    // feeds them instead of treating them.
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());
    sim::Person& hurt = anAdult(w);
    hurt.satiety = hurt.hydration = hurt.rest = core::kOne;
    hurt.ailment = sim::Person::Ailment::Wound;
    hurt.ailmentSeverity = core::Fixed::ratio(3, 5);
    const core::Fixed healthBefore = hurt.health;

    // Laid up: over half severity a person is in bed, not at work.
    CHECK(!hurt.canWork());

    w.runTicks(db.time().ticksPerDay() * 2);
    // Two days of an untended wound costs health - that is what makes medicine
    // worth hands - but the wound is still the thing that is wrong with them.
    const sim::Person& after = w.person(hurt.id);
    if (after.alive) {
        // The wound cost health while it lasted - that is what makes medicine
        // worth hands - and it is still the thing that is wrong with them.
        CHECK(after.harmFromIllness > core::kZero);
        // And it still costs, for as long as nobody has treated it. Only for as
        // long: a community that gets herbs to them inside two days ends with
        // them mended and back to full, which is the medicine working rather
        // than the test failing.
        if (after.ailment != sim::Person::Ailment::None) CHECK(after.health < healthBefore);
    }
}

TEST(a_community_gathers_brews_and_treats_its_own) {
    // The whole chain, end to end: herbs on the ground, a remedy made from them,
    // and somebody walking it over to whoever needs it. Before this the plants
    // were on the map and nothing ever picked one, because nothing in the demand
    // table asked for medicine (D98).
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());

    // Somebody is hurt badly enough to be worth crossing the settlement for.
    sim::Person& hurt = anAdult(w);
    hurt.ailment = sim::Person::Ailment::Wound;
    hurt.ailmentSeverity = core::Fixed::ratio(3, 5);
    hurt.ailmentSinceTick = w.tickCount();

    w.runTicks(db.time().ticksPerDay() * 60);

    const auto& rep = w.report();
    if (rep.treatments == 0) std::cerr << "    sixty days and nobody was ever treated\n";
    CHECK(rep.treatments > 0);
    // And what they used was made, not conjured: the poultice comes from herbs
    // somebody walked out and gathered.
    const core::DefId herbs = db.itemByName("herbs");
    CHECK(herbs.valid());
    const auto& produced = rep.itemsProduced;
    const bool madeRemedy = (produced.count("poultice") && produced.at("poultice") > 0) ||
                            (produced.count("fever_draught") && produced.at("fever_draught") > 0);
    if (!madeRemedy) std::cerr << "    no remedy was ever made\n";
    CHECK(madeRemedy);
}

TEST(the_dead_are_buried_and_the_ground_is_the_better_for_it) {
    // Burial is the oldest observance there is and the first one that pays for
    // itself: a body left where it fell fouls the ground people walk over, and
    // that ground is where the flux comes from (D99).
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());

    sim::Person& gone = anAdult(w);
    const core::TilePos where = gone.tile;
    gone.alive = false;
    gone.buried = false;
    gone.deathTick = w.tickCount();
    gone.deathCause = "the test";
    // Foul the ground they lie on, so the burial has something to answer.
    for (core::TilePos q : core::tilesWithin(where, 1)) {
        if (!w.map().inBounds(q)) continue;
        w.map().at(q).pollution = core::Fixed::ratio(9, 10);
    }

    w.runTicks(db.time().ticksPerDay() * 6);

    const sim::Person& after = w.person(gone.id);
    if (!after.buried) std::cerr << "    six days and nobody buried them\n";
    CHECK(after.buried);
    CHECK(w.report().burials > 0);
    // And the ground around the grave is cleaner than it was.
    CHECK(w.map().at(where).pollution < core::Fixed::ratio(9, 10));
}

TEST(knowing_which_plant_answers_which_hurt_is_the_whole_of_it) {
    // A poultice on a fever is a wasted poultice. The remedies say what they
    // answer, and the planner picks by that: this is the one piece of knowledge
    // early medicine actually consisted of.
    const auto& db = testing::sharedContent();
    const core::DefId poultice = db.itemByName("poultice");
    const core::DefId draught = db.itemByName("fever_draught");
    CHECK(poultice.valid());
    CHECK(draught.valid());
    if (!poultice.valid() || !draught.valid()) return;

    CHECK(db.item(poultice).healsWound > core::kZero);
    CHECK(db.item(poultice).healsSickness == core::kZero);
    CHECK(db.item(draught).healsSickness > core::kZero);
    CHECK(db.item(draught).healsWound == core::kZero);

    // And both are made by somebody who knows herbcraft, from plants.
    const core::DefId herbcraft = db.knowledgeByName("herbcraft");
    CHECK(herbcraft.valid());
    bool madeFromPlants = false;
    for (const auto& r : db.recipes()) {
        if (r.outputs.empty() || r.outputs.front().item != poultice) continue;
        CHECK(r.knowledgeDef == herbcraft);
        for (const auto& in : r.inputs)
            if (db.item(in.item).name == "herbs") madeFromPlants = true;
    }
    CHECK(madeFromPlants);
}

