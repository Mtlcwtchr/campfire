#include "framework.hpp"

#include <filesystem>
#include <fstream>
#include <set>

#include "nlohmann/json.hpp"

#include "support.hpp"

#include "game/content/ground_materials.hpp"
#include "game/generation/local_map_gen.hpp"
#include "game/simulation/world.hpp"

TEST(content_loads_without_errors) {
    const auto& db = testing::sharedContent();
    for (const auto& e : db.errors()) std::cerr << "    content error: " << e << "\n";
    CHECK(db.errors().empty());
    CHECK(!db.items().empty());
    CHECK(!db.recipes().empty());
    CHECK(!db.buildings().empty());
    CHECK(!db.resourceNodes().empty());
    CHECK(db.ethnosByName("achaean").valid());
}

TEST(housing_has_rts_progression_and_bread_needs_a_bakery) {
    const auto& db = testing::sharedContent();
    // Three roofs, not four: a reed hut, a house, a large house. A second reed
    // hut of a slightly different size was one rung too many on a three-rung
    // ladder, so there is one small hut and it is called a hut.
    const auto hut = db.buildingByName("hut");
    const auto house = db.buildingByName("mudbrick_house");
    const auto bakery = db.buildingByName("mudbrick_bakery");
    const auto campfire = db.buildingByName("campfire");
    CHECK(hut.valid() && house.valid() && bakery.valid() && campfire.valid());
    CHECK(!db.buildingByName("reed_hut").valid());
    CHECK(!db.buildingByName("reed_shelter").valid());
    CHECK(db.building(hut).comfortBonus < db.building(house).comfortBonus);
    CHECK(db.building(hut).sleepingSlots < db.building(house).sleepingSlots);
    // Three tiers of roof, so a family builds to the size it actually is: a
    // shelter for one or two, a house for three or four, a large house for a
    // family of five or six.
    const core::DefId manor = db.buildingByName("mudbrick_manor");
    CHECK(manor.valid());
    CHECK(db.building(house).sleepingSlots < db.building(manor).sleepingSlots);
    CHECK(db.building(bakery).kind == content::BuildingKind::Workshop);
    // A production building takes real room: two by three, like a house or a
    // store. Everything was an odd square before, so a bakery and a campfire
    // were the same shape.
    CHECK_EQ(db.building(bakery).footprintWidth, 3);
    CHECK_EQ(db.building(bakery).footprintDepth, 2);
    CHECK_EQ(db.building(campfire).footprintWidth, 1);
    CHECK(db.building(bakery).function == "oven");
    CHECK(db.building(campfire).function.empty());
    CHECK(!db.buildingByName("bread_oven").valid());
}

TEST(new_pawn_skirt_represents_a_real_craftable_garment) {
    const auto& db = testing::sharedContent();
    const auto skirt = db.itemByName("loincloth");
    const auto recipe = db.recipeByName("make_loincloth");
    CHECK(skirt.valid() && recipe.valid());
    CHECK(db.item(skirt).category == content::ItemCategory::Clothing);
    bool producesSkirt = false;
    for (const auto& output : db.recipe(recipe).outputs)
        if (output.item == skirt && output.count == 1) producesSkirt = true;
    CHECK(producesSkirt);
}

TEST(every_recipe_produces_something_it_does_not_also_consume) {
    // A recipe whose only output is also an input is a no-op that would let the
    // planner loop forever on a job that never changes the world.
    const auto& db = testing::sharedContent();
    for (const auto& r : db.recipes()) {
        bool producesSomethingNew = false;
        for (const auto& o : r.outputs) {
            bool alsoInput = false;
            for (const auto& in : r.inputs) if (in.item == o.item) alsoInput = true;
            if (!alsoInput) producesSomethingNew = true;
        }
        if (!producesSomethingNew) std::cerr << "    circular recipe: " << r.name << "\n";
        CHECK(producesSomethingNew);
    }
}

TEST(every_culture_can_live_on_its_own_ground) {
    // GDD 7: from bare hands and untouched ground, a community must be able to
    // reach a stable material life, with no "you need the tool to build the
    // workbench that makes the tool". Each culture is asked about the country it
    // belongs in: a marsh people cannot reach an oak and should not have to.
    const auto& db = testing::sharedContent();
    CHECK(!db.ethnoi().empty());

    for (const auto& eth : db.ethnoi()) {
        std::vector<core::DefId> terrain;
        for (const auto& name : generation::resourceNamesFor(generation::parseBiome(eth.biome))) {
            const core::DefId id = db.resourceNodeByName(name);
            CHECK(id.valid());
            if (id.valid()) terrain.push_back(id);
        }
        std::vector<std::string> problems;
        const bool ok = db.validateEthnos(eth.id, terrain, problems);
        for (const auto& p : problems) std::cerr << "    " << p << "\n";
        CHECK(ok);
    }
}

