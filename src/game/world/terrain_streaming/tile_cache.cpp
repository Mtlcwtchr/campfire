#include "game/world/terrain_streaming/tile_cache.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <type_traits>

#include "engine/core/hash.hpp"
#include "game/world/terrain_streaming/cache_bytes.hpp"

namespace world::streaming {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'A', 'S', 'R', 'T', 'I', 'L', 'E', '1'};
constexpr std::size_t kMaxTileSamples = 4u * 1024u * 1024u;
constexpr std::size_t kMaxPayloadBytes = 128u * 1024u * 1024u;

CacheResult invalid(std::string detail) {
    return {CacheStatus::InvalidArgument, std::move(detail)};
}

bool descriptorIsSane(const CacheRecordDescriptor& descriptor) {
    if (descriptor.width == 0 || descriptor.height == 0 || descriptor.sampleMetres <= 0) return false;
    if (descriptor.kind != CacheRecordKind::Base && descriptor.kind != CacheRecordKind::Residual) return false;
    if (descriptor.kind == CacheRecordKind::Residual &&
        descriptor.residualLevel != ResidualLevel::Large &&
        descriptor.residualLevel != ResidualLevel::Medium &&
        descriptor.residualLevel != ResidualLevel::Fine) return false;
    // A base record without its height channel is not a base record, and a
    // residual has no channels to name.
    if (descriptor.kind == CacheRecordKind::Base &&
        !containsChannel(descriptor.channels, BaseTileChannel::Height)) return false;
    if (descriptor.kind == CacheRecordKind::Residual && descriptor.channels != 0) return false;
    const auto samples = sampleCountIncludingPadding(descriptor.width, descriptor.height, descriptor.padding);
    return samples > 0 && samples <= kMaxTileSamples && descriptor.sampleCount == samples;
}

void appendDescriptor(std::vector<std::uint8_t>& bytes, const CacheRecordDescriptor& d) {
    bytes.push_back(static_cast<std::uint8_t>(d.kind));
    bytes.push_back(static_cast<std::uint8_t>(d.residualLevel));
    writeUnsigned<std::uint16_t>(bytes, 0);
    writeInteger(bytes, d.key.x);
    writeInteger(bytes, d.key.y);
    bytes.push_back(d.key.level);
    bytes.push_back(d.channels);
    bytes.insert(bytes.end(), 2, 0);
    writeUnsigned(bytes, d.width);
    writeUnsigned(bytes, d.height);
    writeUnsigned(bytes, d.padding);
    writeInteger(bytes, d.sampleMetres);
    writeInteger(bytes, d.elevationMinRaw);
    writeInteger(bytes, d.elevationMaxRaw);
    writeUnsigned(bytes, d.sampleCount);
}

bool readDescriptor(const std::vector<std::uint8_t>& bytes, std::size_t& cursor,
                    CacheRecordDescriptor& d) {
    std::uint8_t kind = 0;
    std::uint8_t residual = 0;
    std::uint16_t reserved16 = 0;
    std::uint8_t reserved8 = 0;
    if (!readUnsigned(bytes, cursor, kind) || !readUnsigned(bytes, cursor, residual) ||
        !readUnsigned(bytes, cursor, reserved16) || !readInteger(bytes, cursor, d.key.x) ||
        !readInteger(bytes, cursor, d.key.y) || !readUnsigned(bytes, cursor, d.key.level)) return false;
    if (!readUnsigned(bytes, cursor, d.channels)) return false;
    for (int i = 0; i < 2; ++i) if (!readUnsigned(bytes, cursor, reserved8)) return false;
    if (!readUnsigned(bytes, cursor, d.width) || !readUnsigned(bytes, cursor, d.height) ||
        !readUnsigned(bytes, cursor, d.padding) || !readInteger(bytes, cursor, d.sampleMetres) ||
        !readInteger(bytes, cursor, d.elevationMinRaw) ||
        !readInteger(bytes, cursor, d.elevationMaxRaw) || !readUnsigned(bytes, cursor, d.sampleCount)) return false;
    d.kind = static_cast<CacheRecordKind>(kind);
    d.residualLevel = static_cast<ResidualLevel>(residual);
    return descriptorIsSane(d);
}

