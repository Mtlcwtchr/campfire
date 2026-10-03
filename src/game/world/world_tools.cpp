#include "game/world/world_tools.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace world::tools {
using delta::Op;
using delta::Origin;

const char* brushName(BrushKind kind) {
    switch (kind) {
        case BrushKind::Raise: return "Raise";
        case BrushKind::Lower: return "Lower";
        case BrushKind::Smooth: return "Smooth";
        case BrushKind::Flatten: return "Flatten";
        case BrushKind::Noise: return "Roughen";
        case BrushKind::Thermal: return "Scree";
        case BrushKind::Hydraulic: return "Erode";
        case BrushKind::Carve: return "Channel";
        case BrushKind::Restore: return "Restore";
        case BrushKind::Count: break;
    }
    return "Brush";
}

const char* brushHint(BrushKind kind) {
    switch (kind) {
        case BrushKind::Raise: return "Lifts the ground under the brush while the button is held.";
        case BrushKind::Lower: return "Sinks the ground under the brush while the button is held.";
        case BrushKind::Smooth: return "Pulls the ground towards the average around it.";
        case BrushKind::Flatten: return "Levels the ground to the height under the brush's centre.";
        case BrushKind::Noise: return "Stamps ridged noise: broken ground, not fuzz. Scale sets its size.";
        case BrushKind::Thermal: return "Slumps slopes steeper than loose rock can stand.";
        case BrushKind::Hydraulic: return "Water's work: wears the shoulders, fills the hollows.";
        case BrushKind::Carve: return "Cuts a rounded channel. Scale sets its width; drag along the course.";
        case BrushKind::Restore: return "Takes hand edits back to the generated ground, a little at a time.";
        case BrushKind::Count: break;
    }
    return "";
}

const char* modelName(std::uint32_t model) {
    static constexpr const char* names[] = {"Broadleaf tree", "Pine", "Bush", "Rock",
                                            "Mushrooms", "Fallen log", "Stump", "Branch", "Grey cliff", "Warm cliff",
                                            "Moss cushion", "Fern", "Dense shrub", "Low shrub"};
    return model < std::size(names) ? names[model] : "Object";
}

void Editing::begin(std::string label) {
    if (open_) end();
    pending_ = Step{};
    pending_.label = std::move(label);
    open_ = true;
}

void Editing::end() {
    if (!open_) return;
    open_ = false;
    if (pending_.empty()) return;
    done_.push_back(std::move(pending_));
    pending_ = Step{};
    undone_.clear();
    if (done_.size() > depth_) done_.erase(done_.begin());
}

Editing::Step& Editing::current() { return pending_; }

std::size_t Editing::sculpt(const Brush& brush, const GroundAt& ground, core::WorldPos at, double seconds) {
    // Resolved on a layer of its own, then recorded sample by sample: the
    // same thing WorldDelta::brush does, kept here so the step knows exactly
    // what it added.
    EditLayer scratch;
    if (!applyBrush(scratch, ground, brush, at, seconds, delta_.heights().get())) return 0;
    delta::TerrainOp op;
    op.tool = brush.kind;
    op.centreX = at.x.toDouble();
    op.centreY = at.y.toDouble();
    op.radius = brush.radiusMetres;
    op.strength = brush.strength;
    op.seconds = seconds;
    // At the level the brush's size asks for (EditLayer::levelFor): a stroke
    // across a country is a few thousand samples, not millions.
    op.level = std::uint8_t(EditLayer::levelFor(brush.radiusMetres));
    const std::int64_t side = EditLayer::blockSamplesOf(op.level);
    for (const auto& [bx, by] : scratch.blockKeys(op.level)) {
        std::vector<EditLayer::BlockCopy> copy;
        scratch.copyBlocks(op.level, bx, by, bx, by, copy);
        for (const auto& block : copy)
            for (std::int64_t i = 0; i < std::int64_t(block.delta.size()); ++i)
                if (block.delta[std::size_t(i)] != core::kZero)
                    op.samples.push_back({block.x * side + i % side, block.y * side + i / side,
                                          block.delta[std::size_t(i)]});
    }
    if (op.samples.empty()) return 0;
    const bool implicit = !open_;
    if (implicit) begin(brushName(brush.kind));
    auto& step = current();
    step.tool = brush.kind;
    for (const auto& s : op.samples) step.heights[{int(op.level), s.x, s.y}] += s.add;
    const std::size_t count = op.samples.size();
    delta_.apply(Op{0, Origin::Authoring, std::move(op)});
    if (implicit) end();
    return count;
}

