#include "game/world/terrain_streaming/hydrology_cache.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>

#include "engine/core/hash.hpp"
#include "game/world/terrain_streaming/cache_bytes.hpp"

namespace world::streaming {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{'A', 'S', 'R', 'H', 'Y', 'D', 'R', '1'};

// A corrupt header must not be able to ask for an allocation before anything
// has been checked. These are far above the largest world the generator
// offers - 192 cells gives about a thousand nodes, seven hundred segments and
// thirty-five thousand pages - and far below what would exhaust a worker.
constexpr std::uint32_t kMaxNodes = 4u * 1024u * 1024u;
constexpr std::uint32_t kMaxSegments = 4u * 1024u * 1024u;
constexpr std::uint32_t kMaxWaterBodies = 1u * 1024u * 1024u;
constexpr std::uint32_t kMaxSpatialPages = 16u * 1024u * 1024u;
constexpr std::size_t kMaxCentrelinePoints = 32u * 1024u * 1024u;
constexpr std::size_t kMaxListEntries = 64u * 1024u * 1024u;
constexpr std::uint64_t kMaxPayloadBytes = 512ull * 1024ull * 1024ull;

CacheResult invalid(std::string detail) {
    return {CacheStatus::InvalidArgument, std::move(detail)};
}
CacheResult corrupt(std::string detail) { return {CacheStatus::Corrupt, std::move(detail)}; }

void writePoint(std::vector<std::uint8_t>& bytes, core::WorldPos point) {
    writeInteger(bytes, point.x.raw);
    writeInteger(bytes, point.y.raw);
}

bool readPoint(const std::vector<std::uint8_t>& bytes, std::size_t& cursor, core::WorldPos& out) {
    return readInteger(bytes, cursor, out.x.raw) && readInteger(bytes, cursor, out.y.raw);
}

// Ascending IDs, so only the step between them is written. A page that lists
// one water body - most of a coastal world, where that body is the ocean -
// costs a single byte for it.
template <class Id>
void writeAscending(std::vector<std::uint8_t>& bytes, const std::vector<Id>& ids) {
    writeVarUInt(bytes, static_cast<std::uint32_t>(ids.size()));
    std::uint32_t previous = 0;
    for (const Id id : ids) {
        writeVarUInt(bytes, static_cast<std::uint32_t>(id) - previous);
        previous = static_cast<std::uint32_t>(id);
    }
}

template <class Id>
bool readAscending(const std::vector<std::uint8_t>& bytes, std::size_t& cursor,
                   std::size_t& budget, std::vector<Id>& out) {
    std::uint32_t count = 0;
    if (!readVarUInt(bytes, cursor, count)) return false;
    if (count > budget) return false;
    budget -= count;
    out.clear();
    out.reserve(count);
    std::uint32_t previous = 0;
    for (std::uint32_t n = 0; n < count; ++n) {
        std::uint32_t step = 0;
        if (!readVarUInt(bytes, cursor, step)) return false;
        // Strictly ascending: a zero step would repeat an ID, and a wrap would
        // make the list unsearchable. Both are rejected here rather than
        // discovered by a binary search that quietly misses.
        if (step == 0 || step > std::numeric_limits<std::uint32_t>::max() - previous) return false;
        previous += step;
        out.push_back(static_cast<Id>(previous));
    }
    return true;
}

std::vector<std::uint8_t> encodeHeader(const HydrologyCacheHeader& header, bool withChecksum) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kHydrologyCacheHeaderBytes);
    bytes.insert(bytes.end(), kMagic.begin(), kMagic.end());
    writeUnsigned(bytes, kHydrologyCacheFormatVersion);
    writeUnsigned(bytes, kHydrologyCacheHeaderBytes);
    writeUnsigned(bytes, header.graphVersion);
    writeUnsigned(bytes, header.worldSeed);
    writeUnsigned(bytes, header.sourceFingerprint);
    writeInteger(bytes, header.macroWidth);
    writeInteger(bytes, header.macroHeight);
    writeInteger(bytes, header.macroCellMetres);
    writeUnsigned(bytes, header.nodeCount);
    writeUnsigned(bytes, header.segmentCount);
    writeUnsigned(bytes, header.waterBodyCount);
    writeUnsigned(bytes, header.spatialPageCount);
    writeUnsigned(bytes, header.payloadBytes);
    writeUnsigned(bytes, header.payloadChecksum);
    writeUnsigned(bytes, withChecksum ? header.headerChecksum : 0ull);
    bytes.insert(bytes.end(), kHydrologyCacheHeaderBytes - bytes.size(), 0);
    return bytes;
}