TEST(a_culture_builds_out_of_what_its_country_gives_it) {
    // The Mesopotamian set is brick and reed because the floodplain has clay and
    // marsh and almost no timber; the Aegean set is timber and thatch. Neither
    // should be able to build the other's houses out of nothing.
    const auto& db = testing::sharedContent();
    const auto sumerian = db.ethnosByName("sumerian");
    CHECK(sumerian.valid());

    std::vector<core::DefId> marsh;
    for (const auto& name : generation::resourceNamesFor(generation::Biome::RiverValley)) {
        const core::DefId id = db.resourceNodeByName(name);
        if (id.valid()) marsh.push_back(id);
    }
    std::vector<std::string> knowledge = db.ethnos(sumerian).commonKnowledge;
    for (const auto& k : db.ethnos(sumerian).familyKnowledgePool) knowledge.push_back(k);

    const auto reach = db.reachableFrom(marsh, knowledge);
    CHECK(reach.canBuild(db.buildingByName("mudbrick_house")));
    CHECK(reach.canBuild(db.buildingByName("mudbrick_granary")));
    CHECK(reach.canBuild(db.buildingByName("hut")));
    CHECK(reach.has(db.itemByName("mudbrick")));
    CHECK(reach.has(db.itemByName("reed")));
}

TEST(something_edible_exists_without_any_tool_or_building) {
    // The first day has to be survivable, so at least one food source must be
    // harvestable with no tool and no workplace.
    const auto& db = testing::sharedContent();
    bool found = false;
    for (const auto& n : db.resourceNodes()) {
        if (n.harvest.requiredTool != content::ToolClass::None) continue;
        for (const auto& y : n.harvest.yields) {
            const auto& item = db.item(y.item);
            if (item.category == content::ItemCategory::Food && item.edibleRaw && !item.rawUnsafe) found = true;
        }
    }
    CHECK(found);
}

TEST(nothing_ordinary_demands_a_level_of_skill) {
    // The rule: knowing the method is what lets you try, and the level you have
    // decides how fast and how well it goes. A level requirement belongs on the
    // exceptional thing - a ziggurat, an embroidered robe - and there is no such
    // thing in content yet, so nothing may carry one.
    const auto& db = testing::sharedContent();
    for (const auto& r : db.recipes()) {
        if (r.requiredSkill.demanded())
            std::cerr << "    recipe " << r.name << " demands a level of skill\n";
        CHECK(!r.requiredSkill.demanded());
    }
    for (const auto& b : db.buildings()) {
        if (b.requiredSkill.demanded())
            std::cerr << "    building " << b.name << " demands a level of skill\n";
        CHECK(!b.requiredSkill.demanded());
    }
}

TEST(every_drawable_definition_has_a_sprite) {
    // The art library is keyed by content definition name. When it was keyed by
    // render category instead, a reed hut and a mud-brick house shared one
    // drawing, four of the eight mappings turned out to be crops of the printed
    // captions on the source sheets, and three whole atlases went unused - and
    // none of it could fail a build. This is that check.
    const auto root = testing::contentRoot().parent_path() / "assets" / "sprites";
    const auto manifestPath = root / "sprites.json";
    std::ifstream in(manifestPath);
    CHECK(in.good());
    if (!in.good()) return;

    nlohmann::json manifest;
    in >> manifest;
    std::set<std::string> have;
    for (auto it = manifest["sprites"].begin(); it != manifest["sprites"].end(); ++it) {
        have.insert(it.key());
        // A manifest entry naming a file that is not there is worse than no
        // entry: the renderer silently falls back and nobody notices.
        const auto file = root / it.value().value("file", std::string{});
        if (!std::filesystem::exists(file)) std::cerr << "    missing file: " << file << "\n";
        CHECK(std::filesystem::exists(file));
    }

    const auto& db = testing::sharedContent();
    std::vector<std::string> absent;
    for (const auto& b : db.buildings())
        if (!have.count("buildings/" + b.name)) absent.push_back("buildings/" + b.name);
    for (const auto& n : db.resourceNodes())
        if (!have.count("nodes/" + n.name)) absent.push_back("nodes/" + n.name);
    for (const auto& c : db.crops())
        for (int stage = 0; stage < 4; ++stage) {
            const auto key = "crops/" + c.name + "_" + std::to_string(stage);
            if (!have.count(key)) absent.push_back(key);
        }
    for (const char* terrain : {"grass", "forest", "dirt", "rock", "water", "sand", "marsh", "tilled"})
        if (!have.count(std::string("terrain/") + terrain))
            absent.push_back(std::string("terrain/") + terrain);

    for (const auto& key : absent) std::cerr << "    no sprite for " << key << "\n";
    CHECK(absent.empty());
}

