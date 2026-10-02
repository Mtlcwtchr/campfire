#pragma once
// A world as a grid of regions, each generated on its own terms.
//
// A region is 256 macro cells a side - 256 x 512 m = 131 072 m, the binary
// "128 km" - so it holds exactly 256 x 256 terrain pages and a page quadtree
// never straddles two regions. The world is a whole number of them across and
// down, up to thirty-two a side (4 194 km).
//
// What a region owns: whether it has been generated at all (an empty region is
// open sea), and the dials it was generated with - its own seed, how much of it
// is sea, how long the weather has worked on it, how wet it is. What it does
// NOT own: the plates, the climate and the rivers. Those are the world's, and
// run across region borders as if the borders were not there; a river rising
// in one region reaches the sea through the next.
//
// Regions are independent of each other, but their borders are not lines: over
// a transition band either side of a border the two regions' dials and noise
// are blended, and the band itself wanders, so two regions made from different
// presets meet as a change of country rather than a seam.
//
// This is the first layer of the brush-painted world: a region is the coarsest
// brush there is (a whole region, painted by selecting it). The finer layers -
// continents, ranges, hills, sea, weathering, rain - are maps of their own
// resolutions painted over the regions (world_layers.hpp) with manual and
// procedural brushes (world_brush.hpp); the layout carries them, sized to it.
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/generation/world_layers.hpp"
#include "game/generation/world_map_gen.hpp"

namespace generation {

class ImportedSource;

inline constexpr std::int32_t kCellsPerRegion = 256;
inline constexpr std::int64_t kRegionMetres = std::int64_t(kCellsPerRegion) * kMetresPerCell;
inline constexpr std::int32_t kMaxRegionsPerSide = 32;   // 4 194 km
// The narrowest and widest a transition band may be, half-width, in metres.
inline constexpr std::int32_t kMinRegionBlendMetres = 2048;
inline constexpr std::int32_t kMaxRegionBlendMetres = 32768;

// The dials a region is generated with.
struct RegionSettings {
    std::string preset;            // the preset these came from, for the interface
    std::uint64_t seed = 1;
    std::int32_t seaPercent = 71;
    std::int32_t erosionPasses = 3;
    std::int32_t rainfallPercent = 100;
    // Made by hand: no continents of its own, so there is land exactly where
    // the continents layer is painted and nowhere else. The rest of the
    // generator - relief, weather, rivers - still works the painted land.
    bool manual = false;
    bool operator==(const RegionSettings&) const = default;
};

// How far a region made by hand has been taken - the authoring pipeline
// (doc/authoring_pipeline_2026-09-30.md). Each stage computes only what the
// person has decided so far; nothing is generated ahead of the decision that
// needs it. A region from a preset or a whole-world run, and every region of a
// world saved before there were stages, is Full: everything, as it always was.
enum class RegionStage : std::uint8_t {
    Full,      // everything the generator makes
    Sketch,    // a coastline painted, not yet pinned: drawn as a sketch, never computed
    Primary,   // pinned: noise, primary relief, a ragged natural coast, slopes - no climate, no water
    Relief,    // mountains grown from the painted ranges, weather and erosion - no water yet
    Water,     // and the drainage: rivers and lakes
};
const char* stageName(RegionStage stage);          // in files and the interface
std::optional<RegionStage> stageNamed(std::string_view name);

struct Region {
    bool generated = false;
    RegionSettings settings;
    // Which of the layout's generations its ground comes from; -1 for none
    // (open sea, or whatever the layers paint there).
    std::int32_t source = -1;
    // For a region made by hand (painted, not generated): how far it is taken.
    RegionStage stage = RegionStage::Full;
    bool operator==(const Region&) const = default;
};

// One run of the generator: a rectangle of regions made as one world of that
// size, with one seed and one set of plates across it.
//
// A world is not generated once; it is made of these. "Generate the whole
// world" is one of them over every region - the world as a single planet, its
// plates and rivers running across all of it. A region added later is empty
// until something fills it: its own generation, the size of that region and
// no bigger, with its own seed and dials; or paint; or a new whole-world run.
// And a generation holds what it was run WITH - its regions as they were -
// so nothing done elsewhere afterwards changes what it makes: adding a region
// beside it, or making one of its own regions again on other terms (that
// region is then taken from the newer generation, and the old run still
// shapes the ground around it as it always did).
struct Generation {
    std::int32_t x = 0, y = 0, w = 1, h = 1;   // regions
    std::uint64_t seed = 1;
    std::int32_t plates = 0;                   // 0 = worked out from its area
    std::vector<Region> regions;               // w x h, row by row, as run
    bool contains(std::int32_t rx, std::int32_t ry) const {
        return rx >= x && ry >= y && rx < x + w && ry < y + h;
    }
    const Region& at(std::int32_t rx, std::int32_t ry) const {
        return regions[std::size_t(ry - y) * std::size_t(w) + std::size_t(rx - x)];
    }
    Region& at(std::int32_t rx, std::int32_t ry) {
        return regions[std::size_t(ry - y) * std::size_t(w) + std::size_t(rx - x)];
    }
    bool operator==(const Generation&) const = default;
};

struct WorldLayout {
    std::int32_t regionsX = 1, regionsY = 1;
    // The world's own seed: plates, climate, rivers and everything else that
    // runs across regions.
    std::uint64_t seed = 1;
    std::int32_t plates = 0;       // 0 = worked out from the area
    // Half-width of the transition band at a region border.
    std::int32_t blendMetres = 12288;
    std::vector<Region> regions;   // row by row, regionsX to a row
    // The runs the regions' ground came from (Region::source indexes these).
    std::vector<Generation> generations;
    // Where the world lies on its planet. Not fixed in a layout made before
    // there was a choice; fixed the moment anything but a whole-world run is
    // made, so the climate of what exists stays where it is.
    Latitude latitude;
    // How the land painted by hand is worked out (AuthoringDials).
    AuthoringDials authoring;
    // What has been painted over the regions, one map per layer, each at its
    // own resolution and sized to the world (world_layers.hpp).
    std::array<LayerMap, kLayerCount> layers;
    // The authored skeleton and masks imported into the world's source
    // (world_import.hpp): a region holding height there is built from it.
    // Not saved with the layout - it is the directory beside it - but opened
    // with it and carried with it; an import opens a new one.
    std::shared_ptr<const ImportedSource> imported;

