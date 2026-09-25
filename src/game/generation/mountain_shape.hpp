#pragma once

#include <array>
#include <vector>
#include "engine/core/fixed.hpp"

namespace generation::mountains {
using core::Fixed;

inline std::int64_t floorDiv(std::int64_t n, std::int64_t d) {
    return n/d-(n%d<0);
}
inline Fixed smooth(Fixed t) {
    t=core::saturate(t);
    return t*t*t*(t*(t*6-Fixed::fromInt(15))+Fixed::fromInt(10));
}

inline core::Fixed crest(Fixed distance, Fixed width) {
    const auto q=core::saturate(core::kOne-core::abs(distance)/width);
    // Defined crest, rounded foot: zero slope at the toe, not at the summit.
    return q*q;
}

struct Shape {
    Fixed body;
    Fixed junction;
    [[nodiscard]] Fixed height() const { return body+junction; }
};

inline constexpr std::int64_t kRegionMetres=8192;
inline constexpr int kBranchGenerations=5;
inline constexpr int kSegmentsPerBranch=3; // two separated junctions, then a terminal taper
inline constexpr int kLocalSupportMetres=9216; // full local footprint, including widths and placement warp
inline constexpr int kSupportMetres=11264; // also covers regional rotation and centre jitter
inline constexpr std::size_t kMaxNodes=320;
inline constexpr std::size_t kMaxEdges=320;
static_assert(1+3*kSegmentsPerBranch*((std::size_t{1}<<kBranchGenerations)-1)<=kMaxNodes);
static_assert(3*kSegmentsPerBranch*((std::size_t{1}<<kBranchGenerations)-1)<=kMaxEdges);

struct Node {
    Fixed x,y,height,width,lift;
    std::uint8_t degree=0;
};
struct Edge {
    std::uint16_t from=0,to=0;
    std::uint8_t order=0;
};
struct Skeleton {
    std::array<Node,kMaxNodes> nodes{};
    std::array<Edge,kMaxEdges> edges{};
    std::uint16_t nodeCount=0,edgeCount=0;
    Fixed centreX,centreY;
};

// Five finite generations of displaced, staggered branching. Height is
// an extrusion of this 2D skeleton, not an independently sampled noise field.
// Builders are public for diagnostics/tests; runtime uses a thread-local LRU
// and a spatial edge index. No camera, chunk or LOD changes the skeleton.
[[nodiscard]] Skeleton makeSkeleton(std::uint64_t seed);
[[nodiscard]] Skeleton regionSkeleton(std::uint64_t seed,std::int64_t rx,std::int64_t ry);
[[nodiscard]] Shape sampleSkeleton(const Skeleton& skeleton,Fixed x,Fixed y);
// One patch-local skeleton, already oriented by the hybrid placement pass.
[[nodiscard]] Shape sample(std::uint64_t seed,Fixed x,Fixed y);
// Different regional orientations, overlapping smoothly across region borders.
[[nodiscard]] Shape worldSample(std::uint64_t seed,Fixed x,Fixed y);
// Bake-only drainage incision, in unsigned decimetres to SUBTRACT. Accumulation
// follows the actual descending surface, not an independent positive fractal.
// Limits contain local erosion depth budgets; zero protects plains/water/rims.
[[nodiscard]] std::vector<std::uint16_t> drainageIncisions(
    const std::vector<Fixed>& heights,const std::vector<Fixed>& limits,
    int width,int height,int spacing);
} // namespace generation::mountains