bool decodeHeader(const std::vector<std::uint8_t>& bytes, HydrologyCacheHeader& out) {
    if (bytes.size() != kHydrologyCacheHeaderBytes ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
        return false;
    std::size_t cursor = kMagic.size();
    std::uint16_t version = 0;
    std::uint16_t headerBytes = 0;
    if (!readUnsigned(bytes, cursor, version) || !readUnsigned(bytes, cursor, headerBytes) ||
        version != kHydrologyCacheFormatVersion || headerBytes != kHydrologyCacheHeaderBytes ||
        !readUnsigned(bytes, cursor, out.graphVersion) ||
        !readUnsigned(bytes, cursor, out.worldSeed) ||
        !readUnsigned(bytes, cursor, out.sourceFingerprint) ||
        !readInteger(bytes, cursor, out.macroWidth) ||
        !readInteger(bytes, cursor, out.macroHeight) ||
        !readInteger(bytes, cursor, out.macroCellMetres) ||
        !readUnsigned(bytes, cursor, out.nodeCount) ||
        !readUnsigned(bytes, cursor, out.segmentCount) ||
        !readUnsigned(bytes, cursor, out.waterBodyCount) ||
        !readUnsigned(bytes, cursor, out.spatialPageCount) ||
        !readUnsigned(bytes, cursor, out.payloadBytes) ||
        !readUnsigned(bytes, cursor, out.payloadChecksum) ||
        !readUnsigned(bytes, cursor, out.headerChecksum))
        return false;
    if (out.graphVersion != kHydrologyGraphVersion) return false;
    if (out.nodeCount > kMaxNodes || out.segmentCount > kMaxSegments ||
        out.waterBodyCount > kMaxWaterBodies || out.spatialPageCount > kMaxSpatialPages)
        return false;
    if (out.macroWidth < 0 || out.macroHeight < 0 || out.macroCellMetres < 0) return false;
    if (out.payloadBytes > kMaxPayloadBytes) return false;
    const auto canonical = encodeHeader(out, false);
    return core::hashBytes(canonical.data(), canonical.size()) == out.headerChecksum;
}

HydrologyCacheHeader describe(const HydrologyGraph& graph) {
    HydrologyCacheHeader header;
    header.worldSeed = graph.worldSeed;
    header.sourceFingerprint = graph.sourceFingerprint;
    header.graphVersion = kHydrologyGraphVersion;
    header.macroWidth = graph.macroWidth;
    header.macroHeight = graph.macroHeight;
    header.macroCellMetres = graph.macroCellMetres;
    header.nodeCount = static_cast<std::uint32_t>(graph.nodes.size());
    header.segmentCount = static_cast<std::uint32_t>(graph.segments.size());
    header.waterBodyCount = static_cast<std::uint32_t>(graph.waterBodies.size());
    header.spatialPageCount = static_cast<std::uint32_t>(graph.spatialPages.size());
    return header;
}

} // namespace

std::filesystem::path hydrologyCacheRelativePath() {
    return std::filesystem::path("hydrology") / "graph.hyd";
}

