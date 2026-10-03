#pragma once
// Ground made by the generator instead of drawn somewhere else: the other way
// into the world's source (engine/world_source, D158), beside an import.
//
// An import brings a height map and a control map into the regions a person
// selects (world_import.hpp), and what is worked out from them - drainage,
// climate, rivers and lakes - is asked for a stage at a time afterwards (D159).
// These are the same maps made procedurally, at any time and over any
// selection - not only when the world is first made - one layer at a time:
//
//   1. Heights. The generator run over the rectangle around the regions (one
//      planet over all of them when the whole world is asked for, as
//      generateRegions / generateWholeWorld would), taken only as far as its
//      primary stage - continents, plates, relief noise, weathering, slopes;
//      no climate, no water - and its ground sampled onto the source's grid.
//      Written like an imported height map: the regions it goes into become
//      imported ones at RegionStage::Primary, the heights as made.
//   2. Control maps. Moisture, forest, mountain strength and erosion strength
//      worked out from the heights already in the source - generated or
//      imported - the dials and a wander of noise. Written like a control map
//      imported on its own: over the heights, no stage changes.
//   3. Drainage & climate, 4. rivers & lakes: the stages an import is taken
//      through as it is (WorldEditor::stageImported).
//
// Each phase is a pure function of what it is given: the same layout, dials
// and seed make the same samples on every machine. Nothing here writes; the
// caller lays the rasters into the source (engine/world_source importGrids).
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "game/generation/world_layout.hpp"

namespace generation {

using RegionList = std::vector<std::pair<std::int32_t, std::int32_t>>;

// What a phase made, over a rectangle of whole regions, one value a source
// sample, row by row from the north-west - in the canonical schema's numbers
// (engine/world_source canonicalSchema), so it is laid in without a single
// resample or conversion.
struct SourceRasters {
    std::int32_t regionX = 0, regionY = 0, regionsW = 0, regionsH = 0;
    std::int64_t samplesX = 0, samplesY = 0;
    std::int32_t sampleMetres = 256;
    // The `height` raster's 16-bit samples (the open sea its default), or empty.
    std::vector<std::uint16_t> height;
    // The `control_0` raster's four bytes a sample (moisture, forest, mountain,
    // erosion; at sea every channel at rest), or empty.
    std::vector<std::uint8_t> control;
    std::int64_t landSamples = 0;
    std::int32_t highestMetres = 0;
    // The regions it holds values for - what is to be written; the rest of
    // the rectangle is only there to make it a rectangle.
    RegionList regions;
    // The rectangle in metres: x0, y0, x1, y1.
    std::array<double, 4> rect() const {
        const double r = double(kRegionMetres);
        return {regionX * r, regionY * r, (regionX + regionsW) * r, (regionY + regionsH) * r};
    }
};

// The dials of the heights phase: what a generated region is made with.
struct ProceduralHeights {
    RegionSettings settings;
    std::uint64_t seed = 1;
    bool ownSeeds = true;        // each region its seed derived from `seed`
};

// Phase 1. `regions` empty: the whole world, as one planet. The samples are
// the layout's source grid (`sampleMetres`, 64..256). The painted layers over
// the rectangle shape the run as they shape any generation; whatever is
// imported is not read.
std::optional<SourceRasters> generateSourceHeights(const WorldLayout& layout, const RegionList& regions,
                                                   const ProceduralHeights& dials, std::int32_t sampleMetres,
                                                   std::string* why = nullptr);

// The dials of the control phase.
struct ProceduralControls {
    std::uint64_t seed = 1;
    std::int32_t rainfallPercent = 100;   // the moisture bias's middle: 100 is the generator's own
    std::int32_t erosionPasses = 3;       // the erosion strength's middle: 3 passes is 0.5, as the dials
    float variation = 1.0f;               // how far the noise moves them, 0..2
};

// Phase 2: over the regions that hold heights in the source at `sourceRoot`
// among `regions` (every one that does when empty). Nothing when none does.
std::optional<SourceRasters> generateSourceControls(const std::filesystem::path& sourceRoot, const WorldLayout& layout,
                                                    const RegionList& regions, const ProceduralControls& dials,
                                                    std::string* why = nullptr);

// The regions a phase over `regions` covers: all of the world when empty.
RegionList phaseRegions(const WorldLayout& layout, const RegionList& regions);

} // namespace generation

