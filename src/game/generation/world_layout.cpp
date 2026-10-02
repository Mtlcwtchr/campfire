#include "game/generation/world_layout.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

#include "engine/core/rng.hpp"
#include "game/generation/world_noise.hpp"

namespace generation {
namespace {

double hashUnit(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    std::uint64_t h = core::splitmix64(seed ^ (std::uint64_t(x) * 0x9e3779b97f4a7c15ull));
    h = core::splitmix64(h ^ (std::uint64_t(y) * 0xc2b2ae3d27d4eb4full));
    return double(h >> 11) * (1.0 / 9007199254740992.0);
}

// Smooth value noise, 0..1, one lattice point a unit apart.
double smooth(std::uint64_t seed, double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    double tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.0 - 2.0 * tx);
    ty = ty * ty * (3.0 - 2.0 * ty);
    const auto ix = std::int64_t(fx), iy = std::int64_t(fy);
    const double a = hashUnit(seed, ix, iy), b = hashUnit(seed, ix + 1, iy);
    const double c = hashUnit(seed, ix, iy + 1), d = hashUnit(seed, ix + 1, iy + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Along one axis: which region column (or row) a coordinate is in, and within
// `band` cells of a border, the neighbour it shares with. At the border itself
// the two are half and half; a band's width in from it, the region is itself.
struct AxisMix {
    std::int32_t a = 0, b = -1;
    double weightA = 1.0;
};
AxisMix axisMix(double u, std::int32_t count, double band) {
    AxisMix m;
    const double span = kCellsPerRegion;
    m.a = std::clamp(std::int32_t(std::floor(u / span)), 0, std::max(0, count - 1));
    if (band <= 0.0 || count <= 1) return m;
    const double local = u - double(m.a) * span;
    if (local < band && m.a > 0) {
        m.b = m.a - 1;
        m.weightA = ease(0.5 + 0.5 * local / band);
    } else if (span - local < band && m.a + 1 < count) {
        m.b = m.a + 1;
        m.weightA = ease(0.5 + 0.5 * (span - local) / band);
    }
    return m;
}

using Json = nlohmann::json;

Json regionJson(const Region& r) {
    return {{"generated", r.generated}, {"preset", r.settings.preset}, {"seed", r.settings.seed},
            {"sea_percent", r.settings.seaPercent}, {"erosion_passes", r.settings.erosionPasses},
            {"rainfall_percent", r.settings.rainfallPercent}, {"manual", r.settings.manual},
            {"stage", stageName(r.stage)}};
}

Region regionFrom(const Json& r, std::uint64_t seed) {
    Region region;
    region.generated = r.value("generated", false);
    region.settings.preset = r.value("preset", std::string{});
    region.settings.seed = r.value("seed", seed);
    region.settings.seaPercent = std::clamp(r.value("sea_percent", 71), 0, 100);
    region.settings.erosionPasses = std::clamp(r.value("erosion_passes", 3), 0, 24);
    region.settings.rainfallPercent = std::clamp(r.value("rainfall_percent", 100), 20, 250);
    region.settings.manual = r.value("manual", false);
    // Saved before there were stages: everything, as it was made then.
    region.stage = stageNamed(r.value("stage", std::string("full"))).value_or(RegionStage::Full);
    return region;
}

// Layer tiles go into the file as base64 of their little-endian floats: exact,
// so a world loads back equal to the one saved, and a third the size of the
// same numbers written out as text.
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string encodeFloats(const float* values, std::size_t count) {
    std::vector<std::uint8_t> bytes(count * 4);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &values[i], 4);
        for (int b = 0; b < 4; ++b) bytes[i * 4 + std::size_t(b)] = std::uint8_t(bits >> (8 * b));
    }
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t n = std::uint32_t(bytes[i]) << 16 |
                                (i + 1 < bytes.size() ? std::uint32_t(bytes[i + 1]) << 8 : 0u) |
                                (i + 2 < bytes.size() ? std::uint32_t(bytes[i + 2]) : 0u);
        out += kBase64[(n >> 18) & 63];
        out += kBase64[(n >> 12) & 63];
        out += i + 1 < bytes.size() ? kBase64[(n >> 6) & 63] : '=';
        out += i + 2 < bytes.size() ? kBase64[n & 63] : '=';
    }
    return out;
}

