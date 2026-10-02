#pragma once
// Baking H_sim into square, world-aligned pages.
//
// This is the migration adapter the architecture asks for and nothing more:
// the height of a BaseTile sample is whatever `HeightField::sampleHeight`
// says at that global lattice coordinate. Keeping the old answer is the whole
// point - it makes the new storage comparable against the geometry it is going
// to replace, one change at a time. Replacing the surface itself with graph
// rasterisation comes later, and only once the topology is trusted.
//
// It may only run on a worker. Constructing a baker constructs a HeightField,
// which runs MacroWorld::attach over the entire macro map, and a HeightField
// caches a drainage neighbourhood in itself - so a baker is one worker's
// private object, never shared and never made per tile.
//
// A tile owns bytes, not truth. Every sample is addressed by its global
// lattice index, so two pages that overlap do not agree on their shared
// samples - they compute the same numbers, because they ask the same
// question. The quantisation range is world-wide for the same reason: a range
// taken per tile would store one world coordinate as two different integers
// on either side of a page boundary.

#include <cstdint>
#include <functional>

#include "engine/core/fixed.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_lod.hpp"
#include "game/world/terrain_streaming/base_tile.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"
#include "game/world/terrain_streaming/water_tile.hpp"
#include "game/world/terrain_streaming/worker_runtime.hpp"

namespace generation { struct WorldMapData; }

namespace world::streaming {

// The one range every page of a world quantises against.
//
// Sixteen bits over the whole world rather than over a tile. Per tile would
// be finer, and it would also mean that the sample at x = 512 is one integer
// in the page west of it and another in the page east of it - which is
// exactly the seam this storage exists to make impossible. Everything here is
// integer arithmetic, so the answer is the same on every machine.
struct HsimQuantisation {
    core::Fixed low{};
    core::Fixed high{};

    [[nodiscard]] bool valid() const { return high.raw > low.raw; }
    [[nodiscard]] std::uint16_t quantise(core::Fixed height) const;
    [[nodiscard]] core::Fixed dequantise(std::uint16_t stored) const;
    // The worst error the round trip can introduce, which is half a step.
    [[nodiscard]] core::Fixed resolution() const;
};

// How a body gets over a sample, in the byte a page stores it in.
//
// Travel plus one, so zero keeps the meaning the channel mask gives it:
// nobody has decided. A page's outermost ring of padding has no neighbours of
// its own to take a slope from and stays undecided; every interior sample is
// answered, which is what a navmesh reads.
constexpr std::uint8_t encodeTravel(Travel travel) {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(travel) + 1u);
}
constexpr bool travelDecided(std::uint8_t stored) { return stored != 0; }
constexpr Travel decodeTravel(std::uint8_t stored) {
    return static_cast<Travel>(stored - 1u);
}

// How a body gets over ground of this depth, steepness and slope.
//
// One rule, wherever it is asked. A page answers it per sample when it is
// baked, and a reader answers it per point between them; two copies of the
// thresholds would be two answers, and the disagreement would show up as a
// navmesh that does not match the ground it was built from.
//
// `depth` is how deep the water stands, zero on dry ground. `steepness` is
// the sharpest step to a neighbour over the run between them - what a body
// actually meets, because a four-metre bank is a bank however gentle the
// hillside around it. `rolling` is the hillside itself, which is smooth:
// classifying by the sharp step alone made a navmesh of hundreds of slivers
// where the country is one hillside.
inline Travel travelFrom(core::Fixed depth, core::Fixed steepness, core::Fixed rolling) {
    if (depth.raw > 0) {
        if (depth.raw <= fordableDepth().raw) return Travel::Ford;
        if (depth.raw <= swimmableDepth().raw) return Travel::Swim;
        return Travel::None;
    }
    if (steepness >= unclimbableSlope()) return Travel::None;
    if (steepness >= cliffSlope()) return Travel::Climb;
    if (rolling < walkableSlope()) return Travel::Walk;
    if (rolling < scrambleSlope()) return Travel::Scramble;
    if (rolling < unclimbableSlope()) return Travel::Climb;
    return Travel::None;
}

// Derived once from the finished macro map, before any tile is baked. The
// envelope is conservative on purpose: a height that fell outside it would be
// clamped, and a clamped mountain is a quiet loss of terrain rather than a
// loud one.
HsimQuantisation hsimQuantisationFor(const generation::WorldMapData& world);

// The ground of a page and the water over it, made together.
//
// Two products of one sweep rather than two derivations: the carve answers
// both at every sample, so a bed cannot end up above its own surface and a
// shoreline cannot fall in two places.
// How many centimetres one stored unit of a residual is worth. Centimetres
// because the bands swing about five metres at the most and a hundredth of a
// metre is finer than anything can see, and signed sixteen bits hold three
// hundred metres of it.
inline constexpr std::int32_t kResidualPerMetre = 100;

