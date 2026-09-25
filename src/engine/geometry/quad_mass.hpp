#pragma once
// Turning a mass of alpha cards into one surface that can be retopologised.
//
// This is the piece that was missing, and it is why impostors exist at all.
//
// A tree's crown is not geometry a DAG can simplify: it is 960 separate
// two-triangle quads, nothing touching anything, so there is nothing to group
// and nothing to collapse. The only two answers the renderer had were "draw all
// 960 quads" and "draw a billboard", and the billboard is a picture of one side
// of the tree - which from a camera looking down is a squashed side view of a
// tree lying about which way it faces.
//
// But a crown IS a surface. Not the surface of its leaves - the surface of the
// MASS they form. Once that is a mesh, everything the rest of this engine knows
// how to do applies to it: it clusters, it simplifies, it becomes the coarse end
// of the same DAG, and the last level of that DAG is the two triangles a
// billboard used to be. The picture stops being a special case.
//
// The same algorithm answers the other scale. A grove is the same question with
// the quads of several trees in the input instead of one: merge the mass, build
// a surface, retopologise it. A "grove impostor" is then not a special kind of
// thing with its own asset and its own code path - it is this, at a coarser
// cell size.
//
// How it works, and why this way:
//
//   1. Splat every quad's area into a density field on a regular grid. Cards
//      are flat and have holes; what matters at the scale where they merge is
//      how much leaf there is per volume, not where each leaf is.
//   2. Extract the level set of that field by surface nets - one vertex per
//      cell that straddles the threshold, placed at the average of its edge
//      crossings. Marching cubes would give the same surface with three times
//      the triangles and a worse one to simplify, because its vertices sit on
//      edges rather than inside cells.
//   3. Carry the density to the vertices as COVERAGE, so a thin canopy stays
//      thin: the shader reads it and the silhouette survives the merge instead
//      of turning a lacy crown into a potato.
//
// The cell size is the whole control. It is the scale below which the mass
// stops being separate leaves and becomes one thing, and it is the caller's
// decision because that scale is different for a crown and for a grove.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::geometry {

// One card, as the thing that has area and a hole in it rather than as two
// triangles. Corners in order around the quad.
struct Quad {
    float corner[4][3]{};
    // How much of its own area is actually opaque, 0 to 1. An alpha-cut leaf
    // card is mostly hole, and a merge that believed the quad was solid would
    // build a shell around the bounding box of the foliage rather than around
    // the foliage.
    float coverage = 1;
    // Which material this piece of the mass came from. A crown is not one
    // substance: it is foliage with a trunk up the middle of it, and a shell
    // that forgets which is which paints the trunk with leaves - a tree with a
    // green trunk is a bush, whatever its silhouette does.
    float layer = 0;
};

struct QuadMassOptions {
    // The scale at which separate cards stop being separate, in model units.
    double cellMetres = 0.5;
    // How much leaf per cell counts as inside the mass. Low, because a canopy
    // is mostly air and the surface wanted is the one a viewer reads as the
    // edge of the foliage, not the one that encloses every leaf.
    double solidDensity = 0.12;
    // Close the inside. A crown of cards is a hollow shell, so the level set
    // has an inner surface as well as an outer one - and the inner one costs
    // exactly as much as the outer for a thing no viewer will ever be inside.
    // Filling what the outside cannot reach removes it, and leaves a mass that
    // simplifies better besides: a blob collapses where a double shell folds.
    bool fillCavities = true;
    // Passes of averaging over the extracted surface. Surface nets come out
    // faceted at cell scale; two passes remove that without moving the
    // silhouette anywhere a viewer would notice.
    int relax = 2;
    // A bound on the work, not a guess at it: the cell size grows until the
    // grid fits. Reported, so a caller that cared can see it happened.
    std::size_t maxSamples = 8u << 20;
};

struct QuadMass {
    std::vector<float> positions;   // three per vertex
    std::vector<float> normals;     // three per vertex
    // Per vertex, how solid the mass is there, 0 to 1. What keeps a lacy crown
    // lacy when it has become one surface.
    std::vector<float> coverage;
    // Per vertex, the layer of the quads that built the mass there - a weighted
    // mean, so the join between bark and leaf is a blend rather than a seam.
    std::vector<float> layer;
    std::vector<std::uint32_t> indices;
    // What the merge actually did, for a tool to print and a test to check.
    std::size_t quads = 0;
    std::size_t solidCells = 0;
    std::size_t filledCells = 0;   // cavities the outside could not reach
    double cellMetres = 0;
    bool coarsened = false;         // the cell size grew to fit the budget

    [[nodiscard]] std::size_t triangles() const { return indices.size() / 3; }
    [[nodiscard]] bool empty() const { return indices.empty(); }
};

[[nodiscard]] QuadMass mergeQuadMass(std::span<const Quad> quads, const QuadMassOptions& = {});

// The quads of an indexed triangle mesh that are alpha cards: every connected
// component of two triangles, as one quad. What a tree's crown is made of, and
// the input the merge above wants.
// `corners`, when given, receives the four source vertices of each card in the
// same order as the quads. What wants it: a card wears a different texture from
// the thing it hangs on, and the only way to know which is to ask the cards
// rather than the mesh - a tree has more trunk than leaves, so counting every
// vertex of the model gives the trunk's answer and the crown comes out in bark.
[[nodiscard]] std::vector<Quad> cardsOf(std::span<const float> positions,
                                        std::span<const std::uint32_t> indices,
                                        std::vector<std::array<std::uint32_t, 4>>* corners = nullptr);

} // namespace engine::geometry