bool decodeFloats(const std::string& text, float* values, std::size_t count) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(count * 4);
    std::uint32_t n = 0;
    int held = 0;
    for (const char c : text) {
        if (c == '=') break;
        const char* at = std::strchr(kBase64, c);
        if (!at || c == '\0') return false;
        n = n << 6 | std::uint32_t(at - kBase64);
        if (++held == 4) {
            bytes.push_back(std::uint8_t(n >> 16));
            bytes.push_back(std::uint8_t(n >> 8));
            bytes.push_back(std::uint8_t(n));
            n = 0;
            held = 0;
        }
    }
    if (held == 2) bytes.push_back(std::uint8_t(n >> 4));
    if (held == 3) {
        bytes.push_back(std::uint8_t(n >> 10));
        bytes.push_back(std::uint8_t(n >> 2));
    }
    if (bytes.size() != count * 4) return false;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t bits = 0;
        for (int b = 0; b < 4; ++b) bits |= std::uint32_t(bytes[i * 4 + std::size_t(b)]) << (8 * b);
        std::memcpy(&values[i], &bits, 4);
        if (!std::isfinite(values[i])) return false;
    }
    return true;
}

// Every layer, empty, at its own resolution over a world of this many regions.
std::array<LayerMap, kLayerCount> emptyLayers(std::int32_t regionsX, std::int32_t regionsY) {
    std::array<LayerMap, kLayerCount> layers;
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const auto id = static_cast<LayerId>(i);
        layers[i] = LayerMap(id, layerTexels(id, regionsX * kCellsPerRegion),
                             layerTexels(id, regionsY * kCellsPerRegion));
    }
    return layers;
}

// A number made per region, blended as a macro cell is made of regions.
template <class Value>
double regionalAt(const WorldLayout& layout, double cellX, double cellY, Value&& value) {
    const RegionMix mix = regionMixAt(layout, cellX, cellY);
    double sum = 0.0, total = 0.0;
    for (int k = 0; k < mix.count; ++k) {
        const double w = mix.weight[std::size_t(k)];
        sum += w * double(value(layout.regions[std::size_t(mix.region[std::size_t(k)])]));
        total += w;
    }
    return total > 0.0 ? sum / total : 0.0;
}

} // namespace

bool WorldLayout::anyGenerated() const {
    return std::any_of(regions.begin(), regions.end(), [](const Region& r) { return r.generated; });
}

bool WorldLayout::anyPainted() const {
    return std::any_of(layers.begin(), layers.end(), [](const LayerMap& m) { return !m.empty(); });
}

WorldLayout emptyLayout(std::int32_t regionsX, std::int32_t regionsY, std::uint64_t seed) {
    WorldLayout layout;
    layout.regionsX = std::clamp(regionsX, 1, kMaxRegionsPerSide);
    layout.regionsY = std::clamp(regionsY, 1, kMaxRegionsPerSide);
    layout.seed = seed;
    layout.regions.assign(std::size_t(layout.regionsX) * std::size_t(layout.regionsY), Region{});
    for (std::int32_t y = 0; y < layout.regionsY; ++y)
        for (std::int32_t x = 0; x < layout.regionsX; ++x)
            layout.at(x, y).settings.seed = regionSeed(seed, x, y);
    layout.layers = emptyLayers(layout.regionsX, layout.regionsY);
    return layout;
}

namespace { void takeFromWholeWorld(WorldLayout& layout); }

bool reshapeLayout(WorldLayout& layout, std::int32_t west, std::int32_t east, std::int32_t north, std::int32_t south) {
    const std::int32_t wide = layout.regionsX + west + east, high = layout.regionsY + north + south;
    if (wide < 1 || high < 1 || wide > kMaxRegionsPerSide || high > kMaxRegionsPerSide) return false;
    if (west == 0 && north == 0) {
        resizeLayout(layout, wide, high);
        return true;
    }
    if (layout.generations.empty() && layout.anyGenerated()) takeFromWholeWorld(layout);
    WorldLayout moved = emptyLayout(wide, high, layout.seed);
    moved.plates = layout.plates;
    moved.blendMetres = layout.blendMetres;
    moved.authoring = layout.authoring;
    moved.latitude = layout.latitude;
    if (!moved.latitude.fixed) moved.latitude = legacyLatitude(layout.widthCells(), layout.heightCells(), layout.seed);
    moved.latitude.rowOffset += north * kCellsPerRegion;
    for (std::int32_t y = 0; y < layout.regionsY; ++y)
        for (std::int32_t x = 0; x < layout.regionsX; ++x)
            if (moved.inBounds(x + west, y + north)) moved.at(x + west, y + north) = layout.at(x, y);
    moved.generations = layout.generations;
    for (Generation& g : moved.generations) { g.x += west; g.y += north; }
    // The paint, texel for texel: a region is a whole number of texels of
    // every layer.
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const LayerMap& from = layout.layers[i];
        if (from.empty()) continue;
        LayerMap& into = moved.layers[i];
        const std::int32_t per = layerTexels(from.id(), kCellsPerRegion);
        const std::int32_t dx = west * per, dy = north * per;
        for (std::int32_t ty = 0; ty < from.tilesY(); ++ty)
            for (std::int32_t tx = 0; tx < from.tilesX(); ++tx) {
                if (!from.tile(tx, ty)) continue;
                for (std::int32_t y = ty * kLayerTile; y < std::min(from.texelsY(), (ty + 1) * kLayerTile); ++y)
                    for (std::int32_t x = tx * kLayerTile; x < std::min(from.texelsX(), (tx + 1) * kLayerTile); ++x) {
                        const float value = from.value(x, y), cover = from.cover(x, y);
                        if ((value != 0.0f || cover != 0.0f) && into.inBounds(x + dx, y + dy))
                            into.set(x + dx, y + dy, value, cover);
                    }
            }
    }
    layout = std::move(moved);
    pruneGenerations(layout);
    return true;
}

