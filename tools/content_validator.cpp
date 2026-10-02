// Content validator (GDD 7, 11).
//
// Loads the content tree, reports every unresolved reference, and then proves the
// thing GDD 7 insists on: that a naked community standing on untouched ground can
// reach a stable material life without any circular "you need a workbench to make
// the tool that builds the workbench" dependency.

#include <filesystem>
#include <iostream>

#include "engine/biomes/registry.hpp"
#include "game/content/content_db.hpp"
#include "game/generation/local_map_gen.hpp"
#include "game/simulation/world.hpp"
#include "game/world/scene_scatter.hpp"

int main(int argc, char** argv) {
    std::filesystem::path root = argc > 1 ? argv[1] : "content";
    if (!std::filesystem::exists(root)) {
        for (const char* prefix : {"..", "../..", "../../..", "../../../.."}) {
            std::filesystem::path candidate = std::filesystem::path(prefix) / root;
            if (std::filesystem::exists(candidate)) { root = candidate; break; }
        }
    }

    content::ContentDb db;
    const bool ok = db.load(root);

    std::cout << "content root: " << std::filesystem::absolute(root) << "\n";
    std::cout << db.items().size() << " items, " << db.recipes().size() << " recipes, "
              << db.buildings().size() << " buildings, " << db.resourceNodes().size()
              << " resources, " << db.knowledge().size() << " methods, "
              << db.traits().size() << " traits, " << db.ethnoi().size() << " ethnoi\n\n";

    for (const auto& warn : db.warnings()) std::cout << "warning: " << warn << "\n";
    for (const auto& err : db.errors()) std::cout << "error:   " << err << "\n";
    if (!ok) return 1;

    // Reachability is asked per ethnos on the ground its own country offers. A
    // marsh people cannot reach an oak and should not be asked to; what it must
    // reach is food, shelter, storage, a fire, an edge and a coat.
    int failures = 0;
    for (const auto& eth : db.ethnoi()) {
        const auto biome = generation::parseBiome(eth.biome);
        std::vector<core::DefId> terrain;
        for (const auto& name : generation::resourceNamesFor(biome)) {
            const core::DefId id = db.resourceNodeByName(name);
            if (id.valid()) terrain.push_back(id);
            else std::cout << "warning: biome " << eth.biome << " names a resource that does not exist: "
                           << name << "\n";
        }

        std::vector<std::string> problems;
        const bool ok = db.validateEthnos(eth.id, terrain, problems);
        std::cout << "ethnos " << eth.name << " (" << eth.biome << ", " << terrain.size()
                  << " natural features): " << (ok ? "can live here" : "PROBLEMS") << "\n";
        for (const auto& p : problems) std::cout << "  " << p << "\n";
        if (!ok) ++failures;
    }

    // The terrain categories and their libraries (content/config/terrain,
    // engine/biomes): references, ids, shares, inheritance, the texture
    // layers on disk, and the scene models their plants and props name.
    {
        const auto dir = root / "config" / "terrain";
        std::vector<engine::biomes::Problem> problems;
        const auto registry = engine::biomes::Registry::load(dir, &problems);
        for (const auto& p : registry->validate(root.parent_path() / "assets")) problems.push_back(p);
        const auto known = [](const std::string& model) {
            for (const char* m : world::decor::kModels) if (model == m) return true;
            return false;
        };
        for (const auto& p : registry->plants())
            if (!known(p.model)) problems.push_back({"plants.json", p.name + ": model " + p.model + " is not a scene model"});
        for (const auto& p : registry->props())
            if (!known(p.model)) problems.push_back({"props.json", p.name + ": model " + p.model + " is not a scene model"});
        std::cout << "\nterrain categories: " << registry->categories().size() << " categories, "
                  << registry->forestBiomes().size() << " forest, " << registry->waterBiomes().size() << " water, "
                  << registry->decorBiomes().size() << " decor biomes\n";
        for (const auto& p : problems) std::cout << "error:   " << p.file << ": " << p.what << "\n";
        if (!problems.empty()) ++failures;
    }

    std::cout << "\n" << (failures == 0 ? "content OK" : "content has unreachable definitions") << "\n";
    return failures == 0 ? 0 : 1;
}
