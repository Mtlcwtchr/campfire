#pragma once
// What the persistent delta is made of, and how it is written down.
//
// These are the records of history (spec §12-§14). Each is world-space and
// self-contained: an op says what it did in terms that do not depend on the
// generator, on a loaded window or on what else was in memory, so replaying
// it on any machine, after any regeneration of the base, does the same thing.
// A terrain op, in particular, is stored RESOLVED - the exact amount added to
// each sample - and not as the brush that made it: a brush reads the ground
// under it, and the ground under it is the generator's, which is allowed to
// change between versions while the history is not.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "engine/world_store/chunk_key.hpp"
#include "game/world/ecology.hpp"
#include "game/world/edit_layer.hpp"
#include "game/world/terrain_brush.hpp"

namespace world::delta {

using engine::world_store::Block;
using engine::world_store::ChunkKey;
using engine::world_store::ChunkLevel;
using engine::world_store::MetreRect;

// One delta file per authoring chunk; height kept in runtime patches inside it.
inline constexpr ChunkLevel kFileLevel = ChunkLevel::AuthoringChunk;
inline constexpr std::int64_t kFileMetres = engine::world_store::chunkMetres(kFileLevel);
inline constexpr std::int64_t kPatchMetres = engine::world_store::chunkMetres(ChunkLevel::RuntimePatch);
static_assert(kFileMetres % EditLayer::kBlockMetres == 0, "an edit block never straddles two files");
static_assert(kFileMetres % EditLayer::blockMetresOf(EditLayer::kLevels - 1) == 0 &&
              kFileMetres % EditLayer::blockMetresOf(1) == 0 && kFileMetres % EditLayer::blockMetresOf(2) == 0,
              "nor does a coarse one");
static_assert(EditLayer::kBlockMetres % kPatchMetres == 0 && kPatchMetres % EditLayer::kSampleMetres == 0);
static_assert(kFileMetres % ecology::kPageMetres == 0 && kFileMetres % ecology::kCellMetres == 0);

// Who made a change. Provenance, not permission: before the game and during it
// the records are the same (§12), and this says which history one belongs to.
enum class Origin : std::uint8_t { Authoring = 1, Gameplay = 2, Simulation = 3 };
const char* originName(Origin origin);

struct TerrainSample {
    std::int64_t x = 0, y = 0;   // sample coordinates, EditLayer::stepOf(the op's level) apart
    core::Fixed add;
    bool operator==(const TerrainSample&) const = default;
};
struct TerrainOp {
    // How it was made, for tracing a result back to the tool (§21). Not what
    // it did: that is the samples.
    BrushKind tool = BrushKind::Raise;
    double centreX = 0, centreY = 0, radius = 0, strength = 0, seconds = 0;
    // Which of the edit layer's levels the samples are at (EditLayer::levelFor):
    // nought, four metres, for everything written before there were levels.
    std::uint8_t level = 0;
    std::vector<TerrainSample> samples;
    bool operator==(const TerrainOp&) const = default;
};
struct RemoveOp {
    std::uint64_t id = 0;
    double x = 0, y = 0;
    bool operator==(const RemoveOp&) const = default;
};
struct PlantOp {
    std::uint64_t id = 0;
    ecology::Added object;
    bool operator==(const PlantOp&) const = default;
};
struct ClearOp {
    double x = 0, y = 0;
    std::uint32_t models = 0;
    bool operator==(const ClearOp&) const = default;
};
struct EcologyOp {
    double x = 0, y = 0;
    ecology::Cell cell;
    bool operator==(const EcologyOp&) const = default;
};
// A removed generated object standing again: the inverse of a RemoveOp, which
// is what undo records. Appended last, so every older journal reads unchanged.
struct RestoreOp {
    std::uint64_t id = 0;
    double x = 0, y = 0;
    bool operator==(const RestoreOp&) const = default;
};

struct Op {
    // World-wide and increasing: the order history happened in. One edit that
    // spans two files is two records with the same number.
    std::uint64_t sequence = 0;
    Origin origin = Origin::Gameplay;
    std::variant<TerrainOp, RemoveOp, PlantOp, ClearOp, EcologyOp, RestoreOp> what;
    bool operator==(const Op&) const = default;
};
const char* opName(const Op& op);

namespace codec {

enum BlockType : std::uint16_t {
    Heights = 1,    // 64 m patches of height difference, 16 x 16 samples
    Removed = 2,    // generated objects taken away, by stable id
    Planted = 3,    // objects people put there
    Cleared = 4,    // pages emptied of whole kinds of object
    Cells = 5,      // ecology cells set by hand or by simulation
    CoarseHeights = 6,   // whole edit blocks of the coarse levels, 16 m to 256 m a sample
    Journal = 16,   // ops since the snapshot above was taken
};
const char* blockName(std::uint16_t type);

// A chunk's share of the object/ecology delta. Filled by `partition`.
struct ChunkObjects {
    std::vector<std::pair<std::uint64_t, ecology::Key>> removed;
    std::vector<std::pair<ecology::Key, ecology::Cell>> cells;
    std::vector<std::pair<ecology::Key, const std::map<std::uint64_t, ecology::Added>*>> planted;
    std::vector<std::pair<ecology::Key, std::uint32_t>> cleared;
};
// Which file a thing in the object delta belongs to.
ChunkKey chunkOfCell(const ecology::Key& cell64);
ChunkKey chunkOfPage(const ecology::Key& page128);
ChunkKey chunkOfPoint(double x, double y);
ChunkKey chunkOfSample(std::int64_t sx, std::int64_t sy);
ChunkKey chunkOfEditBlock(std::int64_t bx, std::int64_t by);
// The same at a level of the edit layer, in that level's own coordinates.
ChunkKey chunkOfSample(int level, std::int64_t sx, std::int64_t sy);
ChunkKey chunkOfEditBlock(int level, std::int64_t bx, std::int64_t by);

// The object delta cut up by file, for the chunks asked for only. `delta` must
// outlive the result: planted objects are pointed at, not copied.
std::map<ChunkKey, ChunkObjects> partition(const ecology::Delta& delta, const std::vector<ChunkKey>& wanted);

// A chunk's state as snapshot blocks. Empty blocks are left out, so a chunk
// with nothing in it is no blocks at all.
std::vector<Block> encodeSnapshot(const ChunkKey& chunk, const ChunkObjects& objects,
                                  const std::vector<EditLayer::BlockCopy>& heights);
Block encodeJournal(const ChunkKey& chunk, const std::vector<Op>& ops);

// Applied to a delta being built and to an edit layer. Blocks of other types
// are ignored; a block that does not parse, or names something outside its
// chunk, fails the whole chunk.
bool decodeSnapshot(const ChunkKey& chunk, const std::vector<Block>& blocks, ecology::Delta& objects,
                    EditLayer& heights, std::string* why);
std::optional<std::vector<Op>> decodeJournal(const ChunkKey& chunk, const Block& block, std::string* why);

// The same change the live path makes, on a delta being built: what replaying
// a journal does. Returns whether anything changed.
bool applyTo(const Op& op, ecology::Delta& objects, EditLayer& heights);

} // namespace codec
} // namespace world::delta


