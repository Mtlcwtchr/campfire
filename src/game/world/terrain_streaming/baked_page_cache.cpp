#include "game/world/terrain_streaming/baked_page_cache.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include "engine/core/hash.hpp"
#include "engine/core/lane_hash.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/cache_bytes.hpp"
#include "game/world/terrain_streaming/hydrology_cache.hpp"

namespace world::streaming {
namespace {
constexpr std::size_t kMaxPageBytes = 1u << 20;

// Hash values, never native object padding or addresses. Include every macro
// terrain field so editing a world with the same seed cannot reuse stale pages.
std::uint64_t fingerprint(const generation::WorldMapData& w, const HydrologyGraph& graph) {
    core::Checksum sum;
    sum.add(static_cast<std::uint64_t>(kBakedPageGenerationVersion));
    sum.add(kTerrainTileCacheFormatVersion);
    sum.add(w.seed); sum.add(w.width); sum.add(w.height);
    sum.add(w.hybridTerrain ? w.hybridTerrain->fingerprint() : std::uint64_t{0});
    sum.add(w.terrainFoundation ? w.terrainFoundation->fingerprint() : std::uint64_t{0});
    sum.add(generation::kMetresPerCell); sum.add(generation::kMetresPerElevationStep);
    const auto& c = w.constants;
    sum.add(c.worldSeed); sum.add(c.worldWidth); sum.add(c.worldHeight); sum.add(c.seaLevel);
    sum.add(c.globalTemperatureScale); sum.add(c.globalMoistureScale); sum.add(c.planetRotationSign);
    sum.add(c.season.amplitude); sum.add(c.season.wetSeasonBias); sum.add(c.season.coldSeasonBias);
    sum.add(static_cast<std::uint64_t>(w.cells.size()));
    for (const auto& cell : w.cells) {
        sum.add(cell.elevation); sum.add(static_cast<int>(cell.biome)); sum.add(cell.sea);
        sum.add(cell.river); sum.add(cell.riverOut); sum.add(cell.riverSize);
        sum.add(cell.drainOut); sum.add(cell.drainSize); sum.add(cell.moisture);
        sum.add(cell.fertility); sum.add(cell.temperature); sum.add(static_cast<int>(cell.climate));
    }
    const auto field = [&](const auto& values) {
        sum.add(static_cast<std::uint64_t>(values.size()));
        for (const auto& value : values) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) sum.add(static_cast<std::int64_t>(value));
            else if constexpr (std::is_same_v<T, generation::ClimateVector>) {
                sum.add(value.x); sum.add(value.y);
            } else for (auto weight : value) sum.add(weight);
        }
    };
    field(w.continentalField); field(w.distanceToCoast); field(w.initialLandMask);
    field(w.upliftField); field(w.riftField); field(w.faultField); field(w.geologyRegion);
    field(w.macroHeightField); field(w.rockTypeField); field(w.erosionResistanceField);
    field(w.soilParentMaterialField); field(w.permeabilityField); field(w.thermallyRelaxedHeightField);
    field(w.hydrologicallyCorrectedHeightField); field(w.basinIdField); field(w.spillPointField);
    field(w.flowDirectionField); field(w.flowAccumulationField); field(w.riverDischargeField);
    field(w.riverSourceField); field(w.lakeRegionField); field(w.lakeLevelField); field(w.lakeDepthField);
    field(w.waterfallField); field(w.sedimentPotentialField); field(w.erosionField);
    field(w.floodplainPotentialField); field(w.deltaPotentialField); field(w.erodedHeightField);
    field(w.baseTemperatureField); field(w.prevailingWindField); field(w.windStrengthField);
    field(w.windVariabilityField); field(w.precipitationField); field(w.airMoistureField);
    field(w.rainShadowField); field(w.oceanCurrentField); field(w.seaSurfaceTemperatureBiasField);
    field(w.coastalClimateBiasField); field(w.temperatureField); field(w.annualRainfallField);
    field(w.humidityField); field(w.seasonalityField); field(w.winterRainField); field(w.summerRainField);
    field(w.drySeasonStrengthField); field(w.soilTypeField); field(w.soilFertilityField);
    field(w.soilDrainageField); field(w.soilOrganicPotentialField);
    field(w.primaryBiomeSuitabilityField); field(w.secondaryBiomeSuitabilityField);
    field(w.biomeTransitionField); field(w.materialSuitabilityField);
    field(w.waterPaintField);
    sum.add(std::uint64_t(std::lround(double(w.riverShare) * 1000.0)));
    // The graph's source hash alone does not cover edits to individual reaches.
    std::vector<std::uint8_t> bytes;
    if (const auto result = encodeHydrologyGraphPayload(graph, bytes); !result)
        throw std::runtime_error(result.detail);
    sum.add(graph.worldSeed); sum.add(graph.sourceFingerprint);
    sum.add(graph.macroWidth); sum.add(graph.macroHeight); sum.add(graph.macroCellMetres);
    sum.add(static_cast<std::uint64_t>(graph.nodes.size()));
    sum.add(static_cast<std::uint64_t>(graph.segments.size()));
    sum.add(static_cast<std::uint64_t>(graph.waterBodies.size()));
    sum.add(static_cast<std::uint64_t>(graph.spatialPages.size()));
    sum.add(core::hashBytes(bytes.data(), bytes.size()));
    return sum.value();
}