std::size_t Editing::clear(const std::vector<decor::Object>& present, double x, double y, double radius,
                           std::uint32_t models) {
    const bool implicit = !open_;
    if (implicit) begin("Clear objects");
    auto& step = current();
    const auto objects = delta_.objects()->read();
    std::size_t gone = 0;
    for (const auto& o : present) {
        if (o.model >= 32 || ((models >> o.model) & 1u) == 0) continue;
        if (std::hypot(o.x - x, o.y - y) > radius) continue;
        const auto page = objects->added.find(ecology::page(o.x, o.y));
        const bool planted = page != objects->added.end() && page->second.contains(o.id);
        if (planted) {
            const ecology::Added what = page->second.at(o.id);
            if (!delta_.removeObject(o.id, o.x, o.y, Origin::Authoring)) continue;
            // Planted by this very step and taken away again: nothing happened.
            const auto mine = std::find_if(step.planted.begin(), step.planted.end(),
                                           [&](const auto& p) { return p.first == o.id; });
            if (mine != step.planted.end()) step.planted.erase(mine);
            else step.unplanted.emplace_back(o.id, what);
        } else {
            if (!delta_.removeObject(o.id, o.x, o.y, Origin::Authoring)) continue;
            step.removed.push_back({o.id, o.x, o.y});
        }
        ++gone;
    }
    if (implicit) end();
    return gone;
}

std::uint64_t Editing::plant(const ecology::Added& object) {
    const std::uint64_t id = delta_.plant(object, Origin::Authoring);
    if (!id) return 0;
    const bool implicit = !open_;
    if (implicit) begin("Plant");
    current().planted.emplace_back(id, object);
    if (implicit) end();
    return id;
}

void Editing::addHeights(const std::map<HeightAt, core::Fixed>& heights, BrushKind tool, bool negate) {
    if (heights.empty()) return;
    // One op a level: the map is ordered by level first.
    for (int level = 0; level < EditLayer::kLevels; ++level) {
        delta::TerrainOp op;
        op.tool = tool;
        op.level = std::uint8_t(level);
        for (auto it = heights.lower_bound({level, INT64_MIN, INT64_MIN});
             it != heights.end() && std::get<0>(it->first) == level; ++it)
            if (it->second != core::kZero)
                op.samples.push_back({std::get<1>(it->first), std::get<2>(it->first), negate ? -it->second : it->second});
        if (!op.samples.empty()) delta_.apply(Op{0, Origin::Authoring, std::move(op)});
    }
}

void Editing::play(Step& step, bool forwards) {
    if (forwards) {
        addHeights(step.heights, step.tool, false);
        for (const auto& r : step.removed) delta_.removeObject(r.id, r.x, r.y, Origin::Authoring);
        for (const auto& [id, what] : step.unplanted) delta_.removeObject(id, what.x, what.y, Origin::Authoring);
        for (auto& [id, what] : step.planted) id = delta_.plant(what, Origin::Authoring);
        return;
    }
    // Backwards, in the reverse of the order it was done in.
    for (auto& [id, what] : step.planted) delta_.removeObject(id, what.x, what.y, Origin::Authoring);
    for (auto& [id, what] : step.unplanted) id = delta_.plant(what, Origin::Authoring);
    for (const auto& r : step.removed) delta_.restoreObject(r.id, r.x, r.y, Origin::Authoring);
    addHeights(step.heights, step.tool, true);
}

bool Editing::undo() {
    end();
    if (done_.empty()) return false;
    Step step = std::move(done_.back());
    done_.pop_back();
    play(step, false);
    undone_.push_back(std::move(step));
    return true;
}

bool Editing::redo() {
    end();
    if (undone_.empty()) return false;
    Step step = std::move(undone_.back());
    undone_.pop_back();
    play(step, true);
    done_.push_back(std::move(step));
    return true;
}

decor::ScatterBounds boundsAround(double x, double y, double radius) {
    const double r = std::max(1.0, radius);
    const auto down = [](double v) { return std::int64_t(std::floor(v / decor::kCell)) * decor::kCell; };
    const auto up = [](double v) { return std::int64_t(std::ceil(v / decor::kCell)) * decor::kCell; };
    return {down(x - r), down(y - r), up(x + r), up(y + r)};
}

} // namespace world::tools
