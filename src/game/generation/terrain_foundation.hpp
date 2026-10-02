#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "engine/core/fixed.hpp"
#include "game/generation/cell_field.hpp"

namespace generation {
struct WorldMapData;
struct WorldMapParams;
enum class TerrainStage : std::uint8_t { Noise, Tectonics, Thermal, Volcanoes, Slopes, Water, Rivers, Final };
inline constexpr std::array<const char*,8> kTerrainStageNames{
    "1 Noise", "2 Tectonics", "3 Thermal", "4 Volcanoes", "5 Slope erosion",
    "6 Water", "7 Channels / erosion", "8 Final"};

// Immutable, world-space H64. The first five stages own geometry; water and
// channels are evaluated by the existing shared hydrology graph afterwards.
// No per-chunk random state, no H4 displacement, no external terrain assets.
// How many cells of foundation a machine will hold: sixteen million, which is
// three hundred and twenty megabytes of height planes and half as much again
// of drainage beside them.
//
// It is a budget, not a resolution, and that is the point. The grid used to be
// sixty-four metres whatever the world was, so its cost was the square of the
// world's side: a five-hundred-kilometre world wanted sixty-four million cells
// and a two-thousand-kilometre one wanted a BILLION. The step is chosen against
// this budget instead, so a small world gets sixty-four metres and a very large
// one gets a coarser authority rather than no world at all.
inline constexpr std::size_t kFoundationCellBudget = 16u * 1024 * 1024;

// The finest the foundation is ever built at, and what it is built at for every
// world that fits.
inline constexpr int kFinestFoundationStep = 64;

// Doubled until the grid fits the budget. Both the builder and the loader have
// to agree, so neither works it out for itself.
inline int foundationStepFor(std::int64_t widthMetres, std::int64_t heightMetres) {
    int step = kFinestFoundationStep;
    for (int guard = 0; guard < 8; ++guard) {
        const auto columns = (widthMetres + step - 1) / step + 1;
        const auto rows = (heightMetres + step - 1) / step + 1;
        if (columns * rows <= std::int64_t(kFoundationCellBudget)) break;
        step *= 2;
    }
    return step;
}

// The coarsest a world put together from regions (world_compose.hpp) holds
// its ground at, whatever its size: the authored skeleton's own 256 m
// (D158). Held sparsely - only where there is land and its shore - so it
// costs what the land does, not the square of the world's side; the budget
// above is for a foundation built densely, which a composite is not.
inline constexpr int kCompositeFoundationStep = 256;
inline int compositeFoundationStepFor(std::int64_t widthMetres, std::int64_t heightMetres) {
    return std::min(foundationStepFor(widthMetres, heightMetres), kCompositeFoundationStep);
}

// What the open sea floor is, in decimetres: what a plane holds wherever it
// holds nothing.
inline constexpr std::int32_t kFoundationSeaFloorDm = -600;

struct TerrainFoundation {
    // A plane of the lattice, row by row, held only where it is not the open
    // sea floor (cell_field.hpp).
    using Plane = CellField<std::int32_t>;
    static constexpr int kStep = kFinestFoundationStep;
    // What THIS foundation was built at, which is kStep for any world that fits
    // in the budget and coarser for one that does not.
    int step = kStep;
    // And the same as a shift, because the step divides every coordinate this
    // grid is ever asked about.
    //
    // It used to be a constant, and dividing by a constant is a multiply the
    // compiler writes for you. Making it a runtime number turned that into a
    // genuine hundred-and-twenty-eight bit division - a library call on this
    // architecture - twice for every sample, and a height query went from four
    // and a half microseconds to twenty-two. The step is always a power of two
    // by construction, so the division is a shift and the cost goes away
    // entirely.
    int stepShift = 6;   // 1 << 6 == 64
    static constexpr int kWorkers = 6;
    int columns = 0, rows = 0, pageColumns = 0, pageRows = 0;
    std::array<Plane,5> heightDm;
    std::vector<std::uint8_t> detailPages; // slopes/gullies; rivers are added by the planner
    CellField<std::int32_t> receiver;      // strictly descending drainage forest; -1 where nothing drains
    CellField<std::uint32_t> accumulation; // 1 where nothing drains into it
    struct Divide { std::uint32_t a, b; }; // edges separating opposing catchments
    std::vector<Divide> divides;
    std::array<double,5> milliseconds{};
    // What the thermal stage actually moved. Counted because the stage looked
    // like it was working for as long as nobody counted: its threshold was a
    // height rather than an angle, so it did nothing at all at any spacing but
    // the one it was written for, and its delta mask came out blank.
    std::size_t thermalCells = 0;
    double thermalMetres = 0;
    core::Fixed sample(core::Fixed x, core::Fixed y, TerrainStage stage) const;
    // The lowest and highest height of any stage, in decimetres.
    std::pair<std::int32_t, std::int32_t> heightRange() const;
    bool needsDetail(int minX, int minY, int maxX, int maxY) const;
    // The canonical identity (saves, the 4242 regression): byte-wise FNV over
    // every plane. On the reference world that is 170 MB and four hundred
    // milliseconds, so a sealed foundation works it out once, on first ask.
    std::uint64_t fingerprint() const;
    // What caches key on (the drainage graph, the page cache): every value of
    // every plane too, but four words in flight at a time (core::LaneHash)
    // rather than one byte - tens of milliseconds where fingerprint() is
    // hundreds. Not interchangeable with fingerprint(); a cache key only has
    // to change when the ground does.
    std::uint64_t contentKey() const;
    // Called by a builder once the foundation is final: from here on it is
    // immutable, and both identities are held rather than recomputed. An
    // unsealed foundation (a test's hand-made grid, still being written) is
    // hashed afresh every time.
    void seal();
    void applyTo(WorldMapData& world) const;
    // The same, a land cell taking the lowest land inside it rather than its
    // corner: for an imported skeleton, whose valleys are finer than a cell.
    void applyLowestTo(WorldMapData& world) const;
private:
    // Copyable; a copy starts unworked-out and hashes on its own first ask.
    struct Memo {
        mutable std::atomic<std::uint64_t> value{0};
        mutable std::atomic<bool> ready{false};
        Memo() = default;
        Memo(const Memo&) {}
        Memo& operator=(const Memo&) { ready = false; return *this; }
    };
    bool sealed_ = false;
    std::uint64_t contentKey_ = 0;
    Memo canonical_;
    std::uint64_t computeFingerprint() const;
    std::uint64_t computeContentKey() const;
};
std::shared_ptr<const TerrainFoundation> buildTerrainFoundation(
    WorldMapData& world, const WorldMapParams& params, int seaLevel, int highest);
std::string saveTerrainFoundation(const TerrainFoundation& foundation);
std::shared_ptr<const TerrainFoundation> loadTerrainFoundation(
    const std::string& payload, int width, int height);
} // namespace generation
