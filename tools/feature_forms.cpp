// feature_forms - procedural feature meshes as source assets
// (doc/plan_procedural_environment_2026-10-03.md part F; fantasy_80s_art_asset_style_spec §4).
//
// The forms a heightfield cannot carry - overhanging lips, shelves, undercut
// banks, root plates, and later spires and arches - made from the engine's
// signed distance fields (engine/environment/feature_mesh.hpp) and written as
// OBJ, each with its asset record (.meta.json). They enter the game as any
// other model does (tools/prepare_environment_models.py and the model
// catalogue), and a recipe names them like any model: one pipeline for every
// mesh the world draws.
//
//   feature_forms --form overhang [--length 12] [--depth 3] [--height 5]
//                 [--roughness 0.3] [--cell 0.4] [--seed 1] [--variants 4] --out DIR
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "engine/environment/feature_mesh.hpp"

namespace env = engine::environment;

int main(int argc, char** argv) {
    env::MeshRule rule;
    rule.form = env::ProceduralForm::None;
    float cell = 0.4f;
    std::uint64_t seed = 1;
    int variants = 1;
    std::filesystem::path out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--form") {
            const auto name = next();
            for (std::size_t f = 0; f < std::size(env::kProceduralFormNames); ++f)
                if (name == env::kProceduralFormNames[f]) rule.form = env::ProceduralForm(f);
        } else if (a == "--length") rule.length = std::atof(next().c_str());
        else if (a == "--depth") rule.depth = std::atof(next().c_str());
        else if (a == "--height") rule.height = std::atof(next().c_str());
        else if (a == "--roughness") rule.roughness = std::atof(next().c_str());
        else if (a == "--cell") cell = float(std::atof(next().c_str()));
        else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--variants") variants = std::max(1, std::atoi(next().c_str()));
        else if (a == "--out") out = next();
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    if (rule.form == env::ProceduralForm::None || out.empty()) {
        std::cerr << "usage: feature_forms --form shelf|overhang|undercut|root_plate|spire|arch [--length M] [--depth M]\n"
                     "                     [--height M] [--roughness 0..1] [--cell M] [--seed N] [--variants N] --out DIR\n";
        return 2;
    }
    std::filesystem::create_directories(out);
    const std::string form = env::kProceduralFormNames[int(rule.form)];
    for (int v = 0; v < variants; ++v) {
        const auto mesh = env::formMesh(rule, seed + std::uint64_t(v), cell);
        if (mesh.empty()) { std::cerr << form << ": nothing made (sizes too small for the cell?)\n"; return 1; }
        const std::string name = "Form_" + form + "_" + std::to_string(seed + std::uint64_t(v));
        std::ofstream obj(out / (name + ".obj"));
        obj << "# " << name << ": engine/environment feature form, z up, metres\n";
        for (const auto& p : mesh.positions) obj << "v " << p[0] << ' ' << p[1] << ' ' << p[2] << '\n';
        for (const auto& n : mesh.normals) obj << "vn " << n[0] << ' ' << n[1] << ' ' << n[2] << '\n';
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const auto a = mesh.indices[i] + 1, b = mesh.indices[i + 1] + 1, c = mesh.indices[i + 2] + 1;
            obj << "f " << a << "//" << a << ' ' << b << "//" << b << ' ' << c << "//" << c << '\n';
        }
        std::ofstream meta(out / (name + ".meta.json"));
        meta << "{\n  \"name\": \"" << name << "\",\n  \"source\": \"procedural: tools/feature_forms --form " << form
             << " --seed " << seed + std::uint64_t(v) << "\",\n  \"author\": \"generated\",\n"
             << "  \"license\": \"proprietary-owned\",\n  \"category\": \"rock\",\n  \"tags\": [\"" << form << "\"]\n}\n";
        std::cout << name << ": " << mesh.positions.size() << " vertices, " << mesh.indices.size() / 3 << " triangles\n";
    }
    return 0;
}
