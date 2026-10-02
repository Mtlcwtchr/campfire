#include "game/generation/world_layers.hpp"

#include <algorithm>
#include <cmath>

#include "game/generation/world_map_gen.hpp"

namespace generation {
namespace {

// Resolutions are whole powers of two of the macro cell, so every layer's texel
// lines fall on cell lines, on page lines and on region lines alike.
constexpr std::array<LayerDef, kLayerCount> kLayers{{
        {LayerId::Continents, "continents", "Continents", "G1 continental shape", 2048,
         LayerKind::Override, 0.0f, 1023.0f, 820.0f, "shape"},
        {LayerId::Ranges, "ranges", "Ranges", "plates: uplift", 2048,
         LayerKind::Delta, -900.0f, 900.0f, 600.0f, "uplift"},
        {LayerId::Hills, "hills", "Hills", "G3 relief noise", 2048,
         LayerKind::Override, 0.0f, 1023.0f, 512.0f, "relief"},
        {LayerId::Sea, "sea", "Sea share", "sea level", 16384,
         LayerKind::Override, 0.0f, 100.0f, 90.0f, "%"},
        {LayerId::Weathering, "weathering", "Weathering", "G5 thermal erosion", 4096,
         LayerKind::Override, 0.0f, 24.0f, 8.0f, "passes"},
        {LayerId::Rain, "rain", "Rainfall", "C3 rainfall", 8192,
         LayerKind::Override, 20.0f, 250.0f, 180.0f, "%"},
        // A cell to a texel: what is painted is a kind, not an amount - dry
        // below a quarter, a river course up to three quarters, a lake above
        // (waterPaintOf, world_map_gen.hpp).
        {LayerId::Water, "water", "Water", "G6 lakes and rivers", 512,
         LayerKind::Override, 0.0f, 1000.0f, 1000.0f, "kind"},
}};

std::int32_t tilesFor(std::int32_t texels) { return (texels + kLayerTile - 1) / kLayerTile; }

} // namespace

const LayerDef& layerDef(LayerId id) { return kLayers[std::min(std::size_t(id), kLayerCount - 1)]; }

std::optional<LayerId> layerNamed(std::string_view name) {
    for (const LayerDef& def : kLayers)
        if (name == def.name) return def.id;
    return std::nullopt;
}

std::int32_t layerTexels(LayerId id, std::int32_t cells) {
    const std::int64_t metres = std::int64_t(std::max(0, cells)) * kMetresPerCell;
    const std::int64_t texel = layerDef(id).texelMetres;
    return static_cast<std::int32_t>((metres + texel - 1) / texel);
}

LayerMap::LayerMap(LayerId id, std::int32_t texelsX, std::int32_t texelsY) : id_(id) {
    resize(texelsX, texelsY);
}

bool LayerMap::empty() const {
    return std::none_of(tiles_.begin(), tiles_.end(), [](const auto& t) { return bool(t); });
}

std::size_t LayerMap::tileCount() const {
    return std::size_t(std::count_if(tiles_.begin(), tiles_.end(), [](const auto& t) { return bool(t); }));
}

float LayerMap::value(std::int32_t tx, std::int32_t ty) const {
    if (!inBounds(tx, ty)) return 0.0f;
    const auto& t = tiles_[std::size_t(ty / kLayerTile) * std::size_t(tilesX_) + std::size_t(tx / kLayerTile)];
    return t ? t->value[std::size_t(ty % kLayerTile) * kLayerTile + std::size_t(tx % kLayerTile)] : 0.0f;
}

float LayerMap::cover(std::int32_t tx, std::int32_t ty) const {
    if (!inBounds(tx, ty)) return 0.0f;
    const auto& t = tiles_[std::size_t(ty / kLayerTile) * std::size_t(tilesX_) + std::size_t(tx / kLayerTile)];
    return t ? t->cover[std::size_t(ty % kLayerTile) * kLayerTile + std::size_t(tx % kLayerTile)] : 0.0f;
}

LayerMap::Tile& LayerMap::writable(std::int32_t tileX, std::int32_t tileY) {
    auto& slot = tiles_[std::size_t(tileY) * std::size_t(tilesX_) + std::size_t(tileX)];
    if (!slot) slot = std::make_shared<Tile>();
    // Shared with a copy (an undo step, the world being built): this map's
    // own from here on, and the copy keeps what it had.
    else if (slot.use_count() > 1) slot = std::make_shared<Tile>(*slot);
    return *slot;
}

void LayerMap::set(std::int32_t tx, std::int32_t ty, float value, float cover) {
    if (!inBounds(tx, ty)) return;
    Tile& t = writable(tx / kLayerTile, ty / kLayerTile);
    const std::size_t i = std::size_t(ty % kLayerTile) * kLayerTile + std::size_t(tx % kLayerTile);
    if (def().kind == LayerKind::Override) {
        // Nothing covered holds nothing: a texel with no cover reads nought,
        // whatever was painted there before it was wiped, so two maps that
        // show the same thing are the same map.
        t.cover[i] = std::clamp(cover, 0.0f, 1.0f);
        t.value[i] = t.cover[i] > 0.0f ? value : 0.0f;
    } else {
        t.value[i] = value;
        t.cover[i] = 0.0f;
    }
}

bool LayerMap::holdsNothing(const Tile& tile) const {
    if (def().kind == LayerKind::Override)
        return std::all_of(tile.cover.begin(), tile.cover.end(), [](float c) { return c <= 0.0f; });
    return std::all_of(tile.value.begin(), tile.value.end(), [](float v) { return v == 0.0f; });
}

void LayerMap::prune(std::int32_t tx0, std::int32_t ty0, std::int32_t tx1, std::int32_t ty1) {
    const std::int32_t gx0 = std::max(0, tx0 / kLayerTile), gy0 = std::max(0, ty0 / kLayerTile);
    const std::int32_t gx1 = std::min(tilesX_ - 1, tx1 / kLayerTile), gy1 = std::min(tilesY_ - 1, ty1 / kLayerTile);
    for (std::int32_t gy = gy0; gy <= gy1; ++gy)
        for (std::int32_t gx = gx0; gx <= gx1; ++gx) {
            auto& slot = tiles_[std::size_t(gy) * std::size_t(tilesX_) + std::size_t(gx)];
            if (slot && holdsNothing(*slot)) slot.reset();
        }
}

void LayerMap::clear() {
    for (auto& slot : tiles_) slot.reset();
}

LayerMap::Sample LayerMap::sample(double u, double v) const {
    Sample s;
    if (texelsX_ <= 0 || texelsY_ <= 0) return s;
    u = std::clamp(u, 0.0, double(texelsX_ - 1));
    v = std::clamp(v, 0.0, double(texelsY_ - 1));
    const auto x0 = std::int32_t(std::floor(u)), y0 = std::int32_t(std::floor(v));
    const std::int32_t x1 = std::min(x0 + 1, texelsX_ - 1), y1 = std::min(y0 + 1, texelsY_ - 1);
    const double fx = u - x0, fy = v - y0;
    const bool delta = def().kind == LayerKind::Delta;
    const auto corner = [&](std::int32_t x, std::int32_t y, double w) {
        if (w <= 0.0) return;
        const float c = delta ? 1.0f : cover(x, y);
        s.painted += float(w) * value(x, y) * c;
        s.cover += float(w) * (delta ? 0.0f : c);
    };
    corner(x0, y0, (1.0 - fx) * (1.0 - fy));
    corner(x1, y0, fx * (1.0 - fy));
    corner(x0, y1, (1.0 - fx) * fy);
    corner(x1, y1, fx * fy);
    // Covered all round is covered: the four weights do not always sum to one
    // in floating point, and ground painted solid must read as the paint, not
    // as the paint and a ten-millionth of what was under it.
    if (!delta && s.cover > 0.99999f) {
        s.painted /= s.cover;
        s.cover = 1.0f;
    }
    return s;
}

LayerMap::Sample LayerMap::atCell(std::int32_t cellX, std::int32_t cellY) const {
    const double perTexel = double(def().texelMetres) / kMetresPerCell;
    return sample((cellX + 0.5) / perTexel - 0.5, (cellY + 0.5) / perTexel - 0.5);
}

void LayerMap::resize(std::int32_t texelsX, std::int32_t texelsY) {
    const std::int32_t tilesX = tilesFor(std::max(0, texelsX)), tilesY = tilesFor(std::max(0, texelsY));
    std::vector<std::shared_ptr<Tile>> tiles(std::size_t(tilesX) * std::size_t(tilesY));
    for (std::int32_t y = 0; y < std::min(tilesY, tilesY_); ++y)
        for (std::int32_t x = 0; x < std::min(tilesX, tilesX_); ++x)
            tiles[std::size_t(y) * std::size_t(tilesX) + std::size_t(x)] =
                    tiles_[std::size_t(y) * std::size_t(tilesX_) + std::size_t(x)];
    texelsX_ = std::max(0, texelsX);
    texelsY_ = std::max(0, texelsY);
    tilesX_ = tilesX;
    tilesY_ = tilesY;
    tiles_ = std::move(tiles);
    // A tile that now hangs over the new edge keeps texels nobody can reach;
    // they are cleared, so the map is equal to one painted at this size.
    for (std::int32_t gy = 0; gy < tilesY_; ++gy)
        for (std::int32_t gx = 0; gx < tilesX_; ++gx) {
            if (!tile(gx, gy)) continue;
            const bool overX = (gx + 1) * kLayerTile > texelsX_, overY = (gy + 1) * kLayerTile > texelsY_;
            if (!overX && !overY) continue;
            Tile& t = writable(gx, gy);
            for (std::int32_t y = 0; y < kLayerTile; ++y)
                for (std::int32_t x = 0; x < kLayerTile; ++x)
                    if (gx * kLayerTile + x >= texelsX_ || gy * kLayerTile + y >= texelsY_) {
                        t.value[std::size_t(y) * kLayerTile + std::size_t(x)] = 0.0f;
                        t.cover[std::size_t(y) * kLayerTile + std::size_t(x)] = 0.0f;
                    }
        }
    prune(0, 0, texelsX_ - 1, texelsY_ - 1);
}

const LayerMap::Tile* LayerMap::tile(std::int32_t tileX, std::int32_t tileY) const {
    if (tileX < 0 || tileY < 0 || tileX >= tilesX_ || tileY >= tilesY_) return nullptr;
    return tiles_[std::size_t(tileY) * std::size_t(tilesX_) + std::size_t(tileX)].get();
}

void LayerMap::putTile(std::int32_t tileX, std::int32_t tileY, const Tile& tile) {
    if (tileX < 0 || tileY < 0 || tileX >= tilesX_ || tileY >= tilesY_) return;
    tiles_[std::size_t(tileY) * std::size_t(tilesX_) + std::size_t(tileX)] = std::make_shared<Tile>(tile);
    prune(tileX * kLayerTile, tileY * kLayerTile, tileX * kLayerTile, tileY * kLayerTile);
}

bool LayerMap::operator==(const LayerMap& other) const {
    if (id_ != other.id_ || texelsX_ != other.texelsX_ || texelsY_ != other.texelsY_) return false;
    static const Tile nothing{};
    for (std::size_t i = 0; i < tiles_.size(); ++i) {
        const Tile* a = tiles_[i].get();
        const Tile* b = other.tiles_[i].get();
        if (a == b) continue;
        if (!(*(a ? a : &nothing) == *(b ? b : &nothing))) return false;
    }
    return true;
}

LayerCells layerCells(const LayerMap& map, std::int32_t width, std::int32_t height) {
    LayerCells cells;
    if (map.empty() || width <= 0 || height <= 0) return cells;
    cells.any = true;
    const std::size_t count = std::size_t(width) * std::size_t(height);
    cells.painted.assign(count, 0.0f);
    cells.cover.assign(count, 0.0f);
    // Only the tiles that exist are visited, and a cell is only sampled if a
    // tile is near enough to reach it: a layer painted in one corner of a
    // giant world costs that corner.
    const double perTexel = double(map.def().texelMetres) / kMetresPerCell;
    for (std::int32_t gy = 0; gy < map.tilesY(); ++gy)
        for (std::int32_t gx = 0; gx < map.tilesX(); ++gx) {
            if (!map.tile(gx, gy)) continue;
            // Texels of this tile, one either side for the bilinear reach, in cells.
            const auto x0 = std::int32_t(std::floor((gx * kLayerTile - 1) * perTexel));
            const auto y0 = std::int32_t(std::floor((gy * kLayerTile - 1) * perTexel));
            const auto x1 = std::int32_t(std::ceil(((gx + 1) * kLayerTile + 1) * perTexel));
            const auto y1 = std::int32_t(std::ceil(((gy + 1) * kLayerTile + 1) * perTexel));
            for (std::int32_t y = std::max(0, y0); y < std::min(height, y1); ++y)
                for (std::int32_t x = std::max(0, x0); x < std::min(width, x1); ++x) {
                    const std::size_t i = std::size_t(y) * std::size_t(width) + std::size_t(x);
                    const LayerMap::Sample s = map.atCell(x, y);
                    cells.painted[i] = s.painted;
                    cells.cover[i] = s.cover;
                }
        }
    return cells;
}

} // namespace generation

