#include "game/render/terrain_look.hpp"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "engine/biomes/registry.hpp"

namespace game {

void TerrainLook::clampAll() {
    tileScale = std::clamp(tileScale, 0.25f, 4.0f);
    octaves = std::clamp(octaves, 0.0f, 4.0f);
    octaveStart = std::clamp(octaveStart, 2.0f, 64.0f);
    macroVariation = std::clamp(macroVariation, 0.0f, 3.0f);
}

bool TerrainLook::load(const std::filesystem::path& file) {
    *this = TerrainLook{};
    std::ifstream in(file);
    if (!in) return false;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object()) return false;
    tileScale = j.value("tile_scale", tileScale);
    octaves = j.value("distance_octaves", octaves);
    octaveStart = j.value("octave_start_texels", octaveStart);
    macroVariation = j.value("macro_variation", macroVariation);
    clampAll();
    return true;
}

bool TerrainLook::save(const std::filesystem::path& file, std::string* why) const {
    nlohmann::ordered_json j;
    j["_comment"] = "How the ground's textures are laid, for every material (terrain_material.hlsli). "
                    "tile_scale: times every material's own tile size. distance_octaves: how many times the "
                    "scan may double its size as it shrinks on the screen (0: one size everywhere). "
                    "octave_start_texels: texels a pixel at which the next octave starts. macro_variation: "
                    "the broad light/dark variation over the scan.";
    j["tile_scale"] = tileScale;
    j["distance_octaves"] = octaves;
    j["octave_start_texels"] = octaveStart;
    j["macro_variation"] = macroVariation;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto partial = std::filesystem::path(file.string() + ".partial");
    {
        std::ofstream out(partial, std::ios::trunc);
        out << j.dump(1, ' ') << "\n";
        if (!out) {
            if (why) *why = "cannot write " + partial.string();
            return false;
        }
    }
    std::filesystem::rename(partial, file, ec);
    if (ec) {
        if (why) *why = ec.message();
        return false;
    }
    return true;
}

std::filesystem::path terrainLookFile() {
    return engine::biomes::defaultDirectory().parent_path() / "terrain_look.json";
}

} // namespace game

