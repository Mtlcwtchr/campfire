#include "framework.hpp"
#include "support.hpp"

#include "game/simulation/report.hpp"
#include "game/simulation/world.hpp"

namespace {

sim::WorldConfig testConfig(std::uint64_t seed) {
    sim::WorldConfig cfg;
    cfg.seed = seed;
    cfg.mapWidth = 96;
    cfg.mapHeight = 96;
    // Test scale. The played world is a thousand cells a side; a suite that
    // builds one per fixture pays a quarter of a second each for country nobody
    // in the test ever looks at.
    cfg.worldCells = 96;
    cfg.startingPopulation = 10;
    return cfg;
}

} // namespace

TEST(two_worlds_from_one_seed_stay_identical) {
    // This is the property the whole lockstep design rests on (GDD 11). It is
    // checked every tick, not only at the end, so a divergence names its tick.
    const auto& db = testing::sharedContent();
    sim::World a(db, testConfig(1234));
    sim::World b(db, testConfig(1234));

    CHECK_EQ(a.checksum(), b.checksum());
    CHECK_EQ(a.people().size(), b.people().size());

    for (int i = 0; i < 3000; ++i) {
        a.tick();
        b.tick();
        if (a.checksum() != b.checksum()) {
            std::cerr << "    diverged at tick " << a.tickCount() << "\n";
            CHECK(false);
            return;
        }
    }
}

TEST(different_seeds_produce_different_worlds) {
    const auto& db = testing::sharedContent();
    sim::World a(db, testConfig(1));
    sim::World b(db, testConfig(2));
    a.runTicks(600);
    b.runTicks(600);
    CHECK(a.checksum() != b.checksum());
}

TEST(invariants_hold_over_a_long_run) {
    const auto& db = testing::sharedContent();
    sim::World w(db, testConfig(99));
    for (int i = 0; i < 5000; ++i) {
        w.tick();
        const auto problems = sim::checkInvariants(w);
        if (!problems.empty()) {
            std::cerr << "    tick " << w.tickCount() << ": " << problems.front() << "\n";
            CHECK(false);
            return;
        }
    }
}
