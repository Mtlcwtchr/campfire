#pragma once
// What every imported asset must carry (doc/plan_procedural_environment_2026-10-03.md, part I;
// fantasy_80s_art_asset_style_spec §4).
//
// An asset from anywhere - CC0 packs, photogrammetry, retro kits, kitbash - is
// kept only with where it came from, who made it, under what licence, what was
// done to it and what that licence forbids. The same record says what it is
// for: its category, the zones it suits, the tags recipes and cover rules
// pick it by.
//
// Kept beside the asset as <asset>.meta.json, or as one manifest of many:
//
//   { "assets": [ { "name": "Boulder_Round", "source": "https://...", "author": "...",
//                   "license": "CC0-1.0", "modifications": ["recoloured", "decimated"],
//                   "redistribution": "", "category": "rock", "zones": ["rocky_slope"],
//                   "tags": ["boulder", "talus"] } ] }
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine::environment {

struct AssetMeta {
    std::string name;
    std::string file;              // the manifest or sidecar it was read from
    std::string source, author, license, licenseUrl, redistribution, category;
    std::vector<std::string> modifications, zones, tags;
};

struct AssetProblem {
    std::string file;
    std::string what;
};

// Licences the validator knows, and whether each asks for attribution.
struct KnownLicence {
    const char* id;
    bool attribution;
    bool shareAlike;
};
inline constexpr KnownLicence kLicences[] = {
        {"CC0-1.0", false, false},       {"public-domain", false, false}, {"CC-BY-4.0", true, false},
        {"CC-BY-3.0", true, false},      {"CC-BY-SA-4.0", true, true},    {"MIT", true, false},
        {"Unlicense", false, false},     {"Quixel-Megascans", false, false}, {"proprietary-owned", false, false},
        {"custom", true, false}};

// Every *.meta.json and every manifest (a JSON with "assets") under `dir`.
std::vector<AssetMeta> loadAssetMetas(const std::filesystem::path& dir, std::vector<AssetProblem>* problems);
// Problems with one record: missing source/author/licence, an unknown licence,
// a share-alike licence (which the game must not ship without saying so), a
// custom licence with no redistribution note.
void validateAssetMeta(const AssetMeta& meta, std::vector<AssetProblem>& problems);
// The names of models used by content that have no record.
std::vector<std::string> unrecorded(const std::vector<AssetMeta>& metas, const std::vector<std::string>& used);

} // namespace engine::environment