// Identical order and size rules for writer and reader. Provenance pointers and
// work counters are not persisted; exact Fixed refinement values ARE persisted.
template<class Page, class Function>
bool channels(Page& p, Function&& f) {
    const auto n = p.base.sampleCount();
    return f(p.base.heightQuantized, n, false) && f(p.base.waterBodyId, n, false) &&
        f(p.base.watershedId, n, false) && f(p.base.buildability, n, true) &&
        f(p.base.walkability, n, false) && f(p.water.surfaceQuantized, n, false) &&
        f(p.water.waterBodyId, n, false) && f(p.water.riverId, n, false) &&
        f(p.water.shoreDecimetres, n, false) && f(p.water.coverage, n, false) &&
        f(p.water.flowX, n, false) && f(p.water.flowY, n, false) && f(p.water.estuary, n, false) &&
        f(p.large.deltaQuantized, n, false) && f(p.medium.deltaQuantized, 0, false) &&
        f(p.materials, std::size_t(p.materialWidth) * p.materialWidth * kMaterialCount, false) &&
        f(p.envMasks, std::size_t(p.materialWidth) * p.materialWidth * 8u, true) &&
        f(p.envZones, std::size_t(p.materialWidth) * p.materialWidth * 4u, true) &&
        f(p.refinementDepth, n, false) && f(p.refinementAshore, n, false) &&
        f(p.featureCells, p.base.sampleMetres == 16 && p.base.padding >= 1 ? 1024u : 0u, false);
}

template<class Tile>
bool sameLayout(const Tile& a, const Tile& b) {
    return a.key == b.key && a.width == b.width && a.height == b.height &&
        a.padding == b.padding && a.sampleMetres == b.sampleMetres;
}

// Private staging directory avoids collisions between workers and processes.
// Never remove a good destination if publication fails (e.g. read-only disk).
CacheResult publish(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return {CacheStatus::IoError, ec.message()};
    static std::atomic<std::uint64_t> sequence{0};
    const auto temporary = path.string() + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
        std::to_string(sequence++);
    if (!std::filesystem::create_directory(temporary, ec))
        return {CacheStatus::IoError, "cannot create private page staging directory: " + ec.message()};
    struct Cleanup {
        std::filesystem::path directory;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    } cleanup{temporary};
    const auto staging = cleanup.directory / "page.bin";
    std::ofstream out(staging, std::ios::binary | std::ios::trunc);
    if (!out) return {CacheStatus::IoError, "cannot open page staging file"};
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    out.close();
    if (!out) return {CacheStatus::IoError, "cannot write page staging file"};
    std::filesystem::rename(staging, path, ec);
    return ec ? CacheResult{CacheStatus::IoError, ec.message()} : CacheResult::ok();
}
} // namespace

BakedPageCache::BakedPageCache(std::filesystem::path root, const generation::WorldMapData& world,
                               const HydrologyGraph& graph, HsimQuantisation quantisation,
                               std::uint16_t padding)
    : world_(world), graph_(graph), quantisation_(quantisation), padding_(padding), root_(std::move(root)) {}

void BakedPageCache::prepare() {
    std::call_once(prepared_, [&] {
        identity_ = {world_.seed, fingerprint(world_, graph_)};
        directory_ = root_ / ("v" + std::to_string(kBakedPageCacheVersion)) /
            (std::to_string(identity_.worldSeed) + "-" + std::to_string(identity_.generationFingerprint)) /
            ("p" + std::to_string(padding_) + "-" + std::to_string(quantisation_.low.raw) +
             "-" + std::to_string(quantisation_.high.raw));
    });
}

std::filesystem::path BakedPageCache::file(TileKey key, std::uint64_t ground) {
    prepare();
    std::string name = std::to_string(key.x) + "_" + std::to_string(key.y);
    if (ground) {
        char hex[20];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(ground));
        name += std::string("-g") + hex;
    }
    return directory_ / ("h" + std::to_string(4 << key.level)) / (name + ".bin");
}

bool BakedPageCache::layout(TileKey key, BakedPage& p) const {
    if ((key.level != 2 && key.level != 4) || padding_ > 16 || !quantisation_.valid()) return false;
    const int step = 4 << key.level;
    const auto init = [&](auto& tile) {
        tile.key = key;
        tile.width = tile.height = interiorSamples(step);
        tile.padding = padding_; tile.sampleMetres = step;
    };
    init(p.base); init(p.water); init(p.large);
    p.base.elevationMin = p.water.elevationMin = quantisation_.low;
    p.base.elevationMax = p.water.elevationMax = quantisation_.high;
    p.large.level = ResidualLevel::Large;
    p.materialWidth = p.base.width + 2 * padding_;
    p.materialMetres = step;
    return true;
}