struct BakedPage {
    BaseTile base;
    WaterTile water;
    // Visual detail below what H_sim carries, kept beside the heights and
    // never folded into authoritative H_sim. Persistent H64 and H16 carry the
    // same large-band values at shared samples, so refinement cannot move their
    // common vertices; H8 refines the large band and H4 adds medium detail.
    ResidualTile large;
    ResidualTile medium;
    // What the ground is made of, as weights that sum to one, one byte each.
    //
    // Worked out here rather than per mesh vertex. Materials are six gradient
    // noises, a read of the coarse map and a carve of the graph - measured,
    // three and a fifth microseconds a point - and a mesh asked for them at
    // every lattice point it drew, every time the camera moved far enough to
    // start a new snapshot. That is four seconds of processor for one view,
    // paid again on the next. A page is baked once and kept, and the whole
    // visible world is sixty of them.
    //
    // On a coarser grid than the heights, because they do not need a finer
    // one: the shortest noise in the material rules is twenty-six metres, so
    // eight metres to the sample is three times over what it can carry, and
    // the fine mottling belongs to the shader anyway. Interpolated on the way
    // out, so a transition is still a curve rather than a step.
    std::uint16_t materialWidth = 0;      // samples per side, padding included
    std::int32_t materialMetres = 0;      // metres between them
    std::vector<std::uint8_t> materials;  // materialWidth^2 * kMaterialCount

    // Bake-only provenance, never serialized or dereferenced by a reader.
    const generation::WorldMapData* world = nullptr;
    const HydrologyGraph* graph = nullptr;
    // Preserve exact (pre-quantisation) depth and residual damping at parent
    // points. Reconstructing either from packed channels changes thresholds.
    std::vector<core::Fixed> refinementDepth, refinementAshore;
    std::size_t evaluatedSamples = 0, reusedSamples = 0;
    // H16-cell feature flags: 1 = river valley, 2 = surface bend, 4 = bank. Permanent
    // metadata, independent of camera LOD and of optional medium residual.
    std::vector<std::uint8_t> featureCells;
    // The ground the page was baked over, when the world has edits
    // (edit_layer.hpp). `groundRevision` is the layer's revision as the bake
    // began: the page is current while nothing inside its reach is newer.
    // `ground` is the fingerprint of what the layer held within that reach -
    // nought for ground nobody touched - and is what names the page on disk
    // and tells a mesh built from it apart from one built from other ground.
    std::uint64_t groundRevision = 0, ground = 0;
};

// One worker's baker. The world and the graph must outlive it, and so must the
// edit layer when there is one: its field reads what people dug.
class BaseTileBaker {
public:
    BaseTileBaker(const generation::WorldMapData& world, const HydrologyGraph& graph,
                  HsimQuantisation quantisation, const EditLayer* edits = nullptr);

    // `sampleMetres` has to divide the page and be a whole number of lattice
    // steps. An invalid request gives back a tile that fails BaseTile::valid,
    // never a plausible one.
    //
    // Cancellation is polled between rows: a page is seventeen thousand
    // samples and a worker abandoning one must not finish it first.
    [[nodiscard]] BakedPage bakePage(TileKey key, std::int32_t sampleMetres = kSampleMetres,
                                     std::uint16_t padding = kDefaultPaddingSamples,
                                     const std::function<bool()>& cancelled = {},
                                     const BakedPage* parent = nullptr) const;

    // The ground alone, for a caller that has no use for the water.
    [[nodiscard]] BaseTile bake(TileKey key, std::int32_t sampleMetres = kSampleMetres,
                                std::uint16_t padding = kDefaultPaddingSamples,
                                const std::function<bool()>& cancelled = {}) const;

    [[nodiscard]] const HsimQuantisation& quantisation() const { return quantisation_; }
    [[nodiscard]] const HeightField& field() const { return field_; }
    // Where the finer levels have pages at all (PageStore::containsLand). A
    // page coarser than H64 spans many 512 m squares, and over a square that
    // would have no page of its own the ground is what an absent page reads -
    // the open sea's floor - so the coarse level and the fine one under it
    // agree, and nothing rises out of the sea as the camera comes down.
    void landMask(const terrain::LandMask64* mask) { landMask_ = mask; }
    // How many distinct catchments the macro map holds, after the dense remap.
    [[nodiscard]] std::size_t watershedCount() const { return watershedCount_; }

private:
    const generation::WorldMapData& world_;
    const HydrologyGraph& graph_;
    HsimQuantisation quantisation_;
    HeightField field_;
    const terrain::LandMask64* landMask_ = nullptr;
    // Per macro cell, resolved once so a sample costs a lookup rather than a
    // search through the graph.
    std::vector<std::uint16_t> watershedOfCell_;
    std::size_t watershedCount_ = 0;
};

// A BuildFunction for TerrainWorkerRuntime that bakes base tiles, keeping one
// baker per worker thread. Returns nothing for a request that does not ask for
// a base tile: this function makes base tiles and nothing else yet.
//
// The world and graph must outlive the runtime that runs it.
TerrainWorkerRuntime::BuildFunction makeBaseTileBuildFunction(
        const generation::WorldMapData& world, const HydrologyGraph& graph,
        HsimQuantisation quantisation);

} // namespace world::streaming
