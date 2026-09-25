#include "game/world/tile_mesh.hpp"

#include <algorithm>
#include <stdexcept>

#include "game/world/terrain_streaming/page_ground.hpp"
#include "game/world/terrain_lod.hpp"

namespace world {
namespace {
using core::Fixed;
using Key = TileSampleCache::Key;

// The square lattice of one level, read off the pages.
//
// The same numbers the ring sampler read, on a grid instead of on a circle.
// What is new is that the grid is the world's own: a vertex is at a multiple
// of this level's spacing, so the coarse level's vertices are a subset of the
// fine level's and the two surfaces meet exactly where they share a sample.
class TileSampler {
public:
    TileSampler(const HeightField& field, streaming::PageStore& pages,
                const ClimateField& climate, std::int32_t lod, TileSampleCache& cache)
        : field_(field), climate_(climate),
          lattice_(pages, terrain::dataLevelForGeometryLevel(lod)),
          // The level above, for the morph target. It is a different surface,
          // not a coarser reading of this one: its pages carry fewer bands of
          // visual detail, which is exactly what refinement means. Averaging
          // this level's own samples would aim the morph at a smoothed copy of
          // where it already is, and the swap would still be visible.
          above_(pages, terrain::dataLevelForGeometryLevel(lod + 1)),
          stride_(std::int64_t(1) << std::clamp(lod, 0, 7)),
          heights_(cache.heights), vertices_(cache.vertices) {
        // The cache holds one level at a time and a bounded number of samples:
        // a worker walks a tile and then its neighbour, so what it shares is
        // an edge, and remembering a whole region would be remembering ground
        // it will not be asked for again.
        if (cache.lod != lod || vertices_.size() > 32768 || heights_.size() > 131072) {
            vertices_.clear();
            heights_.clear();
        }
        cache.lod = lod;
    }

    // A vertex of the lattice itself, not an interpolation of it. This is the
    // whole difference from the polar mesh: the point asked for is a sample.
    const TerrainVertex& at(std::int64_t sx, std::int64_t sy) {
        const Key key{sx, sy};
        const auto found = vertices_.find(key);
        if (found != vertices_.end()) return found->second;

        TerrainVertex v{};
        v.position = {Fixed::fromInt(sx * kSampleMetres), Fixed::fromInt(sy * kSampleMetres)};
        v.height = height(sx, sy);
        const Fixed run = Fixed::fromInt(2 * stride_ * kSampleMetres);
        const Fixed dx = (height(sx + stride_, sy) - height(sx - stride_, sy)) / run;
        const Fixed dy = (height(sx, sy + stride_) - height(sx, sy - stride_)) / run;
        const Fixed length = core::sqrt(dx * dx + dy * dy + core::kOne);
        v.normal = {-dx / length, -dy / length, core::kOne / length};
        v.materials = lattice_.materials(sx, sy);
        // From the one field the world has, not from a copy of it kept in
        // every page: climate is a macro quantity and a page is the wrong
        // home for a thing there is only one of.
        const auto weather = climate_.at(v.position);
        v.foliage = weather.foliage;
        v.desertCover = weather.desert;
        v.environment = weather.environment;
        if (stride_ <= 8 && v.materials.of(Material::Sand) > Fixed::ratio(18, 100))
            v.windExposure = field_.sandWindExposureAt(v.position, v.height);
        // How this point stands against the ground a little way off. Four
        // samples the builder already has in hand.
        v.openness = v.height - (height(sx - 2 * stride_, sy) + height(sx + 2 * stride_, sy) +
                                 height(sx, sy - 2 * stride_) + height(sx, sy + 2 * stride_)) /
                                        Fixed::fromInt(4);
        // Where the level above puts this same point. Every second vertex of
        // this lattice is a vertex of that one, so for those it is this height
        // and nothing moves; the ones between sit on the coarse surface, which
        // is the average of the two coarse samples either side of them. That
        // is the target a morph walks to, and it exists here because the
        // lattices nest - on a polar mesh there was nothing to walk to at all.
        const bool oddX = ((sx / stride_) & 1) != 0, oddY = ((sy / stride_) & 1) != 0;
        if (!oddX && !oddY) {
            // A vertex the coarse level has too: its own height there.
            v.coarseHeight = coarse(sx, sy);
        } else if (oddX && !oddY) {
            v.coarseHeight = (coarse(sx - stride_, sy) + coarse(sx + stride_, sy)) / 2;
        } else if (!oddX && oddY) {
            v.coarseHeight = (coarse(sx, sy - stride_) + coarse(sx, sy + stride_)) / 2;
        } else {
            // The middle of a coarse cell: the coarse surface passes over it
            // along the diagonal of the quad the coarse mesh draws there.
            v.coarseHeight = (coarse(sx - stride_, sy - stride_) +
                              coarse(sx + stride_, sy + stride_)) / 2;
        }
        // And how the level above lights it. A hillside drawn from samples
        // twice as far apart is a different shape, so its normal is a
        // different normal; leaving this equal to the fine one made the
        // geometry morph while the light jumped.
        {
            const std::int64_t wide = stride_ * 2;
            const Fixed run = Fixed::fromInt(2 * wide * kSampleMetres);
            const Fixed cdx = (coarse(sx + wide, sy) - coarse(sx - wide, sy)) / run;
            const Fixed cdy = (coarse(sx, sy + wide) - coarse(sx, sy - wide)) / run;
            const Fixed len = core::sqrt(cdx * cdx + cdy * cdy + core::kOne);
            v.coarseNormal = {-cdx / len, -cdy / len, core::kOne / len};
        }
        return vertices_.emplace(key, v).first->second;
    }

private:
    // The level above's own surface at a lattice point it shares with this one.
    Fixed coarse(std::int64_t x, std::int64_t y) {
        const Key key{~x, ~y};   // its own room in the same memo
        const auto found = heights_.find(key);
        if (found != heights_.end()) return found->second;
        return heights_.emplace(key, above_.visualHeight(x, y)).first->second;
    }