void resizeLayout(WorldLayout& layout, std::int32_t regionsX, std::int32_t regionsY) {
    // A world that grows keeps what it had: the run that made it is kept as a
    // run of its size, and what is added is empty.
    if (layout.generations.empty() && layout.anyGenerated() &&
        (regionsX != layout.regionsX || regionsY != layout.regionsY))
        takeFromWholeWorld(layout);
    WorldLayout resized = emptyLayout(regionsX, regionsY, layout.seed);
    resized.plates = layout.plates;
    resized.blendMetres = layout.blendMetres;
    for (std::int32_t y = 0; y < std::min(layout.regionsY, resized.regionsY); ++y)
        for (std::int32_t x = 0; x < std::min(layout.regionsX, resized.regionsX); ++x)
            resized.at(x, y) = layout.at(x, y);
    // What was painted stays where it was painted; whatever falls off the new
    // edge goes with the regions it was on.
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        LayerMap kept = layout.layers[i];
        kept.resize(resized.layers[i].texelsX(), resized.layers[i].texelsY());
        resized.layers[i] = std::move(kept);
    }
    // The generations as they were run: a region added is empty and changes
    // nothing that exists, and a run that reaches past a shrunk edge still
    // makes what it made inside it.
    resized.generations = layout.generations;
    // Growing a world is the moment its latitude has to stop being its height.
    resized.latitude = layout.latitude;
    if ((regionsX != layout.regionsX || regionsY != layout.regionsY) && !resized.latitude.fixed)
        resized.latitude = legacyLatitude(layout.widthCells(), layout.heightCells(), layout.seed);
    layout = std::move(resized);
    pruneGenerations(layout);
}

std::uint64_t regionSeed(std::uint64_t worldSeed, std::int32_t x, std::int32_t y) {
    return core::splitmix64(worldSeed ^ 0x5E610A5EEDull ^
                            (std::uint64_t(std::uint32_t(x)) << 32) ^ std::uint32_t(y));
}

RegionSettings regionSettingsFrom(const WorldPreset& preset, std::uint64_t seed) {
    RegionSettings s;
    s.preset = preset.name;
    s.seed = seed;
    s.seaPercent = std::clamp(preset.params.seaPercent, 0, 100);
    s.erosionPasses = std::clamp(preset.params.erosionPasses, 0, 24);
    s.rainfallPercent = std::clamp(preset.params.rainfallPercent, 20, 250);
    return s;
}

namespace {
// A whole-world generation of the layout as its regions now are.
Generation wholeWorld(const WorldLayout& layout) {
    Generation g;
    g.x = g.y = 0;
    g.w = layout.regionsX;
    g.h = layout.regionsY;
    g.seed = layout.seed;
    g.plates = layout.plates;
    g.regions = layout.regions;
    for (Region& r : g.regions) r.source = -1;
    return g;
}
void takeFromWholeWorld(WorldLayout& layout) {
    layout.generations = {wholeWorld(layout)};
    for (Region& r : layout.regions) r.source = r.generated ? 0 : -1;
}
} // namespace

void generateAll(WorldLayout& layout, const WorldPreset& preset) {
    if (preset.params.plates > 0) layout.plates = preset.params.plates;
    for (std::int32_t y = 0; y < layout.regionsY; ++y)
        for (std::int32_t x = 0; x < layout.regionsX; ++x) {
            Region& r = layout.at(x, y);
            r.generated = true;
            r.settings = regionSettingsFrom(preset, regionSeed(layout.seed, x, y));
        }
    takeFromWholeWorld(layout);
}

void generateWholeWorld(WorldLayout& layout, const RegionSettings& settings, std::uint64_t seed, bool ownSeeds) {
    for (std::int32_t y = 0; y < layout.regionsY; ++y)
        for (std::int32_t x = 0; x < layout.regionsX; ++x) {
            Region& r = layout.at(x, y);
            r.generated = true;
            r.settings = settings;
            r.settings.seed = ownSeeds ? regionSeed(seed, x, y) : seed;
        }
    takeFromWholeWorld(layout);
}