std::vector<std::uint8_t> encodeHeader(const CacheHeader& header, bool withChecksum) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kTerrainTileCacheHeaderBytes);
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    writeUnsigned(bytes, kTerrainTileCacheFormatVersion);
    writeUnsigned(bytes, kTerrainTileCacheHeaderBytes);
    writeUnsigned(bytes, header.identity.worldSeed);
    writeUnsigned(bytes, header.identity.generationFingerprint);
    appendDescriptor(bytes, header.record);
    writeUnsigned(bytes, header.payloadBytes);
    writeUnsigned(bytes, header.payloadChecksum);
    writeUnsigned(bytes, withChecksum ? header.headerChecksum : 0ull);
    bytes.insert(bytes.end(), kTerrainTileCacheHeaderBytes - bytes.size(), 0);
    return bytes;
}

bool decodeHeader(const std::vector<std::uint8_t>& bytes, CacheHeader& out) {
    if (bytes.size() != kTerrainTileCacheHeaderBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) return false;
    std::size_t cursor = kMagic.size();
    std::uint16_t version = 0;
    std::uint16_t headerBytes = 0;
    if (!readUnsigned(bytes, cursor, version) || !readUnsigned(bytes, cursor, headerBytes) ||
        version != kTerrainTileCacheFormatVersion || headerBytes != kTerrainTileCacheHeaderBytes ||
        !readUnsigned(bytes, cursor, out.identity.worldSeed) ||
        !readUnsigned(bytes, cursor, out.identity.generationFingerprint) ||
        !readDescriptor(bytes, cursor, out.record) ||
        !readUnsigned(bytes, cursor, out.payloadBytes) ||
        !readUnsigned(bytes, cursor, out.payloadChecksum) ||
        !readUnsigned(bytes, cursor, out.headerChecksum)) return false;
    if (out.payloadBytes > kMaxPayloadBytes) return false;
    auto canonical = encodeHeader(out, false);
    return terrainTileCacheChecksum(canonical.data(), canonical.size()) == out.headerChecksum;
}

template <class Value>
void encodeRuns(const std::vector<Value>& values, std::vector<std::uint8_t>& out) {
    std::size_t at = 0;
    while (at < values.size()) {
        std::size_t end = at + 1;
        while (end < values.size() && values[end] == values[at] && end - at < UINT32_MAX) ++end;
        if constexpr (sizeof(Value) == 1) out.push_back(static_cast<std::uint8_t>(values[at]));
        else writeUnsigned(out, static_cast<std::uint16_t>(values[at]));
        writeVarUInt(out, static_cast<std::uint32_t>(end - at));
        at = end;
    }
}

template <class Value>
bool decodeRuns(const std::vector<std::uint8_t>& bytes, std::size_t& cursor, std::size_t count,
                std::vector<Value>& out) {
    out.clear();
    out.reserve(count);
    while (out.size() < count) {
        Value value{};
        if constexpr (sizeof(Value) == 1) {
            std::uint8_t raw = 0;
            if (!readUnsigned(bytes, cursor, raw)) return false;
            value = static_cast<Value>(raw);
        } else {
            std::uint16_t raw = 0;
            if (!readUnsigned(bytes, cursor, raw)) return false;
            value = static_cast<Value>(raw);
        }
        std::uint32_t run = 0;
        if (!readVarUInt(bytes, cursor, run) || run == 0 || run > count - out.size()) return false;
        out.insert(out.end(), run, value);
    }
    return true;
}

CacheResult writeFile(const std::filesystem::path& path, CacheHeader header,
                      const std::vector<std::uint8_t>& payload) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return {CacheStatus::IoError, "could not create cache directory: " + ec.message()};
    header.payloadBytes = payload.size();
    header.payloadChecksum = terrainTileCacheChecksum(payload.data(), payload.size());
    auto canonical = encodeHeader(header, false);
    header.headerChecksum = terrainTileCacheChecksum(canonical.data(), canonical.size());
    const auto bytes = encodeHeader(header, true);
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return {CacheStatus::IoError, "could not open temporary cache file"};
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        out.flush();
        if (!out) return {CacheStatus::IoError, "could not write cache file"};
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec) return {CacheStatus::IoError, "could not atomically publish cache file: " + ec.message()};
    return CacheResult::ok();
}

