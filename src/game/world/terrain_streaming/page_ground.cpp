#include "game/world/terrain_streaming/page_ground.hpp"

#include <algorithm>

#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::streaming {
namespace {
using core::Fixed;
}

PageGround::PageGround(const BakedPage& page)
    : page_(page), quantisation_{page.base.elevationMin, page.base.elevationMax} {
    if (!page_.base.valid()) return;
    side_ = static_cast<std::int64_t>(page_.base.width) + 2 * page_.base.padding;
    const auto corner = tileOrigin(page_.base.key);
    const Fixed back = Fixed::fromInt(static_cast<std::int64_t>(page_.base.padding) *
                                      page_.base.sampleMetres);
    origin_ = {corner.x - back, corner.y - back};
}

PageGround::Spot PageGround::spotOf(core::WorldPos at) const {
    Spot spot;
    if (side_ <= 0) return spot;
    const Fixed step = Fixed::fromInt(page_.base.sampleMetres);
    const Fixed gx = (at.x - origin_.x) / step;
    const Fixed gy = (at.y - origin_.y) / step;
    spot.column = gx.toInt();
    spot.row = gy.toInt();
    spot.fx = gx - Fixed::fromInt(spot.column);
    spot.fy = gy - Fixed::fromInt(spot.row);
    // A sample needs its own cardinal neighbours for a slope, so the outermost
    // ring cannot answer and neither can a square that reaches into it.
    spot.inside = spot.column >= 1 && spot.row >= 1 && spot.column + 2 < side_ &&
                  spot.row + 2 < side_;
    return spot;
}

Fixed PageGround::heightOf(std::int64_t column, std::int64_t row) const {
    const auto at = static_cast<std::size_t>(row * side_ + column);
    return quantisation_.dequantise(page_.base.heightQuantized[at]);
}

Fixed PageGround::depthOf(std::int64_t column, std::int64_t row) const {
    const auto at = static_cast<std::size_t>(row * side_ + column);
    if (page_.water.surfaceQuantized.size() != page_.base.heightQuantized.size()) return core::kZero;
    if (page_.water.waterBodyId[at] == kInvalidWaterBodyId &&
        page_.water.surfaceQuantized[at] == 0)
        return core::kZero;
    const Fixed surface = quantisation_.dequantise(page_.water.surfaceQuantized[at]);
    return core::max(core::kZero, surface - heightOf(column, row));
}

Fixed PageGround::slopeOf(std::int64_t column, std::int64_t row) const {
    const Fixed run = Fixed::fromInt(2 * page_.base.sampleMetres);
    const Fixed dzdx = (heightOf(column + 1, row) - heightOf(column - 1, row)) / run;
    const Fixed dzdy = (heightOf(column, row + 1) - heightOf(column, row - 1)) / run;
    return core::sqrt(dzdx * dzdx + dzdy * dzdy);
}

bool PageGround::covers(core::WorldPos at) const { return spotOf(at).inside; }

Fixed PageGround::heightAt(core::WorldPos at) const {
    const Spot spot = spotOf(at);
    if (!spot.inside) return core::kZero;
    const Fixed top = core::lerp(heightOf(spot.column, spot.row),
                                 heightOf(spot.column + 1, spot.row), spot.fx);
    const Fixed bottom = core::lerp(heightOf(spot.column, spot.row + 1),
                                    heightOf(spot.column + 1, spot.row + 1), spot.fx);
    return core::lerp(top, bottom, spot.fy);
}

Fixed PageGround::depthAt(core::WorldPos at) const {
    const Spot spot = spotOf(at);
    if (!spot.inside) return core::kZero;
    const Fixed top = core::lerp(depthOf(spot.column, spot.row),
                                 depthOf(spot.column + 1, spot.row), spot.fx);
    const Fixed bottom = core::lerp(depthOf(spot.column, spot.row + 1),
                                    depthOf(spot.column + 1, spot.row + 1), spot.fx);
    return core::max(core::kZero, core::lerp(top, bottom, spot.fy));
}