CacheResult encodeHydrologyGraphPayload(const HydrologyGraph& graph,
                                        std::vector<std::uint8_t>& out) {
    out.clear();
    if (graph.nodes.size() > kMaxNodes || graph.segments.size() > kMaxSegments ||
        graph.waterBodies.size() > kMaxWaterBodies || graph.spatialPages.size() > kMaxSpatialPages)
        return invalid("hydrology graph is larger than the format allows");

    // IDs are the position in the list, so none of them is written down. What
    // is written is what an ID cannot be recomputed from.
    for (const RiverNode& node : graph.nodes) {
        writeVarUInt(out, node.downstream);
        writeVarUInt(out, node.waterBody);
        writeVarUInt(out, static_cast<std::uint32_t>(node.macroCell + 1));   // -1 becomes 0
        out.push_back(static_cast<std::uint8_t>(node.kind));
        out.push_back(node.order);
        writePoint(out, node.position);
        writeInteger(out, node.surface.raw);
    }

    for (const RiverSegment& segment : graph.segments) {
        writeVarUInt(out, segment.from);
        writeVarUInt(out, segment.to);
        writeVarUInt(out, segment.sourceWaterBody);
        writeVarUInt(out, segment.destinationWaterBody);
        out.push_back(segment.order);
        writeInteger(out, segment.discharge.raw);
        writeInteger(out, segment.width.raw);
        writeInteger(out, segment.depth.raw);
        writeInteger(out, segment.valleyReach.raw);
        writePoint(out, segment.bounds.min);
        writePoint(out, segment.bounds.max);
        if (segment.course.size() > kMaxCentrelinePoints)
            return invalid("a segment course is longer than the format allows");
        writeVarUInt(out, static_cast<std::uint32_t>(segment.macroCells.size()));
        for (const std::int32_t member : segment.macroCells)
            writeVarUInt(out, static_cast<std::uint32_t>(member));
        writeVarUInt(out, static_cast<std::uint32_t>(segment.course.size()));
        for (const ReachPoint& point : segment.course) {
            writePoint(out, point.position);
            writeInteger(out, point.surface.raw);
            writeInteger(out, point.halfWidth.raw);
            writeInteger(out, point.depth.raw);
            writeInteger(out, point.valleyReach.raw);
        }
    }

    for (const WaterBody& body : graph.waterBodies) {
        out.push_back(static_cast<std::uint8_t>(body.kind));
        writeInteger(out, body.sourceRegion);
        writeInteger(out, body.level.raw);
        // Row-major and ascending, so the footprint is a run of small steps.
        writeVarUInt(out, static_cast<std::uint32_t>(body.macroCells.size()));
        std::int64_t previousCell = -1;
        for (const core::TilePos cell : body.macroCells) {
            const std::int64_t index =
                    static_cast<std::int64_t>(cell.y) * graph.macroWidth + cell.x;
            if (index <= previousCell)
                return invalid("a water body footprint is not in row-major order");
            writeVarUInt(out, static_cast<std::uint32_t>(index - previousCell));
            previousCell = index;
        }
        for (const core::Fixed floor : body.macroCellFloor) writeInteger(out, floor.raw);
        writeInteger(out, body.basinStep);
        writeVarUInt(out, static_cast<std::uint32_t>(body.basinSamples.size()));
        for (const auto sample : body.basinSamples) {
            writeVarUInt(out, static_cast<std::uint32_t>(sample.x));
            writeVarUInt(out, static_cast<std::uint32_t>(sample.y));
        }
        writeAscending(out, body.inlets);
        writeAscending(out, body.outlets);
    }

    // Pages are sorted by (y, x), so a page costs the step from the last one.
    // Most of them hold one water body and no segment at all.
    std::int32_t previousX = 0, previousY = 0;
    for (const HydrologySpatialPage& page : graph.spatialPages) {
        writeVarUInt(out, zigZag(page.key.x - previousX));
        writeVarUInt(out, zigZag(page.key.y - previousY));
        out.push_back(page.key.level);
        previousX = page.key.x;
        previousY = page.key.y;
        writeAscending(out, page.segments);
        writeAscending(out, page.waterBodies);
    }
    return CacheResult::ok();
}

