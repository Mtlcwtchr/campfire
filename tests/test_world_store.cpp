#include "framework.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>

#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "engine/world_store/chunk_key.hpp"
#include "engine/world_store/codec.hpp"
#include "engine/world_store/world_root.hpp"

namespace {
using namespace engine::world_store;

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("campfire_store_" + name + "_" + std::to_string(rd()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

std::vector<std::uint8_t> bytesOf(std::size_t n, std::uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::uint8_t> out(n);
    for (auto& b : out) b = std::uint8_t(rng());
    return out;
}

std::vector<Block> someBlocks() {
    std::vector<Block> blocks;
    blocks.push_back({1, 1, std::vector<std::uint8_t>(20000, 0)});     // compresses
    blocks.push_back({2, 3, bytesOf(3000, 7)});                        // does not
    blocks.push_back({900, 1, {1, 2, 3, 4, 5}});                       // a type nobody here knows
    return blocks;
}

ChunkHeader someHeader() {
    ChunkHeader h;
    h.kind = ChunkKind::Delta;
    h.generatorVersion = 4;
    h.worldSeed = 0xfeedfacecafebeefULL;
    h.key = {ChunkLevel::AuthoringChunk, -3, 7};
    h.sourceRevision = 11;
    h.deltaRevision = 12;
    return h;
}

std::size_t temporariesIn(const std::filesystem::path& dir) {
    std::size_t n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir))
        if (e.path().filename().string().ends_with(".tmp")) ++n;
    return n;
}
} // namespace

TEST(world_chunk_keys_floor_negative_coordinates_and_nest_exactly) {
    CHECK((chunkAt(ChunkLevel::AuthoringChunk, std::int64_t(-1), std::int64_t(-1)) ==
           ChunkKey{ChunkLevel::AuthoringChunk, -1, -1}));
    CHECK((chunkAt(ChunkLevel::AuthoringChunk, std::int64_t(8191), std::int64_t(8192)) ==
           ChunkKey{ChunkLevel::AuthoringChunk, 0, 1}));
    CHECK((chunkAt(ChunkLevel::RuntimePatch, -0.5, 63.9) == ChunkKey{ChunkLevel::RuntimePatch, -1, 0}));
    // Every level is an exact multiple of the one below: a patch never straddles a file.
    for (std::size_t i = 1; i < kChunkLevels; ++i) CHECK_EQ(kChunkMetres[i] % kChunkMetres[i - 1], std::int64_t(0));
    const ChunkKey patch{ChunkLevel::RuntimePatch, -1, 128};
    CHECK((patch.within(ChunkLevel::AuthoringChunk) == ChunkKey{ChunkLevel::AuthoringChunk, -1, 1}));
    CHECK((patch.within(ChunkLevel::Region) == ChunkKey{ChunkLevel::Region, -1, 0}));
    CHECK((ChunkKey{ChunkLevel::AuthoringChunk, -1, 1}.contains(patch)));
    CHECK(!(ChunkKey{ChunkLevel::AuthoringChunk, 0, 1}.contains(patch)));
    for (const ChunkKey key : {ChunkKey{ChunkLevel::AuthoringChunk, -3, -7}, ChunkKey{ChunkLevel::AuthoringChunk, 0, 12},
                               ChunkKey{ChunkLevel::AuthoringChunk, 4000000000LL, -1}}) {
        const auto parsed = ChunkKey::parse(key.level, key.stem());
        CHECK(parsed.has_value());
        CHECK(parsed && *parsed == key);
    }
    for (const char* junk : {"", "_", "1_", "_1", "a_b", "1_2_3", "1-2", "--1_2"})
        CHECK(!ChunkKey::parse(ChunkLevel::AuthoringChunk, junk).has_value());
}

TEST(world_chunks_overlapping_a_rectangle_are_half_open) {
    CHECK_EQ(chunksOverlapping(ChunkLevel::AuthoringChunk, {0, 0, 8192, 8192}).size(), std::size_t(1));
    CHECK_EQ(chunksOverlapping(ChunkLevel::AuthoringChunk, {0, 0, 8192.5, 8192}).size(), std::size_t(2));
    CHECK_EQ(chunksOverlapping(ChunkLevel::AuthoringChunk, {-1, -1, 1, 1}).size(), std::size_t(4));
    CHECK(chunksOverlapping(ChunkLevel::AuthoringChunk, {5, 5, 5, 9}).empty());
    const auto keys = chunksOverlapping(ChunkLevel::GenerationTile, {-600, 0, 600, 100});
    CHECK_EQ(keys.size(), std::size_t(4));
    CHECK((keys.front() == ChunkKey{ChunkLevel::GenerationTile, -2, 0}));
    CHECK((keys.back() == ChunkKey{ChunkLevel::GenerationTile, 1, 0}));
}