void generateRegions(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which,
                     const RegionSettings& settings, std::uint64_t seed, bool ownSeeds) {
    std::int32_t x0 = layout.regionsX, y0 = layout.regionsY, x1 = -1, y1 = -1;
    for (const auto& [x, y] : which) {
        if (!layout.inBounds(x, y)) continue;
        x0 = std::min(x0, x); y0 = std::min(y0, y);
        x1 = std::max(x1, x); y1 = std::max(y1, y);
    }
    if (x1 < 0) return;
    // What was made so far, as the one run it was.
    if (layout.generations.empty() && layout.anyGenerated()) takeFromWholeWorld(layout);
    // A generation of its own is a place on a planet, so the world's
    // latitude stops being "whatever its height is" from here on.
    fixLatitude(layout);
    Generation g;
    g.x = x0; g.y = y0; g.w = x1 - x0 + 1; g.h = y1 - y0 + 1;
    g.seed = seed;
    g.plates = 0;
    g.regions.assign(std::size_t(g.w) * std::size_t(g.h), Region{});
    for (std::int32_t y = g.y; y < g.y + g.h; ++y)
        for (std::int32_t x = g.x; x < g.x + g.w; ++x) g.at(x, y).settings.seed = regionSeed(seed, x, y);
    const auto index = std::int32_t(layout.generations.size());
    for (const auto& [x, y] : which) {
        if (!layout.inBounds(x, y)) continue;
        Region made;
        made.generated = true;
        made.settings = settings;
        made.settings.seed = ownSeeds ? regionSeed(seed, x, y) : seed;
        g.at(x, y) = made;
        made.source = index;
        layout.at(x, y) = made;
    }
    layout.generations.push_back(std::move(g));
    pruneGenerations(layout);
}

void clearRegions(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which) {
    if (layout.generations.empty() && layout.anyGenerated()) takeFromWholeWorld(layout);
    for (const auto& [x, y] : which) {
        if (!layout.inBounds(x, y)) continue;
        Region& r = layout.at(x, y);
        r.generated = false;
        r.source = -1;
        for (Generation& g : layout.generations)
            if (g.contains(x, y)) g.at(x, y).generated = false;
    }
    pruneGenerations(layout);
}

void pruneGenerations(WorldLayout& layout) {
    std::vector<std::int32_t> renumber(layout.generations.size(), -1);
    for (const Region& r : layout.regions)
        if (r.source >= 0 && std::size_t(r.source) < renumber.size()) renumber[std::size_t(r.source)] = 0;
    std::vector<Generation> kept;
    for (std::size_t i = 0; i < layout.generations.size(); ++i)
        if (renumber[i] == 0) {
            renumber[i] = std::int32_t(kept.size());
            kept.push_back(std::move(layout.generations[i]));
        }
    layout.generations = std::move(kept);
    for (Region& r : layout.regions) {
        if (r.source >= 0 && std::size_t(r.source) < renumber.size()) r.source = renumber[std::size_t(r.source)];
        else r.source = -1;
    }
}

bool isWholeWorld(const WorldLayout& layout) {
    // No generations at all is the generator's classic input - regions marked
    // generated or not, run once over the whole world - which is what every
    // layout was before there were generations.
    if (layout.generations.empty()) return true;
    if (layout.generations.size() != 1) return false;
    const Generation& g = layout.generations.front();
    if (g.x != 0 || g.y != 0 || g.w != layout.regionsX || g.h != layout.regionsY || g.seed != layout.seed ||
        g.plates != layout.plates || g.regions.size() != layout.regions.size())
        return false;
    for (std::size_t i = 0; i < layout.regions.size(); ++i) {
        const Region& now = layout.regions[i];
        const Region& ran = g.regions[i];
        if (now.generated != ran.generated || now.settings != ran.settings) return false;
        if (now.source != (now.generated ? 0 : -1)) return false;
    }
    return true;
}

void fixLatitude(WorldLayout& layout) {
    if (!layout.latitude.fixed)
        layout.latitude = legacyLatitude(layout.widthCells(), layout.heightCells(), layout.seed);
}

WorldLayout generationLayout(const WorldLayout& layout, const Generation& g) {
    WorldLayout sub = emptyLayout(g.w, g.h, g.seed);
    sub.plates = g.plates;
    sub.blendMetres = layout.blendMetres;
    sub.regions = g.regions;
    for (Region& r : sub.regions) r.source = -1;
    sub.latitude = layout.latitude;
    sub.authoring = layout.authoring;
    // The paint over the rectangle, texel for texel: a region is a whole
    // number of texels of every layer, so nothing is resampled.
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const LayerMap& from = layout.layers[i];
        LayerMap& into = sub.layers[i];
        if (from.empty()) continue;
        const std::int32_t perRegion = layerTexels(from.id(), kCellsPerRegion);
        const std::int32_t ox = g.x * perRegion, oy = g.y * perRegion;
        for (std::int32_t ty = 0; ty < into.texelsY(); ++ty)
            for (std::int32_t tx = 0; tx < into.texelsX(); ++tx) {
                const float value = from.value(ox + tx, oy + ty), cover = from.cover(ox + tx, oy + ty);
                if (value != 0.0f || cover != 0.0f) into.set(tx, ty, value, cover);
            }
    }
    return sub;
}

