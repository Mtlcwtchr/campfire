// Saved worlds on disk, and the edit tools' steps and their undo.
#include "framework.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

#include "engine/world_store/world_root.hpp"
#include "game/world/world_delta.hpp"
#include "game/world/world_saves.hpp"
#include "game/world/world_tools.hpp"

namespace {
using namespace world;
using core::Fixed;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("campfire_saves_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};

std::vector<generation::WorldPreset> presets() {
    generation::WorldPreset flat;
    flat.name = "flat";
    flat.label = "Flat";
    generation::WorldPreset wet = flat;
    wet.name = "wet";
    wet.params.rainfallPercent = 180;
    return {flat, wet};
}

core::WorldPos at(double x, double y) { return {Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)}; }

GroundAt flatPlus(const delta::WorldDelta& d) {
    return [layer = d.heights()](Fixed x, Fixed y) { return Fixed::fromInt(50) + layer->at(x, y); };
}

core::WorldRect around(double x, double y, double r) {
    return {{Fixed::fromDoubleForContent(x - r), Fixed::fromDoubleForContent(y - r)},
            {Fixed::fromDoubleForContent(x + r), Fixed::fromDoubleForContent(y + r)}};
}
} // namespace

TEST(world_saves_names_are_file_names_that_keep_their_letters) {
    CHECK_EQ(saves::cleanName("  a/b:c*d?  "), std::string("abcd"));
    CHECK_EQ(saves::cleanName("Мир / 1"), std::string("Мир 1"));
    CHECK_EQ(saves::cleanName(" ..  "), std::string());
    // Cut at sixty bytes, never inside a letter.
    std::string cyrillic;
    for (int i = 0; i < 40; ++i) cyrillic += "Ж";
    const auto cut = saves::cleanName(cyrillic);
    CHECK(cut.size() <= 60);
    CHECK_EQ(cut.size() % 2, std::size_t(0));
}

TEST(world_saves_create_list_and_delete_a_world) {
    TempDir dir;
    CHECK(saves::list(dir.path).empty());
    saves::NewWorld spec;
    spec.name = "Долина / north";
    spec.preset = "wet";
    spec.regions = 2;
    spec.seed = 77;
    std::string error;
    const auto made = saves::create(dir.path, spec, presets(), &error);
    CHECK(made.has_value());
    if (!made) return;
    CHECK_EQ(made->name, std::string("Долина north"));
    CHECK_EQ(made->regionsX, 2);
    CHECK_EQ(made->regionsY, 2);
    CHECK_EQ(made->generatedRegions, 4);
    CHECK_EQ(made->seed, std::uint64_t(77));
    CHECK(!made->hasHistory);
    CHECK(std::abs(made->widthKm() - 262.144) < 0.01);
    const auto layout = generation::loadWorldLayout(made->layout);
    CHECK(layout && layout->at(1, 1).settings.rainfallPercent == 180);

    // The same name again is refused, and a free one is offered.
    CHECK(!saves::create(dir.path, spec, presets(), &error));
    CHECK(!error.empty());
    CHECK_EQ(saves::freeName(dir.path, made->name), std::string("Долина north 2"));

    // A second world, changed later: listed first.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    spec.name = "Second";
    spec.regions = 1;
    const auto second = saves::create(dir.path, spec, presets());
    CHECK(second.has_value());
    auto listed = saves::list(dir.path);
    CHECK_EQ(listed.size(), std::size_t(2));
    if (listed.size() == 2) CHECK_EQ(listed.front().name, std::string("Second"));

    // History saved beside the first: it says so, and it goes with it.
    {
        auto history = delta::WorldDelta::open(engine::world_store::WorldRoot(made->root), 77);
        CHECK(history != nullptr);
        if (history) {
            history->heights()->add(10, 10, Fixed::fromInt(3));
            CHECK(history->save().ok);
        }
    }
    listed = saves::list(dir.path);
    const auto first = std::find_if(listed.begin(), listed.end(), [&](const auto& w) { return w.name == made->name; });
    CHECK(first != listed.end() && first->hasHistory && first->historyBytes > 0);
    if (first != listed.end()) CHECK(saves::remove(*first));
    CHECK(!fs::exists(made->layout));
    CHECK(!fs::exists(made->root));
    CHECK_EQ(saves::list(dir.path).size(), std::size_t(1));
}

