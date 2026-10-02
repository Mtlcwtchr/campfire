#include "engine/world_source/bake.hpp"

#include <chrono>
#include <cmath>
#include <queue>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/core/progress.hpp"
#include "engine/world_source/world_source.hpp"
#include "engine/world_store/atomic_file.hpp"

namespace engine::world_source {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

const RasterDesc* heightOf(const Schema& schema) {
    for (const auto& r : schema.rasters)
        if (r.kind == RasterKind::Height) return &r;
    return nullptr;
}

RasterDesc flowDesc() {
    RasterDesc flow;
    flow.name = "flow";
    flow.file = "raster/flow.png";
    flow.type = SampleType::U8;
    flow.kind = RasterKind::Control;
    flow.channels = {ChannelDesc{"flow_log2", 0, 32, 0, Interpolation::Bilinear, true, 1, {"hydrology", "ecology"}}};
    return flow;
}

constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};

} // namespace

std::optional<std::uint64_t> heightKeyOf(const std::filesystem::path& root, std::string* why) {
    const auto source = WorldSource::open(root, why);
    if (!source) return std::nullopt;
    const auto* height = heightOf(source->schema());
    if (!height) { fail(why, "the source has no height"); return std::nullopt; }
    std::uint64_t key = 0xBA4EDull;
    key = mix(key, std::uint64_t(source->schema().world.widthMetres));
    key = mix(key, std::uint64_t(source->schema().world.heightMetres));
    for (const auto& [chunk, record] : source->chunks()) {
        const auto it = record.layers.find(height->name);
        if (it == record.layers.end()) continue;
        key = mix(key, std::uint64_t(chunk.x) * 2654435761ull + std::uint64_t(chunk.y));
        key = mix(key, it->second);
    }
    return key;
}

bool bakeCurrent(const std::filesystem::path& source, const std::filesystem::path& baked) {
    const auto key = heightKeyOf(source);
    if (!key || !WorldSource::exists(baked)) return false;
    const auto bytes = world_store::readFileBytes(baked / "from.json");
    if (!bytes) return false;
    const auto j = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    return !j.is_discarded() && j.value("height_key", std::uint64_t(0)) == *key;
}

