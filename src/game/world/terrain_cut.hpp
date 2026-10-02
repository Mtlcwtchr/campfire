#pragma once
// A non-overlapping quadtree cut. Preload requests never become draw nodes.
#include <array>
#include <functional>
#include <map>
#include <vector>

#include "game/world/tile_mesh.hpp"
#include "game/world/terrain_lod.hpp"

namespace world::terrain {

class TerrainCut {
public:
    struct Block { TileId tile; float parentMorph = 0; };
    using Predicate = std::function<bool(TileId)>;
    using Priority = std::function<double(TileId)>;
    using Refinement = std::function<bool(TileId, bool)>;
    using Family = std::function<std::vector<TileId>(TileId)>;

    void update(const std::vector<TileId>& roots, int target, const Predicate& visible,
                const Predicate& resident, double seconds, const Predicate& admit = {},
                const Priority& priority = {}, bool retainCoverage = false, const Refinement& refine = {},
                double morphSeconds = 0.25, const Family& family = {}) {
        drawing_.clear();
        coverage_.clear();
        activeParents_.clear();
        needsUpdate_ = false;
        retainCoverage_ = retainCoverage;
        morphSeconds_ = morphSeconds;
        family_ = family;
        auto ordered = roots;
        order(ordered, priority);
        for (const auto root : ordered) visit(root, target, visible, resident, seconds, true, admit, priority, refine);
        for (const auto& block : coverage_) if (visible(block.tile)) drawing_.push_back(block);
    }
    const std::vector<Block>& drawing() const { return drawing_; }
    // A complete ready cut, independent of the camera that triggered planning.
    // Parents are replaced by children, never drawn on top of them.
    const std::vector<Block>& coverage() const { return coverage_; }
    const std::vector<TileId>& activeParents() const { return activeParents_; }
    bool needsUpdate() const { return needsUpdate_; }
    void clear() { nodes_.clear(); drawing_.clear(); coverage_.clear(); activeParents_.clear(); needsUpdate_ = false; }
    // Folds the subtree under `tile` back into `tile` itself: its transition
    // starts again from Parent and every descendant's is forgotten. The rest of
    // the cut keeps its state. For pages that are gone - the square that stood
    // on them falls back to its nearest ancestor that still can be drawn, and
    // nothing else moves. Returns the number of descendant states dropped.
    std::size_t collapse(TileId tile, const Family& family = {}) {
        if (family) family_ = family;
        std::size_t dropped = 0;
        std::vector<TileId> stack{tile};
        while (!stack.empty()) {
            const auto at = stack.back();
            stack.pop_back();
            for (const auto child : descendants(at)) {
                if (nodes_.erase(tileKeyOf(child))) {
                    ++dropped;
                    stack.push_back(child);
                }
            }
        }
        nodes_[tileKeyOf(tile)] = RefinementTransition(morphSeconds_);
        needsUpdate_ = true;
        return dropped;
    }

    static std::array<TileId, 4> children(TileId tile) {
        return {{{tile.x * 2, tile.y * 2, tile.lod - 1},
                 {tile.x * 2 + 1, tile.y * 2, tile.lod - 1},
                 {tile.x * 2, tile.y * 2 + 1, tile.lod - 1},
                 {tile.x * 2 + 1, tile.y * 2 + 1, tile.lod - 1}}};
    }

private:
    std::vector<TileId> descendants(TileId tile) const {
        if (tile.lod <= 0) return {};
        if (family_) return family_(tile);
        const auto tiles = children(tile);
        return {tiles.begin(), tiles.end()};
    }
    template<class Tiles> static void order(Tiles& tiles, const Priority& priority) {
        if (priority) std::stable_sort(tiles.begin(), tiles.end(), [&](TileId a, TileId b) {
            return priority(a) < priority(b);
        });
    }
    // Returns true only after all descendant reverse morphs have settled.
    bool visit(TileId tile, int target, const Predicate& visible, const Predicate& resident,
               double dt, bool maySplit, const Predicate& admit, const Priority& priority, const Refinement& refineAt) {
        auto& transition = nodes_[tileKeyOf(tile)];
        transition.setDuration(morphSeconds_);
        auto orderedChildren = descendants(tile);
        order(orderedChildren, priority);
        const bool onScreen = visible(tile);
        if (!onScreen && !transition.drawChildren()) {
            transition.requestParent();
            if (retainCoverage_ && resident(tile)) coverage_.push_back({tile, 0});
            return true;
        }
        const bool refine = maySplit && onScreen && tile.lod > 0 &&
                            (refineAt ? refineAt(tile, transition.drawChildren()) : tile.lod > target) &&
                            (transition.drawChildren() || !admit || admit(tile));
        const bool parentReady = resident(tile); // also requests/pins the parent
        bool childrenReady = true;
        if (tile.lod > 0 && (refine || transition.drawChildren()))
            for (const auto child : orderedChildren)
                childrenReady = resident(child) && childrenReady;
        if (refine && parentReady) transition.requestChildren(childrenReady);
        const bool wasChildren = transition.drawChildren();
        bool descendantsSettled = true;
        if (wasChildren && tile.lod > 0) {
            // A second split cannot start until its parent's morph is complete.
            const bool deeper = refine && transition.phase() == RefinementPhase::Children;
            for (const auto child : orderedChildren)
                descendantsSettled = visit(child, target, visible, resident, dt, deeper, admit, priority, refineAt) && descendantsSettled;
        }
        if (!refine && descendantsSettled && parentReady) transition.requestParent();
        // Include the last tick: it can unlock a deeper split on the next pass.
        needsUpdate_ = needsUpdate_ || transition.phase() == RefinementPhase::Refining ||
                       transition.phase() == RefinementPhase::Coarsening;
        transition.tick(dt);
        // Publish the last reverse-morph endpoint with the CHILD topology.
        // Removing its vertices in the same snapshot loses the final segment
        // of the animation; the renderer must display it before the next cut.
        if (wasChildren && !transition.drawChildren()) {
            for (auto& block : coverage_)
                for (const auto child : orderedChildren)
                    if (tileKeyOf(block.tile) == tileKeyOf(child)) block.parentMorph = 1;
            activeParents_.push_back(tile);
            needsUpdate_ = true;
            return false;
        }
        if (transition.drawChildren()) {
            activeParents_.push_back(tile);
            // Descendants emitted their own cut. During a direct transition all
            // four children must use this same morph (including offscreen ones).
            if (!wasChildren) {
                for (const auto child : orderedChildren)
                    if (retainCoverage_ || visible(child)) coverage_.push_back({child, 1.0f - transition.morph()});
            } else if (transition.phase() != RefinementPhase::Children) {
                for (auto& block : coverage_)
                    for (const auto child : orderedChildren)
                        if (tileKeyOf(block.tile) == tileKeyOf(child))
                            block.parentMorph = 1.0f - transition.morph();
            }
        } else {
            if (wasChildren) {
                const auto isChild = [&](const Block& block) {
                    for (const auto child : orderedChildren)
                        if (tileKeyOf(block.tile) == tileKeyOf(child)) return true;
                    return false;
                };
                std::erase_if(coverage_, isChild);
            }
            if ((retainCoverage_ || onScreen) && parentReady) coverage_.push_back({tile, 0});
        }
        return !transition.drawChildren();
    }
    std::map<std::int64_t, RefinementTransition> nodes_;
    std::vector<Block> drawing_, coverage_;
    std::vector<TileId> activeParents_;
    bool needsUpdate_ = false, retainCoverage_ = false;
    double morphSeconds_ = 0.25;
    Family family_;
};

} // namespace world::terrain

