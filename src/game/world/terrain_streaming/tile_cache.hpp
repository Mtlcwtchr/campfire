#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "game/world/terrain_streaming/base_tile.hpp"

namespace world::streaming {

// This cache is an immutable, worker-owned persistence boundary.  The main
// thread only asks the streamer for ready tiles; disk reads, writes and
// validation belong to background jobs.  Files are deliberately independent
// from the renderer and from the current mesh format.
// Version 3: material weights are now classified in a fixed world-space
// resolution. Any page made with the old stride-dependent classifier can turn
// into rock when the camera changes LOD, so it must never be accepted again.
inline constexpr std::uint16_t kTerrainTileCacheFormatVersion = 3;
inline constexpr std::uint16_t kTerrainTileCacheHeaderBytes = 104;

enum class CacheRecordKind : std::uint8_t { Base = 1, Residual = 2 };

// Which of a base tile's channels the record actually carries.
//
// H_sim is the tile; the rest are owned by whoever decides them, and until
// that owner exists the honest record is one without the channel in it. A
// column of "fully walkable" written because the format insisted is a fact
// nobody established, and a later reader has no way to tell it from one that
// was measured.
enum class BaseTileChannel : std::uint8_t {
    Height = 1u << 0u,
    WaterBody = 1u << 1u,
    Watershed = 1u << 2u,
    Buildability = 1u << 3u,
    Walkability = 1u << 4u,
};

constexpr std::uint8_t channelMask(BaseTileChannel channel) {
    return static_cast<std::uint8_t>(channel);
}
constexpr bool containsChannel(std::uint8_t channels, BaseTileChannel channel) {
    return (channels & channelMask(channel)) != 0;
}

// Bump generationFingerprint whenever an input affecting H_sim or a residual
// changes. It makes old cache files safely unusable without depending on their
// directory name or modification time.
struct CacheIdentity {
    std::uint64_t worldSeed = 0;
    std::uint64_t generationFingerprint = 0;
};

// Logical descriptor written in every file header. It is intentionally a
// value type, but it is never dumped as a native C++ struct: tile_cache.cpp
// writes every field in a specified little-endian order.
struct CacheRecordDescriptor {
    CacheRecordKind kind = CacheRecordKind::Base;
    ResidualLevel residualLevel = ResidualLevel::Large;
    TileKey key{};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t padding = 0;
    std::int32_t sampleMetres = 0;
    std::int64_t elevationMinRaw = 0;
    std::int64_t elevationMaxRaw = 0;
    std::uint32_t sampleCount = 0;
    // Base records only; a residual carries none.
    std::uint8_t channels = 0;
};

struct CacheHeader {
    CacheIdentity identity{};
    CacheRecordDescriptor record{};
    std::uint64_t payloadBytes = 0;
    std::uint64_t payloadChecksum = 0;
    std::uint64_t headerChecksum = 0;
};

enum class CacheStatus : std::uint8_t {
    Ok,
    NotFound,
    IoError,
    InvalidArgument,
    InvalidHeader,
    Incompatible,
    Corrupt,
};

struct CacheResult {
    CacheStatus status = CacheStatus::Ok;
    std::string detail;

    explicit operator bool() const { return status == CacheStatus::Ok; }
    static CacheResult ok() { return {}; }
};

// Tile arrays include their padded border. These guards prevent a malformed
// cache from allocating arbitrary amounts of RAM before validation completes.
constexpr std::size_t sampleCountIncludingPadding(std::uint16_t width,
                                                   std::uint16_t height,
                                                   std::uint16_t padding) {
    const auto fullWidth = std::size_t(width) + 2u * padding;
    const auto fullHeight = std::size_t(height) + 2u * padding;
    return fullWidth * fullHeight;
}

CacheRecordDescriptor describeBaseTile(const BaseTile& tile);
CacheRecordDescriptor describeResidualTile(const ResidualTile& tile);

// Stable relative paths let callers batch neighbouring square tiles in the
// same directory. Their row-major payloads remain cache-local even when the
// scheduler prioritises them by rings or frustum error.
std::filesystem::path cacheRelativePath(const CacheRecordDescriptor& descriptor);

CacheResult validateBaseTile(const BaseTile& tile);
CacheResult validateResidualTile(const ResidualTile& tile);

CacheResult writeBaseTileCache(const std::filesystem::path& root, const CacheIdentity& identity,
                               const BaseTile& tile);
CacheResult writeResidualTileCache(const std::filesystem::path& root,
                                   const CacheIdentity& identity, const ResidualTile& tile);

CacheResult readBaseTileCache(const std::filesystem::path& root, const CacheIdentity& identity,
                              const TileKey& key, BaseTile& out);
CacheResult readResidualTileCache(const std::filesystem::path& root,
                                  const CacheIdentity& identity, const TileKey& key,
                                  ResidualLevel level, ResidualTile& out);

// Exposed for deterministic cache tests and tooling. The payload is compact:
// smooth height samples use delta-varints; categorical channels use canonical
// runs; residual values stay packed signed 16-bit values.
CacheResult encodeBaseTilePayload(const BaseTile& tile, std::vector<std::uint8_t>& out);
CacheResult decodeBaseTilePayload(const CacheRecordDescriptor& descriptor,
                                  const std::vector<std::uint8_t>& payload, BaseTile& out);
CacheResult encodeResidualTilePayload(const ResidualTile& tile, std::vector<std::uint8_t>& out);
CacheResult decodeResidualTilePayload(const CacheRecordDescriptor& descriptor,
                                      const std::vector<std::uint8_t>& payload,
                                      ResidualTile& out);

std::uint64_t terrainTileCacheChecksum(const void* bytes, std::size_t count);

} // namespace world::streaming