std::optional<BakeReport> bakeSource(const std::filesystem::path& root, const std::filesystem::path& bakedRoot,
                                     const BakeOptions& options, std::string* why) {
    BakeReport report;
    if (bakeCurrent(root, bakedRoot)) return report;
    const auto began = std::chrono::steady_clock::now();
    core::progress("bake: reading the height");
    const auto key = heightKeyOf(root, why);
    auto source = WorldSource::open(root, why);
    if (!key || !source) return std::nullopt;
    const Schema& schema = source->schema();
    const RasterDesc* height = heightOf(schema);
    const std::int64_t W = schema.world.samplesX(), H = schema.world.samplesY();
    const std::size_t N = std::size_t(W) * std::size_t(H);
    // Decimetres, the world whole: the open sea is its default, land where a
    // chunk holds it. (A breach runs to wherever its water leaves, which is
    // any distance away, so this cannot be done chunk by chunk.)
    const std::int32_t seaDm = std::int32_t(std::lround(height->channels[0].defaultValue * 10.0));
    std::vector<std::int32_t> h(N, seaDm);
    // Lakes the source marks (a flags layer with a "lake" bit): basins
    // somebody meant, never breached.
    const RasterDesc* water = nullptr;
    int lakeBit = 0;
    for (const auto& r : schema.rasters)
        if (r.kind == RasterKind::Flags)
            if (const auto bit = r.bits.find("lake"); bit != r.bits.end()) { water = &r; lakeBit = bit->second; break; }
    std::vector<std::uint8_t> lake(water ? N : 0, 0);
    std::vector<ChunkKey> land;
    for (const auto& [chunk, record] : source->chunks()) {
        if (!record.layers.count(height->name)) continue;
        land.push_back(chunk);
        const auto read = source->read(chunk, why);
        if (!read) return std::nullopt;
        const Tile tile = source->tile(*read, height->name);
        const Tile lakes = water ? source->tile(*read, water->name) : Tile{};
        for (std::int64_t y = 0; y < kChunkSamples; ++y)
            for (std::int64_t x = 0; x < kChunkSamples; ++x) {
                const std::int64_t gx = chunk.x * kChunkSamples + x, gy = chunk.y * kChunkSamples + y;
                if (gx >= W || gy >= H) continue;
                h[std::size_t(gy * W + gx)] = std::int32_t(std::lround(height->decode(0, tile.at(x, y, 0)) * 10.0));
                if (water) lake[std::size_t(gy * W + gx)] = std::uint8_t((lakes.at(x, y, 0) >> lakeBit) & 1u);
            }
    }

    // --- breaching, over a priority flood from the coast inwards ----------
    core::progress("bake: opening the drainage");
    constexpr std::int8_t kUnseen = -2, kOutlet = -1;
    std::vector<std::int8_t> parent(N, kUnseen);   // which neighbour a sample drains to
    std::vector<std::uint8_t> changed(N, 0);
    using Front = std::pair<std::int32_t, std::uint32_t>;
    std::priority_queue<Front, std::vector<Front>, std::greater<Front>> queue;
    for (std::int64_t y = 0; y < H; ++y)
        for (std::int64_t x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y * W + x);
            if (h[i] <= 0) continue;
            ++report.landSamples;
            bool coast = x == 0 || y == 0 || x == W - 1 || y == H - 1;
            for (int d = 0; d < 8 && !coast; ++d) coast = h[std::size_t((y + kDy[d]) * W + (x + kDx[d]))] <= 0;
            if (!coast) continue;
            parent[i] = kOutlet;
            queue.push({h[i], std::uint32_t(i)});
        }
    const std::int32_t cap = std::int32_t(std::lround(options.maxBreachMetres * 10.0));
    const auto up = [&](std::size_t i) -> std::int64_t {
        const int d = parent[i];
        if (d < 0) return -1;
        return std::int64_t(i) + kDy[d] * W + kDx[d];
    };
    std::vector<std::uint32_t> order;
    order.reserve(std::size_t(report.landSamples));
    while (!queue.empty()) {
        const auto [level, c] = queue.top();
        queue.pop();
        order.push_back(c);
        const std::int64_t cx = std::int64_t(c) % W, cy = std::int64_t(c) / W;
        for (int d = 0; d < 8; ++d) {
            const std::int64_t nx = cx + kDx[d], ny = cy + kDy[d];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const std::size_t n = std::size_t(ny * W + nx);
            if (parent[n] != kUnseen || h[n] <= 0) continue;
            parent[n] = std::int8_t((d + 4) % 8);   // back to c
            if (h[n] < h[c]) {
                // A hollow: its floor is below the lowest way out found so
                // far. Its way out - c and on down to the coast - is cut to
                // the floor, unless the floor is too far below the rim.
                if (h[c] - h[n] <= cap && !(water && lake[n])) {
                    ++report.breached;
                    for (std::int64_t p = c; p >= 0 && h[std::size_t(p)] > h[n]; p = up(std::size_t(p))) {
                        h[std::size_t(p)] = h[n];
                        changed[std::size_t(p)] = 1;
                        ++report.lowered;
                    }
                } else {
                    ++report.kept;
                }
            }
            queue.push({h[n], std::uint32_t(n)});
        }
    }

    // --- flow: every sample's water down its way out ------------------------
    core::progress("bake: flow");
    std::vector<std::uint32_t> flow(N, 0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        flow[*it] += 1;
        if (const auto p = up(*it); p >= 0) flow[std::size_t(p)] += flow[*it];
    }

    // --- written: the chunks that hold land, and nothing else ----------------
    core::progress("bake: writing", 0, std::int64_t(land.size()));
    std::error_code ec;
    std::filesystem::remove_all(bakedRoot, ec);
    Schema bakedSchema;
    bakedSchema.world = schema.world;
    RasterDesc bakedHeight = *height;
    bakedHeight.name = "height";
    const RasterDesc flowLayer = flowDesc();
    bakedSchema.rasters = {bakedHeight, flowLayer};
    auto baked = WorldSource::create(bakedRoot, bakedSchema, why);
    if (!baked) return std::nullopt;
    std::int64_t written = 0;
    for (const auto& chunk : land) {
        core::progressStep(++written);
        const auto read = source->read(chunk, why);
        if (!read) return std::nullopt;
        Chunk out;
        Tile heights = source->tile(*read, height->name);
        heights.expand();
        Tile flows = defaultTile(flowLayer);
        flows.expand();
        for (std::int64_t y = 0; y < kChunkSamples; ++y)
            for (std::int64_t x = 0; x < kChunkSamples; ++x) {
                const std::int64_t gx = chunk.x * kChunkSamples + x, gy = chunk.y * kChunkSamples + y;
                if (gx >= W || gy >= H) continue;
                const std::size_t i = std::size_t(gy * W + gx);
                const std::size_t k = std::size_t(y * kChunkSamples + x);
                // Unchanged samples keep the very number the source has.
                if (changed[i]) heights.values[k] = height->encode(0, double(h[i]) / 10.0);
                if (flow[i] > 1) flows.values[k] = flowLayer.encode(0, std::log2(double(flow[i])));
            }
        heights.settle();
        flows.settle();
        out.rasters["height"] = std::move(heights);
        out.rasters["flow"] = std::move(flows);
        if (!baked->write(chunk, out, why)) return std::nullopt;
    }
    if (!baked->commit(why)) return std::nullopt;
    const nlohmann::json from{{"height_key", *key}, {"max_breach_m", options.maxBreachMetres},
                              {"breached", report.breached}, {"kept", report.kept}, {"lowered", report.lowered}};
    if (!world_store::writeFileAtomic(bakedRoot / "from.json", from.dump(1), why)) return std::nullopt;
    report.rebuilt = true;
    report.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    return report;
}

} // namespace engine::world_source
