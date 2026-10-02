#pragma once
// A world on disk.
//
//   <root>/manifest.world         what is committed: versions, seeds, and the
//                                 revision of every delta chunk
//   <root>/delta/x_y.rN.wdelta    persistent history, one authoring chunk a file
//   <root>/cache/<layer>/...      derived data; deleting it loses nothing
//
// The authored source stays where the editor keeps it (a layout JSON beside
// the root; `WorldRoot::forLayout`), because that is the format people edit
// and diff. What is chunked is what grows with play: the delta, and caches.
//
// The manifest is the commit. A save writes each changed chunk to a NEW file
// named for its revision - never over a file the current manifest names - and
// only then replaces the manifest, atomically. A crash anywhere before that
// last rename leaves the previous manifest pointing at the previous files,
// which are all still there; a crash after it leaves the new ones. Either way
// every chunk agrees with every other, which is what lets one edit span two
// chunk files. Files the manifest does not name are leftovers of one of those
// two cases and are swept when the root is next opened.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine/world_store/chunk_key.hpp"

namespace engine::world_store {

struct DeltaEntry {
    ChunkKey key;
    std::uint64_t revision = 0;
    std::string file;              // relative to the delta directory
    std::uint64_t payloadHash = 0;
    std::uint64_t bytes = 0;
    bool operator==(const DeltaEntry&) const = default;
};

struct Manifest {
    static constexpr int kFormat = 1;
    int format = kFormat;
    std::uint64_t worldSeed = 0;
    // The layout file this root belongs to, relative to the root's parent, and
    // what it hashed to at the last commit. Informative: a delta outlives any
    // number of edits to its source.
    std::string source;
    std::uint64_t sourceHash = 0;
    // The generator versions a delta depends on, by name: at present only the
    // candidate lattice procedural objects are numbered in. A change is a
    // migration, reported when the root is opened and never applied silently.
    std::map<std::string, std::uint32_t> stableDomains;
    std::uint64_t commit = 0;          // how many commits this root has had
    std::uint64_t nextSequence = 1;    // the next edit's number, world-wide
    std::vector<DeltaEntry> delta;     // sorted by key
    bool operator==(const Manifest&) const = default;
};

class WorldRoot {
public:
    static constexpr const char* kManifestName = "manifest.world";
    static constexpr const char* kDeltaExtension = ".wdelta";

    explicit WorldRoot(std::filesystem::path directory) : directory_(std::move(directory)) {}
    // worlds/world.json -> worlds/world: the root that belongs to a layout.
    static std::filesystem::path forLayout(const std::filesystem::path& layoutFile);

    [[nodiscard]] const std::filesystem::path& directory() const { return directory_; }
    [[nodiscard]] std::filesystem::path manifestFile() const { return directory_ / kManifestName; }
    [[nodiscard]] std::filesystem::path deltaDirectory() const { return directory_ / "delta"; }
    [[nodiscard]] std::filesystem::path cacheDirectory(const std::string& layer) const {
        return directory_ / "cache" / layer;
    }
    // "x_y.rN.wdelta": a new revision is a new file, so a commit never has to
    // overwrite what the previous one names.
    [[nodiscard]] static std::string deltaFileName(const ChunkKey& key, std::uint64_t revision);
    [[nodiscard]] std::filesystem::path deltaFile(const std::string& name) const { return deltaDirectory() / name; }

    [[nodiscard]] bool exists() const;
    // Nothing, with `why` empty, when there is no manifest yet: a new world.
    // Nothing with `why` set when there is one and it cannot be read.
    [[nodiscard]] std::optional<Manifest> readManifest(std::string* why = nullptr) const;
    bool writeManifest(const Manifest& manifest, std::string* why = nullptr) const;
    // Delete every file in delta/ the manifest does not name, and every
    // temporary anywhere in the root. Returns how many went.
    std::size_t sweep(const Manifest& manifest) const;

private:
    std::filesystem::path directory_;
};

} // namespace engine::world_store