TEST(world_tools_a_stroke_is_one_step_and_undo_puts_the_ground_back) {
    delta::WorldDelta d(5);
    tools::Editing edit(d);
    Brush raise;
    raise.kind = BrushKind::Raise;
    raise.radiusMetres = 20;
    raise.strength = 4;
    edit.begin("Raise");
    for (int frame = 0; frame < 10; ++frame)
        CHECK(edit.sculpt(raise, flatPlus(d), at(200 + frame * 3, 200), 1.0 / 30) > 0);
    edit.end();
    CHECK_EQ(edit.undoable(), std::size_t(1));
    CHECK_EQ(edit.undoLabel(), std::string("Raise"));
    const auto area = around(215, 200, 60);
    const auto dug = d.heights()->fingerprint(area);
    CHECK(dug != 0);
    CHECK(d.heights()->at(Fixed::fromInt(212), Fixed::fromInt(200)) > Fixed::fromInt(1));

    CHECK(edit.undo());
    CHECK_EQ(d.heights()->fingerprint(area), std::uint64_t(0));   // exactly, not nearly
    CHECK_EQ(edit.redoable(), std::size_t(1));
    CHECK(edit.redo());
    CHECK_EQ(d.heights()->fingerprint(area), dug);
    // A new edit forgets what was undone.
    CHECK(edit.undo());
    raise.kind = BrushKind::Lower;
    CHECK(edit.sculpt(raise, flatPlus(d), at(900, 900), 0.5) > 0);   // a step of its own
    CHECK_EQ(edit.redoable(), std::size_t(0));
    CHECK_EQ(edit.undoLabel(), std::string("Lower"));
}

TEST(world_tools_removed_objects_stand_again_after_undo_and_after_reload) {
    TempDir dir;
    const engine::world_store::WorldRoot root(dir.path / "w");
    std::vector<decor::Object> present;
    for (int i = 0; i < 6; ++i) {
        decor::Object o;
        o.id = 0x5000 + std::uint64_t(i);
        o.x = 1000 + i * 10;
        o.y = 1000;
        o.model = i % 2 == 0 ? 0u : 3u;   // trees and rocks
        present.push_back(o);
    }
    {
        auto d = delta::WorldDelta::open(root, 9);
        CHECK(d != nullptr);
        if (!d) return;
        tools::Editing edit(*d);
        // Trees only, within 25 m of the second: ids 0x5000 and 0x5002 (0x5004 is 30 m off).
        CHECK_EQ(edit.clear(present, 1010, 1000, 25, tools::kTrees), std::size_t(2));
        CHECK_EQ(d->objects()->read()->removed.size(), std::size_t(2));
        CHECK(edit.undo());
        CHECK(d->objects()->read()->removed.empty());
        // A planting, undone and redone: gone, then back under a new name.
        ecology::Added pine;
        pine.x = 1100;
        pine.y = 1100;
        pine.model = 1;
        const auto id = edit.plant(pine);
        CHECK(id != 0);
        CHECK(edit.undo());
        CHECK(d->objects()->read()->added.empty());
        CHECK(edit.redo());
        CHECK_EQ(d->objects()->read()->added.size(), std::size_t(1));
        // Removed again for the reload below.
        CHECK_EQ(edit.clear(present, 1000, 1000, 5, tools::kAllObjects), std::size_t(1));
        CHECK(d->save().ok);
    }
    // Everything the journal said - removed, restored, removed again - read back.
    auto again = delta::WorldDelta::open(root, 9);
    CHECK(again != nullptr);
    if (!again) return;
    const auto objects = again->objects()->read();
    CHECK_EQ(objects->removed.size(), std::size_t(1));
    CHECK(objects->removed.contains(0x5000));
    CHECK_EQ(objects->added.size(), std::size_t(1));
}