Fixed PageGround::slopeAt(core::WorldPos at) const {
    const Spot spot = spotOf(at);
    if (!spot.inside) return core::kZero;
    const Fixed top = core::lerp(slopeOf(spot.column, spot.row),
                                 slopeOf(spot.column + 1, spot.row), spot.fx);
    const Fixed bottom = core::lerp(slopeOf(spot.column, spot.row + 1),
                                    slopeOf(spot.column + 1, spot.row + 1), spot.fx);
    return core::lerp(top, bottom, spot.fy);
}

Fixed PageGround::steepnessAt(core::WorldPos at) const {
    const Spot spot = spotOf(at);
    if (!spot.inside) return core::kZero;
    // The sharpest of the four edges of the square the point falls in. It is
    // constant within that square on purpose: a bank is a bank wherever in
    // the square you stand, and reading it any finer only makes it flicker.
    const Fixed a = heightOf(spot.column, spot.row);
    const Fixed b = heightOf(spot.column + 1, spot.row);
    const Fixed c = heightOf(spot.column, spot.row + 1);
    const Fixed d = heightOf(spot.column + 1, spot.row + 1);
    Fixed sharpest = core::kZero;
    for (const Fixed drop : {core::abs(b - a), core::abs(d - c), core::abs(c - a),
                             core::abs(d - b)})
        sharpest = core::max(sharpest, drop);
    return sharpest / Fixed::fromInt(page_.base.sampleMetres);
}

Travel PageGround::travelAt(core::WorldPos at) const {
    const Spot spot = spotOf(at);
    if (!spot.inside) return Travel::None;
    return travelFrom(depthAt(at), steepnessAt(at), slopeAt(at));
}

PageLattice::PageLattice(PageStore& store, std::uint8_t level)
    : store_(store), level_(level) {
    stride_ = std::int64_t(1) << level_;
    // A page is five hundred and twelve metres at every level, so it is always
    // a hundred and twenty-eight four-metre indices across; what the level
    // changes is how many of them carry a sample.
    perPage_ = 128 / stride_;
    if (perPage_ < 1) perPage_ = 1;
}

const BakedPage* PageLattice::pageFor(std::int64_t sx, std::int64_t sy) {
    const TileKey key{static_cast<std::int32_t>(floorDiv(sx, 128)),
                      static_cast<std::int32_t>(floorDiv(sy, 128)), level_};
    if (!(key == heldKey_) || held_ == nullptr) {
        held_ = store_.page(key);
        heldKey_ = key;
    }
    return held_.get();
}

bool PageLattice::known(std::int64_t sx, std::int64_t sy) {
    return pageFor(sx, sy) != nullptr;
}

MaterialWeights PageLattice::materials(std::int64_t sx, std::int64_t sy) {
    MaterialWeights out{};
    const BakedPage* page = pageFor(sx, sy);
    if (page == nullptr || page->materials.empty()) {
        out.weight[static_cast<std::size_t>(Material::Grass)] = core::kOne;
        return out;
    }
    // Where this lattice index falls on the material grid, in its own units.
    const BaseTile& tile = page->base;
    const std::int64_t side = static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
    const std::int64_t every = page->materialMetres / tile.sampleMetres;
    const std::int64_t column =
            floorDiv(sx, stride_) - std::int64_t(heldKey_.x) * perPage_ + tile.padding;
    const std::int64_t row =
            floorDiv(sy, stride_) - std::int64_t(heldKey_.y) * perPage_ + tile.padding;
    if (column < 0 || row < 0 || column >= side || row >= side || every <= 0) {
        out.weight[static_cast<std::size_t>(Material::Grass)] = core::kOne;
        return out;
    }
    const std::int64_t wide = page->materialWidth;
    const std::int64_t gx = std::min(column / every, wide - 1);
    const std::int64_t gy = std::min(row / every, wide - 1);
    const std::int64_t gx1 = std::min(gx + 1, wide - 1);
    const std::int64_t gy1 = std::min(gy + 1, wide - 1);
    const Fixed fx = Fixed::ratio(column - gx * every, every);
    const Fixed fy = Fixed::ratio(row - gy * every, every);
    const auto weightOf = [&](std::int64_t x, std::int64_t y, std::size_t m) {
        return Fixed::ratio(
                page->materials[static_cast<std::size_t>(y * wide + x) * kMaterialCount + m], 255);
    };
    for (std::size_t m = 0; m < kMaterialCount; ++m) {
        const Fixed top = weightOf(gx, gy, m) + (weightOf(gx1, gy, m) - weightOf(gx, gy, m)) * fx;
        const Fixed bottom =
                weightOf(gx, gy1, m) + (weightOf(gx1, gy1, m) - weightOf(gx, gy1, m)) * fx;
        out.weight[m] = top + (bottom - top) * fy;
    }
    out.normalise();
    return out;
}