CacheResult readFile(const std::filesystem::path& path, const CacheIdentity& identity,
                     CacheRecordKind kind, CacheHeader& header, std::vector<std::uint8_t>& payload) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return {CacheStatus::NotFound, "cache file is absent"};
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {CacheStatus::IoError, "could not open cache file"};
    const auto fileBytes = in.tellg();
    if (fileBytes < static_cast<std::streamoff>(kTerrainTileCacheHeaderBytes))
        return {CacheStatus::Corrupt, "cache file is shorter than its header"};
    in.seekg(0);
    std::vector<std::uint8_t> headerBytes(kTerrainTileCacheHeaderBytes);
    in.read(reinterpret_cast<char*>(headerBytes.data()), static_cast<std::streamsize>(headerBytes.size()));
    if (!in || !decodeHeader(headerBytes, header)) return {CacheStatus::InvalidHeader, "invalid cache header"};
    if (header.identity.worldSeed != identity.worldSeed ||
        header.identity.generationFingerprint != identity.generationFingerprint)
        return {CacheStatus::Incompatible, "cache belongs to another generated world"};
    if (header.record.kind != kind) return {CacheStatus::Incompatible, "cache record has another kind"};
    if (header.payloadBytes != static_cast<std::uint64_t>(fileBytes -
                                                          static_cast<std::streamoff>(kTerrainTileCacheHeaderBytes)))
        return {CacheStatus::Corrupt, "cache payload size does not match file size"};
    payload.resize(static_cast<std::size_t>(header.payloadBytes));
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!in || terrainTileCacheChecksum(payload.data(), payload.size()) != header.payloadChecksum)
        return {CacheStatus::Corrupt, "cache payload checksum failed"};
    return CacheResult::ok();
}

} // namespace

std::uint64_t terrainTileCacheChecksum(const void* bytes, std::size_t count) {
    return core::hashBytes(bytes, count);
}

CacheRecordDescriptor describeBaseTile(const BaseTile& tile) {
    const auto count = sampleCountIncludingPadding(tile.width, tile.height, tile.padding);
    std::uint8_t channels = channelMask(BaseTileChannel::Height);
    if (!tile.waterBodyId.empty()) channels |= channelMask(BaseTileChannel::WaterBody);
    if (!tile.watershedId.empty()) channels |= channelMask(BaseTileChannel::Watershed);
    if (!tile.buildability.empty()) channels |= channelMask(BaseTileChannel::Buildability);
    if (!tile.walkability.empty()) channels |= channelMask(BaseTileChannel::Walkability);
    return {CacheRecordKind::Base, ResidualLevel::Large, tile.key, tile.width, tile.height, tile.padding,
            tile.sampleMetres, tile.elevationMin.raw, tile.elevationMax.raw,
            count <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(count) : 0u,
            channels};
}

CacheRecordDescriptor describeResidualTile(const ResidualTile& tile) {
    const auto count = sampleCountIncludingPadding(tile.width, tile.height, tile.padding);
    return {CacheRecordKind::Residual, tile.level, tile.key, tile.width, tile.height, tile.padding,
            tile.sampleMetres, 0, 0,
            count <= std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(count) : 0u,
            0u};
}

std::filesystem::path cacheRelativePath(const CacheRecordDescriptor& d) {
    const auto domain = d.kind == CacheRecordKind::Base ? "base" : "residual";
    std::string level = "l" + std::to_string(d.key.level);
    if (d.kind == CacheRecordKind::Residual) {
        const char* name = d.residualLevel == ResidualLevel::Large ? "large" :
                           d.residualLevel == ResidualLevel::Medium ? "medium" : "fine";
        return std::filesystem::path(domain) / name / level /
               (std::to_string(d.key.x) + "_" + std::to_string(d.key.y) + ".tile");
    }
    return std::filesystem::path(domain) / level /
           (std::to_string(d.key.x) + "_" + std::to_string(d.key.y) + ".tile");
}

