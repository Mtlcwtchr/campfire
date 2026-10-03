#include "game/generation/world_import.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>

#include "engine/biomes/category_field.hpp"
#include "engine/biomes/detail_edits.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/world_source/bake.hpp"
#include "engine/world_source/world_source.hpp"
#include "game/generation/world_layout.hpp"

namespace generation {
namespace {
namespace ws = engine::world_source;

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h * 0xFF51AFD7ED558CCDull;
}

// Catmull-Rom weights at t/256 of the way from p1 to p2, summing to 2 * 256^3.
std::array<std::int64_t, 4> cubicWeights(std::int64_t t, std::int64_t T) {
    const std::int64_t t2 = t * t, t3 = t2 * t;
    return {-t * T * T + 2 * t2 * T - t3, 2 * T * T * T - 5 * t2 * T + 3 * t3, t * T * T + 4 * t2 * T - 3 * t3,
            -t2 * T + t3};
}

std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

} // namespace

std::int32_t ImportedGround::heightAt(std::int64_t x, std::int64_t y) const {
    if (side <= 0) return -600;
    // Sample centres are half a sample in.
    const std::int64_t T = sampleMetres;
    const std::int64_t u = x - T / 2, v = y - T / 2;
    const std::int64_t i0 = floorDiv(u, T), j0 = floorDiv(v, T);
    const auto wx = cubicWeights(u - i0 * T, T), wy = cubicWeights(v - j0 * T, T);
    const auto at = [&](std::int64_t i, std::int64_t j) {
        i = std::clamp<std::int64_t>(i + kMargin, 0, side - 1);
        j = std::clamp<std::int64_t>(j + kMargin, 0, side - 1);
        return std::int64_t(heightDm[std::size_t(j * side + i)]);
    };
    std::int64_t sum = 0;
    for (int b = 0; b < 4; ++b) {
        std::int64_t row = 0;
        for (int a = 0; a < 4; ++a) row += wx[std::size_t(a)] * at(i0 - 1 + a, j0 - 1 + b);
        sum += wy[std::size_t(b)] * (row / 1024);   // keeps the product inside 64 bits
    }
    // Both weights sum to 2 * 256^3 = 2^25; the row was divided by 2^10.
    const std::int64_t whole = (2 * T * T * T) * (2 * T * T * T) / 1024;
    return std::int32_t(sum >= 0 ? (sum + whole / 2) / whole : -((-sum + whole / 2) / whole));
}

std::int32_t ImportedGround::maskAt(const std::vector<std::uint8_t>& mask, std::int64_t x, std::int64_t y,
                                    std::int32_t fallback) const {
    if (mask.empty() || side <= 0) return fallback;
    const std::int64_t T = sampleMetres;
    const std::int64_t u = x - T / 2, v = y - T / 2;
    const std::int64_t i0 = floorDiv(u, T), j0 = floorDiv(v, T);
    const std::int64_t fx = u - i0 * T, fy = v - j0 * T;
    const auto at = [&](std::int64_t i, std::int64_t j) {
        i = std::clamp<std::int64_t>(i + kMargin, 0, side - 1);
        j = std::clamp<std::int64_t>(j + kMargin, 0, side - 1);
        return std::int64_t(mask[std::size_t(j * side + i)]);
    };
    const std::int64_t top = at(i0, j0) * (T - fx) + at(i0 + 1, j0) * fx;
    const std::int64_t bottom = at(i0, j0 + 1) * (T - fx) + at(i0 + 1, j0 + 1) * fx;
    return std::int32_t((top * (T - fy) + bottom * fy) / (T * T));
}

ImportedSource::~ImportedSource() = default;

