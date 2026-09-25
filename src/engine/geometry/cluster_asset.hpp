#pragma once
// The cluster DAG as bytes, and back.
//
// A sidecar beside the mesh rather than a section inside it: the mesh is
// written by tools/prepare_scene_models.py and read by the renderer, and
// putting a C++-built structure inside a Python-written file would make both
// ends of that pipeline learn each other's format for no gain. A model without
// a sidecar simply has no clusters yet, which is what every model is until the
// tool has been run over it.
//
// Indices address the MODEL's own vertex array, not a welded one. The build
// welds matching geometry, but regular assets supply an attribute identity key
// so UV/normal/material seams remain distinct. It writes the resulting source
// indices back out, so one vertex buffer can serve every level without borrowing
// a neighbouring seam's attributes.
#include <cstddef>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"
#include "engine/geometry/quad_mass.hpp"

namespace engine::geometry {

struct ClusterAsset {
    // What the file was built from, so a renderer can refuse a sidecar that
    // belongs to a mesh it is not holding.
    std::uint32_t sourceVertices = 0;
    std::uint32_t sourceTriangles = 0;
    // Triangles deliberately left out: alpha cards do not go through a DAG.
    // 960 separate two-triangle quads per tree is the worst case one can be
    // handed, and thinning with area compensation beats it. See VG3.
    std::uint32_t cardTriangles = 0;
    std::uint32_t levels = 0;
    std::uint32_t dominated = 0;
    std::vector<std::uint32_t> indices;
    // One position triplet per cluster-index occurrence. The source mesh
    // remains the attribute vertex buffer, while this parallel stream carries
    // QEM-moved positions for replacement surfaces without inventing a second
    // material/UV vertex format.
    std::vector<float> clusterPositions;
    std::vector<MeshCluster> clusters;

    // The alpha cards of each level of the mesh's OWN chain, pulled out so a
    // model can draw its solid part from the DAG and its leaves from the level
    // the chain already thinned for that distance. Without this a tree either
    // keeps the chain for everything or loses its leaves, and both are worse
    // than the tree being clustered at all.
    std::vector<std::uint32_t> cardIndices;
    std::vector<IndexRange> cardLevels;

    // The shell: the WHOLE model - its cards and its solid geometry together -
    // merged into one surface and clustered like any other geometry.
    //
    // Not only the foliage, and the reason is a measurement. A tree's solid
    // part cannot be simplified past about two hundred triangles: the group
    // rims that make a cut seamless are most of a thin branchy thing. So at the
    // distance where almost every tree in a frame sits, the trunk's cut plus
    // its cards still cost more than the whole model's coarsest chain level,
    // and the cut is declined. Merged together they are one mass, and one mass
    // collapses.
    //
    // This is what a billboard was. It is right from every angle, it arrives by
    // the same rule as every other level, and nothing about it is a special
    // case.
    //
    // Its vertices are its OWN - a shell of a leaf mass is not made of the
    // model's vertices - so they travel with it. Positions and normals only:
    // what texture it wears and how it is laid out is the renderer's business,
    // and this library does not know what a texture layer is.
    //
    // This is what lets a tree stop being a billboard at distance. A billboard
    // is a picture of one side; a shell is right from every angle, and it
    // simplifies through the same DAG as the trunk instead of being a separate
    // asset with a separate code path.
    std::vector<float> crownPositions;   // three per vertex
    std::vector<float> crownNormals;
    std::vector<float> crownCoverage;    // one per vertex, 0..1
    // One per vertex: the texture layer the shell wears THERE.
    //
    // A crown is not one substance. The shell is built over the whole model,
    // trunk included - leaving the trunk out makes a shell that costs more
    // than the billboards it replaces - and painting the lot with the foliage
    // layer puts leaves on the trunk. A tree with a green trunk is a bush, and
    // that is what these looked like from every distance the shell is drawn at.
    std::vector<float> crownLayers;
    std::vector<std::uint32_t> crownIndices;
    std::vector<MeshCluster> crownClusters;
    std::uint32_t crownLevels = 0;
    // Which texture layer the crown wears, set by whoever built the asset from
    // a mesh that has layers. Carried, never interpreted.
    float crownLayer = 0;
    double crownCellMetres = 0;

    // A crown alone is an answer: a bush is entirely alpha cards, has no
    // solid geometry to cluster, and is the case the crown exists for.
    [[nodiscard]] bool empty() const { return clusters.empty() && crownClusters.empty(); }
};

// Bytes, little-endian, in the order the struct declares. Both directions here
// so that the writer and the reader cannot drift: a tool and a renderer that
// each described the layout would agree until one of them was edited.
[[nodiscard]] std::vector<std::byte> encodeClusters(const ClusterAsset& asset);

// `why` is filled with what was wrong when the result is empty. Every field a
// reader will index by is checked against the file's own sizes: a sidecar is
// content, and content arrives corrupted.
[[nodiscard]] ClusterAsset decodeClusters(std::span<const std::byte> bytes, std::string& why);

// Complete source mesh, including disconnected foliage. No hand-authored LODs,
// card thinning or shell reconstruction. Indices retain source UV/material splits.
[[nodiscard]] ClusterAsset buildSourceClusterAsset(std::span<const float> positions,
                                                   std::span<const std::uint32_t> indices,
                                                   ClusterDagOptions options = {});

// The DAG of the SOLID part of a mesh, with its indices written back into the
// caller's vertex numbering. Components of two triangles or fewer are alpha
// cards: the DAG is built from the finest level without them, and each level's
// cards are carried out separately so they can still be drawn.
//
// `levels` are the chain's ranges into `indices`, finest first. Only the first
// is clustered - the coarser ones are a different answer to the same question -
// but every one of them is searched for cards, because a coarse level's cards
// are the ones the offline thinning already reduced for that distance.
[[nodiscard]] ClusterAsset buildClusterAsset(std::span<const float> positions,
                                             std::span<const std::uint32_t> indices,
                                             std::span<const IndexRange> levels,
                                             const ClusterDagOptions& options = {},
                                             std::span<const std::uint64_t> attributeKeys = {},
                                             std::span<const std::uint64_t> hardBoundaryKeys = {});

// Merges the whole finest level - cards as quads, solid triangles as
// degenerate ones - into a shell and clusters it, filling the crown fields. Separate from the build so a caller that does not want a
// crown does not pay for one, and so the cell size - the scale at which leaves
// stop being separate leaves - is a decision made where the model's size is
// known rather than guessed in a library.
// `layers` is one texture layer per POSITION, as the model stores them. Empty
// leaves every shell vertex on `asset.crownLayer`, which is what a caller with
// no layers to give should get.
void buildCrown(ClusterAsset& asset, std::span<const float> positions,
                std::span<const std::uint32_t> indices, std::span<const IndexRange> levels,
                double cellMetres, std::span<const float> layers = {},
                const ClusterDagOptions& options = {});

} // namespace engine::geometry
