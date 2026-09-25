#pragma once
// Reading the ground off a baked page.
//
// Everything that used to ask `HeightField` for a height, a slope or how a
// body crosses a point was asking a procedural field to work it out again -
// per corner, per cell, per chunk. A page already holds the answer at four
// metres and a padded border so a sample can see its own neighbours; between
// them the ground interpolates, exactly as it did before.
//
// The point of routing readers through this rather than through the field is
// not speed. It is that a navmesh built from a page and the terrain drawn
// from that same page cannot disagree: they are the same numbers. A field
// query beside a page lookup is two answers again.

#include <cstdint>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace world::streaming {

// One page, read at any point inside it.
//
// A chunk is sixty-four metres and a page five hundred and twelve, so a
// chunk always lies inside one page - including the far corner of its last
// cell, which the page's padding covers.
class PageGround {
public:
    explicit PageGround(const BakedPage& page);

    // Whether this page can answer for a point: inside its own square, plus
    // the ring of padding it can still interpolate over.
    [[nodiscard]] bool covers(core::WorldPos at) const;

    // The ground, between the samples.
    [[nodiscard]] core::Fixed heightAt(core::WorldPos at) const;
    // How deep the water stands, zero on dry ground. Interpolated over the
    // depth rather than over the surface: a surface averaged with the dry
    // ground beside it would pull the shoreline out to sea.
    [[nodiscard]] core::Fixed depthAt(core::WorldPos at) const;
    // The hillside, and the sharpest step to a neighbour within the sample
    // square the point falls in.
    [[nodiscard]] core::Fixed slopeAt(core::WorldPos at) const;
    [[nodiscard]] core::Fixed steepnessAt(core::WorldPos at) const;
    // The same rule the page itself was answered with.
    [[nodiscard]] Travel travelAt(core::WorldPos at) const;

    [[nodiscard]] const BaseTile& base() const { return page_.base; }
    [[nodiscard]] const WaterTile& water() const { return page_.water; }

private:
    struct Spot {
        std::int64_t column = 0, row = 0;   // of the sample south-west of the point
        core::Fixed fx, fy;
        bool inside = false;
    };
    [[nodiscard]] Spot spotOf(core::WorldPos at) const;
    [[nodiscard]] core::Fixed heightOf(std::int64_t column, std::int64_t row) const;
    [[nodiscard]] core::Fixed depthOf(std::int64_t column, std::int64_t row) const;
    [[nodiscard]] core::Fixed slopeOf(std::int64_t column, std::int64_t row) const;

    BakedPage page_;
    HsimQuantisation quantisation_;
    std::int64_t side_ = 0;
    core::WorldPos origin_{};   // world position of stored sample (0, 0)
};

} // namespace world::streaming

// The lattice of one level, read across pages.
//
// A mesh does not stop at a page any more than a query stops at a chunk: a
// normal needs its neighbours and an openness its neighbours' neighbours, and
// some of those are in the page next door. This finds the page a sample
// belongs to and keeps the last one, because a mesh walks and the next sample
// is almost always in the same page.
//
// Coordinates are global lattice indices in four-metre units, the same
// convention the meshes have always used, and must be multiples of this
// level's stride. That is what makes a coarse reading a subsample of the fine
// one rather than a different grid: level 3 reads every eighth index of the
// same lattice.
namespace world::streaming {

class PageLattice {
public:
    PageLattice(PageStore& store, std::uint8_t level);

    // What the water does at a point, from the same page the ground came
    // from. Read off the field beside a page-read ground, these would be two
    // answers again - a dry bed under standing water, or a surface hanging
    // over ground that moved beneath it.
    struct Water {
        bool any = false;
        core::Fixed level{};
        core::Fixed cover{};        // nought to one
        core::Fixed flowX{}, flowY{};
        WaterBodyId body = kInvalidWaterBodyId;
    };
    // `footprint` is how wide a patch of ground the caller is drawing with
    // this one sample, in metres. Coverage is the share of it that is under
    // water: water is a yes or a no at a point, which is a lie once the
    // points are further apart than the stream between them, and it is what
    // lets a shader fade surf onto dry land instead of ending it at a line.
    // Nought asks about the point alone.
    [[nodiscard]] Water waterAt(core::WorldPos at, core::Fixed footprint = core::kZero);

    [[nodiscard]] std::int64_t stride() const { return stride_; }
    // The ground at a lattice index, or the level's floor where no page can be
    // had - a page that cannot be baked is a hole, and a hole in the terrain
    // is worse than flat ground at the bottom of the world.
    [[nodiscard]] core::Fixed height(std::int64_t sx, std::int64_t sy);
    // The same sample with the visual bands the page carries added to it.
    //
    // What is drawn, as against what is walked on. H_sim is the ground the
    // navmesh and the simulation read and it is never touched; this is that
    // ground plus the detail below the seventy-metre floor the generator's own
    // octaves stop at, which a page holds only where its spacing can draw it.
    // A coarse page therefore returns H_sim and a fine one returns H_sim with
    // metres of hillside on it, which is what refinement is supposed to mean.
    [[nodiscard]] core::Fixed visualHeight(std::int64_t sx, std::int64_t sy);
    // What the ground is made of here, off the page rather than worked out
    // again. Interpolated between the material grid's own samples, which are
    // coarser than the heights because the rules that decide them are.
    [[nodiscard]] MaterialWeights materials(std::int64_t sx, std::int64_t sy);
    // Whether the sample was answered from a page rather than filled in.
    [[nodiscard]] bool known(std::int64_t sx, std::int64_t sy);

private:
    const BakedPage* pageFor(std::int64_t sx, std::int64_t sy);

    PageStore& store_;
    std::uint8_t level_ = 0;
    std::int64_t stride_ = 1;
    std::int64_t perPage_ = 128;
    std::shared_ptr<const BakedPage> held_;
    TileKey heldKey_{0, 0, 255};   // a level no page has, so the first look misses
};

} // namespace world::streaming
