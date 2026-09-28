#include "game/content/ground_materials.hpp"

#include <fstream>

#include "nlohmann/json.hpp"

namespace content {
namespace {

// The built-in table, for when the file is missing or broken. The same numbers
// the file ships with: they are here as a floor rather than as the truth.
std::vector<GroundMaterial> builtIn() {
    auto one = [](const char* name, const char* set, const char* variant, float metres,
                  float width, float tear, float tearMetres) {
        GroundMaterial m;
        m.name = name;
        m.set = set;
        m.variant = variant;
        m.metresPerTurn = metres;
        m.blendWidth = width;
        m.tear = tear;
        m.tearMetres = tearMetres;
        return m;
    };
    return {one("grass", "Grass", "1", 2.0f, 0.34f, 0.30f, 3.4f),
            one("dirt", "Mud", "1", 2.07f, 0.34f, 0.34f, 3.0f),
            one("sand", "Sand", "1", 3.0f, 0.11f, 0.16f, 1.8f),
            one("rock", "Ground", "5", 3.0f, 0.08f, 0.22f, 1.4f),
            one("marsh", "Swamp", "1", 1.3f, 0.44f, 0.55f, 1.2f),
            one("snow", "Snow", "1", 2.0f, 0.28f, 0.24f, 2.6f)};
}

} // namespace

bool readGroundMaterials(const std::filesystem::path& file, std::vector<GroundMaterial>& into) {
    std::ifstream in(file);
    if (!in) return false;
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception&) {
        return false;
    }
    if (!j.is_array()) return false;
    std::vector<GroundMaterial> out;
    for (const auto& e : j) {
        // What makes an entry a material: a name, the pictures it is baked
        // from, and the scale the renderer draws it at. Checked, and strictly,
        // because this is what a file dropped on the editor is judged by - and
        // the world presets are also an array of things with names, and saving
        // a ground table over them would be a data loss done by a tool.
        if (!e.is_object() || !e.contains("name") || !e.contains("set") ||
            !e.contains("metres_per_turn"))
            return false;
        GroundMaterial m;
        m.name = e.value("name", std::string());
        m.set = e.value("set", std::string());
        m.variant = e.value("variant", std::string("1"));
        if (e.contains("tint") && e["tint"].is_array() && e["tint"].size() == 3)
            for (int i = 0; i < 3; ++i) m.tint[i] = e["tint"][i].get<float>();
        m.keepColour = e.value("keep_colour", m.keepColour);
        m.metresPerTurn = e.value("metres_per_turn", m.metresPerTurn);
        m.blendWidth = e.value("blend_width", m.blendWidth);
        m.tear = e.value("tear", m.tear);
        m.tearMetres = e.value("tear_metres", m.tearMetres);
        out.push_back(std::move(m));
    }
    // Fewer than the ground blends between is not a table with something
    // missing, it is a different kind of file that happens to be an array of
    // materials.
    if (out.size() < kBlendedMaterials) return false;
    into = std::move(out);
    return true;
}

std::vector<GroundMaterial> loadGroundMaterials(const std::filesystem::path& file) {
    std::vector<GroundMaterial> out;
    if (!readGroundMaterials(file, out)) return builtIn();
    return out;
}

bool saveGroundMaterials(const std::filesystem::path& file,
                         const std::vector<GroundMaterial>& materials) {
    // What was there first, kept once. Once, not every save: the point of a
    // backup is the state before the tweaking started, and a backup rewritten
    // on every save is a copy of the last mistake.
    std::error_code ec;
    const std::filesystem::path backup = file.string() + ".bak";
    if (std::filesystem::exists(file, ec) && !std::filesystem::exists(backup, ec))
        std::filesystem::copy_file(file, backup, ec);

    nlohmann::json j = nlohmann::json::array();
    for (const GroundMaterial& m : materials) {
        nlohmann::json e;
        e["name"] = m.name;
        e["set"] = m.set;
        e["variant"] = m.variant;
        e["tint"] = {m.tint[0], m.tint[1], m.tint[2]};
        e["keep_colour"] = m.keepColour;
        e["metres_per_turn"] = m.metresPerTurn;
        e["blend_width"] = m.blendWidth;
        e["tear"] = m.tear;
        e["tear_metres"] = m.tearMetres;
        j.push_back(std::move(e));
    }
    std::ofstream out(file);
    if (!out) return false;
    out << j.dump(2) << "\n";
    return out.good();
}

} // namespace content
