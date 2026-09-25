#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "engine/core/fixed.hpp"

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

struct TerrainFoundation {
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
    std::array<std::vector<std::int32_t>,5> heightDm;
    std::vector<std::uint8_t> detailPages; // slopes/gullies; rivers are added by the planner
    std::vector<std::int32_t> receiver; // strictly descending drainage forest
    std::vector<std::uint32_t> accumulation;
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
    bool needsDetail(int minX, int minY, int maxX, int maxY) const;
    std::uint64_t fingerprint() const;
    void applyTo(WorldMapData& world) const;
};
std::shared_ptr<const TerrainFoundation> buildTerrainFoundation(
    WorldMapData& world, const WorldMapParams& params, int seaLevel, int highest);
std::string saveTerrainFoundation(const TerrainFoundation& foundation);
std::shared_ptr<const TerrainFoundation> loadTerrainFoundation(
    const std::string& payload, int width, int height);
} // namespace generation
