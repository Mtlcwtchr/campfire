#pragma once
// The authored world a layout is built on: the height skeleton and control
// maps imported into the world's source (engine/world_source, D158), read by
// the generator region by region.
//
// A region whose source holds height is IMPORTED. It is made by hand like a
// painted one - a run of its own, at the stage it has been taken to - but its
// ground is not grown from paint and noise: the skeleton is the ground, at the
// macro cells (sea level, climate, drainage read it there) and on the
// foundation lattice, where it is interpolated from its 256 m samples. The
// foundation's weathering then works it as much as the erosion mask says, and
// the rain follows the moisture mask. What lies below 256 m is the
// foundation's and the runtime's to make, as it is everywhere else.
//
// Importing is heights and masks only. Drainage (the height's hollows
// breached, the valleys and the climate worked out) and water (rivers and
// lakes) are stages of their own the person takes a region to afterwards
// (RegionStage::Relief, RegionStage::Water); an imported region starts as
// Primary, its skeleton as it was drawn.
//
// Read once per build of a region and held (the source never changes under a
// provider: an import opens a new one), and quantised on the way in - heights
// to decimetres, masks to bytes - so that everything after is integer and the
// same on every machine.
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace engine::world_source {
class WorldSource;
}
namespace engine::biomes {
class CategoryField;
class DetailEdits;
}

namespace generation {

struct WorldLayout;

// One region's skeleton, with two samples of margin at every side so it can
// be interpolated to its edge.
struct ImportedGround {
    static constexpr std::int32_t kSampleMetres = 256;
    static constexpr std::int32_t kMargin = 2;
    std::int32_t side = 0;                         // samples a side, margin included
    std::vector<std::int32_t> heightDm;            // decimetres, side x side
    // 0..255 each, side x side; empty when the source has no such mask.
    std::vector<std::uint8_t> erosion, moisture, forest, mountain;
    // 255 where the source's water flags say lake (a basin with a bed the
    // drainage keeps and the generator holds water in); empty when none.
    std::vector<std::uint8_t> lake;
    std::uint64_t key = 0;                         // changes when any of it does

    // At a point of the region, in metres from its north-west corner:
    // Catmull-Rom through the samples, in integers.
    [[nodiscard]] std::int32_t heightAt(std::int64_t x, std::int64_t y) const;
    // A mask there, bilinear; `fallback` when the source has none.
    [[nodiscard]] std::int32_t maskAt(const std::vector<std::uint8_t>& mask, std::int64_t x, std::int64_t y,
                                      std::int32_t fallback) const;
};

class ImportedSource {
public:
    // The source at `root`, or nothing when there is none (or it is empty).
    //
    // An import is heights and control maps and nothing else: opening it does
    // no drainage. The drained height (engine/world_source/bake.hpp) is a
    // stage of its own, asked for by the person (the editor's "Drainage"):
    // it lies beside the source (`<root>/../baked`) and is taken when it was
    // made from the source as it is now. `bakeIfStale` makes it first when it
    // is missing or stale - what a world with drained regions needs.
    static std::shared_ptr<const ImportedSource> open(const std::filesystem::path& root, bool bakeIfStale = false);
    static std::filesystem::path bakedRootOf(const std::filesystem::path& root) {
        return root.parent_path() / "baked";
    }
    // Makes the drained height beside the source unless it is current. False
    // (and why) when it could not be made.
    static bool bake(const std::filesystem::path& root, std::string* why = nullptr);
    ~ImportedSource();

    const std::filesystem::path& root() const { return root_; }
    // Whether any height is held in the region (131 072 m squares).
    bool holds(std::int32_t regionX, std::int32_t regionY) const;
    // How many regions do.
    std::int32_t regionsHeld() const { return std::int32_t(held_.size()); }
    // Whether the drained height is there, made from the source as it is.
    bool drained() const { return baked_ != nullptr; }
    // The region's skeleton, read on first ask and kept: the source's height
    // as it was imported, or - `drained`, and when the drainage is baked -
    // with its hollows breached. The masks are the source's own either way.
    std::shared_ptr<const ImportedGround> ground(std::int32_t regionX, std::int32_t regionY,
                                                 bool drained = false) const;
    // The categorical control layers (engine/biomes): the ground's category
    // and the forest, water and decor biomes, by the source's numbering -
    // which an import keeps the registry's. Read once, over every chunk the
    // source keeps, and held; null when the source has none of them.
    std::shared_ptr<const engine::biomes::CategoryField> categories() const;
    // The hand edits to the details the categories place (<root>/details):
    // pinned instances, removed ones, density brushed. Read once; empty when
    // there are none.
    std::shared_ptr<const engine::biomes::DetailEdits> details() const;

private:
    ImportedSource() = default;
    std::filesystem::path root_;
    std::unique_ptr<engine::world_source::WorldSource> source_;
    // The height with its drainage made whole (engine/world_source/bake.hpp),
    // beside the source, when it has been made and is current.
    std::unique_ptr<engine::world_source::WorldSource> baked_;
    std::map<std::pair<std::int32_t, std::int32_t>, std::uint64_t> held_;          // region -> key
    std::map<std::pair<std::int32_t, std::int32_t>, std::uint64_t> drainedKeys_;   // region -> key, drained
    mutable std::mutex guard_;
    mutable std::map<std::tuple<std::int32_t, std::int32_t, bool>, std::shared_ptr<const ImportedGround>> grounds_;
    mutable bool categoriesRead_ = false;
    mutable std::shared_ptr<const engine::biomes::CategoryField> categories_;
    mutable std::shared_ptr<const engine::biomes::DetailEdits> details_;
};

// Whether a region of the layout takes its ground from the import.
bool importedIn(const WorldLayout& layout, std::int32_t regionX, std::int32_t regionY);
// Whether any region of the layout takes its ground from the import and has
// been taken as far as its drainage (RegionStage::Relief or further): what
// needs the drained height to be current.
bool importDrained(const WorldLayout& layout);
// The source at `root` opened for `layout` (whose own `imported` is not
// read): its drained height made first when a region of the layout that
// holds imported ground has been taken to its drainage and it is stale.
std::shared_ptr<const ImportedSource> openImported(const std::filesystem::path& root, const WorldLayout& layout);

} // namespace generation
