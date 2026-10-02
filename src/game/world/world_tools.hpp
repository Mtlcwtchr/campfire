#pragma once
// What the edit tools do to a world, as steps that can be taken back.
//
// Every change goes through the world's delta (world_delta.hpp) - the one API
// the editor and the game share - and is recorded here as well, as its
// inverse: a stroke of a terrain brush is the height it added to each sample,
// so undoing it adds the negation; a removal of generated objects is undone by
// restoring them, a planting by taking the planted objects away. Undo is a
// new edit, not a rewind: the history on disk keeps both, which is what a
// history is, and the ground comes back to exactly what it was because the
// heights are fixed point and a sum and its negation cancel.
//
// A step is whatever happens between a press and a release: a stroke held for
// three seconds is one step, however many frames it painted.
//
// No input, no drawing, no camera: the client decides where the pointer is and
// hands this the ground and the objects under it.
#include <cstdint>
#include <map>
#include <tuple>
#include <string>
#include <utility>
#include <vector>

#include "game/world/scene_scatter.hpp"
#include "game/world/terrain_brush.hpp"
#include "game/world/world_delta.hpp"

namespace world::tools {

// Kinds of object, as a mask over decor::kModels, for the object brushes.
inline constexpr std::uint32_t kTrees = (1u << 0) | (1u << 1);
inline constexpr std::uint32_t kBushes = 1u << 2;
inline constexpr std::uint32_t kRocks = 1u << 3;
inline constexpr std::uint32_t kSmall = (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);   // mushrooms, deadwood
inline constexpr std::uint32_t kAllObjects = kTrees | kBushes | kRocks | kSmall;

// What a terrain brush is called and what it does, for the interface.
const char* brushName(BrushKind kind);
const char* brushHint(BrushKind kind);
// What an object model is called, for the interface ("Oak", "Pine" ...).
const char* modelName(std::uint32_t model);

class Editing {
public:
    explicit Editing(delta::WorldDelta& delta, std::size_t depth = 64) : delta_(delta), depth_(depth) {}

    // A step: everything between these two calls is undone at once.
    void begin(std::string label);
    void end();
    bool stroking() const { return open_; }

    // One application of a terrain brush for `seconds` of stroke, onto
    // `ground` (which must include what the delta already holds). Returns how
    // many samples it changed.
    std::size_t sculpt(const Brush& brush, const GroundAt& ground, core::WorldPos at, double seconds);
    // Every object of the kinds in `models` whose foot is within `radius` of
    // (x, y), out of `present` - what stands there now (the scatter over the
    // delta). Returns how many went.
    std::size_t clear(const std::vector<decor::Object>& present, double x, double y, double radius,
                      std::uint32_t models);
    // One object, where it says. Returns its id, nought if it was refused.
    std::uint64_t plant(const ecology::Added& object);

    bool undo();
    bool redo();
    std::size_t undoable() const { return done_.size(); }
    std::size_t redoable() const { return undone_.size(); }
    // What the next undo / redo would take back or put back, for the interface.
    std::string undoLabel() const { return done_.empty() ? std::string() : done_.back().label; }
    std::string redoLabel() const { return undone_.empty() ? std::string() : undone_.back().label; }
    // Nothing recorded - after a world is reloaded, say.
    void forget() { done_.clear(); undone_.clear(); open_ = false; }

private:
    struct Removed { std::uint64_t id; double x, y; };
    // A sample of the edit layer: its level and where, in that level's steps.
    using HeightAt = std::tuple<int, std::int64_t, std::int64_t>;
    struct Step {
        std::string label;
        BrushKind tool = BrushKind::Raise;
        std::map<HeightAt, core::Fixed> heights;                             // added, per sample
        std::vector<Removed> removed;                                        // generated, now gone
        std::vector<std::pair<std::uint64_t, ecology::Added>> unplanted;     // planted, now gone
        std::vector<std::pair<std::uint64_t, ecology::Added>> planted;       // planted by this step
        bool empty() const { return heights.empty() && removed.empty() && unplanted.empty() && planted.empty(); }
    };
    // Applies a step forwards (redo) or backwards (undo). Ids of objects it
    // plants again are new, and written back into the step.
    void play(Step& step, bool forwards);
    void addHeights(const std::map<HeightAt, core::Fixed>& heights, BrushKind tool, bool negate);
    Step& current();

    delta::WorldDelta& delta_;
    std::size_t depth_;
    std::vector<Step> done_, undone_;
    Step pending_;
    bool open_ = false;
};

// The objects standing in a square around a point, as the scatter places them
// over the delta: what `Editing::clear` chooses from. `bounds` is aligned to
// the scatter's cells by the caller's convenience.
decor::ScatterBounds boundsAround(double x, double y, double radius);

} // namespace world::tools

