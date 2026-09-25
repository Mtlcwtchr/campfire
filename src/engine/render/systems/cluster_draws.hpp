#pragma once
// Turning what a frame decided to draw into the arguments of one indirect draw.
//
// The selection produced runs of instances that share an asset and a level. The
// card reads a list of draw arguments, one per piece of geometry, and issues
// them all from a single recorded command - so a forest that was a draw per
// asset per level becomes a draw.
//
// Two shapes of geometry go through here and the difference is worth stating,
// because it is the whole of what virtual geometry buys:
//
// - An asset with only a CHAIN contributes one argument per run. Its level was
//   chosen for the whole run, so every instance in it draws the same triangles
//   wherever it stands on the model.
// - An asset with a cluster DAG contributes one argument per cluster of the
//   CUT. The cut is chosen by the run's allowance, so the parts of the model
//   that simplify well are coarse and the parts that do not are not - within
//   one model, in one draw.
//
// This is pure arithmetic over descriptions. It touches no device, so the thing
// that decides what the card draws is checked by a test that runs in a
// millisecond rather than by looking at a picture.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"
#include "engine/render/draw_arguments.hpp"

namespace engine::render {

// Where one asset's geometry sits in the buffers the whole pass shares, and
// what representations it offers.
struct MeshGeometry {
    // Added to every index of this asset, so one vertex buffer holds them all.
    std::int32_t vertexBase = 0;
    // The discrete chain, finest first, as ranges of the shared index buffer.
    std::span<const IndexRange> levels;
    // The cluster DAG, if the asset has one. Its ranges are in the same shared
    // index buffer, and its indices already carry this asset's own numbering.
    std::span<const geometry::MeshCluster> clusters;
    // Added to every cluster range, for the same reason vertexBase exists.
    std::uint32_t clusterBase = 0;
    // The alpha cards of each chain level, which a DAG does not cover. A run
    // drawn from clusters still draws these: the solid part comes from the cut
    // and the leaves from the level the offline thinning already reduced for
    // that distance. Empty for an asset that has none.
    std::span<const IndexRange> cardLevels;
    std::uint32_t cardBase = 0;
    // The crown: the same cards, merged into one surface and clustered. A run
    // drawn from a cut takes whichever of the two is cheaper at its allowance -
    // the cards up close, where they are individual leaves and the shell is
    // just a shell, and the crown beyond that, where it simplifies and they do
    // not. Its vertices are its own, so it carries its own base.
    std::span<const geometry::MeshCluster> crownClusters;
    std::uint32_t crownBase = 0;
    std::int32_t crownVertexBase = 0;
};

// One run of instances that share an asset and a level.
struct DrawRun {
    std::uint32_t asset = 0;
    std::uint32_t level = 0;
    // Where the run starts in the frame's instance arena, as an instance count
    // from wherever the draw binds it - which is what a draw argument carries.
    std::uint32_t first = 0;
    std::uint32_t count = 0;
    // How far this run's geometry may sit from the model, in model units. It is
    // the tightest of the run's instances, so the coarsest instance is never
    // drawn finer than it asked and the finest is never drawn coarser. Zero or
    // less says the asset has no measured cut and the chain level decides.
    double allowance = 0;
};

struct DrawPlan {
    std::vector<DrawArguments> draws;
    std::size_t fromClusters = 0;
    std::size_t fromChain = 0;
    std::size_t fromCards = 0;
    std::size_t fromCrown = 0;
    // Runs whose cut was built and then dropped because the chain was cheaper
    // at the same allowance. A build with many of these has a DAG whose coarse
    // end stalls, and that is a fact about the asset, not about the frame.
    std::size_t declined = 0;
    // Triangles the card will read, summed over instances: what the plan
    // actually costs, rather than how many draws it took to say it.
    std::size_t triangles = 0;
    // Runs naming an asset that is not there, or a run of nothing.
    std::size_t rejected = 0;
};

// A shell may replace a whole object only when its complete finest surface
// fits the allowance and a nonempty cut exists.
[[nodiscard]] bool crownFits(std::span<const geometry::MeshCluster> clusters, double allowance);
// A wide proxy has a fixed height. Uniformly scaling a shell to that width
// spends some of its screen-space error budget on changing the height as well.
[[nodiscard]] double proxyShellAllowance(double pixelsPerMetre, double scale,
                                         double height, double pixelError);

[[nodiscard]] DrawPlan planDraws(std::span<const DrawRun> runs,
                                 std::span<const MeshGeometry> assets);

} // namespace engine::render
