#pragma once
// The canonical authored source of a world (world_authoring_import_export_spec
// §3, §12): everything the procedural compiler needs, chunked, and nothing it
// makes.
//
//   <root>/metadata.world          the schema, which block type each layer is,
//                                  and every chunk there is with its layers'
//                                  hashes - the commit, written last
//   <root>/chunks/+0001_-0002.wchunk   one 32 km square (128 x 128 samples at
//                                  256 m), a block per layer (world_store's
//                                  chunk container, kind Source)
//   <root>/indexes/vectors.idx     stable id -> kind, bounds, chunks it touches
//   <root>/indexes/regions.idx     categorical layer -> id -> chunks, bounds
//   <root>/indexes/poi.idx         stable id -> chunk
//
// Only what differs from the defaults is kept. A chunk every layer of which
// is its default and that no feature touches - open sea - has no file at all
// (§12.1, UniformOcean); a layer of a chunk that is its default everywhere is
// no block; a layer one value everywhere is a block of a few bytes. So a world
// 2000 km a side of which a third is land holds a third of its samples.
//
// Everything is in world coordinates: a sample is (x, y) in samples of the
// whole world, a vertex is in metres (§4). A chunk is a container, never a
// coordinate system.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/world_source/schema.hpp"
#include "engine/world_store/chunk_key.hpp"

namespace engine::world_source {

using world_store::ChunkKey;
using world_store::ChunkLevel;

inline constexpr int kSourceFormat = 1;
inline constexpr std::int64_t kChunkSamples = 128;

// ---- vectors (§6, §14) ---------------------------------------------------------

enum class Geometry : std::uint8_t { Line, Polygon, Point };
const char* geometryName(Geometry geometry);

struct Ring {
    std::vector<std::array<double, 2>> points;          // metres
    std::map<std::string, std::vector<double>> values; // per vertex: "width", ...
    bool operator==(const Ring&) const = default;
};

// One authored feature: a river, a lake, a ridge, a coast correction, a
// region outline, a road, a point of interest. Its id is stable: it is what
// saves, edits and the runtime delta refer to it by, never an index.
struct Feature {
    std::string id;
    std::string kind;                   // the vector file it came from: "rivers", "poi", ...
    Geometry geometry = Geometry::Line;
    std::vector<Ring> rings;            // a line: one; a polygon: outer then holes; a point: one of one
    nlohmann::json properties = nlohmann::json::object();
    bool operator==(const Feature&) const = default;
};

// The part of a feature a chunk holds: the vertices of every segment that
// crosses the chunk, by their index in the ring, so the whole is put back
// together exactly by id and index (§8: merge fragments by stable id).
struct Fragment {
    std::string id, kind;
    Geometry geometry = Geometry::Line;
    struct Part {
        std::uint32_t ring = 0, count = 0;   // which ring, and how many vertices it has
        std::vector<std::uint32_t> indices;
        std::vector<std::array<double, 2>> points;
        std::map<std::string, std::vector<double>> values;
    };
    std::vector<Part> parts;
    nlohmann::json properties = nlohmann::json::object();
};

// The chunks a feature touches (its segments, and for a polygon every chunk
// whose centre it covers), and the fragment of it each holds.
std::map<ChunkKey, Fragment> fragmentsOf(const Feature& feature);
// Every chunk the feature reaches: those holding a fragment, and for a
// polygon those whose centre it covers.
std::set<ChunkKey> chunksTouched(const Feature& feature);
// Fragments of one id back into the feature. Nothing when they disagree or
// leave a vertex out.
std::optional<Feature> mergeFragments(const std::vector<Fragment>& fragments, std::string* why = nullptr);
struct Bounds {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    [[nodiscard]] bool overlaps(double x0, double y0, double x1, double y1) const {
        return minX <= x1 && x0 <= maxX && minY <= y1 && y0 <= maxY;
    }
};
Bounds boundsOf(const Feature& feature);

// ---- rasters ---------------------------------------------------------------------

// One layer of one chunk: 128 x 128 samples, channels interleaved, or one
// value for all of them.
struct Tile {
    std::uint8_t channels = 1;
    std::vector<std::uint16_t> values;    // dense, or empty when uniform
    std::vector<std::uint16_t> uniform;   // one per channel when uniform
    [[nodiscard]] bool isUniform() const { return values.empty(); }
    [[nodiscard]] std::uint16_t at(std::int64_t x, std::int64_t y, std::uint8_t channel) const {
        return isUniform() ? uniform[channel] : values[std::size_t((y * kChunkSamples + x) * channels + channel)];
    }
    // Dense, from whatever it is.
    void expand();
    // Back to one value where it is one.
    void settle();
};
Tile defaultTile(const RasterDesc& layer);

// ---- the store ---------------------------------------------------------------------

struct Chunk {
    std::map<std::string, Tile> rasters;      // layers that are not their default
    std::vector<Fragment> fragments;          // features (poi included)
    [[nodiscard]] bool empty() const { return rasters.empty() && fragments.empty(); }
};

struct ChunkRecord {
    std::string file;
    std::uint64_t payloadHash = 0;
    std::map<std::string, std::uint64_t> layers;   // layer (or "vectors") -> block hash
    bool operator==(const ChunkRecord&) const = default;
};

struct VectorIndexEntry {
    std::string kind;
    Bounds bounds;
    std::set<ChunkKey> chunks;
};

class WorldSource {
public:
    // A new, empty source: every chunk its defaults.
    static std::optional<WorldSource> create(const std::filesystem::path& root, Schema schema, std::string* why = nullptr);
    static std::optional<WorldSource> open(const std::filesystem::path& root, std::string* why = nullptr);
    [[nodiscard]] static bool exists(const std::filesystem::path& root);