TEST(world_store_codec_round_trips_and_keeps_incompressible_blocks_raw) {
    const std::vector<std::uint8_t> zeros(65536, 0), noise = bytesOf(4096, 3);
    const auto packed = encode(zeros);
    CHECK(packed.codec == Codec::Zstd);
    CHECK(packed.bytes.size() < zeros.size() / 20);
    const auto raw = encode(noise);
    CHECK(raw.codec == Codec::Raw);
    CHECK_EQ(raw.bytes.size(), noise.size());
    CHECK(decode(packed.codec, packed.bytes, zeros.size()) == zeros);
    CHECK(decode(raw.codec, raw.bytes, noise.size()) == noise);
    // A size that disagrees with the frame, a damaged frame, an absurd size.
    CHECK(!decode(packed.codec, packed.bytes, zeros.size() + 1).has_value());
    auto broken = packed.bytes;
    broken[broken.size() / 2] ^= 0x5a;
    const auto damaged = decode(Codec::Zstd, broken, zeros.size());
    CHECK(!damaged || contentHash(*damaged) != contentHash(zeros));
    CHECK(!decode(Codec::Raw, noise, kMaxBlockBytes + 1).has_value());
    // The hash sees every byte and the length.
    auto flipped = noise;
    flipped[1234] ^= 1;
    CHECK(contentHash(noise) != contentHash(flipped));
    CHECK(contentHash(std::vector<std::uint8_t>{'a'}) != contentHash(std::vector<std::uint8_t>{'a', 0}));
    CHECK_EQ(contentHash(noise), contentHash(bytesOf(4096, 3)));
}

TEST(world_chunk_files_round_trip_and_name_their_content_not_their_compression) {
    const auto blocks = someBlocks();
    const auto fast = encodeChunk(someHeader(), blocks, 1);
    const auto small = encodeChunk(someHeader(), blocks, 19);
    ChunkHeader header;
    std::string why;
    const auto decoded = decodeChunk(fast, &header, &why);
    CHECK(decoded.has_value());
    CHECK(decoded && *decoded == blocks);
    CHECK((header.key == someHeader().key));
    CHECK_EQ(header.worldSeed, someHeader().worldSeed);
    CHECK_EQ(header.deltaRevision, std::uint64_t(12));
    CHECK_EQ(header.payloadHash, payloadHashOf(blocks));
    CHECK_EQ(header.uncompressedSize, std::uint64_t(20000 + 3000 + 5));
    CHECK(header.compressedSize < header.uncompressedSize);
    CHECK(header.layerMask & (1u << 1));
    CHECK(header.layerMask & (1u << 2));
    ChunkHeader other;
    CHECK(decodeChunk(small, &other).has_value());
    CHECK_EQ(other.payloadHash, header.payloadHash);
}

TEST(world_chunk_files_are_read_a_block_at_a_time) {
    TempDir dir("blocks");
    const auto path = dir.path / "0_0.r1.wdelta";
    const auto blocks = someBlocks();
    CHECK(writeFileAtomic(path, encodeChunk(someHeader(), blocks)));
    std::string why;
    const auto file = ChunkFile::open(path, &why);
    CHECK(file.has_value());
    if (!file) return;
    CHECK(file->has(2));
    CHECK(!file->has(3));
    CHECK_EQ(file->blocks().size(), std::size_t(3));
    const auto second = file->read(2, &why);
    CHECK(second && *second == blocks[1]);
    CHECK(!file->read(3).has_value());
    CHECK((file->header().key == someHeader().key));
}

TEST(world_chunk_files_refuse_damage_truncation_and_newer_formats) {
    const auto good = encodeChunk(someHeader(), someBlocks());
    std::string why;
    // A block's bytes.
    auto bytes = good;
    bytes[bytes.size() - 2] ^= 0x10;
    CHECK(!decodeChunk(bytes, nullptr, &why).has_value());
    // The header: the seed, which the header hash covers.
    bytes = good;
    bytes[20] ^= 0x01;
    CHECK(!readDirectory(bytes, &why).has_value());
    CHECK(why.find("damaged") != std::string::npos);
    // Truncated anywhere.
    for (const std::size_t keep : {std::size_t(10), std::size_t(96), std::size_t(120), good.size() - 1}) {
        bytes.assign(good.begin(), good.begin() + std::ptrdiff_t(keep));
        CHECK(!decodeChunk(bytes, nullptr, &why).has_value());
    }
    // A newer format is refused before anything else is believed.
    bytes = good;
    bytes[4] = 2;
    CHECK(!readDirectory(bytes, &why).has_value());
    CHECK(why.find("newer") != std::string::npos);
    // Not a chunk at all.
    CHECK(!readDirectory(std::vector<std::uint8_t>(200, 7), &why).has_value());
}