CacheResult validateBaseTile(const BaseTile& tile) {
    const auto descriptor = describeBaseTile(tile);
    if (!descriptorIsSane(descriptor)) return invalid("base tile descriptor is out of bounds");
    const auto count = descriptor.sampleCount;
    // Height is the tile. An optional channel is either the whole tile or
    // absent; half a channel is a tile that reads as if it had been measured.
    const auto sized = [count](const auto& channel) {
        return channel.empty() || channel.size() == count;
    };
    if (tile.heightQuantized.size() != count) return invalid("base tile has no height channel");
    if (!sized(tile.waterBodyId) || !sized(tile.watershedId) || !sized(tile.buildability) ||
        !sized(tile.walkability)) return invalid("base tile channels do not match sample count");
    if (tile.elevationMin.raw > tile.elevationMax.raw) return invalid("base tile elevation range is reversed");
    return CacheResult::ok();
}

CacheResult validateResidualTile(const ResidualTile& tile) {
    const auto descriptor = describeResidualTile(tile);
    if (!descriptorIsSane(descriptor)) return invalid("residual tile descriptor is out of bounds");
    if (tile.deltaQuantized.size() != descriptor.sampleCount)
        return invalid("residual tile channel does not match sample count");
    return CacheResult::ok();
}

CacheResult encodeBaseTilePayload(const BaseTile& tile, std::vector<std::uint8_t>& out) {
    if (const auto valid = validateBaseTile(tile); !valid) return valid;
    out.clear();
    out.reserve(tile.heightQuantized.size() * 2u);
    std::int32_t previous = 0;
    for (const auto value : tile.heightQuantized) {
        const auto delta = static_cast<std::int32_t>(value) - previous;
        writeVarUInt(out, zigZag(delta));
        previous = value;
    }
    // In descriptor order, and only the ones the descriptor claims.
    if (!tile.waterBodyId.empty()) encodeRuns(tile.waterBodyId, out);
    if (!tile.watershedId.empty()) encodeRuns(tile.watershedId, out);
    if (!tile.buildability.empty()) encodeRuns(tile.buildability, out);
    if (!tile.walkability.empty()) encodeRuns(tile.walkability, out);
    return CacheResult::ok();
}

CacheResult decodeBaseTilePayload(const CacheRecordDescriptor& d,
                                  const std::vector<std::uint8_t>& payload, BaseTile& out) {
    if (d.kind != CacheRecordKind::Base || !descriptorIsSane(d))
        return {CacheStatus::InvalidHeader, "invalid base tile descriptor"};
    std::size_t cursor = 0;
    out = {};
    out.key = d.key;
    out.width = d.width;
    out.height = d.height;
    out.padding = d.padding;
    out.sampleMetres = d.sampleMetres;
    out.elevationMin = core::Fixed::fromRaw(d.elevationMinRaw);
    out.elevationMax = core::Fixed::fromRaw(d.elevationMaxRaw);
    out.heightQuantized.reserve(d.sampleCount);
    std::int32_t previous = 0;
    for (std::uint32_t i = 0; i < d.sampleCount; ++i) {
        std::uint32_t encoded = 0;
        if (!readVarUInt(payload, cursor, encoded)) return {CacheStatus::Corrupt, "truncated height channel"};
        const auto value = previous + unZigZag(encoded);
        if (value < 0 || value > std::numeric_limits<std::uint16_t>::max())
            return {CacheStatus::Corrupt, "height delta overflows quantized range"};
        out.heightQuantized.push_back(static_cast<std::uint16_t>(value));
        previous = value;
    }
    const auto optional = [&](BaseTileChannel channel, auto& into) {
        if (!containsChannel(d.channels, channel)) return true;
        return decodeRuns(payload, cursor, d.sampleCount, into);
    };
    if (!optional(BaseTileChannel::WaterBody, out.waterBodyId) ||
        !optional(BaseTileChannel::Watershed, out.watershedId) ||
        !optional(BaseTileChannel::Buildability, out.buildability) ||
        !optional(BaseTileChannel::Walkability, out.walkability) || cursor != payload.size())
        return {CacheStatus::Corrupt, "invalid base tile categorical channel"};
    return CacheResult::ok();
}

