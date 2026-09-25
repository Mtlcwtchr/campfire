#pragma once
// What a rectangle of the world may contain, before anything expensive is built.
//
// The point of this is to STOP work, so its errors have to fall one way. An
// answer of OpenWater means no terrestrial field, erosion, material or object
// will ever be generated there, so it must be a proof, not an observation at
// the overview's sample spacing: a rock stack between two overview nodes would
// otherwise be silently deleted from the world. An answer of Mixed costs some
// wasted work and nothing else. So the classification is conservative in the
// only direction that can lose content, and `Unknown` is a real answer rather
// than an admission of failure - it is what a region whose fine shape has not
// been decided yet is entitled to, and it never certifies emptiness.
//
// The proof comes from the macro field's Lipschitz bound. Each overview cell
// carries the highest and lowest the continuous field can reach anywhere inside
// it; a rectangle is open water only when every cell it touches has a ceiling
// below sea level, and land only when every cell has a floor above it.
#include "game/generation/world_macro.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace generation {

enum class Coverage : std::uint8_t {
    OpenWater,  // proven: no land anywhere in the rectangle, halo included
    Land,       // proven: no open water anywhere in the rectangle
    Mixed,      // a coast, an island, a lake or a river mouth lives here
    Unknown,    // outside the domain, or a query the overview cannot answer
};

// Metres, half-open in both axes, in world coordinates.
struct WorldRect {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool valid() const;
    WorldRect grown(double metres) const;
};

class LandCoverage {
public:
    // Builds a min/max pyramid over the overview, so a rectangle of any size
    // costs O(log) rather than its own area in cells. The pyramid is a third
    // again of the overview and, like it, does not grow with the world.
    //
    // The field is rebuilt from the overview's own descriptor rather than
    // passed in, because the two must be the same composition and a caller
    // holding a different one would be a silent disagreement about the world.
    explicit LandCoverage(std::shared_ptr<const MacroOverview>);

    Coverage classify(const WorldRect&) const;

    // The question the job queues actually ask. Never false for a rectangle
    // that holds any land, which is the invariant worth testing.
    bool mayHaveLand(const WorldRect& rect) const { return classify(rect) != Coverage::OpenWater; }
    // Water detail is its own budget: a dry inland page needs none, but a coast
    // or a river mouth does, so this keeps a halo rather than ending at the line.
    bool mayHaveWater(const WorldRect& rect, double haloMetres = kCoastHaloMetres) const;

    // Coasts move when a later stage cuts a valley to the sea, so the halo is
    // part of the contract rather than a tuning constant hidden in a caller.
    static constexpr double kCoastHaloMetres = 2048;

    const MacroOverview& overview() const { return *overview_; }
    // This world's waterline, in the same raw metres the field answers in.
    double seaLevelMetres() const { return overview_->seaLevelMetres; }
    std::size_t bytes() const;
    // Cells the last query had to look at, for the memory/work budget reports.
    std::size_t levels() const { return levels_.size(); }
    const MacroField& field() const { return field_; }

private:
    struct Level {
        std::uint32_t columns = 0, rows = 0;
        std::vector<float> floors, ceilings;
    };
    std::shared_ptr<const MacroOverview> overview_;
    MacroField field_;
    std::vector<Level> levels_;   // level 0 is the overview itself

    // A cell of the overview is tens of kilometres wide in a large world, and
    // its bound is correspondingly loose: a page deep inside a continent comes
    // back Mixed because the cell it sits in also holds a fjord. For a query
    // small enough to be worth it, the field is asked directly - a handful of
    // samples plus the Lipschitz allowance over their own spacing, which is the
    // same kind of proof at a much tighter scale. Bounded work either way.
    std::array<float, 2> sampledBounds(const WorldRect&) const;

    // Bounds over a rectangle of cells at the coarsest level that still covers
    // it exactly, so a world-sized query does not walk a million cells.
    std::array<float, 2> bounds(double minX, double minY, double maxX, double maxY) const;
};

} // namespace generation
