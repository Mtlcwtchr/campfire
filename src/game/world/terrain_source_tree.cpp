#include "game/world/terrain_source_tree.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace world::terrain {
namespace {
bool sane(const RefinementHints& hints) {
    return std::isfinite(hints.geometryError) && std::isfinite(hints.erosionPotential) &&
           std::isfinite(hints.drainagePotential) && std::isfinite(hints.boundaryComplexity) &&
           std::isfinite(hints.featureImportance);
}
}

bool wantsFinerShape(const RefinementHints& hints, const RefinementPolicy& policy) {
    // An analysis that could not bound the node asks to be looked at closer.
    // The alternative is to treat "I do not know" as "there is nothing here",
    // which is the one reading that loses features without saying so.
    if (hints.unknown || !sane(hints)) return true;
    if (double(hints.geometryError) > policy.geometryTolerance) return true;
    if (double(hints.erosionPotential) > policy.erosionTolerance) return true;
    if (double(hints.drainagePotential) > policy.drainageThreshold) return true;
    if (double(hints.featureImportance) > policy.featureThreshold) return true;
    // boundaryComplexity is deliberately absent. A material edge is an exact
    // field, not a denser mesh: asking for vertices here is how a coastline
    // ends up costing what a mountain range costs and looking no better.
    return false;
}

ShadingDebt debtOf(const RefinementHints& hints, const RefinementPolicy& policy) {
    ShadingDebt debt;
    if (!sane(hints)) return debt;
    // What a leaf at this step could not carry as shape. Recorded rather than
    // dropped: a subsystem that declines to split MUST hand the detail on, or
    // the feature is lost in silence.
    if (double(hints.geometryError) > policy.geometryTolerance)
        debt.relief = float(double(hints.geometryError) - policy.geometryTolerance);
    if (double(hints.erosionPotential) > policy.erosionTolerance)
        debt.erosion = float(double(hints.erosionPotential) - policy.erosionTolerance);
    if (double(hints.drainagePotential) > policy.drainageThreshold)
        debt.drainage = hints.drainagePotential;
    if (double(hints.boundaryComplexity) > policy.boundaryThreshold)
        debt.boundary = hints.boundaryComplexity;
    return debt;
}

std::int64_t materialStepFor(const RefinementHints& hints, const RefinementPolicy& policy,
                             std::int64_t nodeMetres) {
    if (nodeMetres <= 0) return policy.finestMaterialMetres;
    if (!sane(hints) || double(hints.boundaryComplexity) <= policy.boundaryThreshold)
        return nodeMetres;
    // How far past the threshold the boundary runs, over how far it could run.
    const double span = std::max(1e-6, 1.0 - policy.boundaryThreshold);
    const double past = std::clamp((double(hints.boundaryComplexity) - policy.boundaryThreshold) / span,
                                   0.0, 1.0);
    const int halvings = std::max(0, int(std::ceil(past * policy.materialLevels)));
    std::int64_t step = nodeMetres;
    for (int i = 0; i < halvings && step > policy.finestMaterialMetres; ++i) step /= 2;
    return std::max(step, policy.finestMaterialMetres);
}

SourceTree refine(const SourceBounds& root, const Analyze& analyze,
                  const RefinementPolicy& policy) {
    SourceTree tree;
    if (!analyze || root.metres <= 0 || policy.finestMetres <= 0 || policy.maxNodes == 0)
        return tree;

    tree.nodes.reserve(std::min<std::size_t>(policy.maxNodes, 4096));
    SourceNode first;
    first.bounds = root;
    tree.nodes.push_back(first);

    // A queue rather than recursion, and breadth-first rather than depth-first,
    // so that a budget that runs out leaves the tree even rather than deep down
    // one corner and coarse everywhere else.
    std::queue<std::uint32_t> pending;
    pending.push(0);
    while (!pending.empty()) {
        const auto at = pending.front();
        pending.pop();
        {
            SourceNode& node = tree.nodes[at];
            node.hints = analyze(node.bounds);
            ++tree.analysed;
            node.materialMetres = materialStepFor(node.hints, policy, node.bounds.metres);
            tree.deepest = std::max(tree.deepest, std::size_t(node.depth));
        }
        const bool wanted = wantsFinerShape(tree.nodes[at].hints, policy);
        const bool canSplit = tree.nodes[at].bounds.metres / 2 >= policy.finestMetres &&
                              tree.nodes.size() + 4 <= policy.maxNodes;
        if (!wanted || !canSplit) {
            SourceNode& node = tree.nodes[at];
            // Whatever asked for a finer shape and did not get one is the
            // node's debt to its shader, at the amplitude it asked with.
            node.debt = debtOf(node.hints, policy);
            ++tree.leaves;
            if (node.debt.any()) ++tree.indebted;
            if (wanted && !canSplit) ++tree.truncated;
            continue;
        }

        const auto bounds = tree.nodes[at].bounds;
        const auto depth = tree.nodes[at].depth;
        const std::int64_t half = bounds.metres / 2;
        const auto firstChild = std::uint32_t(tree.nodes.size());
        tree.nodes[at].firstChild = firstChild;
        for (int corner = 0; corner < 4; ++corner) {
            SourceNode child;
            child.bounds = {bounds.x + (corner & 1) * half, bounds.y + (corner >> 1) * half, half};
            child.parent = at;
            child.depth = std::uint8_t(depth + 1);
            tree.nodes.push_back(child);
            pending.push(firstChild + std::uint32_t(corner));
        }
    }
    return tree;
}

} // namespace world::terrain