    [[nodiscard]] const Schema& schema() const { return schema_; }
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }
    // A raster layer the package brings and the source has not had yet.
    bool addRaster(const RasterDesc& layer, std::string* why = nullptr);
    // A categorical layer's legend replaced (an import adds the names it
    // brings). Not visible to open() until commit().
    bool setLegend(const std::string& layer, std::map<std::uint32_t, std::string> ids, std::string* why = nullptr);

    [[nodiscard]] const std::map<ChunkKey, ChunkRecord>& chunks() const { return chunks_; }
    // A chunk as stored; the empty chunk when there is no file.
    [[nodiscard]] std::optional<Chunk> read(const ChunkKey& key, std::string* why = nullptr) const;
    // The layer of a chunk, its default when the chunk does not keep it.
    [[nodiscard]] Tile tile(const Chunk& chunk, const std::string& layer) const;
    // Replaces what the chunk holds. An empty chunk is removed. Not visible to
    // open() until commit().
    bool write(const ChunkKey& key, const Chunk& chunk, std::string* why = nullptr);
    // Metadata and indexes, atomically, last.
    bool commit(std::string* why = nullptr);

    // Indexes (§3, §14).
    [[nodiscard]] const std::map<std::string, VectorIndexEntry>& vectors() const { return vectors_; }
    // A feature's entry: what it is, where, and every chunk it touches -
    // polygon interiors included, which hold no fragment. Nothing: it is gone.
    void index(const std::string& id, const std::optional<VectorIndexEntry>& entry);
    // layer -> id -> chunk -> bounds of the id in it, in samples [x0, y0, x1, y1)
    using RegionIndex = std::map<std::string, std::map<std::uint32_t, std::map<ChunkKey, std::array<std::int64_t, 4>>>>;
    [[nodiscard]] const RegionIndex& regions() const { return regions_; }

    // Sample coordinates to the chunk holding them, and back.
    [[nodiscard]] static ChunkKey chunkOfSample(std::int64_t sx, std::int64_t sy) {
        return {ChunkLevel::SourceChunk, world_store::floorDiv(sx, kChunkSamples), world_store::floorDiv(sy, kChunkSamples)};
    }
    [[nodiscard]] static ChunkKey chunkOfPoint(double x, double y, double sampleMetres);
    // "+0001_-0002.wchunk"
    [[nodiscard]] static std::string fileName(const ChunkKey& key);
    [[nodiscard]] std::uint16_t blockTypeOf(const std::string& layer) const;

private:
    WorldSource() = default;
    void reindex(const ChunkKey& key, const Chunk& chunk);

    std::filesystem::path root_;
    Schema schema_;
    std::map<std::string, std::uint16_t> blockTypes_;   // layer -> block type, never reused
    std::uint16_t nextBlockType_ = 16;
    std::map<ChunkKey, ChunkRecord> chunks_;
    std::map<std::string, VectorIndexEntry> vectors_;
    RegionIndex regions_;
    std::map<std::string, ChunkKey> poi_;
};

// Block types of the chunk file. Raster layers get theirs from the metadata
// (16 and up, in the order they were first seen); these two are fixed.
inline constexpr std::uint16_t kVectorBlock = 1;
inline constexpr std::uint16_t kPoiBlock = 2;

} // namespace engine::world_source