const char* stageName(RegionStage stage) {
    switch (stage) {
        case RegionStage::Full: return "full";
        case RegionStage::Sketch: return "sketch";
        case RegionStage::Primary: return "primary";
        case RegionStage::Relief: return "relief";
        case RegionStage::Water: return "water";
    }
    return "full";
}

std::optional<RegionStage> stageNamed(std::string_view name) {
    for (const RegionStage s : {RegionStage::Full, RegionStage::Sketch, RegionStage::Primary, RegionStage::Relief,
                                RegionStage::Water})
        if (name == stageName(s)) return s;
    return std::nullopt;
}

bool authored(const WorldLayout& layout, std::int32_t rx, std::int32_t ry) {
    if (!layout.inBounds(rx, ry)) return false;
    const Region& r = layout.at(rx, ry);
    return !r.generated && r.source < 0 && r.stage != RegionStage::Full;
}

std::int32_t raiseStage(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which,
                        RegionStage to) {
    std::int32_t moved = 0;
    const auto raise = [&](std::int32_t rx, std::int32_t ry) {
        if (!authored(layout, rx, ry)) return;
        Region& r = layout.at(rx, ry);
        // Sketch < Primary < Relief < Water, and never down.
        if (std::uint8_t(r.stage) >= std::uint8_t(to)) return;
        r.stage = to;
        ++moved;
    };
    if (which.empty()) {
        for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
            for (std::int32_t rx = 0; rx < layout.regionsX; ++rx) raise(rx, ry);
    } else {
        for (const auto& [rx, ry] : which) raise(rx, ry);
    }
    return moved;
}

void beginSketch(WorldLayout& layout, std::int32_t rx, std::int32_t ry) {
    if (!layout.inBounds(rx, ry)) return;
    Region& r = layout.at(rx, ry);
    if (r.generated || r.source >= 0 || r.stage != RegionStage::Full) return;
    // Painted already under the old rule (a region made by hand, run in full):
    // it keeps being what it was.
    if (paintedIn(layout, rx, ry)) return;
    r.stage = RegionStage::Sketch;
}

bool paintedIn(const WorldLayout& layout, std::int32_t rx, std::int32_t ry) {
    if (!layout.inBounds(rx, ry)) return false;
    for (const LayerMap& map : layout.layers) {
        if (map.empty()) continue;
        const std::int32_t per = layerTexels(map.id(), kCellsPerRegion);
        // Tile by tile: a tile not there holds nothing.
        const std::int32_t t0x = rx * per / kLayerTile, t1x = ((rx + 1) * per - 1) / kLayerTile;
        const std::int32_t t0y = ry * per / kLayerTile, t1y = ((ry + 1) * per - 1) / kLayerTile;
        for (std::int32_t ty = t0y; ty <= t1y; ++ty)
            for (std::int32_t tx = t0x; tx <= t1x; ++tx) {
                if (!map.tile(tx, ty)) continue;
                for (std::int32_t y = std::max(ty * kLayerTile, ry * per); y < std::min((ty + 1) * kLayerTile, (ry + 1) * per); ++y)
                    for (std::int32_t x = std::max(tx * kLayerTile, rx * per); x < std::min((tx + 1) * kLayerTile, (rx + 1) * per); ++x)
                        if (map.value(x, y) != 0.0f || map.cover(x, y) != 0.0f) return true;
            }
    }
    return false;
}


WorldMapParams paramsFor(const WorldLayout& layout) {
    WorldMapParams p;
    p.seed = layout.seed;
    p.width = layout.widthCells();
    p.height = layout.heightCells();
    p.plates = layout.plates;
    p.latitude = layout.latitude;
    p.authoring = layout.authoring;
    std::int64_t sea = 0, rain = 0, count = 0;
    std::int32_t erosion = 0;
    for (const Region& r : layout.regions) {
        if (!r.generated) continue;
        sea += r.settings.seaPercent;
        rain += r.settings.rainfallPercent;
        erosion = std::max(erosion, r.settings.erosionPasses);
        ++count;
    }
    if (count > 0) {
        p.seaPercent = std::int32_t(sea / count);
        p.rainfallPercent = std::int32_t(rain / count);
        p.erosionPasses = erosion;
    }
    // The erosion passes run as many times as the most weathered place asks
    // for, and a place painted older than any region has to be reached too.
    const LayerMap& weathering = layout.layer(LayerId::Weathering);
    float oldest = 0.0f;
    for (std::int32_t gy = 0; gy < weathering.tilesY(); ++gy)
        for (std::int32_t gx = 0; gx < weathering.tilesX(); ++gx)
            if (const LayerMap::Tile* tile = weathering.tile(gx, gy))
                for (std::size_t i = 0; i < tile->value.size(); ++i)
                    if (tile->cover[i] > 0.0f) oldest = std::max(oldest, tile->value[i]);
    p.erosionPasses = std::clamp(std::max(p.erosionPasses, std::int32_t(std::ceil(oldest))), 0, 24);
    p.layout = std::make_shared<const WorldLayout>(layout);
    return p;
}

