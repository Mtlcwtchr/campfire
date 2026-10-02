#pragma once
// Runtime terrain topology selection. Geometry is an immutable grid; changing
// detail changes only the active quadtree cut and the height field sampled by it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace generation { struct WorldMapData; }

namespace world::terrain {

inline constexpr std::int32_t kFinestGeometryMetres = 4;
inline constexpr std::int32_t kPersistentDetailMetres = 16;
inline constexpr std::int32_t kFoundationMetres = 64;
// Four kilometres to a step at the coarse end, and it was two hundred and
// fifty-six.
//
// The thresholds below are an EDGE LENGTH in pixels - ninety-six of them - and
// the selection walks the step up until it reaches that. It could not: it ran
// into this ceiling first. Pulled back to see a hundred-kilometre world on a
// 1080p screen the ground is about seventy metres to the pixel, which asks for
// a step of six and a half kilometres and got two hundred and fifty-six metres
// - an edge under four pixels where ninety-six were wanted. That is six
// hundred times the triangles the policy asked for, every one of them smaller
// than a pixel, which is why the whole world came out as a flat wash of colour
// with no silhouette in it.
//
// Four kilometres is where it stops being worth going. The data underneath is
// H64, so a step that size already spans sixty-four samples and is a gross
// simplification of them; past that the saving is small and the ground is a
// plane anyway.
inline constexpr std::int32_t kCoarsestRasterMetres = 4096;
// Steps 4, 8, ... 4096: eleven of them.
inline constexpr std::size_t kGeometryLevels = 11;

constexpr bool validGeometryStep(std::int32_t metres) {
    return metres >= kFinestGeometryMetres && metres <= kCoarsestRasterMetres &&
           (metres & (metres - 1)) == 0;
}

// Data quality is a separate budget, not a command to split the mesh. The
// minimum profile remains available to CPU/low-memory consumers. The renderer
// keeps one finer level: 8 m/H4, 16 m/H8, 32-64 m/H16, 128-256 m/H64.
struct DataLodPolicy {
    int finerLevels = 0;
    int finestLevel = 0;
    bool targeted = false;
    bool stableFeatures = false; // fixed H2 feature blocks; independent of view LOD
    bool cameraBudget = false; // geometry lattice follows LOD, bounded incremental builds
    int chunkCells = 64; // legacy CPU policy; runtime chunks explicitly use 32
    // Zero table keeps the legacy quadtree. Equal extents refine one footprint;
    // doubled extents replace a parent with four spatial children.
    std::array<int, kGeometryLevels> chunkMetres{};
    // Past H64: H256 and H1024, whose pages are two and eight kilometres
    // (tile_layout.hpp). The renderer's policies; CPU fixtures stop at H64.
    bool widePages = false;
    std::int64_t metresAt(int lod) const {
        return chunkMetres[0]
                       ? chunkMetres[std::clamp(lod, 0, int(kGeometryLevels) - 1)]
                       : std::int64_t(chunkCells) * (4 << std::clamp(lod, 0, int(kGeometryLevels) - 1));
    }
    int stepAt(int lod) const {
        return int(std::min<std::int64_t>(4 << std::clamp(lod, 0, int(kGeometryLevels) - 1),
                                          metresAt(lod)));
    }
    int cellsAt(int lod) const { return int(metresAt(lod) / stepAt(lod)); }
};
constexpr DataLodPolicy withWidePages(DataLodPolicy policy) { policy.widePages = true; return policy; }
inline constexpr DataLodPolicy kRenderDataLodPolicy{1};
// Runtime landscape stops at H8 geometry: no H4 pages, local H4 windows or H2
// feature meshes. Legacy detail policies below remain CPU comparison fixtures.
inline constexpr int kTerrainChunkCells = 32;
inline constexpr DataLodPolicy kRegionalDataLodPolicy =
        withWidePages(DataLodPolicy{1, 1, true, false, true, kTerrainChunkCells});
// Unbounded fixed coverage remains a CPU comparison fixture, not a runtime policy.
inline constexpr DataLodPolicy kStableFeatureDataLodPolicy{1, 2, true, true};
inline constexpr DataLodPolicy kCameraFeatureDataLodPolicy =
        withWidePages(DataLodPolicy{1, 2, true, false, true, kTerrainChunkCells});
inline constexpr std::size_t kMeshesPerPlan = 4;
inline constexpr std::size_t kCameraPreloadPages = 16;

constexpr std::int32_t dataStepForGeometry(std::int32_t geometryMetres, DataLodPolicy policy = {}) {
    const auto dataMetres = geometryMetres >> std::clamp(policy.finerLevels, 0, 2);
    const std::int32_t coarse = !policy.widePages || dataMetres <= 128 ? 64 : dataMetres <= 512 ? 256 : 1024;
    return std::max(4 << std::clamp(policy.finestLevel,0,2),
        dataMetres <= 4 ? 4 : dataMetres <= 8 ? 8 : dataMetres <= 32 ? 16 : coarse);
}

constexpr std::uint8_t dataLevelForGeometryLevel(std::int32_t geometryLevel, DataLodPolicy policy = {}) {
    const auto level = geometryLevel - std::clamp(policy.finerLevels, 0, 2);
    const int coarse = !policy.widePages || level <= 5 ? 4 : level <= 7 ? 6 : 8;
    return std::uint8_t(std::max(std::clamp(policy.finestLevel,0,2),
        level <= 0 ? 0 : level == 1 ? 1 : level <= 3 ? 2 : coarse));
}

// How far past the target edge length flat ground may go. Three, so a plain
// settles at about a triangle every three hundred pixels rather than every
// ninety-six - coarse enough to stop paying for shape that is not there, and
// bounded so a level surface does not come back as two triangles filling the
// screen, which would leave everything interpolated across them - normals,
// morph, the water head - stretched over a quarter of the view.
inline constexpr double kFlatEdgeSlack = 3.0;

struct LodPolicy {
    // Edge length, not subpixel tessellation. At the 8 -> 4 m threshold a
    // 1.7 m object is about 27 pixels tall (before projection foreshortening).
    // Material/normal detail comes from pages, not triangle density.
    double targetTrianglePixels = 96.0;
    double refineTrianglePixels = 128.0;
    double coarsenTrianglePixels = 48.0;
    double refineErrorPixels = 1.0;
    double coarsenErrorPixels = 0.5;
    bool operator==(const LodPolicy&) const = default;
};

// Edge length alone, for the caller that has a view box and no bound on the
// ground inside it.
//
// The plan picks a level per TILE from how big the tile is on screen; it has no
// node, so it has no deviation, and it used to pass nought for one. That said
// "flat", which since flat ground may now coarsen past the target would make
// every tile in every view the coarsest level there is. Which ground is
// actually flat is settled one level down, by the pass that holds the bounds.
inline bool edgeWantsRefining(std::int32_t step, double metresPerPixel, LodPolicy policy = {}) {
    return step > kFinestGeometryMetres && metresPerPixel > 0.0 &&
           double(step) / metresPerPixel > policy.refineTrianglePixels;
}
inline bool edgeWantsCoarsening(std::int32_t step, double metresPerPixel, LodPolicy policy = {}) {
    return step < kCoarsestRasterMetres && metresPerPixel > 0.0 &&
           double(step) / metresPerPixel < policy.coarsenTrianglePixels;
}

// Chooses an initial cut. Stateful refinement uses the separate hysteresis
// predicates below, so a camera on a threshold cannot toggle every frame.
inline std::int32_t geometryStepFor(double metresPerPixel, double deviationMetres,
                                    LodPolicy policy = {}) {
    if (!(metresPerPixel > 0.0)) return kFinestGeometryMetres;
    std::int32_t step = kFinestGeometryMetres;
    while (step < kCoarsestRasterMetres &&
           double(step * 2) / metresPerPixel <= policy.targetTrianglePixels)
        step *= 2;
    // And past the target where the ground has nothing to say.
    //
    // The target is an edge length, and an edge length is a guess at where
    // detail will be wanted. On ground that is FLAT it is simply wrong: a plain
    // with a deviation of centimetres gains nothing from a triangle every
    // ninety-six pixels rather than every three hundred, and paying for it is
    // how a featureless basin ends up with more geometry in it than the range
    // beside it. What decides is the error, which is nought there.
    while (step < kCoarsestRasterMetres &&
           double(step * 2) / metresPerPixel <= policy.targetTrianglePixels * kFlatEdgeSlack &&
           deviationMetres / metresPerPixel < policy.coarsenErrorPixels)
        step *= 2;
    // The supplied deviation belongs to this candidate node. It may justify
    // one split; children have their own bounds and must be evaluated in turn.
    if (step > kFinestGeometryMetres &&
        deviationMetres / metresPerPixel > policy.refineErrorPixels)
        step /= 2;
    return step;
}

inline bool shouldRefine(std::int32_t step, double metresPerPixel, double deviationMetres,
                         LodPolicy policy = {}) {
    if (!(step > kFinestGeometryMetres && metresPerPixel > 0.0)) return false;
    const double error = deviationMetres / metresPerPixel;
    // Error first, and on its own: a shape showing a pixel of itself is a shape
    // worth drawing however small the triangle already is.
    if (error > policy.refineErrorPixels) return true;
    // A long edge is only a reason to split when there is something under it to
    // find. This used to be an OR, so a flat plain was split for being near the
    // camera and for no other reason - the triangles arrived, carried no shape,
    // and the plain ended up denser than the mountains behind it.
    return error > policy.coarsenErrorPixels &&
           double(step) / metresPerPixel > policy.refineTrianglePixels;
}
inline bool shouldCoarsen(std::int32_t step, double metresPerPixel, double deviationMetres,
                          LodPolicy policy = {}) {
    if (!(step < kCoarsestRasterMetres && metresPerPixel > 0.0)) return false;
    // Ground whose error is under half a pixel is ground nobody can see the
    // shape of, and it may go as coarse as the slack allows. There is no
    // oscillation in that: refine needs the error above this same bar, so the
    // two cannot both be true of one node whatever its edge length is.
    if (deviationMetres / metresPerPixel >= policy.coarsenErrorPixels) return false;
    return double(step) / metresPerPixel < policy.targetTrianglePixels * kFlatEdgeSlack;
}

inline int geometryLevelFor(double metresPerPixel, int previous = -1, LodPolicy policy = {}) {
    // Edge length only, and deliberately.
    //
    // This picks ONE level for a whole view - a budget, not a cut - so it has no
    // node and no deviation to consult. It used to pass a deviation of nought,
    // which said "flat"; now that flat ground is allowed to coarsen past the
    // target, saying that would hand every view the coarsest level there is and
    // leave the adaptive pass with nothing to refine down from. Flatness is
    // that pass's business, and it has the bounds to judge it with.
    const int coarsest = int(kGeometryLevels) - 1;
    if (!(metresPerPixel > 0.0)) return 0;
    const auto edgePixels = [&](int level) {
        return double(kFinestGeometryMetres << level) / metresPerPixel;
    };
    int level = std::clamp(previous, 0, coarsest);
    if (previous < 0) {
        std::int32_t step = kFinestGeometryMetres;
        while (step < kCoarsestRasterMetres &&
               double(step * 2) / metresPerPixel <= policy.targetTrianglePixels)
            step *= 2;
        level = 0;
        while (level < coarsest && (kFinestGeometryMetres << level) < step) ++level;
    }
    while (level > 0 && edgePixels(level) > policy.refineTrianglePixels) --level;
    while (level < coarsest && edgePixels(level) < policy.coarsenTrianglePixels) ++level;
    return level;
}

// A bit for every 64 m of the world that may hold land - held in tiles of
// 64 x 64 bits (4 km), and a tile only as bits where it is part land and part
// sea. A tile all sea or all land is one word in the table. It was a dense
// bit plane, which is forty-seven megabytes for a world 800 x 2000 km that is
// nothing but sea.
class LandMask64 {
public:
    LandMask64() = default;
    LandMask64(std::int32_t worldWidthMetres, std::int32_t worldHeightMetres)
        : width_((std::max(0, worldWidthMetres) + 63) / 64),
          height_((std::max(0, worldHeightMetres) + 63) / 64),
          tilesWide_((width_ + kTile - 1) / kTile),
          tiles_(static_cast<std::size_t>(tilesWide_) * ((height_ + kTile - 1) / kTile), kSea) {}

