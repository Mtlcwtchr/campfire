#pragma once
// One region of placed objects, merged into geometry of its own.
//
// This is the piece the spatial hierarchy was missing. A node of that hierarchy
// says "these forty trees, here"; what it had to draw with was a grove asset
// baked offline from nine trees standing in a fixed 3x3 pattern, scaled until
// its bounding box matched the node. That is not a representation of the node:
// the trees are not where the node's trees are, there are not as many of them,
// they are not the species the node holds, and the scaling that makes the box
// fit also makes every tree in it the wrong height. A silhouette built that way
// cannot converge on the objects it replaces, however carefully it is faded in.
//
// So the mass is merged from the ACTUAL members, at their actual positions,
// scales and rotations, by the same route a single crown takes: quads in, a
// density field, a surface, a DAG. A region aggregate is then not a special
// kind of asset with a code path of its own - it is a crown built at a coarser
// cell, over more input.
//
// Region-local coordinates throughout. A world-space bake would spend its float
// precision on where the region is rather than on what is in it, and two
// regions holding the same arrangement would produce different geometry.
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"
#include "engine/geometry/quad_mass.hpp"

namespace engine::geometry {

// One placed object. `yaw` turns about +Z, which is the convention the scene
// instances already use, and `scale` is uniform for the same reason.
struct RegionMember {
    std::uint32_t model = 0;
    float position[3]{};     // region-local metres
    float scale = 1;
    float yaw = 0;
};

// The geometry a member's model is made of, as the caller already holds it:
// the finest level, in model-local metres.
struct RegionSource {
    std::span<const float> positions;         // three per vertex
    std::span<const std::uint32_t> indices;
    // One texture layer per POSITION. Empty paints the whole model `layer`,
    // which is what a caller with no per-vertex layers should get.
    std::span<const float> layers;
    float layer = 0;
};

struct RegionMassOptions {
    // The scale at which separate OBJECTS stop being separate, in metres. A
    // crown merges leaves at leaf scale; a region merges trees at tree scale,
    // and asking for a cell near leaf size over a hundred trees is how a bake
    // produces more geometry than the objects it replaces.
    double cellMetres = 2.0;
    // Below this there is no mass to merge, and the individual objects are both
    // cheaper and correct. A region aggregate is never an improvement on two
    // trees.
    std::size_t minimumMembers = 4;
    // A bound on the work. Exceeding it coarsens the cell rather than dropping
    // members: a region that silently forgot objects would leave a hole in the
    // forest exactly where the hierarchy promised a replacement.
    std::size_t maxQuads = 1u << 20;
    // Clusters of an aggregate are deliberately large. A cluster is the unit of
    // culling AND the unit of an indirect draw record, and the whole region is
    // a few dozen pixels across by the time its aggregate is used: splitting it
    // into 128-triangle clusters buys culling nobody can see and costs a draw
    // record each. Measured: at the default it was 95 records per region, which
    // is worse than the individual objects it replaced.
    std::size_t clusterTriangles = 2048;
    ClusterDagOptions dag;
};

struct RegionMass {
    std::vector<float> positions;   // three per vertex, region-local
    std::vector<float> normals;
    std::vector<float> coverage;    // one per vertex
    std::vector<float> layer;       // one per vertex
    std::vector<std::uint32_t> indices;
    std::vector<MeshCluster> clusters;
    std::uint32_t levels = 0;
    // The finest level alone, which is what this aggregate costs when it is
    // drawn instead of its members. `triangles()` below counts EVERY level of
    // the DAG, because they all live in one index buffer - comparing that with
    // the members would charge the aggregate for geometry no cut ever draws
    // together.
    std::size_t finestTriangles = 0;
    // The bounds of what was actually built, region-local. The hierarchy needs
    // these to project the node's error; they are measured, never assumed from
    // the region's own square.
    float centre[3]{};
    float radius = 0;
    // What the bake did, for a tool to print and a test to check.
    std::size_t members = 0;
    std::size_t quads = 0;
    double cellMetres = 0;
    bool coarsened = false;

    [[nodiscard]] bool empty() const { return clusters.empty() || indices.empty(); }
    // Every level, as stored. Not what one frame draws.
    [[nodiscard]] std::size_t triangles() const { return indices.size() / 3; }
};

// One size of aggregate slot. A pool of these is not one size for the same
// reason a mesh is not one level: a region a hundred metres away and one two
// kilometres away are both "one region", but the far one is a few pixels
// across, and a cell four times coarser is still well inside the same pixel
// error. Sizing every slot for the near case is what held the pool to a few
// dozen regions - the far ones, which are the many, each reserved capacity for
// geometry they would never carry.
struct RegionMassTier {
    double cellMetres = 4.0;
    std::uint32_t slots = 0;
    std::uint32_t vertices = 0;   // capacity of ONE slot, not of the tier
    std::uint32_t indices = 0;
    // What a bake at this cell is expected to be out by before it is measured.
    // The merged surface sits up to a cell from the objects on either side, so
    // this is the estimate the request is made on; what the slot then carries
    // is read off the bake itself and can be worse if the bake had to coarsen.
    [[nodiscard]] double error() const { return 2.0 * cellMetres; }
};

// The COARSEST tier whose expected error still fits `allowedError`, or -1 when
// even the finest is too coarse and the region must be drawn as its objects.
// Coarsest, because a finer tier spends scarce capacity on detail that is by
// construction below the error the caller asked for. Tiers may be given in any
// order; tiers with no slots or no capacity are not chosen.
[[nodiscard]] int chooseRegionMassTier(std::span<const RegionMassTier> tiers,
                                       double allowedError);

// The tiers a renderer reserves its aggregate pool as. Here rather than in the
// pass so that the bake test can hold the capacities against what a real region
// actually bakes to - a capacity nobody measured is a capacity that refuses
// regions in the field, where nothing is watching.
[[nodiscard]] std::span<const RegionMassTier> defaultRegionMassTiers();

// Deterministic in the members given, in the order given. Members naming a
// model outside `sources`, and members with a non-finite or non-positive
// transform, are refused rather than skipped: a bake that quietly dropped one
// is a bake whose error bound no longer covers what it replaces.
[[nodiscard]] RegionMass bakeRegionMass(std::span<const RegionMember> members,
                                        std::span<const RegionSource> sources,
                                        const RegionMassOptions& options = {});

} // namespace engine::geometry

