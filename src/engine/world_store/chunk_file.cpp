#include "engine/world_store/chunk_file.hpp"

#include <fstream>

#include "engine/core/binary.hpp"

namespace engine::world_store {
namespace {

constexpr std::size_t kHeaderBytes = 96;
constexpr std::size_t kHashedHeaderBytes = 88;   // everything before the header hash
constexpr std::size_t kEntryBytes = 40;

void fail(std::string* why, const char* what) {
    if (why) *why = what;
}

void writeEntry(core::BinaryWriter& out, const BlockEntry& e) {
    out.u16(e.type);
    out.u16(e.version);
    out.u8(static_cast<std::uint8_t>(e.codec));
    out.u8(0); out.u8(0); out.u8(0);
    out.u64(e.rawSize);
    out.u64(e.storedSize);
    out.u64(e.offset);
    out.u64(e.hash);
}

BlockEntry readEntry(core::BinaryReader& in) {
    BlockEntry e;
    e.type = in.u16();
    e.version = in.u16();
    e.codec = static_cast<Codec>(in.u8());
    in.u8(); in.u8(); in.u8();
    e.rawSize = in.u64();
    e.storedSize = in.u64();
    e.offset = in.u64();
    e.hash = in.u64();
    return e;
}

std::vector<std::uint8_t> headerBytes(const ChunkHeader& h, std::uint32_t blockCount) {
    core::BinaryWriter out;
    out.u32(kChunkMagic);
    out.u16(h.formatVersion);
    out.u8(static_cast<std::uint8_t>(h.kind));
    out.u8(static_cast<std::uint8_t>(h.key.level));
    out.u32(h.generatorVersion);
    out.u32(blockCount);
    out.u64(h.worldSeed);
    out.i64(h.key.x);
    out.i64(h.key.y);
    out.u64(h.layerMask);
    out.u64(h.sourceRevision);
    out.u64(h.deltaRevision);
    out.u64(h.payloadHash);
    out.u64(h.compressedSize);
    out.u64(h.uncompressedSize);
    return out.take();
}

std::uint64_t headerHashOf(std::span<const std::uint8_t> hashedHeader, std::span<const std::uint8_t> directory) {
    return contentHash(directory, contentHash(hashedHeader));
}

// The fixed part of a header, parsed and checked without the directory.
struct Front {
    ChunkHeader header;
    std::uint32_t blockCount = 0;
    std::uint64_t headerHash = 0;
};

std::optional<Front> readFront(std::span<const std::uint8_t> bytes, std::string* why) {
    if (bytes.size() < kHeaderBytes) { fail(why, "shorter than a chunk header"); return std::nullopt; }
    core::BinaryReader in(bytes.data(), kHeaderBytes);
    if (in.u32() != kChunkMagic) { fail(why, "not a chunk file"); return std::nullopt; }
    Front f;
    f.header.formatVersion = in.u16();
    if (f.header.formatVersion > kChunkFormatVersion) {
        fail(why, "chunk written by a newer format");
        return std::nullopt;
    }
    if (f.header.formatVersion == 0) { fail(why, "chunk format version 0"); return std::nullopt; }
    const auto kind = in.u8();
    const auto level = in.u8();
    if (kind < 1 || kind > 3) { fail(why, "unknown chunk kind"); return std::nullopt; }
    if (level >= kChunkLevels) { fail(why, "unknown chunk level"); return std::nullopt; }
    f.header.kind = static_cast<ChunkKind>(kind);
    f.header.key.level = static_cast<ChunkLevel>(level);
    f.header.generatorVersion = in.u32();
    f.blockCount = in.u32();
    if (f.blockCount > kMaxBlocks) { fail(why, "more blocks than a chunk may hold"); return std::nullopt; }
    f.header.worldSeed = in.u64();
    f.header.key.x = in.i64();
    f.header.key.y = in.i64();
    f.header.layerMask = in.u64();
    f.header.sourceRevision = in.u64();
    f.header.deltaRevision = in.u64();
    f.header.payloadHash = in.u64();
    f.header.compressedSize = in.u64();
    f.header.uncompressedSize = in.u64();
    f.headerHash = in.u64();
    if (!in.ok()) { fail(why, "truncated chunk header"); return std::nullopt; }
    return f;
}

// The directory, checked for self-consistency against the header and the file
// size. `fileBytes` is the whole file's size, not only what was read.
std::optional<ChunkDirectory> parseDirectory(const Front& front, std::span<const std::uint8_t> head,
                                             std::span<const std::uint8_t> directory,
                                             std::uint64_t fileBytes, std::string* why) {
    if (headerHashOf(head.first(kHashedHeaderBytes), directory) != front.headerHash) {
        fail(why, "chunk header or directory damaged");
        return std::nullopt;
    }
    ChunkDirectory d;
    d.header = front.header;
    d.blockArea = kHeaderBytes + std::uint64_t(front.blockCount) * kEntryBytes;
    if (fileBytes < d.blockArea) { fail(why, "truncated chunk directory"); return std::nullopt; }
    const std::uint64_t area = fileBytes - d.blockArea;
    core::BinaryReader in(directory.data(), directory.size());
    std::uint64_t stored = 0, raw = 0;
    d.blocks.reserve(front.blockCount);
    for (std::uint32_t i = 0; i < front.blockCount; ++i) {
        const auto e = readEntry(in);
        if (e.rawSize > kMaxBlockBytes || e.storedSize > area || e.offset > area - e.storedSize) {
            fail(why, "chunk block outside its file");
            return std::nullopt;
        }
        stored += e.storedSize;
        raw += e.rawSize;
        d.blocks.push_back(e);
    }
    if (!in.ok() || stored != front.header.compressedSize || raw != front.header.uncompressedSize ||
        d.blockArea + stored != fileBytes) {
        fail(why, "chunk sizes disagree with its directory");
        return std::nullopt;
    }
    return d;
}

std::optional<Block> decodeBlock(const BlockEntry& e, std::span<const std::uint8_t> stored, std::string* why) {
    auto raw = decode(e.codec, stored, e.rawSize);
    if (!raw) { fail(why, "chunk block does not decompress"); return std::nullopt; }
    if (contentHash(*raw) != e.hash) { fail(why, "chunk block damaged"); return std::nullopt; }
    return Block{e.type, e.version, std::move(*raw)};
}

} // namespace

const BlockEntry* ChunkDirectory::find(std::uint16_t type) const {
    for (const auto& b : blocks)
        if (b.type == type) return &b;
    return nullptr;
}

std::uint64_t payloadHashOf(std::span<const Block> blocks) {
    core::BinaryWriter summary;
    for (const auto& b : blocks) {
        summary.u16(b.type);
        summary.u16(b.version);
        summary.u64(b.bytes.size());
        summary.u64(contentHash(b.bytes));
    }
    return contentHash(summary.data());
}

std::vector<std::uint8_t> encodeChunk(ChunkHeader header, std::span<const Block> blocks, int level) {
    std::vector<BlockEntry> entries;
    std::vector<std::vector<std::uint8_t>> stored;
    entries.reserve(blocks.size());
    stored.reserve(blocks.size());
    header.layerMask = 0;
    header.compressedSize = header.uncompressedSize = 0;
    for (const auto& b : blocks) {
        auto packed = encode(b.bytes, level);
        BlockEntry e;
        e.type = b.type;
        e.version = b.version;
        e.codec = packed.codec;
        e.rawSize = b.bytes.size();
        e.storedSize = packed.bytes.size();
        e.offset = header.compressedSize;
        e.hash = contentHash(b.bytes);
        header.layerMask |= std::uint64_t(1) << (b.type % 64);
        header.compressedSize += e.storedSize;
        header.uncompressedSize += e.rawSize;
        entries.push_back(e);
        stored.push_back(std::move(packed.bytes));
    }
    header.payloadHash = payloadHashOf(blocks);

    core::BinaryWriter directory;
    for (const auto& e : entries) writeEntry(directory, e);
    auto file = headerBytes(header, std::uint32_t(blocks.size()));
    core::BinaryWriter hash;
    hash.u64(headerHashOf(file, directory.data()));
    file.insert(file.end(), hash.data().begin(), hash.data().end());
    file.insert(file.end(), directory.data().begin(), directory.data().end());
    for (const auto& s : stored) file.insert(file.end(), s.begin(), s.end());
    return file;
}

std::optional<ChunkDirectory> readDirectory(std::span<const std::uint8_t> file, std::string* why) {
    const auto front = readFront(file, why);
    if (!front) return std::nullopt;
    const std::uint64_t end = kHeaderBytes + std::uint64_t(front->blockCount) * kEntryBytes;
    if (file.size() < end) { fail(why, "truncated chunk directory"); return std::nullopt; }
    return parseDirectory(*front, file.first(kHeaderBytes), file.subspan(kHeaderBytes, end - kHeaderBytes),
                          file.size(), why);
}

std::optional<std::vector<Block>> decodeChunk(std::span<const std::uint8_t> file, ChunkHeader* header,
                                              std::string* why) {
    const auto directory = readDirectory(file, why);
    if (!directory) return std::nullopt;
    std::vector<Block> blocks;
    blocks.reserve(directory->blocks.size());
    for (const auto& e : directory->blocks) {
        auto block = decodeBlock(e, file.subspan(directory->blockArea + e.offset, e.storedSize), why);
        if (!block) return std::nullopt;
        blocks.push_back(std::move(*block));
    }
    if (payloadHashOf(blocks) != directory->header.payloadHash) {
        fail(why, "chunk payload does not match its header");
        return std::nullopt;
    }
    if (header) *header = directory->header;
    return blocks;
}

std::optional<ChunkFile> ChunkFile::open(const std::filesystem::path& path, std::string* why) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) { fail(why, "chunk file missing"); return std::nullopt; }
    std::ifstream in(path, std::ios::binary);
    if (!in) { fail(why, "chunk file unreadable"); return std::nullopt; }
    std::vector<std::uint8_t> head(kHeaderBytes);
    if (size < kHeaderBytes || !in.read(reinterpret_cast<char*>(head.data()), std::streamsize(head.size()))) {
        fail(why, "shorter than a chunk header");
        return std::nullopt;
    }
    const auto front = readFront(head, why);
    if (!front) return std::nullopt;
    std::vector<std::uint8_t> directory(std::size_t(front->blockCount) * kEntryBytes);
    if (!directory.empty() &&
        !in.read(reinterpret_cast<char*>(directory.data()), std::streamsize(directory.size()))) {
        fail(why, "truncated chunk directory");
        return std::nullopt;
    }
    auto parsed = parseDirectory(*front, head, directory, size, why);
    if (!parsed) return std::nullopt;
    ChunkFile file;
    file.path_ = path;
    file.directory_ = std::move(*parsed);
    file.fileBytes_ = size;
    return file;
}

std::optional<Block> ChunkFile::read(std::uint16_t type, std::string* why) const {
    const auto* e = directory_.find(type);
    if (!e) { fail(why, "no such block"); return std::nullopt; }
    std::ifstream in(path_, std::ios::binary);
    if (!in) { fail(why, "chunk file unreadable"); return std::nullopt; }
    std::vector<std::uint8_t> stored(static_cast<std::size_t>(e->storedSize));
    in.seekg(std::streamoff(directory_.blockArea + e->offset));
    if (!stored.empty() && !in.read(reinterpret_cast<char*>(stored.data()), std::streamsize(stored.size()))) {
        fail(why, "truncated chunk block");
        return std::nullopt;
    }
    return decodeBlock(*e, stored, why);
}

} // namespace engine::world_store


