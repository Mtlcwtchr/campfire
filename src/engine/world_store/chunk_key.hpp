#pragma once
// Where a piece of the world is kept.
//
// A world thousands of kilometres across cannot be one file: one file cannot
// be streamed in parts, saved in parts, generated in parallel or survive one
// damaged sector. So the world is cut into chunks, and the cut is a hierarchy
// rather than one size, because the things that want a unit do not want the
// same one. A file wants to be big enough that a thousand of them cover a
// continent; a worker job wants to be small enough to finish inside a frame's
// patience; a dug hole wants to cost a few kilobytes and not a file rewrite.
//
//   level            side       what it is for
//   RuntimePatch     64 m       a local edit: a plot flattened, a grove cut
//   GenerationTile   512 m      one compiler job; one terrain page
//   AuthoringChunk   8 192 m    one file: the unit of disk I/O and of editing
//   SourceChunk      32 768 m   one WorldSource file: 128 x 128 authored samples
//                               at 256 m (doc/world_authoring_import_export_spec)
//   Region           131 072 m  index and ownership; one WorldLayout region
//
// Each side is a power of two and an exact multiple of the one below, so a
// finer chunk never straddles a coarser one and "which file holds this patch"
// is a shift, not a search. They are also the sizes the rest of the engine
// already cuts the world at - the 64 m chunk, the 512 m page, the 131 km
// region - so no second grid lies across the first.
//
// A key is world-space and signed. Nothing about it is relative to a loaded
// window or to the origin of a file, and negative coordinates floor rather
// than truncate: the chunk left of the origin is -1, not a second chunk 0.
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace engine::world_store {

enum class ChunkLevel : std::uint8_t {
    RuntimePatch = 0,
    GenerationTile = 1,
    AuthoringChunk = 2,
    SourceChunk = 3,
    Region = 4,
};
inline constexpr std::size_t kChunkLevels = 5;
inline constexpr std::array<std::int64_t, kChunkLevels> kChunkMetres{64, 512, 8192, 32768, 131072};

constexpr std::int64_t chunkMetres(ChunkLevel level) {
    return kChunkMetres[static_cast<std::size_t>(level)];
}
const char* levelName(ChunkLevel level);

// Floor division and its remainder, which is what a signed grid needs: -1 / 64
// is chunk -1, remainder 63.
constexpr std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    const std::int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
constexpr std::int64_t floorMod(std::int64_t a, std::int64_t b) {
    const std::int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? r + b : r;
}

// Half-open rectangle of world metres.
struct MetreRect {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    [[nodiscard]] bool empty() const { return !(maxX > minX) || !(maxY > minY); }
    [[nodiscard]] bool overlaps(const MetreRect& o) const {
        return minX < o.maxX && o.minX < maxX && minY < o.maxY && o.minY < maxY;
    }
    [[nodiscard]] MetreRect grown(double by) const { return {minX - by, minY - by, maxX + by, maxY + by}; }
    [[nodiscard]] MetreRect merged(const MetreRect& o) const;
    bool operator==(const MetreRect&) const = default;
};

struct ChunkKey {
    ChunkLevel level = ChunkLevel::AuthoringChunk;
    std::int64_t x = 0, y = 0;

    [[nodiscard]] constexpr std::int64_t metres() const { return chunkMetres(level); }
    [[nodiscard]] constexpr std::int64_t minX() const { return x * metres(); }
    [[nodiscard]] constexpr std::int64_t minY() const { return y * metres(); }
    [[nodiscard]] constexpr std::int64_t maxX() const { return (x + 1) * metres(); }
    [[nodiscard]] constexpr std::int64_t maxY() const { return (y + 1) * metres(); }
    [[nodiscard]] MetreRect bounds() const {
        return {double(minX()), double(minY()), double(maxX()), double(maxY())};
    }
    // The chunk of a coarser (or the same) level this one lies in.
    [[nodiscard]] ChunkKey within(ChunkLevel coarser) const;
    // Whether `finer` lies inside this chunk.
    [[nodiscard]] bool contains(const ChunkKey& finer) const;
    [[nodiscard]] bool containsMetres(double x, double y) const;

    // "x_y", with a minus sign where one is due: what a chunk file is named.
    [[nodiscard]] std::string stem() const;
    [[nodiscard]] static std::optional<ChunkKey> parse(ChunkLevel level, std::string_view stem);

    auto operator<=>(const ChunkKey&) const = default;
};

struct ChunkKeyHash {
    std::size_t operator()(const ChunkKey& k) const noexcept;
};

// The chunk of a level holding a point. A point on a border belongs to the
// chunk on its high side, as everywhere else in the engine (half-open).
ChunkKey chunkAt(ChunkLevel level, std::int64_t xMetres, std::int64_t yMetres);
ChunkKey chunkAt(ChunkLevel level, double xMetres, double yMetres);
// Every chunk of a level a rectangle touches, row by row. Empty for an empty
// rectangle; a rectangle is half-open, so one ending exactly on a border does
// not reach into the next chunk.
std::vector<ChunkKey> chunksOverlapping(ChunkLevel level, const MetreRect& area);

} // namespace engine::world_store