Fixed PageLattice::visualHeight(std::int64_t sx, std::int64_t sy) {
    // One look for the page, not two. This used to call height(), which finds
    // the page, and then find it again for the bands.
    const BakedPage* page = pageFor(sx, sy);
    if (page == nullptr) return core::kZero;
    const BaseTile& tile = page->base;
    const std::int64_t side = static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
    const std::int64_t column =
            floorDiv(sx, stride_) - std::int64_t(heldKey_.x) * perPage_ + tile.padding;
    const std::int64_t row =
            floorDiv(sy, stride_) - std::int64_t(heldKey_.y) * perPage_ + tile.padding;
    if (column < 0 || row < 0 || column >= side || row >= side) return core::kZero;
    const auto at = static_cast<std::size_t>(row * side + column);
    const HsimQuantisation quantisation{tile.elevationMin, tile.elevationMax};
    Fixed detail = core::kZero;
    for (const ResidualTile* band : {&page->large, &page->medium})
        if (at < band->deltaQuantized.size())
            detail += Fixed::ratio(band->deltaQuantized[at], kResidualPerMetre);
    return quantisation.dequantise(tile.heightQuantized[at]) + detail;
}

Fixed PageLattice::height(std::int64_t sx, std::int64_t sy) {
    const BakedPage* page = pageFor(sx, sy);
    if (page == nullptr) return core::kZero;
    const BaseTile& tile = page->base;
    const std::int64_t side = static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
    const std::int64_t column =
            floorDiv(sx, stride_) - std::int64_t(heldKey_.x) * perPage_ + tile.padding;
    const std::int64_t row =
            floorDiv(sy, stride_) - std::int64_t(heldKey_.y) * perPage_ + tile.padding;
    if (column < 0 || row < 0 || column >= side || row >= side) {
        // Off this page's padding: the sample belongs to a neighbour, which
        // only happens when the index was not a multiple of the stride.
        return core::kZero;
    }
    const HsimQuantisation quantisation{tile.elevationMin, tile.elevationMax};
    return quantisation.dequantise(
            tile.heightQuantized[static_cast<std::size_t>(row * side + column)]);
}