    std::int32_t widthCells() const { return regionsX * kCellsPerRegion; }
    std::int32_t heightCells() const { return regionsY * kCellsPerRegion; }
    std::int64_t widthMetres() const { return std::int64_t(widthCells()) * kMetresPerCell; }
    std::int64_t heightMetres() const { return std::int64_t(heightCells()) * kMetresPerCell; }
    bool inBounds(std::int32_t x, std::int32_t y) const {
        return x >= 0 && y >= 0 && x < regionsX && y < regionsY;
    }
    std::size_t indexOf(std::int32_t x, std::int32_t y) const {
        return std::size_t(y) * std::size_t(regionsX) + std::size_t(x);
    }
    Region& at(std::int32_t x, std::int32_t y) { return regions[indexOf(x, y)]; }
    const Region& at(std::int32_t x, std::int32_t y) const { return regions[indexOf(x, y)]; }
    LayerMap& layer(LayerId id) { return layers[std::size_t(id)]; }
    const LayerMap& layer(LayerId id) const { return layers[std::size_t(id)]; }
    bool anyGenerated() const;
    bool anyPainted() const;
    bool operator==(const WorldLayout&) const = default;
};

// A world of the given size with every region empty (open sea).
WorldLayout emptyLayout(std::int32_t regionsX, std::int32_t regionsY, std::uint64_t seed);
// A different size, keeping every region that still fits where it was.
void resizeLayout(WorldLayout& layout, std::int32_t regionsX, std::int32_t regionsY);
// Regions added (positive) or taken away (negative) at each side. What stays
// keeps its ground exactly: its regions, its generations and their runs, the
// paint over it and its latitude (the offset goes into Latitude::rowOffset)
// all move with it. Returns false, changing nothing, if the result would be
// empty or past kMaxRegionsPerSide.
bool reshapeLayout(WorldLayout& layout, std::int32_t west, std::int32_t east, std::int32_t north, std::int32_t south);
// The seed a region gets when a whole world is generated from one seed.
std::uint64_t regionSeed(std::uint64_t worldSeed, std::int32_t x, std::int32_t y);
// A preset's dials as a region's settings.
RegionSettings regionSettingsFrom(const WorldPreset& preset, std::uint64_t seed);
// Every region generated from one preset, each with its own derived seed, as
// one generation over the whole world: the world as a single planet.
void generateAll(WorldLayout& layout, const WorldPreset& preset);
// The same with dials given region by region (what the regions already say
// for those that were generated, `settings` with derived seeds for the rest).
void generateWholeWorld(WorldLayout& layout, const RegionSettings& settings, std::uint64_t seed, bool ownSeeds);
// A generation of their own for these regions: a world the size of the
// rectangle around them, the rest of that rectangle sea, placed where they
// are. Each region's seed is derived from `seed` when `ownSeeds`, or is
// `seed`. Nothing else in the world is touched.
void generateRegions(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which,
                     const RegionSettings& settings, std::uint64_t seed, bool ownSeeds);
// Back to open sea. Every generation that made them makes them no more, so
// the ground around them goes down to the water instead of breaking off.
void clearRegions(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which);
// Generations no region takes its ground from are dropped, and the sources
// renumbered.
void pruneGenerations(WorldLayout& layout);
// One generation over every region, with the layout's own seed and plates:
// the world as the generator has always made it.
bool isWholeWorld(const WorldLayout& layout);
// The latitude made fixed, from the rule it was under, if it was not.
void fixLatitude(WorldLayout& layout);
// The layout a generation is run as: its rectangle, its regions as it holds
// them, its seed and plates, and the layers painted over that rectangle.
WorldLayout generationLayout(const WorldLayout& layout, const Generation& generation);
// Whether a region has anything painted on it, on any layer.
bool paintedIn(const WorldLayout& layout, std::int32_t regionX, std::int32_t regionY);

// --- the authoring stages ---------------------------------------------------
// Whether a region is one made by hand under the staged pipeline: never
// generated, and at a stage other than Full.
bool authored(const WorldLayout& layout, std::int32_t regionX, std::int32_t regionY);
// Take regions made by hand up to `to` (never down, never a generated or Full
// one): those in `which`, or every authored region when `which` is empty.
// Returns how many moved. Pinning is Sketch -> Primary; painting a range on a
// pinned region is -> Relief; asking for the water is -> Water.
std::int32_t raiseStage(WorldLayout& layout, const std::vector<std::pair<std::int32_t, std::int32_t>>& which,
                        RegionStage to);
// The first stroke of land into an empty region makes it a sketch: drawn, not
// computed, until it is pinned. A region with anything already on it keeps
// the stage it has.
void beginSketch(WorldLayout& layout, std::int32_t regionX, std::int32_t regionY);

// The parameters the generator is run with for this layout: the world's size,
// seed and plates, the world-wide averages of the regions' dials (what the
// generator's world-level constants are taken from), and the layout itself,
// which the generator reads region by region.
WorldMapParams paramsFor(const WorldLayout& layout);

// What one macro cell is made of: up to four regions (at a corner of the grid)
// and how much of each, summing to one. Inside a region, away from its border,
// it is that region alone.
struct RegionMix {
    std::array<std::int32_t, 4> region{-1, -1, -1, -1};  // index into regions
    std::array<float, 4> weight{};
    int count = 0;
};
RegionMix regionMixAt(const WorldLayout& layout, double cellX, double cellY);

// How much of a macro cell is generated ground rather than empty sea, 0..1.
float generatedAt(const WorldLayout& layout, double cellX, double cellY);

// What a layer is where nothing has been painted on it - what the generator
// makes there from the regions - at the centre of every texel. Raise, Lower
// and Smooth move a place from what it IS, so they need it; so does the map of
// the layer the editor draws. The ranges have no such thing short of running
// the plates, and read nought: what is painted on them is added to the plates.
struct LayerBase {
    LayerId id = LayerId::Continents;
    std::int32_t texelsX = 0, texelsY = 0;
    std::vector<float> values;
    float at(std::int32_t tx, std::int32_t ty) const {
        if (values.empty()) return 0.0f;
        tx = std::clamp(tx, 0, texelsX - 1);
        ty = std::clamp(ty, 0, texelsY - 1);
        return values[std::size_t(ty) * std::size_t(texelsX) + std::size_t(tx)];
    }
};
LayerBase layerBase(const WorldLayout& layout, LayerId id);

// A region dial with its layer painted over it, for every macro cell: the
// erosion passes (LayerId::Weathering) or the rainfall percentage
// (LayerId::Rain). The regions' dials blend across their border bands as
// always; the layer covers that where it was painted.
std::vector<float> dialCells(const WorldLayout& layout, LayerId id);

// Saved as JSON beside the other editable things: the regions, and the tiles
// of every layer that has anything painted on it.
bool saveWorldLayout(const WorldLayout& layout, const std::filesystem::path& file);
std::optional<WorldLayout> loadWorldLayout(const std::filesystem::path& file);

} // namespace generation

