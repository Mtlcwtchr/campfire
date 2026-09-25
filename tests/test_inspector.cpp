#include "framework.hpp"
#include <cstdlib>
#include <cstdio>
#include "support.hpp"

#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"
#include "game/ui/info_panel.hpp"

namespace {

sim::WorldConfig cfg(std::uint64_t seed = 11) {
    sim::WorldConfig c;
    c.seed = seed;
    c.mapWidth = 96;
    c.mapHeight = 96;
    c.worldCells = 96;   // test scale: see test_determinism
    c.startingPopulation = 10;
    return c;
}

} // namespace

TEST(every_object_in_the_world_can_describe_itself) {
    // Clicking must never be able to crash or come back blank, whatever the thing
    // under the cursor happens to be.
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());
    w.runTicks(db.time().ticksPerDay() * 30);

    std::size_t people = 0, buildings = 0, resources = 0, stacks = 0;

    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        client::Selection s;
        s.kind = client::SelectionKind::Person;
        s.person = p.id;
        const auto info = ui::describe(w, s);
        CHECK(!info.title.empty());
        CHECK(!info.lines.empty());
        ++people;
    }
    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        client::Selection s;
        s.kind = client::SelectionKind::Building;
        s.building = b.id;
        const auto info = ui::describe(w, s);
        CHECK(!info.title.empty());
        ++buildings;
    }
    for (const auto& n : w.nodes()) {
        if (!n.alive) continue;
        client::Selection s;
        s.kind = client::SelectionKind::Resource;
        s.node = n.id;
        const auto info = ui::describe(w, s);
        CHECK(!info.title.empty());
        if (++resources > 200) break;      // a sample is enough; there are thousands
    }
    for (const auto& st : w.stacks()) {
        if (!st.alive || st.count <= 0) continue;
        client::Selection s;
        s.kind = client::SelectionKind::Stack;
        s.stack = st.id;
        s.stackGeneration = st.generation;
        const auto info = ui::describe(w, s);
        CHECK(!info.title.empty());
        if (++stacks > 200) break;
    }

    std::size_t animals = 0;
    for (const auto& a : w.animals()) {
        if (!a.alive) continue;
        client::Selection s;
        s.kind = client::SelectionKind::Animal;
        s.animal = a.id;
        const auto info = ui::describe(w, s);
        CHECK(!info.title.empty());
        ++animals;
    }

    CHECK(people > 0);
    CHECK(resources > 0);
    CHECK(stacks > 0);
    CHECK(animals > 0);
    (void)buildings;
}

TEST(every_tile_describes_itself) {
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg(7));
    w.runTicks(db.time().ticksPerDay() * 5);

    for (std::int32_t y = 0; y < w.map().height(); y += 7) {
        for (std::int32_t x = 0; x < w.map().width(); x += 7) {
            client::Selection s;
            s.kind = client::SelectionKind::Tile;
            s.tile = {x, y};
            const auto info = ui::describe(w, s);
            CHECK(!info.title.empty());
        }
    }
}

TEST(a_click_always_selects_something_and_cycles) {
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg(11));
    w.runTicks(db.time().ticksPerDay() * 20);

    // The tile a pawn is standing on has at least that pawn and the ground.
    const auto& p = w.people().front();
    const core::TilePos t = core::toTile(p.pos);
    const auto candidates = client::candidatesAt(w, t);
    CHECK(candidates.size() >= 2);
    CHECK(candidates.back().kind == client::SelectionKind::Tile);

    // Repeated clicks walk the whole list and come back round.
    client::Selection sel;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        sel = client::pickAt(w, t, sel);
        CHECK(sel.valid());
        CHECK(sel == candidates[i]);
    }
    sel = client::pickAt(w, t, sel);
    CHECK(sel == candidates.front());

    // Bare ground still answers a click.
    client::Selection ground = client::pickAt(w, {0, 0}, {});
    CHECK(ground.valid());
}

TEST(a_selection_does_not_outlive_what_it_points_at) {
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg(3));
    w.runTicks(db.time().ticksPerDay() * 10);

    // A batch that is consumed must not leave a live handle behind, even after
    // its slot is handed to something else (DECISIONS.md D16).
    core::ItemStackId victim;
    std::uint32_t generation = 0;
    for (const auto& s : w.stacks()) {
        if (s.alive && s.count > 0) { victim = s.id; generation = s.generation; break; }
    }
    CHECK(victim.valid());

    client::Selection sel;
    sel.kind = client::SelectionKind::Stack;
    sel.stack = victim;
    sel.stackGeneration = generation;
    CHECK(client::stillExists(w, sel));

    w.destroyStack(victim);
    CHECK(!client::stillExists(w, sel));

    const core::ItemStackId reused = w.spawnStack(w.db().itemByName("branch"), 3, {5, 5}, sim::SettlementId{0});
    CHECK_EQ(reused.value, victim.value);          // the slot really was recycled
    CHECK(!client::stillExists(w, sel));           // and the old selection knows it
}

TEST(the_panels_answer_the_two_standing_questions) {
    // Who does what, and what we have. Both are produced as text so a test can
    // read them without a window - the same reason the inspector is.
    const auto& db = testing::sharedContent();
    sim::World w(db, cfg());
    w.runTicks(db.time().ticksPerDay() * 30);
    const sim::SettlementId home = w.settlements().front().id;

    const ui::InfoText trades = ui::describeTrades(w, home, 0);
    CHECK(!trades.title.empty());
    CHECK(!trades.lines.empty());
    // Closed, every line is a trade heading with a count on it.
    for (const auto& line : trades.lines) {
        CHECK(line.rfind("+ ", 0) == 0 || line.rfind("- ", 0) == 0);
        CHECK(line.find(':') != std::string::npos);
    }

    // The counts add up to the grown people of the settlement: everybody lives
    // by something.
    std::int32_t counted = 0;
    for (const auto& line : trades.lines)
        counted += std::atoi(line.substr(line.find(':') + 1).c_str());
    std::int32_t grown = 0;
    for (sim::PersonId id : w.settlements().front().members)
        if (w.person(id).alive) ++grown;
    CHECK_EQ(counted, grown);

    // Opened, a trade shows its people and what each is doing.
    std::uint32_t open = 0;
    for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) open |= (1u << i);
    const ui::InfoText opened = ui::describeTrades(w, home, open);
    CHECK(opened.lines.size() > trades.lines.size());
    bool sawPerson = false;
    for (const auto& line : opened.lines)
        if (line.rfind("    ", 0) == 0) sawPerson = true;
    CHECK(sawPerson);

    // And the stores summary accounts for every batch the settlement owns.
    const ui::InfoText stores = ui::describeStores(w, home);
    CHECK(!stores.lines.empty());
    std::int32_t owned = 0;
    for (const auto& s : w.stacks())
        if (s.alive && s.count > 0 && s.owner == home &&
            (s.where == sim::StackWhere::Ground || s.where == sim::StackWhere::InBuilding))
            owned += s.count;
    std::int32_t listed = 0;
    for (const auto& line : stores.lines) {
        if (line.empty() || line[0] == '#') continue;
        std::size_t at = line.find(':');
        if (at == std::string::npos) continue;
        std::string rest = line.substr(at + 1);
        for (char& c : rest) if (c == '/') c = ' ';
        std::int32_t a = 0, b = 0, c = 0, d = 0;
        std::sscanf(rest.c_str(), "%d %d %d %d", &a, &b, &c, &d);
        listed += a + b + c + d;
    }
    if (listed != owned)
        std::cerr << "    the panel accounts for " << listed << " of " << owned << "\n";
    CHECK_EQ(listed, owned);
}
