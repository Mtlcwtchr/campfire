#include "framework.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <fstream>

#include "game/simulation/savegame.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "support.hpp"

namespace {

sim::WorldConfig cfg(std::uint64_t seed = 11) {
    sim::WorldConfig c;
    c.seed = seed;
    c.mapWidth = 96;
    c.mapHeight = 96;
    c.worldCells = 96;          // a small country: the test builds several
    c.startingPopulation = 10;
    return c;
}

std::filesystem::path scratchFile(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

} // namespace

TEST(a_saved_world_comes_back_exactly_as_it_was) {
    // The point of a save is that the game goes on from where it stopped -
    // not somewhere close to it. Two worlds are the same world only if their
    // checksums agree, and they only keep agreeing if the random streams came
    // back too.
    const auto& db = testing::sharedContent();
    sim::World original(db, cfg());
    original.runTicks(db.time().ticksPerDay() * 12);

    sim::Game game;
    game.base = cfg();
    game.worldParams.seed = cfg().seed;
    game.worldParams.width = 96;
    game.worldParams.height = 96;

    core::BinaryWriter out;
    original.write(out);
    core::BinaryReader in(out.data());

    sim::WorldConfig blank = cfg();
    sim::World restored(db, blank, sim::World::FromSave{});
    CHECK(restored.read(in));
    CHECK(in.ok());

    CHECK_EQ(restored.tickCount(), original.tickCount());
    CHECK_EQ(static_cast<long long>(restored.checksum()),
             static_cast<long long>(original.checksum()));

    // And it goes on being the same world: a save that only matches at the
    // moment it is loaded is a save that has lost the dice.
    original.runTicks(db.time().ticksPerDay() * 3);
    restored.runTicks(db.time().ticksPerDay() * 3);
    CHECK_EQ(static_cast<long long>(restored.checksum()),
             static_cast<long long>(original.checksum()));
}

TEST(a_save_file_says_what_it_is_and_reads_back) {
    // The file carries its own name and version, so a build that cannot read it
    // can say so instead of crashing on somebody else's bytes.
    const auto& db = testing::sharedContent();
    generation::WorldMapParams params;
    params.seed = 7;
    params.width = 96;
    params.height = 96;
    params.sites = 3;
    params.siteSpacing = 12;

    sim::Game game = sim::newGame(db, cfg(7), params);
    CHECK(!game.communities.empty());
    game.communities.front()->runTicks(db.time().ticksPerDay() * 4);

    const auto file = scratchFile("asr_roundtrip.save");
    std::string error;
    CHECK(sim::saveGame(game, file, &error));

    const sim::SaveSummary summary = sim::inspectSave(file);
    CHECK(summary.readable);
    CHECK_EQ(static_cast<long long>(summary.formatVersion),
             static_cast<long long>(sim::kSaveFormatVersion));
    CHECK_EQ(static_cast<long long>(summary.seed), 7LL);
    CHECK_EQ(summary.communities, static_cast<std::int32_t>(game.communities.size()));

    sim::Game loaded;
    CHECK(sim::loadGame(db, file, loaded, &error));
    CHECK_EQ(loaded.communities.size(), game.communities.size());
    CHECK_EQ(static_cast<long long>(loaded.active().checksum()),
             static_cast<long long>(game.active().checksum()));
    // The country came back too - regenerated from the parameters the save
    // carries rather than stored cell by cell.
    CHECK_EQ(loaded.world().width, game.world().width);
    CHECK_EQ(loaded.world().sites.size(), game.world().sites.size());

    std::filesystem::remove(file);
}

TEST(hybrid_game_save_reuses_forms_without_the_source_library) {
    const auto& db = testing::sharedContent();
    generation::WorldMapParams params;
    params.seed = 42;
    params.width = params.height = 64;
    params.sites = 1;
    params.plates = 9;
    params.erosionPasses = 5;
    params.rainfallPercent = 85;
    // The loaded game's default DEM library may exist, but it must not replace
    // the analytic forms this game was originally generated with.
    params.terrainReferenceRoot = scratchFile("asr-deliberately-absent-references");
    auto config = cfg(42);
    config.mapWidth = config.mapHeight = 24;
    auto game = sim::newGame(db, config, params);
    CHECK(game.country->hybridTerrain != nullptr);
    if (!game.country->hybridTerrain) return;
    CHECK(!game.country->hybridTerrain->patches.empty());
    const auto file = scratchFile("asr_hybrid_snapshot.save");
    std::string error;
    CHECK(sim::saveGame(game, file, &error));
    sim::Game loaded;
    const bool success = sim::loadGame(db, file, loaded, &error);
    std::filesystem::remove(file);
    CHECK(success);
    if (!success || !loaded.country->hybridTerrain) { CHECK(false); return; }
    CHECK_EQ(loaded.country->hybridTerrain->fingerprint(), game.country->hybridTerrain->fingerprint());
    CHECK_EQ(loaded.worldParams.plates, params.plates);
    CHECK_EQ(loaded.worldParams.erosionPasses, params.erosionPasses);
    CHECK_EQ(loaded.worldParams.rainfallPercent, params.rainfallPercent);
    CHECK_EQ(loaded.country->flowDirectionField, game.country->flowDirectionField);
    CHECK_EQ(loaded.country->temperatureField, game.country->temperatureField);
    CHECK_EQ(loaded.country->soilFertilityField, game.country->soilFertilityField);
    CHECK_EQ(loaded.active().checksum(), game.active().checksum());
}

TEST(a_save_from_another_version_is_refused_politely) {
    // Not a compatibility layer - there is deliberately none yet - but the file
    // has to be able to say "I am not from this build" rather than be read as
    // though it were.
    const auto file = scratchFile("asr_not_a_save.save");
    {
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        const char rubbish[] = "this is not a save file at all";
        stream.write(rubbish, sizeof rubbish);
    }
    const sim::SaveSummary summary = sim::inspectSave(file);
    CHECK(!summary.readable);
    CHECK(!summary.note.empty());

    const auto& db = testing::sharedContent();
    sim::Game loaded;
    std::string error;
    CHECK(!sim::loadGame(db, file, loaded, &error));
    CHECK(!error.empty());
    std::filesystem::remove(file);
}

TEST(a_community_nobody_watches_still_lives) {
    // The map holds a dozen communities and one of them is on screen. The rest
    // go on by the day's accounting: the same people, the same stores, the same
    // buildings, and nobody deciding anything. What has to hold is that they
    // stay recognisable communities - fed, growing their stores, finishing what
    // they started - and that switching the detail back on hands the planner a
    // world it understands (D103).
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());
    w.runTicks(db.time().ticksPerDay() * 30);

