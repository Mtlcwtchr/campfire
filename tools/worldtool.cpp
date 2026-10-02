// worldtool: the authoring package <-> WorldSource bridge
// (doc/world_authoring_import_export_spec.docx).
//
//   worldtool import  <package> <source>
//   worldtool export  <source> <package> [--rect x0,y0,x1,y1] [--region layer:id]
//                     [--feature id-or-name] [--layers a,b,c] [--preview]
//   worldtool info    <source>
//   worldtool example <package> [--km 256]
//   worldtool biomes  [--dir content/config/terrain] [--emit [assets/shaders]] [--check-assets assets]
//
// `import` renumbers the package's categorical ids by name onto the terrain
// registry's (content/config/terrain: the ground categories, the forest,
// water and decor biomes), and refuses a name the registry does not know;
// `--no-registry` takes the source's own legend instead.
//
// `biomes` validates the terrain categories and their libraries, prints what
// is wrong, and with --emit writes the shader code made from them
// (terrain_layers.gen.hlsli, terrain_biomes.gen.hlsli) where it changed.
//
// `example` writes a small synthetic package - a skeleton heightmap, a control
// map, region ids, flags, rivers, a lake, a ridge, points of interest - to try
// the round trip on before real maps exist.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/biomes/registry.hpp"
#include "engine/biomes/shader_code.hpp"
#include "engine/world_source/example_package.hpp"
#include "engine/world_source/transfer.hpp"
#include "engine/world_store/atomic_file.hpp"

namespace fs = std::filesystem;
using namespace engine::world_source;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  worldtool import  <package> <source> [--no-registry] [--registry dir]\n"
                 "  worldtool export  <source> <package> [--rect x0,y0,x1,y1] [--region layer:id]\n"
                 "                    [--feature id-or-name] [--layers a,b,c] [--preview]\n"
                 "  worldtool info    <source>\n"
                 "  worldtool example <package> [--km 256]\n"
                 "  worldtool biomes  [--dir content/config/terrain] [--emit [assets/shaders]] [--check-assets assets]\n";
    return 2;
}

std::vector<std::string> split(const std::string& text, char by) {
    std::vector<std::string> out;
    std::stringstream in(text);
    for (std::string part; std::getline(in, part, by);) out.push_back(part);
    return out;
}

int info(const fs::path& root) {
    std::string why;
    const auto source = WorldSource::open(root, &why);
    if (!source) { std::cerr << "worldtool: " << why << "\n"; return 1; }
    const auto& w = source->schema().world;
    std::uintmax_t bytes = 0;
    std::error_code ec;
    for (const auto& [key, record] : source->chunks()) bytes += fs::file_size(root / "chunks" / record.file, ec);
    const auto total = std::int64_t(std::ceil(w.widthMetres / 32768)) * std::int64_t(std::ceil(w.heightMetres / 32768));
    std::cout << "world " << w.widthMetres / 1000 << " x " << w.heightMetres / 1000 << " km at " << w.sampleMetres
              << " m, chunks " << source->chunks().size() << " of " << total << " kept (" << bytes / 1024 << " KB), "
              << source->vectors().size() << " features\n";
    for (const auto& layer : source->schema().rasters) {
        std::size_t kept = 0;
        for (const auto& [key, record] : source->chunks()) kept += record.layers.count(layer.name);
        std::cout << "  " << layer.name << " (" << kindName(layer.kind) << ", " << typeName(layer.type) << "): " << kept << " chunks\n";
        if (!layer.ids.empty()) {
            std::cout << "    ids:";
            for (const auto& [id, name] : layer.ids) std::cout << " " << id << "=" << name;
            std::cout << "\n";
        }
    }
    return 0;
}