    void mark(std::int32_t x, std::int32_t y) {
        if (!inside(x, y)) return;
        auto& tile = tiles_[tileOf(x, y)];
        if (tile == kLand) return;
        (*bitsFor(tile))[y % kTile] |= std::uint64_t{1} << (x % kTile);
    }
    void markWorldRect(std::int32_t minX, std::int32_t minY,
                       std::int32_t maxXExclusive, std::int32_t maxYExclusive) {
        if (maxXExclusive <= minX || maxYExclusive <= minY) return;
        std::int32_t x0, y0, x1, y1;
        if (!cellRect(minX, minY, maxXExclusive, maxYExclusive, x0, y0, x1, y1)) return;
        for (std::int32_t ty = y0 / kTile; ty <= y1 / kTile; ++ty)
            for (std::int32_t tx = x0 / kTile; tx <= x1 / kTile; ++tx) {
                auto& tile = tiles_[static_cast<std::size_t>(ty) * tilesWide_ + tx];
                if (tile == kLand) continue;
                const std::int32_t ax = std::max(x0, tx * kTile), bx = std::min(x1, tx * kTile + kTile - 1);
                const std::int32_t ay = std::max(y0, ty * kTile), by = std::min(y1, ty * kTile + kTile - 1);
                if (ax == tx * kTile && bx == tx * kTile + kTile - 1 &&
                    ay == ty * kTile && by == ty * kTile + kTile - 1) {
                    tile = kLand;   // the whole tile: no bits to hold
                    continue;
                }
                auto& bits = *bitsFor(tile);
                const std::uint64_t row = span(ax - tx * kTile, bx - tx * kTile);
                for (std::int32_t y = ay; y <= by; ++y) bits[y - ty * kTile] |= row;
            }
    }
    [[nodiscard]] bool land(std::int32_t x, std::int32_t y) const {
        if (!inside(x, y)) return false;
        const auto tile = tiles_[tileOf(x, y)];
        if (tile == kSea) return false;
        if (tile == kLand) return true;
        return ((bits_[std::size_t(tile)][y % kTile] >> (x % kTile)) & 1u) != 0;
    }
    [[nodiscard]] bool anyLandInWorldRect(std::int32_t minX, std::int32_t minY,
                                          std::int32_t maxXExclusive,
                                          std::int32_t maxYExclusive) const {
        if (maxXExclusive <= minX || maxYExclusive <= minY) return false;
        std::int32_t x0, y0, x1, y1;
        if (!cellRect(minX, minY, maxXExclusive, maxYExclusive, x0, y0, x1, y1)) return false;
        for (std::int32_t ty = y0 / kTile; ty <= y1 / kTile; ++ty)
            for (std::int32_t tx = x0 / kTile; tx <= x1 / kTile; ++tx) {
                const auto tile = tiles_[static_cast<std::size_t>(ty) * tilesWide_ + tx];
                if (tile == kSea) continue;
                if (tile == kLand) return true;
                const auto& bits = bits_[std::size_t(tile)];
                const std::int32_t ax = std::max(x0, tx * kTile), bx = std::min(x1, tx * kTile + kTile - 1);
                const std::int32_t ay = std::max(y0, ty * kTile), by = std::min(y1, ty * kTile + kTile - 1);
                const std::uint64_t row = span(ax - tx * kTile, bx - tx * kTile);
                for (std::int32_t y = ay; y <= by; ++y)
                    if (bits[y - ty * kTile] & row) return true;
            }
        return false;
    }
    [[nodiscard]] std::int32_t width() const { return width_; }
    [[nodiscard]] std::int32_t height() const { return height_; }
    [[nodiscard]] std::size_t bytes() const {
        return tiles_.size() * sizeof(tiles_[0]) + bits_.size() * sizeof(Bits);
    }

private:
    static constexpr std::int32_t kTile = 64;               // cells a side: one word a row
    static constexpr std::int32_t kSea = -1, kLand = -2;    // tiles with no bits of their own
    using Bits = std::array<std::uint64_t, kTile>;

