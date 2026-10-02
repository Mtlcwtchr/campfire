#pragma once
// Hand edits to the details the terrain categories place
// (doc/plan_ground_types_2026-10-01.md, "Хранение правок").
//
// Decals and decorations are derived - from the world's seed, the chunk and
// the category's sets - and stored nowhere. What a person does to them is
// stored apart from the maps, so that repainting an id re-derives every
// detail and keeps what was placed by hand:
//
//   pinned    an instance put down by hand; it stays whatever the id becomes
//   removed   the stable ids of derived instances that must not be there
//   density   a brush's multipliers on the derived density, on a coarse grid
//
// One file a 32 km source chunk that holds any edit, beside the source:
// <world>/source/details/+0012_-0003.json. Chunks with none have no file
// (store only land - and only what was touched). A package export carries
// them as a vector layer (details/), an import brings them back.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace engine::biomes {

struct PinnedDetail {
    std::string id;          // stable: "pin:<chunk>:<n>" as made by pin()
    std::string kind;        // "prop", "plant", "decal"
    std::string name;        // in its library (props.json, plants.json, decals.json)
    double x = 0, y = 0;     // world metres
    double yaw = 0, scale = 1;
    bool operator==(const PinnedDetail&) const = default;
};

class DetailEdits {
public:
    static constexpr std::int64_t kChunkMetres = 32768;
    static constexpr double kDensityCell = 32.0;   // metres a density multiplier covers

    // Everything under <root>/details. A missing directory is no edits.
    static DetailEdits load(const std::filesystem::path& sourceRoot, std::string* why = nullptr);
    // Writes the chunks that changed since load, and removes the files of
    // chunks left with nothing.
    bool save(const std::filesystem::path& sourceRoot, std::string* why = nullptr);

    // --- edits ---
    // Returns the pinned instance's id.
    std::string pin(PinnedDetail detail);
    bool unpin(const std::string& id);
    // A derived instance's stable id, hidden; and shown again.
    void remove(std::uint64_t derivedId, double x, double y);
    void restore(std::uint64_t derivedId, double x, double y);
    // The density brush: the multiplier of the cell holding (x, y); 1 clears it.
    void setDensity(double x, double y, double multiplier);
    // A round brush: multiplier `by` at the centre, fading to none at `radius`.
    void brushDensity(double x, double y, double radius, double by);

    // --- queries ---
    [[nodiscard]] bool removed(std::uint64_t derivedId, double x, double y) const;
    [[nodiscard]] double density(double x, double y) const;   // 1 when untouched
    // Pinned instances inside [x0, x1) x [y0, y1).
    [[nodiscard]] std::vector<PinnedDetail> pinnedIn(double x0, double y0, double x1, double y1) const;
    [[nodiscard]] bool empty() const { return chunks_.empty(); }
    [[nodiscard]] std::size_t chunks() const { return chunks_.size(); }
    // Changes with every edit: what was placed under another is stale.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }

    // The file of a chunk: "+0012_-0003.json".
    [[nodiscard]] static std::string fileName(std::int64_t cx, std::int64_t cy);

private:
    struct Chunk {
        std::vector<PinnedDetail> pinned;
        std::set<std::uint64_t> removed;
        std::map<std::pair<std::int32_t, std::int32_t>, float> density;   // cell within the chunk -> multiplier
        std::uint32_t nextPin = 0;
        [[nodiscard]] bool empty() const { return pinned.empty() && removed.empty() && density.empty(); }
    };
    using Key = std::pair<std::int64_t, std::int64_t>;
    static Key chunkOf(double x, double y);
    Chunk& at(const Key& key);
    std::map<Key, Chunk> chunks_;
    std::set<Key> dirty_;
    std::uint64_t revision_ = 0;
};

} // namespace engine::biomes