CacheResult decodeHydrologyGraphPayload(const HydrologyCacheHeader& header,
                                        const std::vector<std::uint8_t>& payload,
                                        HydrologyGraph& out) {
    if (header.nodeCount > kMaxNodes || header.segmentCount > kMaxSegments ||
        header.waterBodyCount > kMaxWaterBodies || header.spatialPageCount > kMaxSpatialPages)
        return corrupt("hydrology graph header asks for more than the format allows");

    HydrologyGraph graph;
    graph.worldSeed = header.worldSeed;
    graph.sourceFingerprint = header.sourceFingerprint;
    graph.macroWidth = header.macroWidth;
    graph.macroHeight = header.macroHeight;
    graph.macroCellMetres = header.macroCellMetres;

    std::size_t cursor = 0;
    graph.nodes.resize(header.nodeCount);
    for (std::uint32_t n = 0; n < header.nodeCount; ++n) {
        RiverNode& node = graph.nodes[n];
        std::uint32_t cell = 0;
        std::uint8_t kind = 0;
        if (!readVarUInt(payload, cursor, node.downstream) ||
            !readVarUInt(payload, cursor, node.waterBody) ||
            !readVarUInt(payload, cursor, cell) || !readUnsigned(payload, cursor, kind) ||
            !readUnsigned(payload, cursor, node.order) ||
            !readPoint(payload, cursor, node.position) ||
            !readInteger(payload, cursor, node.surface.raw))
            return corrupt("hydrology node record is truncated");
        node.id = n + 1;
        node.macroCell = static_cast<std::int32_t>(cell) - 1;
        node.kind = static_cast<RiverNodeKind>(kind);
    }

    std::size_t centrelineBudget = kMaxCentrelinePoints;
    graph.segments.resize(header.segmentCount);
    for (std::uint32_t s = 0; s < header.segmentCount; ++s) {
        RiverSegment& segment = graph.segments[s];
        std::uint32_t points = 0;
        if (!readVarUInt(payload, cursor, segment.from) ||
            !readVarUInt(payload, cursor, segment.to) ||
            !readVarUInt(payload, cursor, segment.sourceWaterBody) ||
            !readVarUInt(payload, cursor, segment.destinationWaterBody) ||
            !readUnsigned(payload, cursor, segment.order) ||
            !readInteger(payload, cursor, segment.discharge.raw) ||
            !readInteger(payload, cursor, segment.width.raw) ||
            !readInteger(payload, cursor, segment.depth.raw) ||
            !readInteger(payload, cursor, segment.valleyReach.raw) ||
            !readPoint(payload, cursor, segment.bounds.min) ||
            !readPoint(payload, cursor, segment.bounds.max) ||
            !readVarUInt(payload, cursor, points))
            return corrupt("hydrology segment record is truncated");
        {
            const std::uint32_t members = points;
            if (members > centrelineBudget) return corrupt("hydrology provenance exceeds the cap");
            segment.macroCells.resize(members);
            for (std::uint32_t m = 0; m < members; ++m) {
                std::uint32_t cell = 0;
                if (!readVarUInt(payload, cursor, cell))
                    return corrupt("hydrology segment provenance is truncated");
                segment.macroCells[m] = static_cast<std::int32_t>(cell);
            }
            if (!readVarUInt(payload, cursor, points))
                return corrupt("hydrology course length is truncated");
        }
        if (points > centrelineBudget) return corrupt("hydrology centrelines exceed the format cap");
        centrelineBudget -= points;
        segment.id = s + 1;
        segment.course.resize(points);
        for (std::uint32_t p = 0; p < points; ++p) {
            ReachPoint& point = segment.course[p];
            if (!readPoint(payload, cursor, point.position) ||
                !readInteger(payload, cursor, point.surface.raw) ||
                !readInteger(payload, cursor, point.halfWidth.raw) ||
                !readInteger(payload, cursor, point.depth.raw) ||
                !readInteger(payload, cursor, point.valleyReach.raw))
                return corrupt("hydrology course is truncated");
        }
    }

    std::size_t listBudget = kMaxListEntries;
    graph.waterBodies.resize(header.waterBodyCount);
    for (std::uint32_t b = 0; b < header.waterBodyCount; ++b) {
        WaterBody& body = graph.waterBodies[b];
        std::uint8_t kind = 0;
        std::uint32_t cells = 0;
        if (!readUnsigned(payload, cursor, kind) ||
            !readInteger(payload, cursor, body.sourceRegion) ||
            !readInteger(payload, cursor, body.level.raw) ||
            !readVarUInt(payload, cursor, cells))
            return corrupt("hydrology water body record is truncated");
        if (cells > listBudget) return corrupt("hydrology footprints exceed the format cap");
        listBudget -= cells;
        body.id = b + 1;
        body.kind = static_cast<WaterBodyKind>(kind);
        body.macroCells.resize(cells);
        std::int64_t previousCell = -1;
        for (std::uint32_t c = 0; c < cells; ++c) {
            std::uint32_t step = 0;
            if (!readVarUInt(payload, cursor, step) || step == 0)
                return corrupt("hydrology footprint is truncated or repeats a cell");
            previousCell += step;
            if (graph.macroWidth <= 0) return corrupt("hydrology footprint needs a macro width");
            body.macroCells[c] = {static_cast<std::int32_t>(previousCell % graph.macroWidth),
                                  static_cast<std::int32_t>(previousCell / graph.macroWidth)};
        }
        body.macroCellFloor.resize(cells);
        for (std::uint32_t c = 0; c < cells; ++c)
            if (!readInteger(payload, cursor, body.macroCellFloor[c].raw))
                return corrupt("hydrology water body floor is truncated");
        std::uint32_t samples=0;
        if (!readInteger(payload,cursor,body.basinStep) || !readVarUInt(payload,cursor,samples) ||
            samples>listBudget) return corrupt("hydrology basin samples exceed the format cap");
        listBudget-=samples;
        body.basinSamples.resize(samples);
        for (auto& sample:body.basinSamples) {
            std::uint32_t x=0,y=0;
            if (!readVarUInt(payload,cursor,x) || !readVarUInt(payload,cursor,y) ||
                x>std::uint32_t(INT32_MAX) || y>std::uint32_t(INT32_MAX))
                return corrupt("hydrology basin sample is truncated or outside the grid");
            sample={std::int32_t(x),std::int32_t(y)};
        }
        if (!readAscending(payload, cursor, listBudget, body.inlets) ||
            !readAscending(payload, cursor, listBudget, body.outlets))
            return corrupt("hydrology water body links are truncated");
    }

    graph.spatialPages.resize(header.spatialPageCount);
    std::int32_t previousX = 0, previousY = 0;
    for (std::uint32_t p = 0; p < header.spatialPageCount; ++p) {
        HydrologySpatialPage& page = graph.spatialPages[p];
        std::uint32_t stepX = 0, stepY = 0;
        if (!readVarUInt(payload, cursor, stepX) || !readVarUInt(payload, cursor, stepY) ||
            !readUnsigned(payload, cursor, page.key.level))
            return corrupt("hydrology page record is truncated");
        previousX += unZigZag(stepX);
        previousY += unZigZag(stepY);
        page.key.x = previousX;
        page.key.y = previousY;
        if (!readAscending(payload, cursor, listBudget, page.segments) ||
            !readAscending(payload, cursor, listBudget, page.waterBodies))
            return corrupt("hydrology page lists are truncated");
    }

    // Trailing bytes mean the file says one thing and holds another; a graph
    // that decoded by luck is not a graph anything may stream from.
    if (cursor != payload.size()) return corrupt("hydrology payload has trailing bytes");

    std::string why;
    if (!validateHydrologyGraph(graph, &why))
        return corrupt("decoded hydrology graph is inconsistent: " + why);
    out = std::move(graph);
    return CacheResult::ok();
}