TEST(the_pawn_rig_is_whole) {
    // Every layer is drawn into one rectangle because the atlases share a pivot.
    // A missing layer means people render headless or naked, so the rig is
    // checked as a unit rather than layer by layer at load time.
    const auto pawn = testing::contentRoot().parent_path() / "assets" / "sprites" / "pawn";
    for (const char* layer : {"body.png", "head.png", "arms.png", "garment_kilt.png",
                              "garment_tunic.png", "pawn.json"}) {
        if (!std::filesystem::exists(pawn / layer))
            std::cerr << "    pawn rig is missing " << layer << "\n";
        CHECK(std::filesystem::exists(pawn / layer));
    }

    std::ifstream in(pawn / "pawn.json");
    CHECK(in.good());
    if (!in.good()) return;
    nlohmann::json rig;
    in >> rig;
    // The renderer hardcodes these three numbers; if the rig changes them, the
    // composition silently drifts.
    CHECK_EQ(rig.value("cell", 0), 512);
    CHECK_EQ(rig["pivot"][0].get<int>(), 256);
    CHECK_EQ(rig["pivot"][1].get<int>(), 456);
}

TEST(a_ground_table_is_told_apart_from_any_other_array) {
    // What the editor is handed by a drag from the desktop, and what the
    // running game is handed by the file changing under it, is judged by this
    // and nothing else. It has to be strict in one direction: the world presets
    // are also an array of things with names, and a tool that took them for a
    // material table would offer to save one over the other.
    const std::filesystem::path config = testing::contentRoot() / "config";
    std::vector<content::GroundMaterial> table;
    CHECK(content::readGroundMaterials(config / "ground.json", table));
    CHECK(table.size() >= content::kBlendedMaterials);
    CHECK_EQ(table[0].name, std::string("grass"));

    std::vector<content::GroundMaterial> notATable;
    CHECK(!content::readGroundMaterials(config / "world_presets.json", notATable));
    CHECK(!content::readGroundMaterials(config / "defaults.json", notATable));
    CHECK(!content::readGroundMaterials(config / "no_such_file.json", notATable));
    CHECK(notATable.empty());

    // And the one with the fallback still gives a working table for all of
    // them, because a game that will not start because a look-and-feel file was
    // mistyped is worse than a game that looks wrong and says so.
    CHECK(content::loadGroundMaterials(config / "world_presets.json").size() >=
          content::kBlendedMaterials);
}

TEST(ground_texture_scales_match_the_large_pattern_defaults) {
    const std::filesystem::path config = testing::contentRoot() / "config";
    std::vector<content::GroundMaterial> table;
    CHECK(content::readGroundMaterials(config / "ground.json", table));
    const auto fallback = content::loadGroundMaterials(config / "no_such_file.json");
    CHECK(table.size() >= content::kBlendedMaterials);
    CHECK_EQ(fallback.size(), content::kBlendedMaterials);
    if (table.size() < content::kBlendedMaterials ||
        fallback.size() != content::kBlendedMaterials) return;

    // Metres per repeat, not UV frequency: larger values mean fewer repeats.
    const float metres[] = {48.0f, 40.0f, 52.0f, 36.0f, 44.0f, 56.0f};
    const char* names[] = {"grass", "dirt", "sand", "rock", "marsh", "snow"};
    for (std::size_t i = 0; i < content::kBlendedMaterials; ++i) {
        CHECK_EQ(table[i].name, std::string(names[i]));
        CHECK_EQ(fallback[i].name, table[i].name);
        CHECK_EQ(table[i].metresPerTurn, metres[i]);
        CHECK_EQ(fallback[i].metresPerTurn, metres[i]);
    }
}
