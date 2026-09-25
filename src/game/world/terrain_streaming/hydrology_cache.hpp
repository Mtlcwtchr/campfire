#pragma once
// The persistent river/lake graph on disk.
//
// The graph is global, built once from the finished macro map, and read by
// every terrain worker for the life of a world. Rebuilding it costs 15 ms on
// the largest world, which is cheap - but it costs it in a place where nothing
// else has to happen, and it needs the whole `WorldMapData` in memory to do
// it. A saved world should carry its hydrology the way it carries its cells.
//
// One file holds the graph and its macro-page index together. The index is
// only useful with the graph it points into, and separating them would let a
// loader hold half of each.
//
// Everything here is worker-owned, like the tile cache: the main thread asks
// for a ready graph, it does not open files.

#include <cstdint>
#include <filesystem>
#include <vector>

#include "game/world/terrain_streaming/hydrology_graph.hpp"
#include "game/world/terrain_streaming/tile_cache.hpp"

namespace world::streaming {

inline constexpr std::uint16_t kHydrologyCacheFormatVersion = 2;
inline constexpr std::uint16_t kHydrologyCacheHeaderBytes = 96;

// What the file says it is, before a byte of it is believed. `worldSeed` and
// `sourceFingerprint` are the graph's own; a reader that expects a different
// pair has a file from another world and takes a miss.
struct HydrologyCacheHeader {
    std::uint64_t worldSeed = 0;
    std::uint64_t sourceFingerprint = 0;
    std::uint32_t graphVersion = 0;
    std::int32_t macroWidth = 0;
    std::int32_t macroHeight = 0;
    std::int32_t macroCellMetres = 0;
    std::uint32_t nodeCount = 0;
    std::uint32_t segmentCount = 0;
    std::uint32_t waterBodyCount = 0;
    std::uint32_t spatialPageCount = 0;
    std::uint64_t payloadBytes = 0;
    std::uint64_t payloadChecksum = 0;
    std::uint64_t headerChecksum = 0;
};

// Beside the tile cache rather than inside it: `hydrology/graph.hyd` under the
// same root, so one world's cache is one directory.
std::filesystem::path hydrologyCacheRelativePath();

// Refuses a graph that would not survive validateHydrologyGraph. Publishing is
// a temporary file and a rename, so a reader never sees a half-written graph.
CacheResult writeHydrologyGraphCache(const std::filesystem::path& root,
                                     const HydrologyGraph& graph);

// A file belonging to another world, a corrupt one and a truncated one are all
// misses that leave `out` untouched, never terrain input.
CacheResult readHydrologyGraphCache(const std::filesystem::path& root, std::uint64_t worldSeed,
                                    std::uint64_t sourceFingerprint, HydrologyGraph& out);

// Exposed for deterministic tests and tooling. Encoding is canonical: the same
// graph gives the same bytes on any machine.
CacheResult encodeHydrologyGraphPayload(const HydrologyGraph& graph,
                                        std::vector<std::uint8_t>& out);
CacheResult decodeHydrologyGraphPayload(const HydrologyCacheHeader& header,
                                        const std::vector<std::uint8_t>& payload,
                                        HydrologyGraph& out);

} // namespace world::streaming