std::shared_ptr<const ImportedSource> ImportedSource::open(const std::filesystem::path& root, bool bakeIfStale) {
    if (!ws::WorldSource::exists(root)) return nullptr;
    auto source = ws::WorldSource::open(root);
    if (!source) return nullptr;
    const ws::RasterDesc* height = nullptr;
    for (const auto& r : source->schema().rasters)
        if (r.kind == ws::RasterKind::Height) { height = &r; break; }
    if (!height) return nullptr;
    std::shared_ptr<ImportedSource> out(new ImportedSource());
    out->root_ = root;
    // Imported samples and their spacing together determine the region skeleton.
    // Which regions hold height, and what they hold: a region's key is its
    // chunks' and its margin's, so a change beside it reaches it too.
    const auto sampleMetres = std::int64_t(source->schema().world.sampleMetres);
    // The source key changes when the same stored values are interpreted on a new grid.
    const std::int64_t regionSamples = kRegionMetres / sampleMetres;
    const std::int64_t perRegion = regionSamples / source->chunkSamples();
    std::map<std::pair<std::int32_t, std::int32_t>, bool> any;
    for (const auto& [key, record] : source->chunks())
        if (record.layers.count(height->name))
            any[{std::int32_t(floorDiv(key.x, perRegion)), std::int32_t(floorDiv(key.y, perRegion))}] = true;
    for (const auto& [region, yes] : any) {
        std::uint64_t h = 0x1D0F0AD5EEDull;
        for (std::int64_t cy = region.second * perRegion - 1; cy <= (region.second + 1) * perRegion; ++cy)
            for (std::int64_t cx = region.first * perRegion - 1; cx <= (region.first + 1) * perRegion; ++cx) {
                const auto it = source->chunks().find({ws::ChunkLevel::SourceChunk, cx, cy});
                if (it == source->chunks().end()) continue;
                h = mix(h, std::uint64_t(cx) * 131 + std::uint64_t(cy));
                for (const auto& [layer, hash] : it->second.layers) h = mix(h, hash);
            }
        out->held_[region] = h;
    }
    if (out->held_.empty()) return nullptr;
    out->source_ = std::make_unique<ws::WorldSource>(std::move(*source));
    // The drained height, when it has been asked for and is current. Made
    // here only when the caller says the world needs it: an import alone
    // does no drainage.
    const auto bakedRoot = bakedRootOf(root);
    if (bakeIfStale) {
        std::string why;
        if (!bake(root, &why)) std::cerr << "world source bake: " << why << "\n";
    }
    if (ws::bakeCurrent(root, bakedRoot))
        if (auto baked = ws::WorldSource::open(bakedRoot)) out->baked_ = std::make_unique<ws::WorldSource>(std::move(*baked));
    // A breach runs to wherever its water leaves, so what is baked into a
    // region can change with an import somewhere else: its drained key has
    // the baked chunks in it too.
    for (const auto& [region, key] : out->held_) {
        std::uint64_t drained = mix(key, 0xD4A1ull);
        if (out->baked_)
            for (std::int64_t cy = region.second * perRegion - 1; cy <= (region.second + 1) * perRegion; ++cy)
                for (std::int64_t cx = region.first * perRegion - 1; cx <= (region.first + 1) * perRegion; ++cx) {
                    const auto it = out->baked_->chunks().find({ws::ChunkLevel::SourceChunk, cx, cy});
                    if (it == out->baked_->chunks().end()) continue;
                    for (const auto& [layer, hash] : it->second.layers) drained = mix(drained, hash ^ 0xBAull);
                }
        out->drainedKeys_[region] = drained;
    }
    return out;
}

bool ImportedSource::bake(const std::filesystem::path& root, std::string* why) {
    if (!ws::WorldSource::exists(root)) {
        if (why) *why = "nothing has been imported";
        return false;
    }
    return ws::bakeSource(root, bakedRootOf(root), {}, why).has_value();
}

bool ImportedSource::holds(std::int32_t rx, std::int32_t ry) const { return held_.count({rx, ry}) > 0; }

