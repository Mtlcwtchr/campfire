#pragma once
// The sparse refinement tree the generator's data actually lives in.
//
// This is NOT the render cut. The render cut is chosen from a camera; this is
// chosen from the world, and it is the same tree on every machine that builds
// the same world.
//
// The one thing to understand about it: there is no mandatory chain from 256 m
// down to 4 m. A node is analysed, and only the children that need it are
// opened. Two children of one parent living at different depths is the normal
// state, not a transient one:
//
//     256 m parent
//     |-- 64 m leaf                 gentle land, nothing left to find
//     |-- 64 m leaf
//     |-- 64 m
//     |   |-- 16 m leaf
//     |   |-- 16 m
//     |   |   |-- 4 m leaf          the slot of a gully
//     |   |   \-- ...
//     |   \-- ...
//     \-- 64 m leaf
//
// The cost of such a tree follows what the world CONTAINS, not how large it is.
// A mandatory chain would make ten thousand kilometres of ocean cost what ten
// thousand kilometres of broken coast costs, which is the mistake this whole
// generator rework exists to undo.
//
// The second thing: a detail that does not change the shape at the step being
// shown is not a reason to split. It belongs in the node's SHADING. And the
// rule runs both ways - a decision not to split must hand the detail on, with
// its amplitude, or the feature is lost in silence and the ground comes out
// smooth plastic. That is what ShadingDebt is: the record of what a leaf owes
// its shader.
//
// See doc/scale_aware_procedural_world_generation_plan.md, P4.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace world::terrain {

// A square of world, before anything has been built in it.
struct SourceBounds {
    std::int64_t x = 0, y = 0;     // the corner, in metres
    std::int64_t metres = 0;       // the side
    [[nodiscard]] std::int64_t centreX() const { return x + metres / 2; }
    [[nodiscard]] std::int64_t centreY() const { return y + metres / 2; }
};

// One cheap look at a node, read by every subsystem in its own way, made BEFORE
// any of its children exist.
//
// Every field is an UPPER BOUND, not an average. A node this says nothing is in
// will never be visited again, so understating is a lost feature rather than a
// lost decimal - and where no bound can be established the honest answer is
// `unknown`, never zero. A certificate of no error is a claim, and a claim
// about a shape nobody has looked at is a guess.
//
// And they are a statement about THIS node at THIS size, not about the feature
// in the abstract. A crest is important while a node is too coarse to hold it
// and stops being important once a node can - which is the only thing that
// makes refinement finite. An analysis that returns a feature's weight
// unchanged at every size asks for a tree without a bottom, and the budget,
// not the world, ends up deciding where the ground stops.
struct RefinementHints {
    // How far this node's surface may sit from the one a finer step would give,
    // in metres.
    float geometryError = 0;
    // Metres of relief erosion is capable of moving here.
    float erosionPotential = 0;
    // How concentrated the water through this node is, 0 to 1: a sheet drains,
    // a channel cuts.
    float drainagePotential = 0;
    // How much boundary runs through the node, 0 to 1 - shore, material, snow
    // line. A boundary is a material fact, and on its own it is NOT a reason to
    // add vertices.
    float boundaryComplexity = 0;
    // The weight of physical features: ridge, channel, road, settlement, 0 to 1.
    float featureImportance = 0;
    // No upper bound could be established. Treated as "refine", never as zero.
    bool unknown = false;
};

// What a leaf could not turn into geometry and therefore owes its shading, in
// the units the hint was measured in. A shader that draws these is drawing the
// detail that did not fit; a leaf that dropped them silently is why ground
// comes out looking like plastic.
struct ShadingDebt {
    float relief = 0;      // metres of shape the step could not carry
    float erosion = 0;     // metres of erosion that has to be a picture
    float drainage = 0;    // 0..1, the channel that is a line rather than a valley
    float boundary = 0;    // 0..1, the edge that is a mask rather than an edge
    [[nodiscard]] bool any() const {
        return relief > 0 || erosion > 0 || drainage > 0 || boundary > 0;
    }
};

struct RefinementPolicy {
    // A leaf may carry this much shape error, in metres.
    double geometryTolerance = 0.5;
    // Erosion beyond this much relief is worth vertices; below it, a picture.
    double erosionTolerance = 0.5;
    // Above these, the feature is worth vertices.
    double drainageThreshold = 0.55;
    double featureThreshold = 0.35;
    // A boundary is a material fact. Above this it asks for a finer MATERIAL
    // step, and never for a finer geometric one.
    double boundaryThreshold = 0.2;
    // How many extra halvings a wholly complex boundary asks of the material
    // field. Three is eight times finer than the geometry under it.
    int materialLevels = 3;
    // Never subdivide below this, whatever the hints say. A feature's corridor
    // of influence is finite, and so must the tree be.
    std::int64_t finestMetres = 4;
    std::int64_t finestMaterialMetres = 1;
    // A bound on the work, not a guess at it. Reaching it is reported rather
    // than hidden, because a tree that silently stopped is a world with a flat
    // patch nobody ordered.
    std::size_t maxNodes = 1u << 18;
};

inline constexpr std::uint32_t kNoChild = 0xffffffffu;

struct SourceNode {
    SourceBounds bounds;
    RefinementHints hints;
    ShadingDebt debt;
    // Four children, contiguous, in the order (low x, low y), (high x, low y),
    // (low x, high y), (high x, high y). kNoChild for a leaf.
    std::uint32_t firstChild = kNoChild;
    std::uint32_t parent = kNoChild;
    std::uint8_t depth = 0;
    // The step this node's MATERIAL field needs, which is not the step its
    // shape has and need not be. A shore inside a gentle slope wants an exact
    // edge and no extra vertices.
    std::int64_t materialMetres = 0;
    [[nodiscard]] bool leaf() const { return firstChild == kNoChild; }
};

struct SourceTree {
    std::vector<SourceNode> nodes;   // node zero is the root
    std::size_t leaves = 0;
    // How many times the analysis was asked. The whole approach only pays if
    // this is far smaller than the leaves a mandatory chain would have made,
    // so it is counted rather than assumed.
    std::size_t analysed = 0;
    std::size_t deepest = 0;
    // Leaves that still owe their shading something, and leaves that stopped
    // because the budget ran out rather than because there was nothing left.
    std::size_t indebted = 0;
    std::size_t truncated = 0;
    [[nodiscard]] bool complete() const { return truncated == 0; }
};

// Analysis is supplied rather than owned: the tree is a policy over a world,
// and a test supplies a world it knows the answer for.
using Analyze = std::function<RefinementHints(const SourceBounds&)>;

// Builds the tree over `root`. Deterministic: the same world and policy give
// the same tree, in the same order, whatever a caller does around it.
[[nodiscard]] SourceTree refine(const SourceBounds& root, const Analyze& analyze,
                                const RefinementPolicy& policy = {});

// Whether these hints ask for a finer SHAPE at this step, and what a leaf at
// this step would owe its shading. Exposed because the rule is the thing worth
// testing, and because a subsystem deciding its own refinement has to apply the
// same one or the two trees disagree.
[[nodiscard]] bool wantsFinerShape(const RefinementHints&, const RefinementPolicy&);
[[nodiscard]] ShadingDebt debtOf(const RefinementHints&, const RefinementPolicy&);
[[nodiscard]] std::int64_t materialStepFor(const RefinementHints&, const RefinementPolicy&,
                                           std::int64_t nodeMetres);

} // namespace world::terrain