std::vector<std::uint8_t> BakedPageCache::prefix(TileKey key, std::uint64_t ground) const {
    std::vector<std::uint8_t> bytes{'A','S','R','P','A','G','E','1'};
    writeUnsigned(bytes, kBakedPageCacheVersion);
    writeUnsigned(bytes, kBakedPageGenerationVersion);
    writeUnsigned(bytes, identity_.worldSeed);
    writeUnsigned(bytes, identity_.generationFingerprint);
    writeInteger(bytes, key.x); writeInteger(bytes, key.y); writeUnsigned(bytes, key.level);
    writeUnsigned(bytes, padding_);
    writeInteger(bytes, quantisation_.low.raw); writeInteger(bytes, quantisation_.high.raw);
    // Only edited ground says so: the generator's own pages keep the prefix
    // they have always had, and every cache already on disk stays good.
    if (ground) { bytes.push_back('G'); writeUnsigned(bytes, ground); }
    return bytes;
}

CacheResult BakedPageCache::read(TileKey key, BakedPage& into, std::uint64_t ground) {
    try {
        BakedPage fresh;
        if (!layout(key, fresh)) return {CacheStatus::InvalidArgument, "not a supported persistent page"};
        const auto path = file(key, ground);
        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return {ec ? CacheStatus::IoError : CacheStatus::NotFound, ec ? ec.message() : "page absent"};
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) return {CacheStatus::IoError, "cannot open page cache"};
        const auto size = in.tellg();
        const auto expected = prefix(key, ground);
        if (size < static_cast<std::streamoff>(expected.size() + sizeof(std::uint64_t)) ||
            size > static_cast<std::streamoff>(kMaxPageBytes))
            return {CacheStatus::Corrupt, "page size out of bounds"};
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        in.seekg(0);
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in) return {CacheStatus::Corrupt, "truncated page cache"};
        if (!std::equal(expected.begin(), expected.end(), bytes.begin()))
            return {CacheStatus::Incompatible, "page world/version/layout mismatch"};
        const auto end = bytes.size() - sizeof(std::uint64_t);
        auto cursor = end;
        std::uint64_t checksum = 0;
        if (!readUnsigned(bytes, cursor, checksum) || core::hashBytes(bytes.data(), end) != checksum)
            return {CacheStatus::Corrupt, "page checksum mismatch"};
        bytes.resize(end);
        cursor = expected.size();
        const bool ok = channels(fresh, [&](auto& values, std::size_t count, bool optional) {
            using T = typename std::decay_t<decltype(values)>::value_type;
            constexpr auto wireBytes = std::is_same_v<T, core::Fixed> ? sizeof(std::int64_t) : sizeof(T);
            std::uint32_t length = 0;
            if (!readUnsigned(bytes, cursor, length) ||
                (length != count && !(optional && length == 0)) ||
                length > (bytes.size() - cursor) / wireBytes) return false;
            values.resize(length);
            for (auto& value : values) {
                if constexpr (std::is_same_v<T, core::Fixed>) {
                    if (!readInteger(bytes, cursor, value.raw)) return false;
                } else if (!readInteger(bytes, cursor, value)) return false;
            }
            return true;
        });
        if (!ok || cursor != bytes.size()) return {CacheStatus::Corrupt, "invalid page channels"};
        fresh.world = &world_; fresh.graph = &graph_; // restore live provenance, never disk pointers
        fresh.ground = ground;
        into = std::move(fresh);
        return CacheResult::ok();
    } catch (const std::exception& e) { return {CacheStatus::IoError, e.what()}; }
}

CacheResult BakedPageCache::write(const BakedPage& page) {
    try {
        BakedPage expected;
        if (!layout(page.base.key, expected) || !sameLayout(page.base, expected.base) ||
            !sameLayout(page.water, expected.water) || !sameLayout(page.large, expected.large) ||
            page.large.level != ResidualLevel::Large || page.base.elevationMin != quantisation_.low ||
            page.base.elevationMax != quantisation_.high || page.water.elevationMin != quantisation_.low ||
            page.water.elevationMax != quantisation_.high || page.materialWidth != expected.materialWidth ||
            page.materialMetres != expected.materialMetres || page.world != &world_ || page.graph != &graph_)
            return {CacheStatus::InvalidArgument, "incompatible baked page"};
        const auto path = file(page.base.key, page.ground);
        auto bytes = prefix(page.base.key, page.ground);
        const bool ok = channels(page, [&](const auto& values, std::size_t count, bool optional) {
            using T = typename std::decay_t<decltype(values)>::value_type;
            if (values.size() != count && !(optional && values.empty())) return false;
            writeUnsigned(bytes, static_cast<std::uint32_t>(values.size()));
            for (const auto& value : values) {
                if constexpr (std::is_same_v<T, core::Fixed>) writeInteger(bytes, value.raw);
                else writeInteger(bytes, value);
            }
            return true;
        });
        if (!ok || bytes.size() + sizeof(std::uint64_t) > kMaxPageBytes)
            return {CacheStatus::InvalidArgument, "invalid baked page channels"};
        writeUnsigned(bytes, core::hashBytes(bytes.data(), bytes.size()));
        return publish(path, bytes);
    } catch (const std::exception& e) { return {CacheStatus::IoError, e.what()}; }
}

} // namespace world::streaming
