#pragma once
// Import and export between an authoring package and a WorldSource
// (world_authoring_import_export_spec §7, §8, §10).
//
// The importer ends at a correct WorldSource: it builds no mountains, runs no
// erosion and makes no forest. What it hands on is which chunks and layers
// changed and which derived systems that makes stale.
//
// A package may cover all of the world or a rectangle of it (world.json
// origin_m / size_m). A rectangle overwrites the samples inside it and leaves
// the rest of every chunk it crosses as it was; its vector files replace, by
// stable id, every feature of their kind that reaches into the rectangle - a
// feature of that kind there and not in the package is gone. A feature is
// written whole even when most of it lies outside, and an export of a
// rectangle writes whole every feature reaching into it, so export then
// import is the identity (§9).
#include <array>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/world_source/png_io.hpp"
#include "engine/world_source/world_source.hpp"

namespace engine::world_source {

struct ImportReport {
    bool created = false;
    std::size_t chunksWritten = 0, chunksRemoved = 0, chunksUnchanged = 0;
    std::size_t featuresAdded = 0, featuresChanged = 0, featuresRemoved = 0, featuresUnchanged = 0;
    // chunk -> layers that changed in it ("vectors", "poi" for features)
    std::map<ChunkKey, std::set<std::string>> changed;
    // chunk -> derived systems that are stale (§10)
    std::map<ChunkKey, std::set<std::string>> dirty;
    // Loose pictures: the grey taken as the coast (LooseImages::seaGrey).
    int seaGrey = -1;
    // The stage a package asks its regions be taken to (world.json "stage":
    // "relief" or "water"): drainage and climate, and rivers and lakes, without
    // pressing their buttons. Empty: the heights alone.
    std::string stage;
    // Categorical layers whose ids the import renumbered by name: layer ->
    // the package's id -> the source's.
    std::map<std::string, std::map<std::uint32_t, std::uint32_t>> renumbered;
    // Samples whose id the package's legend does not name, taken as the default.
    std::map<std::string, std::size_t> unnamed;
    [[nodiscard]] nlohmann::json json() const;
};

// Where an import lands, when not where the package says. The editor imports
// into the regions a person selected: the package's pictures are stretched
// over the rectangle around them, only the selected squares take them, and a
// band inside the selection's edge blends the new ground into what was there
// - the world's picture is added to, a piece at a time, rather than replaced.
struct ImportTarget {
    // Metres x0 y0 x1 y1: the package's rasters are stretched over it, whatever
    // origin and size its world.json gives. Empty: where the package says.
    std::optional<std::array<double, 4>> rect;
    // Only these rectangles (metres) take the rasters; the rest of `rect` keeps
    // what it had. Empty: all of `rect`.
    std::vector<std::array<double, 4>> mask;
    // How far inside the mask's edge the new values blend into the old ones.
    // An edge on the world's own border is not blended: nothing is beyond it.
    double featherMetres = 0;
    // Height and control maps only: the package's vectors and points of
    // interest are not read, and nothing already stored of them changes.
    bool rastersOnly = false;
    // The world the source is (or is made as), when it is not the package's:
    // the editor's world, which a package cut for a piece of it does not know.
    std::optional<WorldExtent> world;
    // The numbering the source must keep for a categorical layer, by name:
    // the engine's registry (the terrain categories, the forest biomes...).
    // A package that names an id the registry does not know is refused.
    // A layer not here takes the source's own legend, adding new names.
    std::map<std::string, std::map<std::string, std::uint32_t>> legends;
};

// A package's ids onto the source's by name. `package` is the package's
// legend, `source` the source's (extended with new names), `canonical` the
// registry's when there is one. False, and why (with the names it knows),
// when the package names something the registry does not.
bool renumberLegend(const std::string& layer, const std::map<std::uint32_t, std::string>& package,
                    std::map<std::uint32_t, std::string>& source,
                    const std::map<std::string, std::uint32_t>* canonical,
                    std::map<std::uint32_t, std::uint32_t>& mapping, std::string* why = nullptr);

std::optional<ImportReport> importPackage(const std::filesystem::path& package, const std::filesystem::path& source,
                                          std::string* why = nullptr);
std::optional<ImportReport> importPackage(const std::filesystem::path& package, const std::filesystem::path& source,
                                          const ImportTarget& target, std::string* why = nullptr);

// Loose pictures instead of a package: a grey height map (8 or 16 bits,
// black is `lowMetres`, white `highMetres`) and, if given, a control map
// (RGBA: moisture, forest, mountain strength, erosion strength, 0..1 each).
// A source made by them has the canonical schema (canonicalSchema).
struct LooseImages {
    std::filesystem::path height, control;   // either may be empty, not both
    double lowMetres = -100, highMetres = 3000;
    // The grey (0..255) the coast is at: darker is sea - written as the open
    // sea, which is kept as nothing - and land rises from it to white at
    // `highMetres`. Negative: found in the picture, as the top of its dark
    // peak (a picture's "black" sea is rarely 0: on one archipelago it was
    // grey 7 to 11, and read as land it came out a plain at nine metres).
    double seaGrey = -1;
};
std::optional<ImportReport> importImages(const LooseImages& images, const std::filesystem::path& source,
                                         const ImportTarget& target, std::string* why = nullptr);

// Rasters worked out in memory rather than read from pictures: what the
// editor's procedural phases write (game/generation/world_procedural.hpp)
// in place of an import. Each is already in the canonical schema's numbers
// (canonicalSchema: `height` one 16-bit channel, `control` four 8-bit ones)
// and holds exactly one pixel per source sample of `target.rect` - nothing
// is resampled. Either may be absent, not both. Laid in like an import: the
// mask and its feathered edge, only the chunks whose hash changed written.
struct GridRasters {
    std::optional<Image> height, control;
};
std::optional<ImportReport> importGrids(GridRasters rasters, const std::filesystem::path& source,
                                        const ImportTarget& target, std::string* why = nullptr);

// The rasters a source is made with when nothing else says: height over
// -2000..8000 m in sixteen bits (a sixth of a metre), and control_0 with the
// four channels above.
Schema canonicalSchema(const WorldExtent& world);

// A brush over a categorical layer: `id` into every sample whose centre is
// inside any of the discs (metres) - over land only, unless `landOnly` is
// false (land: the height is not the open sea's default). The layer is made,
// with `legend` (name -> id) as its legend, when the source has none, and its
// legend takes the registry's names otherwise; the source is made (the
// canonical schema) when there is none. What the editor's category brush
// writes: the same chunks an import would.
struct CategoricalDab {
    double x = 0, y = 0, radius = 0;
};
std::optional<ImportReport> paintCategorical(const std::filesystem::path& source, const WorldExtent& world,
                                             const std::string& layer, std::uint32_t id,
                                             const std::vector<CategoricalDab>& dabs,
                                             const std::map<std::string, std::uint32_t>& legend = {},
                                             bool landOnly = true, std::string* why = nullptr);

// The rasters inside these rectangles (metres) back to their defaults - sea,
// and every mask at rest. Vectors are left alone. Returns the report of what
// changed.
std::optional<ImportReport> clearRasters(const std::filesystem::path& source,
                                         const std::vector<std::array<double, 4>>& rects, std::string* why = nullptr);

// The world grown or cut at its west and north by whole chunks (the editor's
// regions are four chunks a side): everything moves with it, and what falls
// outside the new extent is dropped. Rewrites the source.
bool reshapeSource(const std::filesystem::path& source, std::int64_t westChunks, std::int64_t northChunks,
                   const WorldExtent& world, std::string* why = nullptr);

// Rebuilds raster storage on a different supported sample grid without
// changing world-space coverage or vector features.
bool resampleSource(const std::filesystem::path& source, const WorldExtent& world, std::string* why = nullptr);

struct ExportOptions {
    enum class Mode { Source, Preview } mode = Mode::Source;
    // What to export: the whole world unless one of these is given.
    std::optional<std::array<double, 4>> rect;          // metres, x0 y0 x1 y1
    std::optional<std::pair<std::string, std::uint32_t>> regionId;   // categorical layer, id
    std::optional<std::string> feature;                 // a vector feature's id (a named region outline)
    // Raster names, vector kinds and "poi"; empty: all of them.
    std::set<std::string> layers;
};

struct ExportReport {
    double originX = 0, originY = 0, sizeX = 0, sizeY = 0;
    std::size_t rasters = 0, features = 0;
};

std::optional<ExportReport> exportPackage(const std::filesystem::path& source, const std::filesystem::path& package,
                                          const ExportOptions& options, std::string* why = nullptr);

} // namespace engine::world_source