std::shared_ptr<const ImportedGround> ImportedSource::ground(std::int32_t rx, std::int32_t ry, bool drained) const {
    const auto held = held_.find({rx, ry});
    if (held == held_.end()) return nullptr;
    // Drained only when the drainage is there to be read.
    drained = drained && baked_ != nullptr;
    {
        const std::lock_guard<std::mutex> lock(guard_);
        if (const auto it = grounds_.find({rx, ry, drained}); it != grounds_.end()) return it->second;
    }
    auto g = std::make_shared<ImportedGround>();
    g->sampleMetres = std::int32_t(source_->schema().world.sampleMetres);
    const std::int64_t regionSamples = kRegionMetres / g->sampleMetres;
    g->key = drained ? drainedKeys_.at({rx, ry}) : held->second;
    g->side = std::int32_t(regionSamples) + 2 * ImportedGround::kMargin + 1;
    const std::size_t n = std::size_t(g->side) * std::size_t(g->side);
    const auto& schema = source_->schema();
    const ws::RasterDesc* height = nullptr;
    for (const auto& r : schema.rasters)
        if (r.kind == ws::RasterKind::Height) { height = &r; break; }
    // Each control mask by its name, wherever the schema keeps it.
    struct Mask { const char* name; std::vector<std::uint8_t>* into; const ws::RasterDesc* desc = nullptr; std::uint8_t channel = 0; };
    std::array<Mask, 6> masks{{{"erosion_strength", &g->erosion}, {"moisture_bias", &g->moisture},
                               {"forest_bias", &g->forest}, {"mountain_strength", &g->mountain},
                               {"river_strength", &g->river}, {"temperature_bias", &g->temperature}}};
    for (auto& m : masks)
        for (const auto& r : schema.rasters)
            for (std::size_t c = 0; c < r.channels.size(); ++c)
                if (r.kind == ws::RasterKind::Control && r.channels[c].name == m.name && !m.desc) {
                    m.desc = &r;
                    m.channel = std::uint8_t(c);
                }
    g->heightDm.assign(n, 0);
    for (auto& m : masks) if (m.desc) m.into->assign(n, 0);
    // The lakes: a flags layer with a "lake" bit.
    const ws::RasterDesc* water = nullptr;
    int lakeBit = 0;
    for (const auto& r : schema.rasters)
        if (r.kind == ws::RasterKind::Flags)
            if (const auto bit = r.bits.find("lake"); bit != r.bits.end()) { water = &r; lakeBit = bit->second; break; }
    if (water) g->lake.assign(n, 0);
    // The height as baked, when it is asked for; the source's own otherwise.
    const ws::WorldSource& heightSource = drained ? *baked_ : *source_;
    const ws::RasterDesc* heightDesc = drained ? baked_->schema().raster("height") : height;
    if (!heightDesc) { heightDesc = height; }
    const std::int64_t sx0 = std::int64_t(rx) * regionSamples - ImportedGround::kMargin;
    const std::int64_t sy0 = std::int64_t(ry) * regionSamples - ImportedGround::kMargin;
    const std::int64_t maxX = schema.world.samplesX() - 1, maxY = schema.world.samplesY() - 1;
    // Chunk by chunk, each read once; samples past the world's edge repeat it.
    std::map<ws::ChunkKey, ws::Chunk> chunks;
    const auto chunkOf = [&](const ws::ChunkKey& key) -> const ws::Chunk& {
        auto it = chunks.find(key);
        if (it == chunks.end()) it = chunks.emplace(key, source_->read(key).value_or(ws::Chunk{})).first;
        return it->second;
    };
    std::map<std::pair<ws::ChunkKey, std::string>, ws::Tile> tiles;
    const auto tileOf = [&](const ws::ChunkKey& key, const std::string& layer) -> const ws::Tile& {
        auto it = tiles.find({key, layer});
        if (it == tiles.end()) it = tiles.emplace(std::pair{key, layer}, source_->tile(chunkOf(key), layer)).first;
        return it->second;
    };
    std::map<ws::ChunkKey, ws::Tile> heightTiles;
    const auto heightTileOf = [&](const ws::ChunkKey& key) -> const ws::Tile& {
        auto it = heightTiles.find(key);
        if (it == heightTiles.end()) {
            const auto chunk = heightSource.read(key).value_or(ws::Chunk{});
            it = heightTiles.emplace(key, heightSource.tile(chunk, heightDesc->name)).first;
        }
        return it->second;
    };
    for (std::int32_t j = 0; j < g->side; ++j)
        for (std::int32_t i = 0; i < g->side; ++i) {
            const std::int64_t sx = std::clamp<std::int64_t>(sx0 + i, 0, maxX), sy = std::clamp<std::int64_t>(sy0 + j, 0, maxY);
            const auto key = source_->chunkAtSample(sx, sy);
            const std::int64_t lx = sx - key.x * source_->chunkSamples(), ly = sy - key.y * source_->chunkSamples();
            const std::size_t k = std::size_t(j) * std::size_t(g->side) + std::size_t(i);
            // The one place a float is read: the stored number to decimetres.
            g->heightDm[k] = std::int32_t(std::lround(heightDesc->decode(0, heightTileOf(key).at(lx, ly, 0)) * 10.0));
            if (water && (tileOf(key, water->name).at(lx, ly, 0) >> lakeBit) & 1u) g->lake[k] = 255;
            for (auto& m : masks) {
                if (!m.desc) continue;
                const double v = m.desc->decode(m.channel, tileOf(key, m.desc->name).at(lx, ly, m.channel));
                const auto& c = m.desc->channels[m.channel];
                const double t = c.max > c.min ? (v - c.min) / (c.max - c.min) : v;
                (*m.into)[k] = std::uint8_t(std::clamp<long>(std::lround(t * 255.0), 0, 255));
            }
        }
    const std::lock_guard<std::mutex> lock(guard_);
    return grounds_.emplace(std::tuple{rx, ry, drained}, std::move(g)).first->second;
}

