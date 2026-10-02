#include "framework.hpp"

#include <filesystem>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

#include "engine/biomes/category_field.hpp"
#include "engine/biomes/detail_edits.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/biomes/shader_code.hpp"

namespace {
using namespace engine::biomes;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("campfire_biomes_" + name + "_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

fs::path contentTerrain() {
    fs::path dir = "content/config/terrain";
    for (int up = 0; up < 6 && !fs::exists(dir / "categories.json"); ++up) dir = ".." / dir;
    return dir;
}

void write(const fs::path& file, const nlohmann::json& j) { std::ofstream(file) << j.dump(1); }

// A small registry of its own: the engine's sixteen layers and two more, two
// soils, a rock, a decal, two categories and a forest.
void smallRegistry(const fs::path& dir, double tint = 0.9, const std::string& peatLayer = "extra_b") {
    nlohmann::json layers = nlohmann::json::array();
    for (const char* name : kBuiltInLayers) layers.push_back({{"name", name}, {"metres", 2.0}});
    layers.push_back({{"name", "extra_a"}, {"metres", 1.0}});
    layers.push_back({{"name", "extra_b"}, {"metres", 3.0}});
    write(dir / "layers.json", {{"layers", layers}});
    write(dir / "soils.json", {{"soils", {
        {{"name", "dust"}, {"layers", {{"extra_a", 0.7}, {"rocks_ground_05", 0.3}}},
         {"noise", {{"kind", "cellular"}, {"metres", 6}}}, {"tint", {tint, tint, tint}}},
        {{"name", "peat"}, {"layers", {{peatLayer, 1.0}}}}}}});
    write(dir / "rocks.json", {{"rocks", {{{"name", "slate"}, {"layer", "rock_face_03"},
                                          {"strata", {{"metres", 3.0}}}, {"scree", "dust"}}}}});
    write(dir / "decals.json", {{"decals", {{{"name", "flecks"}, {"kind", "speckle"}, {"cell_m", 1.0},
                                            {"colour", {0.8, 0.1, 0.1}}}}}});
    write(dir / "forest_biomes.json", {{"forest_biomes", {{{"name", "pines"}, {"id", 3}, {"floor", {{"soil", "peat"}}}}}}});
    write(dir / "categories.json", {{"categories", {
        {{"name", "default"}, {"id", 0}},
        {{"name", "waste"}, {"id", 6}, {"soils", {{"grass", "dust"}}}, {"slopes", {{"rock", "slate"}}},
         {"decals", {{"flecks", 1.0}}}, {"defaults", {{"forest", "pines"}}}, {"look", {{"saturation", 0.4}}},
         {"controls", {{"moisture", 0.1}}}},
        {{"name", "ash"}, {"id", 7}, {"inherit", "waste"}, {"soils", {{"dirt", "peat"}}}}}}});
}

} // namespace

TEST(biomes_content_registry_is_valid) {
    const auto dir = contentTerrain();
    std::vector<Problem> problems;
    const auto registry = Registry::load(dir, &problems);
    for (const auto& p : registry->validate(dir.parent_path().parent_path().parent_path() / "assets")) problems.push_back(p);
    for (const auto& p : problems) std::fprintf(stderr, "    %s: %s\n", p.file.c_str(), p.what.c_str());
    CHECK(problems.empty());
    CHECK(registry->category(0u) && registry->category(0u)->name == "default");
    CHECK(!registry->category(0u)->changesGround());
    // The ids the hand maps were painted with (ground_legend.json).
    CHECK(registry->category("wastes") && registry->category("wastes")->id == 6);
    CHECK(registry->category("crater") && registry->category("crater")->id == 20);
    CHECK_EQ(registry->textureLayers().size(), std::size_t(22));
}

TEST(biomes_categories_inherit_from_their_parent) {
    const auto registry = Registry::load(contentTerrain());
    const Category* charred = registry->category("charred");
    const Category* wastes = registry->category("wastes");
    CHECK(charred && wastes);
    CHECK_EQ(charred->soils[0], wastes->soils[0]);          // grass: grey dust, the parent's
    CHECK_EQ(charred->slopeRock, wastes->slopeRock);
    CHECK_EQ(charred->forest, std::string("charred_dead"));  // its own
    CHECK_EQ(charred->water, std::string("rotten_green"));   // the parent's
    // A child can take back what its parent set: no scree on the toothed ridge.
    CHECK(registry->category("jagged")->scree.empty());
    CHECK_EQ(registry->category("rock")->scree, std::string("slate_rubble"));
    // Layer biomes resolve through the category where the layer says 0.
    const auto conifer = registry->forestBiome(registry->resolve(Layer::Forest, registry->category("conifer")->id, 0));
    CHECK(conifer && conifer->name == "conifer");
    CHECK_EQ(registry->resolve(Layer::Forest, registry->category("conifer")->id, 11), std::uint32_t(11));
    CHECK(registry->control(registry->category("wastes")->id, "moisture_bias") == std::optional<double>(0.06));
    const auto legend = registry->legend(Layer::Ground);
    CHECK(legend.count("swamp") && legend.at("swamp") == 5);
}

TEST(biomes_validator_reports_what_is_wrong) {
    TempDir dir("invalid");
    smallRegistry(dir.path);
    CHECK(Registry::load(dir.path)->validate().empty());
    // Unknown soil, a share sum off one, a cycle, a doubled id.
    auto categories = nlohmann::json::parse(std::ifstream(dir.path / "categories.json"));
    categories["categories"][1]["soils"]["sand"] = "nowhere";
    categories["categories"][1]["inherit"] = "ash";
    categories["categories"].push_back({{"name", "twin"}, {"id", 7}});
    write(dir.path / "categories.json", categories);
    auto soils = nlohmann::json::parse(std::ifstream(dir.path / "soils.json"));
    soils["soils"][0]["layers"]["rocks_ground_05"] = 0.5;   // written as an object: {layer: share}
    write(dir.path / "soils.json", soils);
    const auto problems = Registry::load(dir.path)->validate();
    const auto says = [&](const std::string& what) {
        for (const auto& p : problems)
            if (p.what.find(what) != std::string::npos) return true;
        return false;
    };
    CHECK(says("nowhere"));
    CHECK(says("shares sum"));
    CHECK(says("cycle"));
    CHECK(says("id 7"));
}

TEST(biomes_numbers_do_not_change_the_code) {
    TempDir a("code_a"), b("code_b"), c("code_c");
    smallRegistry(a.path, 0.9);
    smallRegistry(b.path, 0.5);                       // another tint: a number
    smallRegistry(c.path, 0.9, "extra_a");            // another layer: structure
    const auto ra = Registry::load(a.path), rb = Registry::load(b.path), rc = Registry::load(c.path);
    CHECK(shaderCode(*ra).biomes == shaderCode(*ra).biomes);   // deterministic
    CHECK(shaderCode(*ra).biomes == shaderCode(*rb).biomes);
    CHECK_EQ(structureKey(*ra), structureKey(*rb));
    CHECK(structureKey(*ra) != structureKey(*rc));
    // The number went to the table instead: the soil row's tint.
    const auto ta = shaderTable(*ra), tb = shaderTable(*rb);
    const std::size_t soilTint = (std::size_t(kRowSoil + 0) * kTableColumns + 2) * 4;
    CHECK(std::fabs(ta[soilTint] - 0.9f) < 1e-6f);
    CHECK(std::fabs(tb[soilTint] - 0.5f) < 1e-6f);
    // The code names the category, its layers and its decal family.
    const auto code = shaderCode(*ra).biomes;
    CHECK(code.find("case 6:") != std::string::npos);
    CHECK(code.find("BIOME_NOISE_CELLULAR") != std::string::npos);
    CHECK(code.find("BIOME_NOISE_FBM") == std::string::npos);   // nothing uses it, so it is not compiled in
    CHECK(code.find("BIOME_DECAL_SPECKLE") != std::string::npos);
    CHECK(code.find("BIOME_DECAL_STAIN") == std::string::npos);
    // Category 7 inherits 6's grass and adds its own dirt.
    CHECK(code.find("case 7:") != std::string::npos);
}

TEST(biomes_emit_writes_only_what_changed) {
    TempDir dir("emit"), shaders("emit_shaders");
    smallRegistry(dir.path);
    const auto registry = Registry::load(dir.path);
    const auto first = emitShaderCode(*registry, shaders.path);
    CHECK(first.ok && first.changed);
    const auto second = emitShaderCode(*registry, shaders.path);
    CHECK(second.ok && !second.changed);
    // A registry the validator refuses writes nothing.
    auto soils = nlohmann::json::parse(std::ifstream(dir.path / "soils.json"));
    soils["soils"][0]["layers"] = {{"missing_layer", 1.0}};
    write(dir.path / "soils.json", soils);
    const auto refused = emitShaderCode(*Registry::load(dir.path), shaders.path);
    CHECK(!refused.ok && !refused.changed && !refused.problems.empty());
}

TEST(biomes_category_field_keeps_only_land) {
    CategoryField field;
    CHECK(field.empty());
    field.set(10, 10, {6, 0, 3, 0});
    field.set(11, 10, {5, 0, 0, 0});
    field.set(400, 400, {0, 0, 0, 0});   // nought: no chunk for it
    CHECK_EQ(field.chunksHeld(), std::size_t(1));
    CHECK(field.at(10 * 256.0 + 5, 10 * 256.0 + 200)[0] == 6);
    CHECK(field.at(11 * 256.0 + 5, 10 * 256.0 + 5)[0] == 5);
    CHECK(field.at(-5000, -5000)[0] == 0);
    // Midway between the two samples' centres: an even pair.
    const auto pair = field.groundPair(11 * 256.0, 10 * 256.0 + 128);
    CHECK((pair.a == 6 && pair.b == 5) || (pair.a == 5 && pair.b == 6));
    CHECK(std::fabs(pair.share - 0.5) < 1e-9);
    const auto key = field.key();
    field.set(10, 10, {0, 0, 0, 0});
    field.set(11, 10, {0, 0, 0, 0});
    CHECK(field.key() != key);
    field.settle();
    CHECK(field.empty());
}

TEST(biomes_detail_edits_round_trip_and_keep_only_touched_chunks) {
    TempDir dir("details");
    DetailEdits edits;
    CHECK(edits.empty());
    const auto id = edits.pin({"", "prop", "fallen_log", 1000.0, 2000.0, 0.5, 1.2});
    edits.remove(0xABCDEF0123ull, 40000.0, 100.0);         // another chunk
    edits.setDensity(1000.0, 2000.0, 2.0);
    CHECK(edits.removed(0xABCDEF0123ull, 40000.0, 100.0));
    CHECK(!edits.removed(0xABCDEF0123ull, 1000.0, 100.0));   // the id is the chunk's
    CHECK(std::fabs(edits.density(1010.0, 2010.0) - 2.0) < 1e-6);
    CHECK(std::fabs(edits.density(5000.0, 2010.0) - 1.0) < 1e-6);
    CHECK(edits.save(dir.path));
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir.path / "details")) files += e.path().extension() == ".json";
    CHECK_EQ(files, std::size_t(2));

    auto back = DetailEdits::load(dir.path);
    const auto pinned = back.pinnedIn(0, 0, 32768, 32768);
    CHECK_EQ(pinned.size(), std::size_t(1));
    CHECK(!pinned.empty() && pinned[0].id == id && pinned[0].name == "fallen_log");
    CHECK(back.removed(0xABCDEF0123ull, 40000.0, 100.0));
    CHECK(std::fabs(back.density(1010.0, 2010.0) - 2.0) < 1e-6);
    // Emptied, a chunk leaves no file behind.
    CHECK(back.unpin(id));
    back.setDensity(1000.0, 2000.0, 1.0);
    back.restore(0xABCDEF0123ull, 40000.0, 100.0);
    CHECK(back.save(dir.path));
    files = 0;
    for (const auto& e : fs::directory_iterator(dir.path / "details")) files += e.path().extension() == ".json";
    CHECK_EQ(files, std::size_t(0));
    // A brush fades from its centre.
    DetailEdits brush;
    brush.brushDensity(500.0, 500.0, 100.0, 3.0);
    CHECK(brush.density(500.0, 500.0) > 2.5);
    CHECK(brush.density(590.0, 500.0) < brush.density(530.0, 500.0));
    CHECK(std::fabs(brush.density(700.0, 500.0) - 1.0) < 1e-6);
}
