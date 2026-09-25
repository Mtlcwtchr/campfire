#pragma once
// Surface pages produced on demand from the bounded composition.
//
// The legacy path cannot answer for a large world, and not because it is slow:
// it holds world-sized arrays. A HeightField attaches the whole macro map, a
// TerrainFoundation is five dense H64 planes, a LandMask64 is a bit for every
// sixty-four metres of the world. At the 414 km preset that foundation alone is
// over a gigabyte, and at ten thousand kilometres it is not a number worth
// writing down. Growing the allowed `--world` on top of them is the thing the
// plan says is not the task.
//
// So this source holds none of them. A page is derived from the closed-form
// composition when it is asked for, kept in a bounded cache, and dropped when
// the camera leaves. Cost follows what is looked at, never what exists.
//
// Two things make that honest rather than merely cheap:
//
// - The page addressing is a pyramid, not one spacing over one page size. A
//   page is always the same number of samples, so a coarse level covers more
//   world rather than needing more pages: at 512 m pages a ten-thousand
//   kilometre view would want millions of them to draw a horizon, which is the
//   same failure in a different place. Level zero is still a 512 m page, so the
//   fine end matches the storage the renderer already has.
// - Nothing terrestrial is evaluated over proven open water. The land mask runs
//   BEFORE the samples, not after them, and the count of pages it refused is
//   reported so the claim can be checked rather than believed.
#include "game/generation/land_coverage.hpp"
#include "game/generation/world_macro.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/terrain_streaming/base_tile.hpp"

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace world::terrain {

// Where a page sits and how much world it covers.
//
// The sample count is fixed, so the level decides the spacing AND the extent.
// Level zero is 128 samples of 4 m, which is the 512 m page the existing
// storage uses; level seven is 128 samples of 512 m, which is 65 km of world in
// one page. A horizon is a few hundred pages at every world size.
struct SourcePageLayout {
    static constexpr int kSamples = 128;          // interior samples per side
    static constexpr int kFinestStepMetres = 4;
    static constexpr std::uint8_t kLevels = 12;   // 4 m to 8192 m spacing

    static constexpr std::int64_t stepMetres(std::uint8_t level) {
        return std::int64_t(kFinestStepMetres) << level;
    }
    static constexpr std::int64_t extentMetres(std::uint8_t level) {
        return stepMetres(level) * kSamples;
    }
    static constexpr std::int64_t originX(streaming::TileKey key) {
        return std::int64_t(key.x) * extentMetres(key.level);
    }
    static constexpr std::int64_t originY(streaming::TileKey key) {
        return std::int64_t(key.y) * extentMetres(key.level);
    }
};

// The composition, the mask and a bounded cache of pages made from them.
//
// Thread-safe. `page` may derive and belongs to a worker; `resident` never
// derives and is safe from the frame thread.
class ScaleTerrainSource {
public:
    struct Config {
        // What the cache may hold. A budget in bytes rather than in pages,
        // because a page is the same size at every level but a view is not the
        // same number of pages at every level.
        std::size_t residentBytes = 64u << 20;
        // Samples of overlap on each side, so a mesh can take a gradient and a
        // neighbour's edge without a second page.
        std::uint16_t padding = 2;
    };

    ScaleTerrainSource(generation::WorldDescriptor descriptor, Config config);
    // The defaults, spelled out of line because a default argument cannot see
    // Config's own initialisers from inside this class.
    explicit ScaleTerrainSource(generation::WorldDescriptor descriptor);

    // Derives on a miss. Returns nothing for a key outside the domain or for a
    // level the pyramid does not have.
    [[nodiscard]] std::shared_ptr<const SurfacePage> page(streaming::TileKey key);
    // Whatever is already there, or nothing. Never derives.
    [[nodiscard]] std::shared_ptr<const SurfacePage> resident(streaming::TileKey key) const;

    // Asked BEFORE a page is allocated or a job is queued, not after.
    [[nodiscard]] bool mayHaveLand(streaming::TileKey key) const;
    [[nodiscard]] bool mayHaveWater(streaming::TileKey key) const;
    [[nodiscard]] generation::Coverage coverage(streaming::TileKey key) const;

    // Which level covers the view at this many metres per pixel, so a caller
    // does not have to know the pyramid's arithmetic.
    [[nodiscard]] static std::uint8_t levelFor(double metresPerSample);
    // Every page of a rectangle at one level, in row-major order.
    [[nodiscard]] std::vector<streaming::TileKey> keysOverlapping(double minX, double minY,
                                                                  double maxX, double maxY,
                                                                  std::uint8_t level) const;

    struct Stats {
        std::size_t resident = 0, residentBytes = 0;
        std::size_t hits = 0, derived = 0, evicted = 0;
        // Pages the mask answered without evaluating a single sample. The point
        // of the mask, and the number that shows whether it is working.
        std::size_t refusedOpenWater = 0;
        std::size_t fieldSamples = 0;
    };
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] const generation::WorldDescriptor& descriptor() const { return descriptor_; }
    [[nodiscard]] const generation::LandCoverage& land() const { return *coverage_; }
    // Overview plus pyramid plus the page cache: everything this source holds.
    [[nodiscard]] std::size_t bytes() const;

    void clear();

private:
    [[nodiscard]] std::shared_ptr<const SurfacePage> derive(streaming::TileKey key) const;
    void keepLocked(streaming::TileKey key, std::shared_ptr<const SurfacePage> page);
    [[nodiscard]] generation::WorldRect rectOf(streaming::TileKey key, double haloMetres) const;

    generation::WorldDescriptor descriptor_;
    Config config_;
    generation::MacroField field_;
    std::shared_ptr<const generation::MacroOverview> overview_;
    std::unique_ptr<const generation::LandCoverage> coverage_;

    mutable std::mutex guard_;
    mutable std::list<streaming::TileKey> order_;   // oldest first
    struct Entry {
        std::shared_ptr<const SurfacePage> page;
        std::list<streaming::TileKey>::iterator age;
    };
    mutable std::unordered_map<streaming::TileKey, Entry> pages_;
    mutable std::size_t bytes_ = 0;
    mutable Stats stats_;
};

} // namespace world::terrain