TEST(world_atomic_writes_replace_whole_files_and_leave_no_temporaries) {
    TempDir dir("atomic");
    const auto path = dir.path / "deep" / "file.bin";
    CHECK(writeFileAtomic(path, std::string_view("first version, longer than the second")));
    CHECK(writeFileAtomic(path, std::string_view("second")));
    const auto read = readFileBytes(path);
    CHECK(read && std::string(read->begin(), read->end()) == "second");
    CHECK_EQ(temporariesIn(dir.path), std::size_t(0));
    CHECK(!readFileBytes(dir.path / "missing").has_value());
}

TEST(world_root_manifest_round_trips_and_sweeps_what_it_does_not_name) {
    TempDir dir("root");
    const WorldRoot root(dir.path / "world");
    CHECK((WorldRoot::forLayout(dir.path / "worlds" / "world.json") == dir.path / "worlds" / "world"));
    std::string why = "stale";
    CHECK(!root.readManifest(&why).has_value());
    CHECK(why.empty());   // no manifest is a new world, not an error

    Manifest m;
    m.worldSeed = 0xffffffffffffffffULL;   // the full range survives JSON
    m.source = "../world.json";
    m.sourceHash = 99;
    m.stableDomains = {{"decor", 1}};
    m.commit = 3;
    m.nextSequence = 41;
    m.delta = {{{ChunkLevel::AuthoringChunk, 1, -2}, 5, WorldRoot::deltaFileName({ChunkLevel::AuthoringChunk, 1, -2}, 5), 77, 1000},
               {{ChunkLevel::AuthoringChunk, -1, 0}, 2, WorldRoot::deltaFileName({ChunkLevel::AuthoringChunk, -1, 0}, 2), 78, 900}};
    CHECK(root.writeManifest(m, &why));
    const auto back = root.readManifest(&why);
    CHECK(back.has_value());
    auto sorted = m;
    std::sort(sorted.delta.begin(), sorted.delta.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    CHECK(back && *back == sorted);

    // Leftovers of a save that died before its commit, of one that finished,
    // and of a manifest write: all go. The committed files and a kept-aside
    // damaged file stay.
    const auto touch = [&](const std::string& name) {
        std::filesystem::create_directories(root.deltaDirectory());
        std::ofstream(root.deltaFile(name)) << "x";
    };
    touch(m.delta[0].file);
    touch(m.delta[1].file);
    touch("1_-2.r4.wdelta");
    touch("1_-2.r6.wdelta.123.0.tmp");
    touch("1_-2.r3.wdelta.damaged");
    std::ofstream(root.directory() / "manifest.world.9.9.tmp") << "x";
    CHECK_EQ(root.sweep(m), std::size_t(3));
    CHECK(std::filesystem::exists(root.deltaFile(m.delta[0].file)));
    CHECK(std::filesystem::exists(root.deltaFile(m.delta[1].file)));
    CHECK(std::filesystem::exists(root.deltaFile("1_-2.r3.wdelta.damaged")));
    CHECK(!std::filesystem::exists(root.deltaFile("1_-2.r4.wdelta")));
}

TEST(world_root_refuses_manifests_it_cannot_trust) {
    TempDir dir("manifest");
    const WorldRoot root(dir.path);
    std::string why;
    const auto write = [&](const std::string& text) { std::ofstream(root.manifestFile()) << text; };
    write("{\"format\": 2, \"world_seed\": 1, \"commit\": 0, \"next_sequence\": 1, \"delta\": []}");
    CHECK(!root.readManifest(&why).has_value());
    CHECK(why.find("newer") != std::string::npos);
    write("{\"format\": 1, \"world_seed\": 1, \"commit\": 0, \"next_sequence\": 1, \"delta\": "
          "[{\"x\":0,\"y\":0,\"revision\":1,\"file\":\"../../etc/passwd\",\"hash\":0}]}");
    CHECK(!root.readManifest(&why).has_value());
    write("{\"format\": 1, \"world_seed\": 1, \"commit\": 0, \"next_sequence\": 1, \"delta\": "
          "[{\"x\":0,\"y\":0,\"revision\":1,\"file\":\"a.wdelta\",\"hash\":0},"
          " {\"x\":0,\"y\":0,\"revision\":2,\"file\":\"b.wdelta\",\"hash\":0}]}");
    CHECK(!root.readManifest(&why).has_value());
    write("{ not json");
    CHECK(!root.readManifest(&why).has_value());
    CHECK(!why.empty());
}