int biomes(int argc, char** argv) {
    fs::path dir = engine::biomes::defaultDirectory(), shaders, assets;
    bool emit = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dir" && i + 1 < argc) dir = argv[++i];
        else if (a == "--emit") {
            emit = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') shaders = argv[++i];
        } else if (a == "--check-assets" && i + 1 < argc) assets = argv[++i];
        else return usage();
    }
    std::vector<engine::biomes::Problem> problems;
    const auto registry = engine::biomes::Registry::load(dir, &problems);
    for (const auto& p : registry->validate(assets)) problems.push_back(p);
    for (const auto& p : problems) std::cerr << "  " << p.file << ": " << p.what << "\n";
    std::cout << dir.string() << ": " << registry->categories().size() << " categories, "
              << registry->forestBiomes().size() << " forest, " << registry->waterBiomes().size() << " water, "
              << registry->decorBiomes().size() << " decor biomes; " << registry->soils().size() << " soils, "
              << registry->decals().size() << " decals, " << registry->textureLayers().size() << " texture layers; "
              << problems.size() << " problems\n";
    if (!problems.empty()) return 1;
    if (emit) {
        if (shaders.empty()) shaders = dir.parent_path().parent_path().parent_path() / "assets" / "shaders";
        const auto result = engine::biomes::emitShaderCode(*registry, shaders, assets);
        for (const auto& p : result.problems) std::cerr << "  " << p << "\n";
        if (!result.ok) return 1;
        std::cout << (result.changed ? "wrote " : "unchanged: ") << (shaders / engine::biomes::kLayersFile).string()
                  << ", " << engine::biomes::kBiomesFile << " (structure "
                  << std::hex << engine::biomes::structureKey(*registry) << std::dec << ")\n";
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "biomes") return biomes(argc, argv);
    if (argc < 3) return usage();
    const std::string action = argv[1];
    std::string why;
    if (action == "import" && argc >= 4) {
        ImportTarget target;
        bool registry = true;
        fs::path registryDir = engine::biomes::defaultDirectory();
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--no-registry") registry = false;
            else if (a == "--registry" && i + 1 < argc) registryDir = argv[++i];
            else return usage();
        }
        std::error_code ec;
        if (registry && fs::exists(registryDir / "categories.json", ec)) {
            const auto r = engine::biomes::Registry::load(registryDir);
            for (std::size_t k = 0; k < engine::biomes::kLayers; ++k)
                target.legends[engine::biomes::kLayerNames[k]] = r->legend(engine::biomes::Layer(k));
        }
        const auto report = importPackage(argv[2], argv[3], target, &why);
        if (!report) { std::cerr << "worldtool: " << why << "\n"; return 1; }
        const auto j = report->json();
        std::cout << j.dump(1) << "\n";
        // For the procedural compiler: what went stale, accumulated until it takes it.
        const fs::path dirty = fs::path(argv[3]) / "dirty.json";
        nlohmann::json all = nlohmann::json::object();
        if (const auto b = engine::world_store::readFileBytes(dirty)) {
            auto parsed = nlohmann::json::parse(b->begin(), b->end(), nullptr, false);
            if (!parsed.is_discarded()) all = parsed;
        }
        for (const auto& [stem, systems] : j["dirty"].items())
            for (const auto& s : systems) {
                auto& list = all[stem];
                if (std::find(list.begin(), list.end(), s) == list.end()) list.push_back(s);
            }
        engine::world_store::writeFileAtomic(dirty, all.dump(1));
        return 0;
    }
    if (action == "export" && argc >= 4) {
        ExportOptions options;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
            if (a == "--rect") {
                const auto v = split(next(), ',');
                if (v.size() != 4) return usage();
                options.rect = std::array<double, 4>{std::stod(v[0]), std::stod(v[1]), std::stod(v[2]), std::stod(v[3])};
            } else if (a == "--region") {
                const auto v = split(next(), ':');
                if (v.size() != 2) return usage();
                options.regionId = std::pair{v[0], std::uint32_t(std::stoul(v[1]))};
            } else if (a == "--feature") {
                options.feature = next();
            } else if (a == "--layers") {
                for (const auto& l : split(next(), ',')) options.layers.insert(l);
            } else if (a == "--preview") {
                options.mode = ExportOptions::Mode::Preview;
            } else {
                return usage();
            }
        }
        const auto report = exportPackage(argv[2], argv[3], options, &why);
        if (!report) { std::cerr << "worldtool: " << why << "\n"; return 1; }
        std::cout << "exported " << report->sizeX / 1000 << " x " << report->sizeY / 1000 << " km at ("
                  << report->originX << ", " << report->originY << "): " << report->rasters << " rasters, "
                  << report->features << " features\n";
        return 0;
    }
    if (action == "info") return info(argv[2]);
    if (action == "example") {
        double km = 256;
        for (int i = 3; i + 1 < argc; ++i) if (std::string(argv[i]) == "--km") km = std::stod(argv[i + 1]);
        const auto summary = writeExamplePackage(argv[2], km, &why);
        if (!summary) { std::cerr << "worldtool: " << why << "\n"; return 1; }
        std::cout << *summary << "\n";
        return 0;
    }
    return usage();
}