RegionMix regionMixAt(const WorldLayout& layout, double cellX, double cellY) {
    RegionMix mix;
    if (layout.regions.empty()) return mix;
    const double band = double(std::clamp(layout.blendMetres, 0, kMaxRegionBlendMetres)) / kMetresPerCell;
    // The border wanders, at two scales, by up to seven tenths of the band:
    // a border that is a ruled line is a seam however softly it is blended.
    double u = cellX, v = cellY;
    if (band > 0.0) {
        const double swing = band * 0.7;
        const std::uint64_t s = layout.seed ^ 0xB0DE7A11ull;
        u += ((smooth(s, cellX / 48.0, cellY / 48.0) - 0.5) * 1.4 +
              (smooth(s + 1, cellX / 13.0, cellY / 13.0) - 0.5) * 0.6) * swing;
        v += ((smooth(s + 2, cellX / 48.0, cellY / 48.0) - 0.5) * 1.4 +
              (smooth(s + 3, cellX / 13.0, cellY / 13.0) - 0.5) * 0.6) * swing;
    }
    const AxisMix ax = axisMix(u, layout.regionsX, band);
    const AxisMix ay = axisMix(v, layout.regionsY, band);
    const std::int32_t xs[2] = {ax.a, ax.b};
    const std::int32_t ys[2] = {ay.a, ay.b};
    const double wx[2] = {ax.weightA, 1.0 - ax.weightA};
    const double wy[2] = {ay.weightA, 1.0 - ay.weightA};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            if (xs[i] < 0 || ys[j] < 0) continue;
            const double w = wx[i] * wy[j];
            if (w <= 0.0) continue;
            mix.region[std::size_t(mix.count)] = std::int32_t(layout.indexOf(xs[i], ys[j]));
            mix.weight[std::size_t(mix.count)] = float(w);
            ++mix.count;
        }
    return mix;
}

float generatedAt(const WorldLayout& layout, double cellX, double cellY) {
    const RegionMix mix = regionMixAt(layout, cellX, cellY);
    float generated = 0.0f;
    for (int k = 0; k < mix.count; ++k)
        if (layout.regions[std::size_t(mix.region[std::size_t(k)])].generated)
            generated += mix.weight[std::size_t(k)];
    return std::clamp(generated, 0.0f, 1.0f);
}

LayerBase layerBase(const WorldLayout& layout, LayerId id) {
    LayerBase base;
    base.id = id;
    const LayerMap& map = layout.layer(id);
    base.texelsX = map.texelsX();
    base.texelsY = map.texelsY();
    base.values.assign(std::size_t(base.texelsX) * std::size_t(base.texelsY), 0.0f);
    if (id == LayerId::Ranges) return base;
    const double perTexel = double(layerDef(id).texelMetres) / kMetresPerCell;
    const std::int32_t width = layout.widthCells(), height = layout.heightCells();
    for (std::int32_t ty = 0; ty < base.texelsY; ++ty)
        for (std::int32_t tx = 0; tx < base.texelsX; ++tx) {
            // The macro cell at the texel's centre: the generator's noise is
            // asked at whole cells, and this is the cell it would ask.
            const auto cx = std::min(width - 1, std::int32_t((tx + 0.5) * perTexel));
            const auto cy = std::min(height - 1, std::int32_t((ty + 0.5) * perTexel));
            double v = 0.0;
            switch (id) {
                case LayerId::Continents:
                    v = regionalAt(layout, cx, cy, [&](const Region& r) {
                        return continentNoise(r.settings.seed, cx, cy, width);
                    });
                    break;
                case LayerId::Hills:
                    v = regionalAt(layout, cx, cy, [&](const Region& r) {
                        return landNoise(r.settings.seed, cx, cy, width);
                    });
                    break;
                case LayerId::Sea:
                    v = regionalAt(layout, cx, cy, [](const Region& r) { return r.settings.seaPercent; });
                    break;
                case LayerId::Weathering:
                    v = regionalAt(layout, cx, cy, [](const Region& r) { return r.settings.erosionPasses; });
                    break;
                case LayerId::Rain:
                    v = regionalAt(layout, cx, cy, [](const Region& r) { return r.settings.rainfallPercent; });
                    break;
                case LayerId::Ranges:
                case LayerId::Water:
                case LayerId::Count:
                    break;
            }
            base.values[std::size_t(ty) * std::size_t(base.texelsX) + std::size_t(tx)] = float(v);
        }
    return base;
}