CacheResult writeHydrologyGraphCache(const std::filesystem::path& root,
                                     const HydrologyGraph& graph) {
    std::string why;
    if (!validateHydrologyGraph(graph, &why))
        return invalid("refusing to cache an inconsistent hydrology graph: " + why);

    std::vector<std::uint8_t> payload;
    if (const auto encoded = encodeHydrologyGraphPayload(graph, payload); !encoded) return encoded;

    HydrologyCacheHeader header = describe(graph);
    header.payloadBytes = payload.size();
    header.payloadChecksum = core::hashBytes(payload.data(), payload.size());
    const auto canonical = encodeHeader(header, false);
    header.headerChecksum = core::hashBytes(canonical.data(), canonical.size());
    const auto bytes = encodeHeader(header, true);

    const auto path = root / hydrologyCacheRelativePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return {CacheStatus::IoError, "could not create hydrology cache directory: " + ec.message()};
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return {CacheStatus::IoError, "could not open temporary hydrology cache file"};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.write(reinterpret_cast<const char*>(payload.data()),
                   static_cast<std::streamsize>(payload.size()));
        file.flush();
        if (!file) return {CacheStatus::IoError, "could not write hydrology cache file"};
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(temporary, path, ec);
    }
    if (ec)
        return {CacheStatus::IoError,
                "could not atomically publish the hydrology cache: " + ec.message()};
    return CacheResult::ok();
}

CacheResult readHydrologyGraphCache(const std::filesystem::path& root, std::uint64_t worldSeed,
                                    std::uint64_t sourceFingerprint, HydrologyGraph& out) {
    const auto path = root / hydrologyCacheRelativePath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return {CacheStatus::NotFound, "hydrology cache is absent"};
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {CacheStatus::IoError, "could not open the hydrology cache"};
    const auto fileBytes = file.tellg();
    if (fileBytes < static_cast<std::streamoff>(kHydrologyCacheHeaderBytes))
        return corrupt("hydrology cache is shorter than its header");
    file.seekg(0);
    std::vector<std::uint8_t> headerBytes(kHydrologyCacheHeaderBytes);
    file.read(reinterpret_cast<char*>(headerBytes.data()),
              static_cast<std::streamsize>(headerBytes.size()));
    HydrologyCacheHeader header;
    if (!file || !decodeHeader(headerBytes, header))
        return {CacheStatus::InvalidHeader, "invalid hydrology cache header"};
    if (header.worldSeed != worldSeed || header.sourceFingerprint != sourceFingerprint)
        return {CacheStatus::Incompatible, "hydrology cache belongs to another macro map"};
    if (header.payloadBytes !=
        static_cast<std::uint64_t>(fileBytes -
                                   static_cast<std::streamoff>(kHydrologyCacheHeaderBytes)))
        return corrupt("hydrology payload size does not match the file size");

    std::vector<std::uint8_t> payload(static_cast<std::size_t>(header.payloadBytes));
    file.read(reinterpret_cast<char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    if (!file) return corrupt("could not read the hydrology payload");
    if (core::hashBytes(payload.data(), payload.size()) != header.payloadChecksum)
        return corrupt("hydrology payload checksum failed");
    return decodeHydrologyGraphPayload(header, payload, out);
}

} // namespace world::streaming