    Fixed height(std::int64_t x, std::int64_t y) {
        const Key key{x, y};
        const auto found = heights_.find(key);
        if (found != heights_.end()) return found->second;
        return heights_.emplace(key, lattice_.visualHeight(x, y)).first->second;
    }

    const HeightField& field_;
    const ClimateField& climate_;
    streaming::PageLattice lattice_;
    streaming::PageLattice above_;
    std::int64_t stride_;
    std::unordered_map<Key, Fixed, TileSampleCache::Hash>& heights_;
    std::unordered_map<Key, TerrainVertex, TileSampleCache::Hash>& vertices_;
};
} // namespace

TerrainMesh buildTileMesh(const HeightField& field, streaming::PageStore& pages,
                          const ClimateField& climate, TileId id,
                          const std::function<bool()>& cancelled, TileSampleCache* samples) {
    if (id.lod < 0 || id.lod > 7) throw std::invalid_argument("invalid tile level");
    TerrainMesh mesh;
    mesh.lod = id.lod;
    mesh.chunk = {id.x, id.y};
    mesh.side = kTileCells + 1;
    mesh.lowest = Fixed::fromInt(1 << 20);
    mesh.highest = -mesh.lowest;

    TileSampleCache local;
    TileSampler sampler(field, pages, climate, id.lod, samples ? *samples : local);

    const std::int64_t stride = std::int64_t(1) << id.lod;
    const std::int64_t firstX = static_cast<std::int64_t>(id.x) * kTileCells * stride;
    const std::int64_t firstY = static_cast<std::int64_t>(id.y) * kTileCells * stride;

    constexpr std::int32_t side = kTileCells + 1;
    mesh.vertices.reserve(static_cast<std::size_t>(side) * side + 4 * side);
    for (std::int32_t row = 0; row < side; ++row) {
        if ((row % 8) == 0 && cancelled && cancelled()) return {};
        for (std::int32_t column = 0; column < side; ++column) {
            const TerrainVertex& v = sampler.at(firstX + column * stride, firstY + row * stride);
            mesh.lowest = core::min(mesh.lowest, v.height);
            mesh.highest = core::max(mesh.highest, v.height);
            mesh.vertices.push_back(v);
        }
    }

    mesh.indices.reserve(static_cast<std::size_t>(kTileCells) * kTileCells * 6);
    for (std::int32_t row = 0; row < kTileCells; ++row)
        for (std::int32_t column = 0; column < kTileCells; ++column) {
            const auto at = static_cast<std::uint32_t>(row * side + column);
            const auto below = at + side;
            for (const std::uint32_t index : {at, at + 1, below, at + 1, below + 1, below})
                mesh.indices.push_back(index);
        }

    // The skirt. A copy of each border vertex dropped straight down, and a
    // quad joining the two, so a seam against a neighbour of another level is
    // filled with ground rather than with sky.
    const Fixed drop = Fixed::fromInt(sampleMetresAt(id.lod) * 2);
    const auto hem = [&](std::int32_t from, std::int32_t to, std::int32_t step) {
        const auto first = static_cast<std::uint32_t>(mesh.vertices.size());
        std::int32_t count = 0;
        for (std::int32_t at = from; ; at += step, ++count) {
            TerrainVertex v = mesh.vertices[static_cast<std::size_t>(at)];
            v.height -= drop;
            v.coarseHeight -= drop;
            mesh.vertices.push_back(v);
            if (at == to) break;
        }
        for (std::int32_t n = 0; n < count; ++n) {
            const auto top = static_cast<std::uint32_t>(from + n * step);
            const auto next = static_cast<std::uint32_t>(from + (n + 1) * step);
            const std::uint32_t low = first + static_cast<std::uint32_t>(n);
            for (const std::uint32_t index : {top, low, next, next, low, low + 1})
                mesh.indices.push_back(index);
        }
    };
    // Same outward winding and vertex order as the immutable GPU grid.
    hem(0, side - 1, 1);                                  // north
    hem(side * side - 1, (side - 1) * side, -1);          // south
    hem((side - 1) * side, 0, -side);                     // west
    hem(side - 1, side * side - 1, side);                 // east
    return mesh;
}

} // namespace world