std::vector<float> dialCells(const WorldLayout& layout, LayerId id) {
    const std::int32_t width = layout.widthCells(), height = layout.heightCells();
    std::vector<float> cells(std::size_t(width) * std::size_t(height), 0.0f);
    const LayerCells painted = layerCells(layout.layer(id), width, height);
    for (std::int32_t y = 0; y < height; ++y)
        for (std::int32_t x = 0; x < width; ++x) {
            const double made = regionalAt(layout, x, y, [&](const Region& r) {
                return id == LayerId::Rain ? r.settings.rainfallPercent
                     : id == LayerId::Sea  ? r.settings.seaPercent
                                           : r.settings.erosionPasses;
            });
            const std::size_t i = std::size_t(y) * std::size_t(width) + std::size_t(x);
            cells[i] = float(painted.over(i, made));
        }
    return cells;
}

bool saveWorldLayout(const WorldLayout& layout, const std::filesystem::path& file) {
    try {
        Json j;
        j["format"] = 3;
        j["regions_x"] = layout.regionsX;
        j["regions_y"] = layout.regionsY;
        j["seed"] = layout.seed;
        j["plates"] = layout.plates;
        j["blend_m"] = layout.blendMetres;
        Json regions = Json::array();
        for (std::int32_t y = 0; y < layout.regionsY; ++y)
            for (std::int32_t x = 0; x < layout.regionsX; ++x) {
                const Region& r = layout.at(x, y);
                Json region = regionJson(r);
                region["x"] = x;
                region["y"] = y;
                region["source"] = r.source;
                regions.push_back(std::move(region));
            }
        j["regions"] = std::move(regions);
        Json generations = Json::array();
        for (const Generation& g : layout.generations) {
            Json ran = Json::array();
            for (const Region& r : g.regions) ran.push_back(regionJson(r));
            generations.push_back({{"x", g.x}, {"y", g.y}, {"w", g.w}, {"h", g.h}, {"seed", g.seed},
                                   {"plates", g.plates}, {"regions", std::move(ran)}});
        }
        j["generations"] = std::move(generations);
        j["authoring"] = {{"coast", layout.authoring.coast}, {"coast_km", layout.authoring.coastKm},
                          {"relief", layout.authoring.relief}, {"min_land_m", layout.authoring.minLandMetres},
                          {"rivers", layout.authoring.rivers}, {"lakes", layout.authoring.lakes}};
        j["latitude"] = {{"fixed", layout.latitude.fixed}, {"north_degrees", layout.latitude.northDegrees},
                         {"km_per_degree", layout.latitude.kmPerDegree},
                         {"legacy_from", layout.latitude.legacyFrom}, {"legacy_span", layout.latitude.legacySpan},
                         {"legacy_rows", layout.latitude.legacyRows}, {"row_offset", layout.latitude.rowOffset}};
        // Only what was painted: a layer with nothing on it is not written.
        Json layers = Json::array();
        for (const LayerMap& map : layout.layers) {
            if (map.empty()) continue;
            Json tiles = Json::array();
            for (std::int32_t gy = 0; gy < map.tilesY(); ++gy)
                for (std::int32_t gx = 0; gx < map.tilesX(); ++gx) {
                    const LayerMap::Tile* tile = map.tile(gx, gy);
                    if (!tile) continue;
                    Json t{{"x", gx}, {"y", gy}, {"value", encodeFloats(tile->value.data(), tile->value.size())}};
                    if (map.def().kind == LayerKind::Override)
                        t["cover"] = encodeFloats(tile->cover.data(), tile->cover.size());
                    tiles.push_back(std::move(t));
                }
            layers.push_back({{"name", map.def().name}, {"texel_m", map.def().texelMetres},
                              {"tile", kLayerTile}, {"tiles", std::move(tiles)}});
        }
        j["layers"] = std::move(layers);
        if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file);
        if (!out) return false;
        out << j.dump(2) << '\n';
        return bool(out);
    } catch (...) {
        return false;
    }
}