    [[nodiscard]] bool inside(std::int32_t x, std::int32_t y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }
    [[nodiscard]] std::size_t tileOf(std::int32_t x, std::int32_t y) const {
        return static_cast<std::size_t>(y / kTile) * tilesWide_ + x / kTile;
    }
    Bits* bitsFor(std::int32_t& tile) {
        if (tile == kSea) {
            tile = static_cast<std::int32_t>(bits_.size());
            bits_.push_back(Bits{});
        }
        return &bits_[std::size_t(tile)];
    }
    // Bits `from`..`to` inclusive of a row word.
    static std::uint64_t span(std::int32_t from, std::int32_t to) {
        const std::uint64_t upTo = to >= 63 ? ~std::uint64_t{0} : (std::uint64_t{1} << (to + 1)) - 1;
        return upTo & ~((std::uint64_t{1} << from) - 1);
    }
    // World metres to the cells they touch, clipped to the mask; false if none.
    bool cellRect(std::int32_t minX, std::int32_t minY, std::int32_t maxXExclusive, std::int32_t maxYExclusive,
                  std::int32_t& x0, std::int32_t& y0, std::int32_t& x1, std::int32_t& y1) const {
        const auto floor64 = [](std::int32_t v) { return v >= 0 ? v / 64 : (v - 63) / 64; };
        x0 = std::max(0, floor64(minX));
        y0 = std::max(0, floor64(minY));
        x1 = std::min(width_ - 1, floor64(maxXExclusive - 1));
        y1 = std::min(height_ - 1, floor64(maxYExclusive - 1));
        return x0 <= x1 && y0 <= y1;
    }
    std::int32_t width_ = 0, height_ = 0, tilesWide_ = 0;
    std::vector<std::int32_t> tiles_;   // kSea, kLand or an index into bits_
    std::vector<Bits> bits_;
};

// Conservative by construction: a 64 m cell is land when any generated macro
// land cell overlaps it, so a coast can never disappear from sparse storage.
LandMask64 makeLandMask64(const generation::WorldMapData& world);

enum class RefinementPhase : std::uint8_t { Parent, Waiting, Refining, Children, Coarsening };

class RefinementTransition {
public:
    explicit RefinementTransition(double seconds = 0.25) : seconds_(std::max(0.0, seconds)) {}
    // A live duration change preserves progress and direction, not elapsed time.
    void setDuration(double seconds) { seconds_ = std::max(0.0, seconds); }
    void requestChildren(bool dataResident) {
        if (phase_ == RefinementPhase::Parent) phase_ = dataResident ? RefinementPhase::Refining
                                                                     : RefinementPhase::Waiting;
        else if (phase_ == RefinementPhase::Waiting && dataResident) phase_ = RefinementPhase::Refining;
        else if (phase_ == RefinementPhase::Coarsening) phase_ = RefinementPhase::Refining;
    }
    void dataBecameResident() {
        if (phase_ == RefinementPhase::Waiting) phase_ = RefinementPhase::Refining;
    }
    void requestParent() {
        if (phase_ == RefinementPhase::Children || phase_ == RefinementPhase::Refining)
            phase_ = RefinementPhase::Coarsening;
        else if (phase_ == RefinementPhase::Waiting) phase_ = RefinementPhase::Parent;
    }
    void tick(double dt) {
        const float change = seconds_ == 0 ? 1.0f : static_cast<float>(std::max(0.0, dt) / seconds_);
        if (phase_ == RefinementPhase::Refining) {
            morph_ = std::min(1.0f, morph_ + change);
            if (morph_ == 1.0f) phase_ = RefinementPhase::Children;
        } else if (phase_ == RefinementPhase::Coarsening) {
            morph_ = std::max(0.0f, morph_ - change);
            if (morph_ == 0.0f) phase_ = RefinementPhase::Parent;
        }
    }
    [[nodiscard]] RefinementPhase phase() const { return phase_; }
    [[nodiscard]] float morph() const { return morph_; }
    [[nodiscard]] bool drawChildren() const {
        return phase_ == RefinementPhase::Refining || phase_ == RefinementPhase::Children ||
               phase_ == RefinementPhase::Coarsening;
    }
    [[nodiscard]] bool fineDataMayEvict() const { return phase_ == RefinementPhase::Parent; }

private:
    RefinementPhase phase_ = RefinementPhase::Parent;
    float morph_ = 0.0f;
    double seconds_;
};

} // namespace world::terrain

