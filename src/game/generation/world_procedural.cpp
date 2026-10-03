#include "game/generation/world_procedural.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <thread>

#include "engine/core/progress.hpp"
#include "engine/world_source/transfer.hpp"
#include "engine/world_source/world_source.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_noise.hpp"

namespace generation {
namespace {
namespace ws = engine::world_source;

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

// The canonical rasters: what an import of loose pictures is stored as, and
// so what the phases make (their numbers do not depend on the world's extent).
const ws::Schema& canonical() {
    static const ws::Schema schema = ws::canonicalSchema(ws::WorldExtent{double(kRegionMetres), double(kRegionMetres), 256, 32768});
    return schema;
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The rectangle of whole regions around a list of them.
struct RegionBox {
    std::int32_t x0 = 0, y0 = 0, w = 0, h = 0;
};
RegionBox boxOf(const RegionList& regions) {
    if (regions.empty()) return {};
    std::int32_t x0 = regions.front().first, y0 = regions.front().second, x1 = x0, y1 = y0;
    for (const auto& [x, y] : regions) {
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    return RegionBox{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// Catmull-Rom weights at t of the way from p1 to p2.
std::array<double, 4> cubic(double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2};
}

double smoothstep(double a, double b, double v) {
    const double t = std::clamp((v - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Value noise with a smooth fade between its lattice points, -1..1, lattice
// points `scale` samples apart. On the world's own sample grid, so two phases
// run over neighbouring selections meet without a seam.
double wander(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale) {
    scale = std::max<std::int64_t>(2, scale);
    const std::int64_t cx = floorDiv(x, scale), cy = floorDiv(y, scale);
    double fx = double(x - cx * scale) / double(scale), fy = double(y - cy * scale) / double(scale);
    fx = fx * fx * (3.0 - 2.0 * fx);
    fy = fy * fy * (3.0 - 2.0 * fy);
    const auto v = [&](std::int64_t a, std::int64_t b) { return double(valueNoise(seed, std::int32_t(a), std::int32_t(b))); };
    const double top = v(cx, cy) + (v(cx + 1, cy) - v(cx, cy)) * fx;
    const double bottom = v(cx, cy + 1) + (v(cx + 1, cy + 1) - v(cx, cy + 1)) * fx;
    return (top + (bottom - top) * fy) / 511.5 - 1.0;
}

// Rows shared out over a few threads: every sample is a function of where it
// is and nothing else, so which thread makes it changes nothing.
std::size_t workers() { return std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8); }
template <class Row>
void forRows(std::int64_t count, Row&& row) {
    std::atomic<std::int64_t> next{0};
    const auto work = [&](std::size_t worker) {
        for (std::int64_t j; (j = next.fetch_add(1, std::memory_order_relaxed)) < count;) {
            row(j, worker);
            if (worker == 0 && (j & 31) == 0) core::progressStep(j);
        }
    };
    std::vector<std::thread> pool;
    for (std::size_t w = 1; w < workers(); ++w) pool.emplace_back(work, w);
    work(0);
    for (auto& t : pool) t.join();
}
// What a thread counted as it went.
struct Tally {
    std::int64_t land = 0;
    std::int32_t highest = 0;
};

} // namespace

RegionList phaseRegions(const WorldLayout& layout, const RegionList& regions) {
    RegionList out;
    if (regions.empty()) {
        for (std::int32_t y = 0; y < layout.regionsY; ++y)
            for (std::int32_t x = 0; x < layout.regionsX; ++x) out.emplace_back(x, y);
        return out;
    }
    std::set<std::pair<std::int32_t, std::int32_t>> seen;
    for (const auto& r : regions)
        if (layout.inBounds(r.first, r.second) && seen.insert(r).second) out.push_back(r);
    return out;
}

// --- phase 1: heights ------------------------------------------------------------

std::optional<SourceRasters> generateSourceHeights(const WorldLayout& layout, const RegionList& regions,
                                                   const ProceduralHeights& dials, std::int32_t sampleMetres,
                                                   std::string* why) {
    if (sampleMetres < 16 || kRegionMetres % sampleMetres != 0) {
        fail(why, "the source's samples do not divide a region");
        return std::nullopt;
    }
    if (layout.regions.size() != std::size_t(layout.regionsX) * std::size_t(layout.regionsY)) {
        fail(why, "the layout's regions do not fill it");
        return std::nullopt;
    }
    const bool whole = regions.empty();
    const RegionList which = phaseRegions(layout, regions);
    if (which.empty()) {
        fail(why, "no region to generate");
        return std::nullopt;
    }
    // The layout as the generator would hold it with these regions made
    // afresh - a copy: nothing of the person's world changes here. What is
    // imported is not read; the painted layers over the rectangle are.
    WorldLayout copy = layout;
    copy.imported.reset();
    if (whole) {
        // One planet over everything, with this seed for its plates.
        for (Region& r : copy.regions) r.stage = RegionStage::Full;
        copy.seed = dials.seed;
        generateWholeWorld(copy, dials.settings, dials.seed, dials.ownSeeds);
    } else {
        generateRegions(copy, which, dials.settings, dials.seed, dials.ownSeeds);
    }
    const Region& first = copy.at(which.front().first, which.front().second);
    if (first.source < 0 || std::size_t(first.source) >= copy.generations.size()) {
        fail(why, "the generator was given nothing to run");
        return std::nullopt;
    }
    const Generation g = copy.generations[std::size_t(first.source)];

    // Run as world_compose runs a generation (runGeneration): its rectangle,
    // placed where it is - but only to the primary stage. The heights are all
    // this phase is for; climate, drainage and water are later phases.
    WorldLayout sub = generationLayout(copy, g);
    sub.latitude.rowOffset = 0;
    WorldMapParams params = paramsFor(sub);
    params.latitude = sub.latitude;
    params.originX = g.x * kCellsPerRegion;
    params.originY = g.y * kCellsPerRegion - copy.latitude.rowOffset;
    params.stage = GenerationStage::Primary;
    // No finer than the samples it is read at: anything finer would be
    // built only to be thrown away.
    const std::int64_t widthMetres = std::int64_t(g.w) * kRegionMetres, heightMetres = std::int64_t(g.h) * kRegionMetres;
    if (foundationStepFor(widthMetres, heightMetres) < sampleMetres) params.foundationStep = sampleMetres;
    core::progress("generating heights: the generator");
    const WorldMapData world = generateWorldMap(params);
    if (!world.terrainFoundation || world.terrainFoundation->columns < 2 || world.terrainFoundation->rows < 2) {
        fail(why, "the generator made no ground");
        return std::nullopt;
    }

    // The ground onto the source's grid: sample centres half a sample in,
    // Catmull-Rom through the foundation's lattice, held between the four
    // nodes around it so it invents no island and no pit.
    const TerrainFoundation& f = *world.terrainFoundation;
    const auto& plane = f.heightDm[std::size_t(TerrainStage::Slopes)];
    const ws::RasterDesc& heightDesc = canonical().rasters[0];
    const std::uint16_t openSea = heightDesc.defaultStored(0);
    const std::int64_t regionSamples = kRegionMetres / sampleMetres;
    SourceRasters out;
    out.regionX = g.x;
    out.regionY = g.y;
    out.regionsW = g.w;
    out.regionsH = g.h;
    out.samplesX = std::int64_t(g.w) * regionSamples;
    out.samplesY = std::int64_t(g.h) * regionSamples;
    out.sampleMetres = sampleMetres;
    out.regions = which;
    out.height.assign(std::size_t(out.samplesX * out.samplesY), openSea);
    const double step = double(f.step);
    struct Axis {
        std::int32_t index = 0;
        std::array<double, 4> weight{};
    };
    const auto axis = [&](std::int64_t i) {
        const double u = (double(i) + 0.5) * double(sampleMetres) / step;
        const double base = std::floor(u);
        Axis a;
        a.index = std::int32_t(base);
        a.weight = cubic(u - base);
        return a;
    };
    std::vector<Axis> columns(std::size_t(out.samplesX));
    for (std::int64_t i = 0; i < out.samplesX; ++i) columns[std::size_t(i)] = axis(i);
    const auto node = [&](std::int32_t cx, std::int32_t cy) {
        cx = std::clamp(cx, 0, f.columns - 1);
        cy = std::clamp(cy, 0, f.rows - 1);
        return double(plane[std::size_t(cy) * std::size_t(f.columns) + std::size_t(cx)]);
    };
    core::progress("generating heights: onto the source's grid", 0, out.samplesY);
    std::vector<Tally> tallies(workers());
    forRows(out.samplesY, [&](std::int64_t j, std::size_t worker) {
        Tally& tally = tallies[worker];
        const Axis row = axis(j);
        for (std::int64_t i = 0; i < out.samplesX; ++i) {
            const Axis& col = columns[std::size_t(i)];
            double sum = 0, low = 1e30, high = -1e30;
            for (int b = 0; b < 4; ++b) {
                double line = 0;
                for (int a = 0; a < 4; ++a) {
                    const double v = node(col.index - 1 + a, row.index - 1 + b);
                    line += col.weight[std::size_t(a)] * v;
                    if ((a == 1 || a == 2) && (b == 1 || b == 2)) { low = std::min(low, v); high = std::max(high, v); }
                }
                sum += row.weight[std::size_t(b)] * line;
            }
            const double dm = std::clamp(sum, low, high);
            // The importer's rule for a picture: at or below the sea is the
            // open sea (its default, kept as nothing), land a metre or more.
            if (dm <= 0.0) continue;
            const double metres = std::max(1.0, dm / 10.0);
            out.height[std::size_t(j * out.samplesX + i)] = heightDesc.encode(0, metres);
            ++tally.land;
            tally.highest = std::max(tally.highest, std::int32_t(metres));
        }
    });
    for (const Tally& t : tallies) {
        out.landSamples += t.land;
        out.highestMetres = std::max(out.highestMetres, t.highest);
    }
    return out;
}

// --- phase 2: control maps -------------------------------------------------------

std::optional<SourceRasters> generateSourceControls(const std::filesystem::path& sourceRoot, const WorldLayout& layout,
                                                    const RegionList& regions, const ProceduralControls& dials,
                                                    std::string* why) {
    if (!ws::WorldSource::exists(sourceRoot)) {
        fail(why, "the world's source holds no heights: import or generate them first");
        return std::nullopt;
    }
    auto source = ws::WorldSource::open(sourceRoot, why);
    if (!source) return std::nullopt;
    const ws::RasterDesc* heightDesc = nullptr;
    for (const auto& r : source->schema().rasters)
        if (r.kind == ws::RasterKind::Height) { heightDesc = &r; break; }
    if (!heightDesc) {
        fail(why, "the world's source has no height raster");
        return std::nullopt;
    }
    const std::int64_t sm = std::int64_t(source->schema().world.sampleMetres);
    if (sm <= 0 || kRegionMetres % sm != 0) {
        fail(why, "the source's samples do not divide a region");
        return std::nullopt;
    }
    const std::int64_t regionSamples = kRegionMetres / sm;
    const std::int64_t chunkSamples = source->chunkSamples();
    const std::int64_t perRegion = std::max<std::int64_t>(1, regionSamples / chunkSamples);
    // Which regions hold height at all (what ImportedSource::holds says).
    std::set<std::pair<std::int32_t, std::int32_t>> held;
    for (const auto& [key, record] : source->chunks())
        if (record.layers.count(heightDesc->name))
            held.insert({std::int32_t(floorDiv(key.x, perRegion)), std::int32_t(floorDiv(key.y, perRegion))});
    RegionList which;
    for (const auto& r : phaseRegions(layout, regions))
        if (held.count(r)) which.push_back(r);
    if (which.empty()) {
        fail(why, held.empty() ? "the world's source holds no heights: import or generate them first"
                               : "none of those regions holds heights: import or generate them first");
        return std::nullopt;
    }
    const RegionBox box = boxOf(which);
    SourceRasters out;
    out.regionX = box.x0;
    out.regionY = box.y0;
    out.regionsW = box.w;
    out.regionsH = box.h;
    out.samplesX = std::int64_t(box.w) * regionSamples;
    out.samplesY = std::int64_t(box.h) * regionSamples;
    out.sampleMetres = std::int32_t(sm);
    out.regions = which;
    const std::int64_t sx0 = std::int64_t(box.x0) * regionSamples, sy0 = std::int64_t(box.y0) * regionSamples;
    const std::size_t count = std::size_t(out.samplesX * out.samplesY);

    // The heights under the rectangle - of every region in it that has any,
    // chosen or not: the coast beside a chosen region is the coast it is near.
    core::progress("control maps: reading the heights");
    const float seaMetres = float(heightDesc->channels[0].defaultValue);
    std::vector<float> height(count, std::min(seaMetres, -1.0f));
    for (const auto& [key, record] : source->chunks()) {
        if (!record.layers.count(heightDesc->name)) continue;
        const std::int64_t ox = key.x * chunkSamples, oy = key.y * chunkSamples;
        if (ox + chunkSamples <= sx0 || oy + chunkSamples <= sy0 || ox >= sx0 + out.samplesX || oy >= sy0 + out.samplesY)
            continue;
        const auto chunk = source->read(key, why);
        if (!chunk) return std::nullopt;
        const ws::Tile tile = source->tile(*chunk, heightDesc->name);
        const std::int64_t x0 = std::max(ox, sx0), x1 = std::min(ox + chunkSamples, sx0 + out.samplesX);
        const std::int64_t y0 = std::max(oy, sy0), y1 = std::min(oy + chunkSamples, sy0 + out.samplesY);
        for (std::int64_t y = y0; y < y1; ++y)
            for (std::int64_t x = x0; x < x1; ++x)
                height[std::size_t((y - sy0) * out.samplesX + (x - sx0))] =
                        float(heightDesc->decode(0, tile.at(x - ox, y - oy, 0)));
    }

    // How far each sample of land is from the sea, in thirds of a sample
    // (a chamfer: three across, four diagonally).
    core::progress("control maps: the coast");
    constexpr std::uint16_t kFar = 65535;
    std::vector<std::uint16_t> coast(count, 0);
    for (std::size_t i = 0; i < count; ++i) coast[i] = height[i] > 0.0f ? kFar : 0;
    const std::int64_t W = out.samplesX, H = out.samplesY;
    const auto relax = [&](std::int64_t x, std::int64_t y, std::int64_t nx, std::int64_t ny, int step) {
        if (nx < 0 || ny < 0 || nx >= W || ny >= H) return;
        auto& here = coast[std::size_t(y * W + x)];
        here = std::uint16_t(std::min<int>(here, int(coast[std::size_t(ny * W + nx)]) + step));
    };
    for (std::int64_t y = 0; y < H; ++y)
        for (std::int64_t x = 0; x < W; ++x) {
            relax(x, y, x - 1, y, 3); relax(x, y, x, y - 1, 3);
            relax(x, y, x - 1, y - 1, 4); relax(x, y, x + 1, y - 1, 4);
        }
    for (std::int64_t y = H - 1; y >= 0; --y)
        for (std::int64_t x = W - 1; x >= 0; --x) {
            relax(x, y, x + 1, y, 3); relax(x, y, x, y + 1, 3);
            relax(x, y, x + 1, y + 1, 4); relax(x, y, x - 1, y + 1, 4);
        }

    // The four channels, sample by sample. Their middles are the dials -
    // 0.5 is "as the generator would have it" for moisture and erosion - and
    // the ground moves them: wetter near the coast and up the first slopes,
    // mountains where it is high or steep, forest where it is wet and below
    // the tree line; a wander of noise at two scales keeps any of them from
    // being a function of height alone.
    const ws::RasterDesc& controlDesc = canonical().rasters[1];
    std::array<std::uint8_t, 4> rest{};
    for (std::size_t c = 0; c < 4; ++c) rest[c] = std::uint8_t(controlDesc.defaultStored(c));
    out.control.resize(count * 4);
    const double variation = std::clamp(double(dials.variation), 0.0, 2.0);
    const double moistMiddle = std::clamp(0.5 + (double(dials.rainfallPercent) - 100.0) / 300.0, 0.05, 0.95);
    const double erosionMiddle = std::clamp(double(dials.erosionPasses) / 6.0, 0.0, 1.0);
    const auto scale = [&](double metres) { return std::int64_t(std::llround(metres / double(sm))); };
    const std::uint64_t seed = dials.seed ^ 0xC0A7201ull;
    core::progress("control maps: moisture, forest, mountains, erosion", 0, H);
    std::vector<Tally> tallies(workers());
    forRows(H, [&](std::int64_t y, std::size_t worker) {
        Tally& tally = tallies[worker];
        for (std::int64_t x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y * W + x);
            std::uint8_t* px = &out.control[i * 4];
            const double h = height[i];
            if (h <= 0.0) {
                std::copy(rest.begin(), rest.end(), px);
                continue;
            }
            ++tally.land;
            tally.highest = std::max(tally.highest, std::int32_t(h));
            const auto at = [&](std::int64_t dx, std::int64_t dy) {
                const std::int64_t nx = std::clamp<std::int64_t>(x + dx, 0, W - 1), ny = std::clamp<std::int64_t>(y + dy, 0, H - 1);
                return double(std::max(0.0f, height[std::size_t(ny * W + nx)]));
            };
            const double gx = (at(1, 0) - at(-1, 0)) / (2.0 * double(sm));
            const double gy = (at(0, 1) - at(0, -1)) / (2.0 * double(sm));
            const double slope = std::hypot(gx, gy);
            const double coastKm = double(coast[i]) / 3.0 * double(sm) / 1000.0;
            const std::int64_t X = sx0 + x, Y = sy0 + y;

            const double moisture = moistMiddle + 0.12 * std::exp(-coastKm / 40.0) + 0.06 * smoothstep(300, 1500, h) -
                                    0.08 * smoothstep(2200, 3500, h) +
                                    variation * (0.16 * wander(seed + 1, X, Y, scale(48000)) +
                                                 0.06 * wander(seed + 2, X, Y, scale(12000)));
            const double mountain = 0.65 * smoothstep(250, 2200, h) + 0.45 * smoothstep(0.04, 0.30, slope) +
                                    variation * 0.05 * wander(seed + 3, X, Y, scale(16000));
            const double erosion = erosionMiddle - 0.10 * smoothstep(0.30, 0.70, slope) +
                                   variation * (0.12 * wander(seed + 4, X, Y, scale(32000)) +
                                                0.04 * wander(seed + 5, X, Y, scale(8000)));
            const double forest = 0.5 + 0.9 * (std::clamp(moisture, 0.0, 1.0) - 0.5) - 0.55 * smoothstep(1500, 2600, h) -
                                  0.25 * smoothstep(0.35, 0.70, slope) - 0.12 * std::exp(-coastKm / 2.0) +
                                  variation * (0.15 * wander(seed + 6, X, Y, scale(20000)) +
                                               0.05 * wander(seed + 7, X, Y, scale(5000)));
            const std::array<double, 4> v{moisture, forest, mountain, erosion};
            for (std::size_t c = 0; c < 4; ++c) px[c] = std::uint8_t(controlDesc.encode(c, std::clamp(v[c], 0.0, 1.0)));
        }
    });
    for (const Tally& t : tallies) {
        out.landSamples += t.land;
        out.highestMetres = std::max(out.highestMetres, t.highest);
    }
    return out;
}

} // namespace generation