std::optional<WorldLayout> loadWorldLayout(const std::filesystem::path& file) {
    try {
        std::ifstream in(file);
        if (!in) return std::nullopt;
        const Json j = Json::parse(in);
        // Format 1 is the regions alone, from before there were layers.
        const int format = j.is_object() ? j.value("format", 0) : 0;
        if (format < 1 || format > 3) return std::nullopt;
        WorldLayout layout = emptyLayout(j.value("regions_x", 1), j.value("regions_y", 1),
                                         j.value("seed", std::uint64_t{1}));
        layout.plates = std::clamp(j.value("plates", 0), 0, 256);
        layout.blendMetres = std::clamp(j.value("blend_m", layout.blendMetres), 0, kMaxRegionBlendMetres);
        for (const Json& r : j.value("regions", Json::array())) {
            const std::int32_t x = r.value("x", -1), y = r.value("y", -1);
            if (!layout.inBounds(x, y)) continue;
            Region& region = layout.at(x, y);
            region = regionFrom(r, region.settings.seed);
            region.source = format >= 3 ? r.value("source", -1) : -1;
        }
        if (format >= 3) {
            for (const Json& g : j.value("generations", Json::array())) {
                Generation ran;
                ran.x = g.value("x", 0); ran.y = g.value("y", 0);
                ran.w = std::clamp(g.value("w", 1), 1, kMaxRegionsPerSide);
                ran.h = std::clamp(g.value("h", 1), 1, kMaxRegionsPerSide);
                ran.seed = g.value("seed", std::uint64_t{1});
                ran.plates = std::clamp(g.value("plates", 0), 0, 256);
                const Json regions = g.value("regions", Json::array());
                if (regions.size() != std::size_t(ran.w) * std::size_t(ran.h)) return std::nullopt;
                for (const Json& r : regions) ran.regions.push_back(regionFrom(r, 1));
                layout.generations.push_back(std::move(ran));
            }
            const Json dials = j.value("authoring", Json::object());
            layout.authoring.coast = std::clamp(dials.value("coast", 1.0f), 0.0f, 3.0f);
            layout.authoring.coastKm = std::clamp(dials.value("coast_km", 20.0f), 1.0f, 200.0f);
            layout.authoring.relief = std::clamp(dials.value("relief", 1.0f), 0.0f, 3.0f);
            layout.authoring.minLandMetres = std::clamp(dials.value("min_land_m", 4.0f), 0.0f, 500.0f);
            layout.authoring.rivers = std::clamp(dials.value("rivers", 1.0f), 0.25f, 3.0f);
            layout.authoring.lakes = std::clamp(dials.value("lakes", 1.0f), 0.0f, 3.0f);
            const Json lat = j.value("latitude", Json::object());
            layout.latitude.fixed = lat.value("fixed", false);
            layout.latitude.northDegrees = std::clamp(lat.value("north_degrees", 50.0), -90.0, 90.0);
            layout.latitude.kmPerDegree = std::clamp(lat.value("km_per_degree", 6.0), 0.1, 1000.0);
            layout.latitude.legacyFrom = std::clamp(lat.value("legacy_from", 0), 0, 230);
            layout.latitude.legacySpan = std::clamp(lat.value("legacy_span", 0), 0, 230);
            layout.latitude.legacyRows = std::max(0, lat.value("legacy_rows", 0));
            layout.latitude.rowOffset = lat.value("row_offset", 0);
            for (Region& r : layout.regions)
                if (r.source >= std::int32_t(layout.generations.size())) r.source = -1;
            pruneGenerations(layout);
        }
        // Made before there were generations: no generations, the regions
        // marked generated - the generator run once over the whole world
        // (isWholeWorld), as it was.
        for (const Json& l : j.value("layers", Json::array())) {
            const auto id = layerNamed(l.value("name", std::string{}));
            // A layer this build does not know, or held at a resolution it no
            // longer has, is left out rather than read into the wrong texels.
            if (!id || l.value("texel_m", 0) != layerDef(*id).texelMetres || l.value("tile", 0) != kLayerTile)
                continue;
            LayerMap& map = layout.layer(*id);
            const bool overrides = layerDef(*id).kind == LayerKind::Override;
            for (const Json& t : l.value("tiles", Json::array())) {
                LayerMap::Tile tile;
                if (!decodeFloats(t.value("value", std::string{}), tile.value.data(), tile.value.size()))
                    continue;
                if (overrides && !decodeFloats(t.value("cover", std::string{}), tile.cover.data(), tile.cover.size()))
                    continue;
                const LayerDef& def = layerDef(*id);
                for (std::size_t i = 0; i < tile.value.size(); ++i) {
                    tile.cover[i] = overrides ? std::clamp(tile.cover[i], 0.0f, 1.0f) : 0.0f;
                    // Uncovered texels of an override layer hold nought, as
                    // LayerMap::set leaves them.
                    tile.value[i] = overrides && tile.cover[i] <= 0.0f
                            ? 0.0f : std::clamp(tile.value[i], def.low, def.high);
                }
                map.putTile(t.value("x", -1), t.value("y", -1), tile);
            }
        }
        return layout;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace generation

