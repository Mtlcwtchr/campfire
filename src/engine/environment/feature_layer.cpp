#include "engine/environment/feature_layer.hpp"

#include <algorithm>

namespace engine::environment {

namespace {
std::atomic<std::uint64_t> nextGeneration{1};

std::int64_t blockOf(core::Fixed v) {
    const auto block = core::Fixed::Raw(FeatureLayer::kBlockMetres) << core::Fixed::kFracBits;
    std::int64_t q = v.raw / block;
    if (v.raw % block != 0 && v.raw < 0) --q;
    return q;
}

struct LastBlock {
    const void* owner = nullptr;
    std::uint64_t generation = 0;
    std::int64_t x = 0, y = 0;
    std::shared_ptr<const FeatureLayer::Block> block;
};
thread_local LastBlock last;
} // namespace

FeatureLayer::FeatureLayer(std::shared_ptr<const FeaturePlanner> planner)
    : planner_(std::move(planner)), movesGround_(planner_ && planner_->catalogue().movesGround()),
      generation_(nextGeneration.fetch_add(1)) {
    if (planner_)
        for (const auto& r : planner_->catalogue().recipes())
            for (const auto& op : r.terrain)
                anyWater_ = anyWater_ || (op.kind == TerrainOpKind::CarveProfile && op.waterWidth > 0);
}

std::shared_ptr<const FeatureLayer::Block> FeatureLayer::block(std::int64_t bx, std::int64_t by) const {
    const auto key = std::make_pair(bx, by);
    {
        std::lock_guard guard(lock_);
        if (auto it = blocks_.find(key); it != blocks_.end()) return it->second;
    }
    auto b = std::make_shared<Block>();
    b->x = bx;
    b->y = by;
    const core::WorldRect area{{core::Fixed::fromInt(bx * kBlockMetres), core::Fixed::fromInt(by * kBlockMetres)},
                               {core::Fixed::fromInt((bx + 1) * kBlockMetres), core::Fixed::fromInt((by + 1) * kBlockMetres)}};
    planner_->instancesIn(area, b->instances);
    // Deterministic order whatever order the cells were filled in: the
    // combination rules are order-free, but a probe's list should not wander.
    std::sort(b->instances.begin(), b->instances.end(),
              [](const FeatureInstance& a, const FeatureInstance& c) { return a.id < c.id; });
    std::shared_ptr<const Block> made = std::move(b);
    std::lock_guard guard(lock_);
    if (auto it = blocks_.find(key); it != blocks_.end()) return it->second;
    blocks_.emplace(key, made);
    order_.push_back(key);
    if (order_.size() > kBlocksKept) {
        blocks_.erase(order_.front());
        order_.erase(order_.begin());
    }
    return made;
}

std::shared_ptr<const FeatureLayer::Block> FeatureLayer::blockAt(core::WorldPos p) const {
    const auto bx = blockOf(p.x), by = blockOf(p.y);
    if (last.owner == this && last.generation == generation_ && last.x == bx && last.y == by && last.block)
        return last.block;
    last.block = block(bx, by);
    last.owner = this;
    last.generation = generation_;
    last.x = bx;
    last.y = by;
    return last.block;
}

core::Fixed FeatureLayer::at(core::WorldPos p, core::Fixed base, std::int64_t strideMetres) const {
    if (!movesGround_) return core::kZero;
    const auto b = blockAt(p);
    if (b->instances.empty()) return core::kZero;
    const auto& cat = planner_->catalogue();
    OpTotals totals;
    for (const auto& in : b->instances) {
        if (!in.bounds.contains(p)) continue;
        applyOps(cat.ops(in.recipe), in, p, base, strideMetres, totals);
    }
    return totals.total();
}

core::Fixed FeatureLayer::waterDepth(core::WorldPos p) const {
    if (!anyWater_) return core::kZero;
    const auto b = blockAt(p);
    const auto& cat = planner_->catalogue();
    core::Fixed deepest = core::kZero;
    for (const auto& in : b->instances) {
        if (!in.bounds.contains(p) || in.spline.empty()) continue;
        for (const auto& op : cat.ops(in.recipe)) {
            if (op.kind != TerrainOpKind::CarveProfile || op.waterHalf.raw <= 0) continue;
            const auto half = op.waterHalf * in.scale;
            const auto n = in.spline.nearest(p);
            if (n.distance >= half) continue;
            // A shallow lens: a quarter metre in the middle at scale one,
            // thinning to nothing at the water's edge.
            const auto u = n.distance / half;
            const auto taper = core::min(fixedSmoothstep(core::kZero, op.taper, n.along),
                                         fixedSmoothstep(core::kZero, op.taper, in.spline.length - n.along));
            deepest = core::max(deepest, core::Fixed::ratio(1, 4) * in.scale * (core::kOne - u * u) * taper);
        }
    }
    return deepest;
}

void FeatureLayer::probe(core::WorldPos p, core::Fixed base, std::vector<Hit>& out,
                         std::shared_ptr<const Block>* keep) const {
    if (!planner_) return;
    const auto b = blockAt(p);
    if (keep) *keep = b;
    const auto& cat = planner_->catalogue();
    for (const auto& in : b->instances) {
        if (!in.bounds.contains(p)) continue;
        Hit hit;
        hit.instance = &in;
        OpTotals ignore;
        applyOps(cat.ops(in.recipe), in, p, base, 1, ignore, &hit.geometry);
        if (!hit.geometry.any) {
            // A feature with no ground operation still has a place: the
            // distance to its anchor or line is what its masks are drawn by.
            if (!in.spline.empty()) {
                const auto n = in.spline.nearest(p);
                hit.geometry.distance = float(n.distance.toDouble());
                hit.geometry.side = float(n.side.toDouble());
                hit.geometry.along = float(n.along.toDouble());
            } else {
                hit.geometry.distance = float(core::hypot(p.x - in.anchor.x, p.y - in.anchor.y).toDouble());
            }
        }
        out.push_back(hit);
    }
}

} // namespace engine::environment