CacheResult encodeResidualTilePayload(const ResidualTile& tile, std::vector<std::uint8_t>& out) {
    if (const auto valid = validateResidualTile(tile); !valid) return valid;
    out.clear();
    out.reserve(tile.deltaQuantized.size() * sizeof(std::int16_t));
    for (const auto value : tile.deltaQuantized) writeInteger(out, value);
    return CacheResult::ok();
}

CacheResult decodeResidualTilePayload(const CacheRecordDescriptor& d,
                                      const std::vector<std::uint8_t>& payload, ResidualTile& out) {
    if (d.kind != CacheRecordKind::Residual || !descriptorIsSane(d) ||
        payload.size() != std::size_t(d.sampleCount) * sizeof(std::int16_t))
        return {CacheStatus::Corrupt, "invalid residual tile payload"};
    std::size_t cursor = 0;
    out = {};
    out.key = d.key;
    out.level = d.residualLevel;
    out.width = d.width;
    out.height = d.height;
    out.padding = d.padding;
    out.sampleMetres = d.sampleMetres;
    out.deltaQuantized.resize(d.sampleCount);
    for (auto& value : out.deltaQuantized)
        if (!readInteger(payload, cursor, value)) return {CacheStatus::Corrupt, "truncated residual channel"};
    return CacheResult::ok();
}

CacheResult writeBaseTileCache(const std::filesystem::path& root, const CacheIdentity& identity,
                               const BaseTile& tile) {
    std::vector<std::uint8_t> payload;
    if (const auto encoded = encodeBaseTilePayload(tile, payload); !encoded) return encoded;
    CacheHeader header{};
    header.identity = identity;
    header.record = describeBaseTile(tile);
    return writeFile(root / cacheRelativePath(header.record), header, payload);
}

CacheResult writeResidualTileCache(const std::filesystem::path& root,
                                   const CacheIdentity& identity, const ResidualTile& tile) {
    std::vector<std::uint8_t> payload;
    if (const auto encoded = encodeResidualTilePayload(tile, payload); !encoded) return encoded;
    CacheHeader header{};
    header.identity = identity;
    header.record = describeResidualTile(tile);
    return writeFile(root / cacheRelativePath(header.record), header, payload);
}

CacheResult readBaseTileCache(const std::filesystem::path& root, const CacheIdentity& identity,
                              const TileKey& key, BaseTile& out) {
    CacheRecordDescriptor wanted{};
    wanted.kind = CacheRecordKind::Base;
    wanted.key = key;
    CacheHeader header{};
    std::vector<std::uint8_t> payload;
    if (const auto read = readFile(root / cacheRelativePath(wanted), identity, CacheRecordKind::Base,
                                   header, payload); !read) return read;
    if (!(header.record.key == key)) return {CacheStatus::Corrupt, "base cache key mismatch"};
    return decodeBaseTilePayload(header.record, payload, out);
}

CacheResult readResidualTileCache(const std::filesystem::path& root,
                                  const CacheIdentity& identity, const TileKey& key,
                                  ResidualLevel level, ResidualTile& out) {
    CacheRecordDescriptor wanted{};
    wanted.kind = CacheRecordKind::Residual;
    wanted.key = key;
    wanted.residualLevel = level;
    CacheHeader header{};
    std::vector<std::uint8_t> payload;
    if (const auto read = readFile(root / cacheRelativePath(wanted), identity,
                                   CacheRecordKind::Residual, header, payload); !read) return read;
    if (!(header.record.key == key) || header.record.residualLevel != level)
        return {CacheStatus::Corrupt, "residual cache key mismatch"};
    return decodeResidualTilePayload(header.record, payload, out);
}

} // namespace world::streaming