std::shared_ptr<const engine::biomes::CategoryField> ImportedSource::categories() const {
    const std::lock_guard<std::mutex> lock(guard_);
    if (categoriesRead_) return categories_;
    categoriesRead_ = true;
    const auto& schema = source_->schema();
    std::array<const ws::RasterDesc*, engine::biomes::kLayers> layers{};
    bool any = false;
    for (std::size_t k = 0; k < engine::biomes::kLayers; ++k) {
        const auto* r = schema.raster(engine::biomes::kLayerNames[k]);
        if (r && r->kind == ws::RasterKind::Categorical && r->channels.size() == 1) { layers[k] = r; any = true; }
    }
    if (!any) return nullptr;
    // The forest_bias channel rides along: the forest biomes' density follows it.
    const ws::RasterDesc* control = nullptr;
    std::uint8_t biasChannel = 0;
    for (const auto& r : schema.rasters)
        for (std::size_t c = 0; c < r.channels.size(); ++c)
            if (r.kind == ws::RasterKind::Control && r.channels[c].name == "forest_bias" && !control) {
                control = &r;
                biasChannel = std::uint8_t(c);
            }
    auto field = std::make_shared<engine::biomes::CategoryField>();
    field->setSampleMetres(std::int64_t(schema.world.sampleMetres));
    for (const auto& [key, record] : source_->chunks()) {
        bool holds = false;
        for (const auto* r : layers) holds = holds || (r && record.layers.count(r->name));
        if (!holds) continue;
        const auto chunk = source_->read(key);
        if (!chunk) continue;
        std::array<ws::Tile, engine::biomes::kLayers> tiles;
        for (std::size_t k = 0; k < tiles.size(); ++k)
            if (layers[k]) tiles[k] = source_->tile(*chunk, layers[k]->name);
        const bool biased = control && record.layers.count(control->name);
        const ws::Tile bias = biased ? source_->tile(*chunk, control->name) : ws::Tile{};
        const auto side = std::int64_t(source_->chunkSamples());
        for (std::int64_t y = 0; y < side; ++y)
            for (std::int64_t x = 0; x < side; ++x) {
                engine::biomes::CategoryField::Ids ids{};
                for (std::size_t k = 0; k < tiles.size(); ++k)
                    if (layers[k]) ids[k] = std::uint8_t(std::min<std::uint16_t>(tiles[k].at(x, y, 0), 255));
                if (ids == engine::biomes::CategoryField::Ids{}) continue;   // sea, or nothing painted
                if (biased) {
                    const auto& c = control->channels[biasChannel];
                    const double v = control->decode(biasChannel, bias.at(x, y, biasChannel));
                    ids[4] = engine::biomes::CategoryField::forestBiasByte(c.max > c.min ? (v - c.min) / (c.max - c.min) : v);
                }
                if (ids != engine::biomes::CategoryField::Ids{})
                    field->set(key.x * side + x, key.y * side + y, ids);
            }
    }
    field->settle();
    if (field->empty()) return nullptr;
    categories_ = std::move(field);
    return categories_;
}

std::shared_ptr<const engine::biomes::DetailEdits> ImportedSource::details() const {
    const std::lock_guard<std::mutex> lock(guard_);
    if (!details_) {
        std::string why;
        auto edits = engine::biomes::DetailEdits::load(root_, &why);
        if (!why.empty()) std::cerr << "detail edits: " << why << "\n";
        details_ = std::make_shared<const engine::biomes::DetailEdits>(std::move(edits));
    }
    return details_;
}

bool importedIn(const WorldLayout& layout, std::int32_t rx, std::int32_t ry) {
    return layout.imported && layout.inBounds(rx, ry) && layout.imported->holds(rx, ry);
}

bool importDrained(const WorldLayout& layout) {
    if (!layout.imported) return false;
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx)
            if (importedIn(layout, rx, ry) && std::uint8_t(layout.at(rx, ry).stage) >= std::uint8_t(RegionStage::Relief))
                return true;
    return false;
}

std::shared_ptr<const ImportedSource> openImported(const std::filesystem::path& root, const WorldLayout& layout) {
    auto opened = ImportedSource::open(root);
    if (!opened || opened->drained()) return opened;
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx)
            if (opened->holds(rx, ry) && layout.inBounds(rx, ry) &&
                std::uint8_t(layout.at(rx, ry).stage) >= std::uint8_t(RegionStage::Relief))
                return ImportedSource::open(root, true);
    return opened;
}

} // namespace generation
