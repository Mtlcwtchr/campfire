#pragma once
// One chunk file: a header, a directory of typed blocks, and the blocks.
//
//   ChunkHeader   what chunk this is, of which world, at which revisions, and
//                 a hash over everything the blocks say
//   directory     one entry per block: type, version, codec, sizes, offset,
//                 hash of the raw bytes
//   blocks        each compressed on its own
//
// Blocks are independent so a reader that wants one layer reads one layer: the
// directory is at the front, and a block is found by seeking, not by
// decompressing everything before it. A block type this build does not know is
// skipped, not refused - a newer writer may add a layer an older reader has no
// use for - but a newer FORMAT is refused, because then the directory itself
// may mean something else.
//
// The file says what it is about. A chunk file copied into another world, or
// under another chunk's name, or left half-written, is noticed when it is
// opened: the header is hashed together with the directory, every block with
// its own contents, and the key and seed are checked against what was asked
// for by whoever opens it.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/world_store/chunk_key.hpp"
#include "engine/world_store/codec.hpp"

namespace engine::world_store {

inline constexpr std::uint32_t kChunkMagic = 0x4B435743u;   // "CWCK" on disk
inline constexpr std::uint16_t kChunkFormatVersion = 1;
inline constexpr std::uint32_t kMaxBlocks = 4096;

enum class ChunkKind : std::uint8_t {
    Source = 1,   // authored data
    Delta = 2,    // persistent history of the world: edits over the generated base
    Cache = 3,    // derived, and safe to delete
};

struct ChunkHeader {
    std::uint16_t formatVersion = kChunkFormatVersion;
    ChunkKind kind = ChunkKind::Delta;
    // The generator version whatever is inside was made against. For a delta
    // this is the stable-id domain its removed objects are named in.
    std::uint32_t generatorVersion = 0;
    std::uint64_t worldSeed = 0;
    ChunkKey key;
    // One bit per block type present (type modulo 64), so "does this chunk
    // hold heights" is answered from the header.
    std::uint64_t layerMask = 0;
    std::uint64_t sourceRevision = 0;
    std::uint64_t deltaRevision = 0;
    // Over the raw blocks: types, versions and contents. Set by the encoder.
    std::uint64_t payloadHash = 0;
    std::uint64_t compressedSize = 0;
    std::uint64_t uncompressedSize = 0;
    bool operator==(const ChunkHeader&) const = default;
};

struct Block {
    std::uint16_t type = 0;
    std::uint16_t version = 1;
    std::vector<std::uint8_t> bytes;   // raw, as the owner of the type wrote it
    bool operator==(const Block&) const = default;
};

struct BlockEntry {
    std::uint16_t type = 0;
    std::uint16_t version = 1;
    Codec codec = Codec::Raw;
    std::uint64_t rawSize = 0, storedSize = 0;
    std::uint64_t offset = 0;   // from the start of the block area
    std::uint64_t hash = 0;     // of the raw bytes
};

// The file, whole. `header.payloadHash`, the sizes and the layer mask are
// filled in from the blocks; everything else is written as given.
std::vector<std::uint8_t> encodeChunk(ChunkHeader header, std::span<const Block> blocks, int level = 3);

// What a chunk's blocks say, as one number. The same blocks give the same hash
// however they were compressed.
std::uint64_t payloadHashOf(std::span<const Block> blocks);

struct ChunkDirectory {
    ChunkHeader header;
    std::vector<BlockEntry> blocks;
    std::uint64_t blockArea = 0;   // byte offset of the first block in the file
    [[nodiscard]] const BlockEntry* find(std::uint16_t type) const;
};

// Header and directory from the front of a file. Nothing when they are damaged,
// from a newer format, or not a chunk file at all; `why` says which.
std::optional<ChunkDirectory> readDirectory(std::span<const std::uint8_t> file, std::string* why = nullptr);
// Every block, each checked against its hash, and the payload hash checked
// against the header: a file that decodes is the file that was written.
std::optional<std::vector<Block>> decodeChunk(std::span<const std::uint8_t> file, ChunkHeader* header = nullptr,
                                              std::string* why = nullptr);

// A chunk file on disk, read a block at a time.
class ChunkFile {
public:
    static std::optional<ChunkFile> open(const std::filesystem::path& path, std::string* why = nullptr);

    [[nodiscard]] const ChunkHeader& header() const { return directory_.header; }
    [[nodiscard]] const std::vector<BlockEntry>& blocks() const { return directory_.blocks; }
    [[nodiscard]] bool has(std::uint16_t type) const { return directory_.find(type) != nullptr; }
    // Only this block's bytes are read from disk, and checked against its hash.
    [[nodiscard]] std::optional<Block> read(std::uint16_t type, std::string* why = nullptr) const;
    [[nodiscard]] std::uint64_t fileBytes() const { return fileBytes_; }

private:
    std::filesystem::path path_;
    ChunkDirectory directory_;
    std::uint64_t fileBytes_ = 0;
};

} // namespace engine::world_store