PageLattice::Water PageLattice::waterAt(core::WorldPos at, Fixed footprint) {
    Water out;
    const std::int64_t metres = 4 * stride_;
    const BakedPage* page = pageFor(floorDiv(at.x.toInt(), 4) & ~(stride_ - 1),
                                    floorDiv(at.y.toInt(), 4) & ~(stride_ - 1));
    if (page == nullptr || !page->water.valid()) return out;
    const WaterTile& water = page->water;
    const BaseTile& tile = page->base;
    const std::int64_t side = static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
    // The stored sample south-west of the point, in this page's own indices.
    const Fixed step = Fixed::fromInt(metres);
    const auto corner = tileOrigin(tile.key);
    const Fixed back = Fixed::fromInt(static_cast<std::int64_t>(tile.padding) * metres);
    const Fixed gx = (at.x - (corner.x - back)) / step;
    const Fixed gy = (at.y - (corner.y - back)) / step;
    const std::int64_t column = gx.toInt(), row = gy.toInt();
    if (column < 0 || row < 0 || column + 1 >= side || row + 1 >= side) return out;
    const Fixed fx = gx - Fixed::fromInt(column), fy = gy - Fixed::fromInt(row);

    const HsimQuantisation quantisation{tile.elevationMin, tile.elevationMax};
    const auto index = [&](std::int64_t c, std::int64_t r) {
        return static_cast<std::size_t>(r * side + c);
    };
    // Depth rather than surface, for the reason a shoreline exists at all: a
    // surface averaged with the dry ground beside it would stand over land.
    const auto depthOf = [&](std::int64_t c, std::int64_t r) {
        const auto at2 = index(c, r);
        if (water.waterBodyId[at2] == kInvalidWaterBodyId && water.surfaceQuantized[at2] == 0)
            return core::kZero;
        return core::max(core::kZero,
                         quantisation.dequantise(water.surfaceQuantized[at2]) -
                                 quantisation.dequantise(tile.heightQuantized[at2]));
    };
    const auto blend = [&](auto&& of) {
        const Fixed top = core::lerp(of(column, row), of(column + 1, row), fx);
        const Fixed bottom = core::lerp(of(column, row + 1), of(column + 1, row + 1), fx);
        return core::lerp(top, bottom, fy);
    };
    const Fixed depth = blend(depthOf);
    const Fixed ground = blend([&](std::int64_t c, std::int64_t r) {
        return quantisation.dequantise(tile.heightQuantized[index(c, r)]);
    });
    // Over the footprint the caller is drawing with, when that is wider than
    // a sample; over the sample's own footprint, which the page already
    // measured, when it is not.
    const std::int64_t reach =
            (footprint / Fixed::fromInt(2 * metres)).roundToInt();
    if (reach >= 1) {
        std::int64_t wet = 0, seen = 0;
        for (std::int64_t r = row - reach; r <= row + 1 + reach; ++r)
            for (std::int64_t c = column - reach; c <= column + 1 + reach; ++c) {
                if (r < 0 || c < 0 || r >= side || c >= side) continue;
                ++seen;
                if (depthOf(c, r).raw > 0) ++wet;
            }
        out.cover = seen > 0 ? Fixed::ratio(wet, seen) : core::kZero;
    } else {
        out.cover = core::saturate(blend([&](std::int64_t c, std::int64_t r) {
            return Fixed::ratio(water.coverage[index(c, r)], 255);
        }));
    }
    if (depth.raw <= 0 && out.cover.raw <= 0) return out;
    out.any = true;
    if (depth.raw > 0) {
        // Under water: the surface is the ground plus how deep it stands,
        // which is exact at the shoreline and smooth either side of it.
        out.level = ground + depth;
    } else {
        // Dry, but inside the footprint of water that reaches it. What a
        // shader wants here is where that water stands, so it can run surf up
        // the beach and stop it: the ground is above the level, and the
        // difference is how far up. Water finds its own level, and the level
        // is the highest one that reaches - a step between samples is
        // invisible under a coverage that is fading out anyway.
        Fixed highest = ground;
        bool found = false;
        const std::int64_t look = core::max(Fixed::fromInt(reach), core::kOne).toInt();
        for (std::int64_t r = row - look; r <= row + 1 + look; ++r)
            for (std::int64_t c = column - look; c <= column + 1 + look; ++c) {
                if (r < 0 || c < 0 || r >= side || c >= side) continue;
                if (depthOf(c, r).raw <= 0) continue;
                const Fixed surface = quantisation.dequantise(water.surfaceQuantized[index(c, r)]);
                if (!found || surface.raw > highest.raw) {
                    highest = surface;
                    found = true;
                }
            }
        out.level = highest;
    }
    out.flowX = blend([&](std::int64_t c, std::int64_t r) {
        return Fixed::ratio(water.flowX[index(c, r)], 127);
    });
    out.flowY = blend([&](std::int64_t c, std::int64_t r) {
        return Fixed::ratio(water.flowY[index(c, r)], 127);
    });
    // A body is a name, not a quantity: the nearest sample's, never a blend
    // of two of them.
    const std::int64_t nearC = fx > core::Fixed::ratio(1, 2) ? column + 1 : column;
    const std::int64_t nearR = fy > core::Fixed::ratio(1, 2) ? row + 1 : row;
    out.body = water.waterBodyId[index(nearC, nearR)];
    return out;
}

} // namespace world::streaming
