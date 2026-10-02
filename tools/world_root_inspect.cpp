// world_root_inspect - what a world root on disk holds, chunk by chunk.
//
// The chunk/debug view of the spec (§19) for the part of it that lives on
// disk: which chunks have history, at which revision, in what file, how big,
// made of which blocks, how much journal is waiting to be folded, and whether
// every file still says what its manifest says it does. Reads only; never
// writes, never sweeps.
//
//   world_root_inspect worlds/world            a root directory
//   world_root_inspect worlds/world.json       the root that belongs to a layout
//   world_root_inspect ROOT --ops              every journal record, too
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>

#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "engine/world_store/world_root.hpp"
#include "game/world/world_delta_codec.hpp"

namespace ws = engine::world_store;
namespace wd = world::delta;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: world_root_inspect ROOT|LAYOUT.json [--ops]\n";
        return 2;
    }
    std::filesystem::path path = argv[1];
    const bool ops = argc > 2 && std::string(argv[2]) == "--ops";
    if (path.extension() == ".json") path = ws::WorldRoot::forLayout(path);
    const ws::WorldRoot root(path);
    std::string why;
    const auto manifest = root.readManifest(&why);
    if (!manifest) {
        std::cout << root.directory().string() << ": " << (why.empty() ? "no delta yet (a new world)" : why) << "\n";
        return why.empty() ? 0 : 1;
    }
    std::cout << "root      " << root.directory().string() << "\n"
              << "seed      " << manifest->worldSeed << "\n"
              << "source    " << manifest->source << " (hash " << manifest->sourceHash << ")\n"
              << "commit    " << manifest->commit << ", next record " << manifest->nextSequence << "\n";
    for (const auto& [name, version] : manifest->stableDomains)
        std::cout << "lattice   " << name << " " << version << "\n";
    std::cout << "chunks    " << manifest->delta.size() << " of "
              << ws::chunkMetres(ws::ChunkLevel::AuthoringChunk) << " m\n\n";

    int bad = 0;
    std::uint64_t total = 0;
    std::map<std::string, std::size_t> opCounts;
    for (const auto& entry : manifest->delta) {
        std::printf("%-12s r%-6llu %-26s", entry.key.stem().c_str(), (unsigned long long)entry.revision,
                    entry.file.c_str());
        const auto bytes = ws::readFileBytes(root.deltaFile(entry.file), &why);
        ws::ChunkHeader header;
        const auto blocks = bytes ? ws::decodeChunk(*bytes, &header, &why) : std::nullopt;
        if (!blocks) {
            std::printf("  UNREADABLE: %s\n", why.c_str());
            ++bad;
            continue;
        }
        if (header.key != entry.key || header.deltaRevision != entry.revision || header.payloadHash != entry.payloadHash) {
            std::printf("  NOT THE CHUNK THE MANIFEST NAMES\n");
            ++bad;
            continue;
        }
        total += bytes->size();
        std::printf(" %7zu B  (%llu raw)\n", bytes->size(), (unsigned long long)header.uncompressedSize);
        const auto directory = ws::readDirectory(*bytes);
        for (const auto& b : directory->blocks) {
            std::printf("    %-8s v%u %-4s %8llu -> %8llu B", wd::codec::blockName(b.type), unsigned(b.version),
                        b.codec == ws::Codec::Zstd ? "zstd" : "raw", (unsigned long long)b.rawSize,
                        (unsigned long long)b.storedSize);
            if (b.type == wd::codec::Journal) {
                for (const auto& block : *blocks)
                    if (block.type == wd::codec::Journal)
                        if (const auto journal = wd::codec::decodeJournal(entry.key, block, &why)) {
                            std::printf("   %zu records", journal->size());
                            if (!journal->empty())
                                std::printf(", #%llu..#%llu", (unsigned long long)journal->front().sequence,
                                            (unsigned long long)journal->back().sequence);
                            for (const auto& op : *journal) ++opCounts[wd::opName(op)];
                            if (ops)
                                for (const auto& op : *journal)
                                    std::printf("\n      #%-8llu %-10s %s", (unsigned long long)op.sequence,
                                                wd::originName(op.origin), wd::opName(op));
                        }
            }
            std::printf("\n");
        }
    }
    std::cout << "\n" << total << " bytes of history";
    if (!opCounts.empty()) {
        std::cout << "; waiting in journals:";
        for (const auto& [name, count] : opCounts) std::cout << " " << count << " " << name;
    }
    std::cout << "\n";
    if (bad) std::cout << bad << " chunk(s) unreadable\n";
    return bad ? 1 : 0;
}


