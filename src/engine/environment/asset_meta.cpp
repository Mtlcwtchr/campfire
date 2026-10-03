#include "engine/environment/asset_meta.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

namespace engine::environment {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

AssetMeta read(const json& j, const std::string& file) {
    AssetMeta m;
    m.file = file;
    const auto text = [&](const char* key, std::string& out) {
        if (auto it = j.find(key); it != j.end() && it->is_string()) out = it->get<std::string>();
    };
    const auto list = [&](const char* key, std::vector<std::string>& out) {
        if (auto it = j.find(key); it != j.end() && it->is_array())
            for (const auto& v : *it) if (v.is_string()) out.push_back(v.get<std::string>());
    };
    text("name", m.name);
    text("source", m.source);
    text("author", m.author);
    text("license", m.license);
    text("license_url", m.licenseUrl);
    text("redistribution", m.redistribution);
    text("category", m.category);
    list("modifications", m.modifications);
    list("zones", m.zones);
    list("tags", m.tags);
    return m;
}

} // namespace

std::vector<AssetMeta> loadAssetMetas(const fs::path& dir, std::vector<AssetProblem>* problems) {
    std::vector<AssetMeta> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto name = e.path().filename().string();
        if (name.size() > 10 && name.ends_with(".meta.json")) files.push_back(e.path());
        else if (name == "asset_meta.json" || name == "assets.meta.json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    for (const auto& path : files) {
        std::ifstream in(path);
        std::stringstream text;
        text << in.rdbuf();
        const auto rel = path.lexically_relative(dir).generic_string();
        json j = json::parse(text.str(), nullptr, false, true);
        if (j.is_discarded()) { if (problems) problems->push_back({rel, "not valid JSON"}); continue; }
        if (j.is_object() && j.contains("assets") && j["assets"].is_array()) {
            for (const auto& a : j["assets"]) if (a.is_object()) out.push_back(read(a, rel));
        } else if (j.is_object()) {
            auto m = read(j, rel);
            if (m.name.empty()) {
                // A sidecar is named by its file: Boulder.meta.json -> Boulder.
                auto stem = path.filename().string();
                m.name = stem.substr(0, stem.size() - std::string(".meta.json").size());
            }
            out.push_back(std::move(m));
        }
    }
    return out;
}

void validateAssetMeta(const AssetMeta& m, std::vector<AssetProblem>& problems) {
    const auto problem = [&](const std::string& what) { problems.push_back({m.file, m.name + ": " + what}); };
    if (m.name.empty()) problem("no name");
    if (m.source.empty()) problem("no source: where it came from must be recorded");
    if (m.license.empty()) { problem("no licence"); return; }
    const auto at = std::find_if(std::begin(kLicences), std::end(kLicences),
                                 [&](const KnownLicence& l) { return m.license == l.id; });
    if (at == std::end(kLicences)) { problem("unknown licence \"" + m.license + "\""); return; }
    if (at->attribution && m.author.empty()) problem(m.license + " needs the author for attribution");
    if (at->shareAlike) problem(m.license + " is share-alike: derived assets inherit it; record that in redistribution");
    if (std::string(at->id) == "custom" && m.redistribution.empty()) problem("a custom licence needs its redistribution terms");
}

std::vector<std::string> unrecorded(const std::vector<AssetMeta>& metas, const std::vector<std::string>& used) {
    std::set<std::string> have;
    for (const auto& m : metas) have.insert(m.name);
    std::vector<std::string> out;
    for (const auto& u : used) if (!have.count(u)) out.push_back(u);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace engine::environment
