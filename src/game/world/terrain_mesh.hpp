#pragma once
// The surface of the world as a triangle mesh, built a chunk at a time.
//
// The mesh is the geometry of the ground: vertices in world coordinates with a
// height, a normal and material weights, and triangles between them. It is not
// a picture of a tile map and it is not one quad per cell of anything the
// gameplay knows about - it is a triangulation of the height field, and it can
// be replaced (coarser far away, refined along a cliff, retriangulated
// entirely) without a single caller changing, because callers ask the field or
// the mesh, never the lattice underneath.
//
// Nothing is stitched. Two neighbouring chunks put their border vertices at the
// same world coordinates and compute them from the same field, so they come out
// bit-identical: same height, same normal, same weights. Cracks, normal seams
// and material seams are not fixed here - they are unrepresentable.

#include <array>
#include <cstdint>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"

namespace world {

struct TerrainVertex {
    core::WorldPos position;      // where it is, in world metres
    core::Fixed height;           // metres above sea level
    Normal normal;
    MaterialWeights materials;
    // How this point stands against the ground around it, in metres: the
    // height less the average of the ground a little way off. Positive on a
    // ridge or a spur, negative in a gully or a hollow, nought on an even
    // slope.
    //
    // A normal cannot say this. Two slopes at the same angle are lit the same
    // whether one is the crest of a range and the other the floor of a valley,
    // which is why a heightfield lit by its normals alone reads as crumpled
    // paper rather than as country. This is what puts the shape back: crests
    // catch the light, hollows hold shade, and it costs four heights that the
    // builder has already sampled.
    core::Fixed openness;
    core::Fixed windExposure;
    // Temperature, fertility, moisture, wind XY and drainage. Computed by the
    // worker and carried with the immutable mesh so the frame never resamples
    // the procedural world field.
    std::array<core::Fixed, 6> environment{};

    // The same point as the next level up would build it: where its surface
    // passes through here, and how it is lit.
    //
    // This is what a change of level is morphed across. Every second vertex of
    // this mesh is a vertex of the coarser one at the same world coordinate -
    // the levels share a lattice, they do not merely resemble each other - so
    // for those the coarse height is this height and nothing moves. The ones in
    // between are the ones the coarser mesh does not have, and its surface
    // passes over them along the edge or the diagonal of a coarse triangle. Move
    // them there and this mesh *is* the coarse mesh, to the last bit, and the
    // swap that follows cannot be seen.
    //
    // It costs no samples of the field: the builder has the grid in its hand
    // with two rings of margin, which is exactly the reach a coarse normal
    // needs.
    core::Fixed coarseHeight;
    Normal coarseNormal;
    // Steppe / boreal / temperate / tropical cover; independent of material IDs.
    std::array<core::Fixed, 4> foliage{core::kZero, core::kZero, core::kOne, core::kZero};
    core::Fixed desertCover;
};

struct TerrainMesh {
    ChunkId chunk;
    std::int32_t lod = 0;
    // Nonzero for the runtime annular stream. Square addressing is retained
    // only for offline geometry/navigation builders, not for ring ownership.
    std::uint64_t ringKey = 0;
    core::WorldPos ringCentre;
    double innerRadius = 0, outerRadius = 0;
    // Vertices per side of the lattice this mesh was built from. The renderer
    // does not need it; a rebuild of one vertex does.
    std::int32_t side = 0;
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;   // three to a triangle
    core::Fixed lowest, highest;          // for culling and for the water pass

    std::size_t triangleCount() const { return indices.size() / 3; }
};

// A single edge of the world where the ground breaks. Cliffs are geometry, not
// shading: this is the line along the top of the drop, with the heights either
// side of it, and it is what both the cliff face strip and walkability are cut
// from.
//
// The segment runs between two points on the half-lattice - the dual of the
// sample grid - so segments meet end to end and chain into lines. Because both
// endpoints are world coordinates derived from the field, a line that leaves a
// chunk arrives in the next one at exactly the same point.
struct CliffSegment {
    core::WorldPos a, b;          // along the top of the drop
    core::Fixed top, bottom;      // heights of the high and low sides
    // Unit-ish direction the ground falls, from the high side to the low one.
    // What the face strip is extruded along and what a walker is stopped by.
    core::Fixed fallX, fallY;
    // Whether the broken ground is above this line or below it: the lip of a
    // bank, or the toe of one.
    bool brokenSideIsHigh = true;

    core::Fixed drop() const { return top - bottom; }
};

// The same segments joined end to end. A line is open (it ran off into a
// neighbouring chunk) or closed (it went round a knoll).
struct CliffLine {
    std::vector<core::WorldPos> points;
    std::vector<core::Fixed> top, bottom;
    bool closed = false;
};

// Builds the mesh for one chunk. Reads the field outside the chunk freely -
// that is the ghost margin, and it costs nothing because the field is a
// function rather than a store.
//
// `lod` is how coarse: at 0 a chunk is kChunkMetres of ground sampled every
// kSampleMetres, and each step up doubles both, so a chunk is always the same
// number of triangles and covers four times the ground. That is what lets one
// world be looked at from a metre and from a continent without ever swapping
// to a different picture of it - the far chunks are the same mesh drawn from
// fewer samples of the same field, not a map of somewhere else.
//
// Past thirty-two metres to the sample the level also decides *what is worth
// working out*: the field is told the spacing and leaves out the waves and the
// valleys that spacing cannot draw (height_field.hpp, Detail). A level is
// therefore a model of the world and not merely a sampling rate of one - which
// is what makes a coarse patch cheap rather than expensive - and what keeps the
// change of model from showing is the morph target every vertex carries.
//
// Chunk coordinates are in units of the level's own chunk size, so chunk {1,0}
// at level 3 is not chunk {1,0} at level 0.
TerrainMesh buildChunkMesh(const HeightField& field, ChunkId chunk, std::int32_t lod = 0);

// The ground a chunk covers at a level, in metres.
inline std::int32_t chunkMetresAt(std::int32_t lod) { return kChunkMetres << lod; }
inline std::int32_t sampleMetresAt(std::int32_t lod) { return kSampleMetres << lod; }

// Where the ground breaks inside one chunk. Segments on the chunk's own edge
// are emitted by both chunks that share the edge, identically, so a consumer
// that walks a region of chunks sees one continuous line.
std::vector<CliffSegment> extractCliffs(const HeightField& field, ChunkId chunk);

// Joins segments into lines. Takes segments from as many chunks as the caller
// cares to gather, which is how a cliff line crosses a border: it is not a
// per-chunk structure, it is a structure over whatever the caller loaded.
std::vector<CliffLine> chainCliffs(std::vector<CliffSegment> segments);

// The face of a cliff, as the strip of geometry hung under its top edge. Two
// triangles per segment, from the top line down to where the ground picks up
// again, with the rock material forced on: a face is rock whatever the ground
// above it is wearing.
struct CliffStrip {
    std::vector<TerrainVertex> vertices;
    std::vector<std::uint32_t> indices;
};
CliffStrip buildCliffStrip(const std::vector<CliffSegment>& segments);

} // namespace world