    std::int32_t before = 0;
    for (const auto& p : w.people()) if (p.alive) ++before;
    CHECK(before > 0);

    w.setDetail(sim::World::Detail::Abstract);
    const auto started = std::chrono::steady_clock::now();
    w.runTicks(db.time().ticksPerDay() * 120);
    const double abstractSeconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    std::int32_t after = 0;
    for (const auto& p : w.people()) if (p.alive) ++after;
    std::cerr << "    " << before << " people before, " << after
              << " after four abstract months (" << abstractSeconds << " s)\n";
    // Nobody vanished and the place did not empty out.
    CHECK(after > 0);
    CHECK(after >= before / 2);

    // And the detailed loop takes it back without complaint: a hundred days of
    // ordinary simulation on top of an abstract season.
    w.setDetail(sim::World::Detail::Detailed);
    w.runTicks(db.time().ticksPerDay() * 20);
    std::int32_t living = 0;
    for (const auto& p : w.people()) if (p.alive) ++living;
    CHECK(living > 0);
    CHECK(w.checksum() != 0);
}

TEST(a_community_left_alone_for_ten_years_is_still_there) {
    // The one that matters: every neighbour on the map is unwatched almost all
    // the time, so if the abstract day is even slightly short of feeding them,
    // the whole world dies quietly while the player looks at their own valley.
    // It did: the first version had a pair of hands feeding two mouths against
    // the two a mouth eats, and since children count two fifths of a hand,
    // production was below consumption for every community always (D109).
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg(42));
    std::int32_t start = 0;
    for (const auto& p : w.people()) if (p.alive) ++start;
    CHECK(start > 0);

    w.setDetail(sim::World::Detail::Abstract);
    w.runTicks(db.time().ticksPerDay() * 600);

    std::int32_t alive = 0;
    for (const auto& p : w.people()) if (p.alive) ++alive;
    std::cerr << "    " << start << " left alone, " << alive << " after ten years\n";
    CHECK(alive > 0);
    // Not merely surviving: a community with land and hands grows, and one that
    // only ever shrinks is one whose accounting is wrong in the same direction.
    CHECK(alive >= start);
}

TEST(watching_costs_more_than_not_watching) {
    // The whole point of the abstract day is that it is cheap. A community
    // nobody is looking at should cost a small fraction of one that is, or the
    // map cannot hold a dozen of them.
    const auto& db = testing::sharedContent();
    sim::World detailed(db, cfg(23));
    sim::World abstracted(db, cfg(23));
    detailed.runTicks(db.time().ticksPerDay() * 20);
    abstracted.runTicks(db.time().ticksPerDay() * 20);
    abstracted.setDetail(sim::World::Detail::Abstract);

    const auto time = [&](sim::World& w) {
        const auto started = std::chrono::steady_clock::now();
        w.runTicks(db.time().ticksPerDay() * 40);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const double watched = time(detailed);
    const double unwatched = time(abstracted);
    std::cerr << "    forty days: watched " << watched << " s, unwatched " << unwatched << " s\n";
    CHECK(unwatched * 4 < watched);
}
