#pragma once
// The ground's change from features, as the height field asks for it
// (doc/plan_procedural_environment_2026-10-03.md, part C).
//
// The height field adds this between its own detail and a person's edits:
//
//     country -> detail -> FEATURES -> the hydrology's channels -> edits
//
// so the navmesh, travel and water all see a gully the moment it is planned.
// Instances are indexed in blocks of 512 m, a block's list built once from the
// planner and shared. A world whose recipes move no ground answers with one
// branch, the same promise EditLayer makes for an unedited world.
//
// Thread safety: blocks are shared and immutable; the block a thread last used
// is remembered per thread, so a page walking its samples takes the lock
// once a block rather than once a sample.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine/environment/planner.hpp"
#include "engine/environment/terrain_ops.hpp"

namespace engine::environment {

class FeatureLayer {
public:
    static constexpr std::int64_t kBlockMetres = 512;

    explicit FeatureLayer(std::shared_ptr<const FeaturePlanner> planner);

    [[nodiscard]] bool empty() const { return !movesGround_; }
    // Bumped whenever the layer is rebuilt; whatever was made from the ground
    // remembers it, like EditLayer::revision.
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    struct Block {
        std::int64_t x = 0, y = 0;
        std::vector<FeatureInstance> instances;   // every instance whose bounds touch the block
    };
    [[nodiscard]] std::shared_ptr<const Block> block(std::int64_t bx, std::int64_t by) const;

    // How far the features move the ground at `p`, given the ground there
    // before them. Nought almost everywhere.
    [[nodiscard]] core::Fixed at(core::WorldPos p, core::Fixed base, std::int64_t strideMetres) const;

    // Every instance touching `p` and where `p` falls in its shapes: what the
    // masks and the dressing read.
    struct Hit {
        const FeatureInstance* instance = nullptr;
        OpGeometry geometry;
    };
    void probe(core::WorldPos p, core::Fixed base, std::vector<Hit>& out,
               std::shared_ptr<const Block>* keep = nullptr) const;

    [[nodiscard]] const FeaturePlanner& planner() const { return *planner_; }
    [[nodiscard]] const Catalogue& catalogue() const { return planner_->catalogue(); }
    [[nodiscard]] std::shared_ptr<const FeaturePlanner> plannerShared() const { return planner_; }

private:
    [[nodiscard]] std::shared_ptr<const Block> blockAt(core::WorldPos p) const;

    std::shared_ptr<const FeaturePlanner> planner_;
    bool movesGround_ = false;
    std::uint64_t generation_ = 0;
    mutable std::mutex lock_;
    struct KeyHash {
        std::size_t operator()(const std::pair<std::int64_t, std::int64_t>& k) const noexcept {
            return std::hash<std::int64_t>{}(k.first * 73856093 ^ k.second * 19349663);
        }
    };
    mutable std::unordered_map<std::pair<std::int64_t, std::int64_t>, std::shared_ptr<const Block>, KeyHash> blocks_;
    mutable std::vector<std::pair<std::int64_t, std::int64_t>> order_;
    static constexpr std::size_t kBlocksKept = 4096;
};

} // namespace engine::environment
