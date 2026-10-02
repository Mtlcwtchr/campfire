#include "engine/world_store/world_root.hpp"

#include <algorithm>
#include <set>

#include <nlohmann/json.hpp>

#include "engine/world_store/atomic_file.hpp"

namespace engine::world_store {
namespace {

void fail(std::string* why, std::string what) {
    if (why) *why = std::move(what);
}

bool isTemporary(const std::filesystem::path& p) {
    const auto name = p.filename().string();
    return name.size() >= kTemporarySuffix.size() &&
           name.compare(name.size() - kTemporarySuffix.size(), kTemporarySuffix.size(), kTemporarySuffix) == 0;
}

} // namespace

std::filesystem::path WorldRoot::forLayout(const std::filesystem::path& layoutFile) {
    return layoutFile.parent_path() / layoutFile.stem();
}

std::string WorldRoot::deltaFileName(const ChunkKey& key, std::uint64_t revision) {
    return key.stem() + ".r" + std::to_string(revision) + kDeltaExtension;
}

bool WorldRoot::exists() const {
    std::error_code ec;
    return std::filesystem::exists(manifestFile(), ec);
}

std::optional<Manifest> WorldRoot::readManifest(std::string* why) const {
    if (why) why->clear();
    if (!exists()) return std::nullopt;
    const auto bytes = readFileBytes(manifestFile(), why);
    if (!bytes) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(bytes->begin(), bytes->end());
        Manifest m;
        m.format = j.at("format").get<int>();
        if (m.format > Manifest::kFormat) {
            fail(why, "manifest written by a newer format (" + std::to_string(m.format) + ")");
            return std::nullopt;
        }
        m.worldSeed = j.at("world_seed").get<std::uint64_t>();
        m.source = j.value("source", std::string{});
        m.sourceHash = j.value("source_hash", std::uint64_t{0});
        if (j.contains("stable_domains"))
            for (const auto& [name, version] : j.at("stable_domains").items())
                m.stableDomains[name] = version.get<std::uint32_t>();
        m.commit = j.at("commit").get<std::uint64_t>();
        m.nextSequence = j.at("next_sequence").get<std::uint64_t>();
        for (const auto& e : j.at("delta")) {
            DeltaEntry d;
            d.key = {ChunkLevel::AuthoringChunk, e.at("x").get<std::int64_t>(), e.at("y").get<std::int64_t>()};
            d.revision = e.at("revision").get<std::uint64_t>();
            d.file = e.at("file").get<std::string>();
            d.payloadHash = e.at("hash").get<std::uint64_t>();
            d.bytes = e.value("bytes", std::uint64_t{0});
            // A name with a path in it would reach outside the delta directory.
            if (d.file.empty() || d.file.find('/') != std::string::npos || d.file.find('\\') != std::string::npos ||
                d.file == "." || d.file == "..") {
                fail(why, "manifest names a delta file outside its directory");
                return std::nullopt;
            }
            m.delta.push_back(std::move(d));
        }
        std::sort(m.delta.begin(), m.delta.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
        for (std::size_t i = 1; i < m.delta.size(); ++i)
            if (m.delta[i].key == m.delta[i - 1].key) {
                fail(why, "manifest names one chunk twice");
                return std::nullopt;
            }
        return m;
    } catch (const std::exception& e) {
        fail(why, std::string("manifest unreadable: ") + e.what());
        return std::nullopt;
    }
}

bool WorldRoot::writeManifest(const Manifest& manifest, std::string* why) const {
    nlohmann::json j;
    j["format"] = manifest.format;
    j["kind"] = "campfire world root";
    j["world_seed"] = manifest.worldSeed;
    j["source"] = manifest.source;
    j["source_hash"] = manifest.sourceHash;
    j["stable_domains"] = nlohmann::json::object();
    for (const auto& [name, version] : manifest.stableDomains) j["stable_domains"][name] = version;
    j["chunk_metres"] = {{"patch", chunkMetres(ChunkLevel::RuntimePatch)},
                         {"tile", chunkMetres(ChunkLevel::GenerationTile)},
                         {"chunk", chunkMetres(ChunkLevel::AuthoringChunk)},
                         {"region", chunkMetres(ChunkLevel::Region)}};
    j["commit"] = manifest.commit;
    j["next_sequence"] = manifest.nextSequence;
    auto sorted = manifest.delta;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    j["delta"] = nlohmann::json::array();
    for (const auto& d : sorted)
        j["delta"].push_back({{"x", d.key.x}, {"y", d.key.y}, {"revision", d.revision}, {"file", d.file},
                              {"hash", d.payloadHash}, {"bytes", d.bytes}});
    return writeFileAtomic(manifestFile(), j.dump(2) + "\n", why);
}

std::size_t WorldRoot::sweep(const Manifest& manifest) const {
    std::set<std::string> named;
    for (const auto& d : manifest.delta) named.insert(d.file);
    std::size_t removed = 0;
    std::error_code ec;
    if (std::filesystem::is_directory(deltaDirectory(), ec))
        for (const auto& entry : std::filesystem::directory_iterator(deltaDirectory(), ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto name = entry.path().filename().string();
            const bool delta = entry.path().extension() == kDeltaExtension;
            if ((delta && !named.contains(name)) || isTemporary(entry.path())) {
                std::error_code gone;
                if (std::filesystem::remove(entry.path(), gone)) ++removed;
            }
        }
    // A temporary beside the manifest is a manifest write that never finished.
    if (std::filesystem::is_directory(directory_, ec))
        for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
            std::error_code gone;
            if (entry.is_regular_file(ec) && isTemporary(entry.path()) && std::filesystem::remove(entry.path(), gone))
                ++removed;
        }
    return removed;
}

} // namespace engine::world_store


