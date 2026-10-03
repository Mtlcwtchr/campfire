#pragma once
// What a feature does to the ground (doc/plan_procedural_environment_2026-10-03.md, part C).
//
// These numbers move H_sim - the ground the navmesh and the simulation walk
// on - so they are fixed point from the moment they leave the recipe: a
// recipe's metres are read once into Fixed (content may be read from double),
// and every evaluation after that is integer arithmetic. Each operation is a
// pure function of (instance, point, ground under it): two pages asking about
// one point get one answer.
//
// Overlapping features combine by kind rather than by sum: two gullies that
// cross are as deep as the deeper of them, not twice as deep; two mounds are
// as high as the higher. Steps, terraces and flattening add.
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "engine/environment/recipe.hpp"

namespace engine::environment {

using core::Fixed;
using core::WorldPos;

// A polyline in world metres, fixed point, with its arc length.
struct FixedSpline {
    std::vector<WorldPos> points;
    std::vector<Fixed> cumulative;   // arc length at each point
    Fixed length{};

    void finish();
    [[nodiscard]] bool empty() const { return points.size() < 2; }

    struct Nearest {
        Fixed distance{};    // unsigned, metres
        Fixed side{};        // signed perpendicular distance: + left of travel
        Fixed along{};       // arc length of the nearest point
        Fixed curvature{};   // signed turning per metre there: + turning left
        WorldPos tangent{};  // unit, direction of travel
        WorldPos at{};
    };
    [[nodiscard]] Nearest nearest(WorldPos p) const;
    [[nodiscard]] WorldPos pointAt(Fixed along) const;
    [[nodiscard]] core::WorldRect bounds(Fixed margin) const;
};

// One placed feature. Made by the planner (planner.hpp), read by everything
// that dresses it.
struct FeatureInstance {
    std::uint64_t id = 0;            // stable: recipe and planning cell and index
    std::uint32_t recipe = 0;        // index into the catalogue's recipes
    std::uint64_t seed = 0;
    WorldPos anchor{};
    Fixed anchorHeight{};            // the ground under the anchor before any feature
    Fixed yawCos = core::kOne, yawSin{};
    Fixed scale = core::kOne;
    FixedSpline spline;              // empty for point features
    FeatureAge age = FeatureAge::Ancient;
    ChannelClass channel = ChannelClass::Paleochannel;
    core::WorldRect bounds{};        // everything the instance touches
    bool secondary = false;          // placed round another as part of its composition
    std::int8_t hand = 1;            // +1 / -1: which way an asymmetric form leans

    [[nodiscard]] double yaw() const;
};

// A recipe operation read into fixed point, once.
struct CompiledOp {
    TerrainOpKind kind{};
    Fixed outerHalf, outerDepth, innerHalf, innerDepth, waterHalf, asymmetry, taper;
    Fixed height, halfWidth, breaks, amphitheatre;
    Fixed radiusA, radiusB, edgeNoise, blend;
    std::int32_t exponentWhole = 2;
    Fixed exponentPart{};
    Fixed terraceStep, terraceSharpness;
    bool harden = false;
};
std::vector<CompiledOp> compileOps(const FeatureRecipe& recipe);

// The part of a point's answer the masks and the dressing read: where the
// point lies with respect to the feature's shapes. Metres, float: nothing
// walks on these.
struct OpGeometry {
    float footprint = 0;        // 0..1, inside the operation's area
    float distance = 1e9f;      // to the spline or the anchor's ellipse centre line
    float side = 0;             // signed, + left of the spline
    float along = 0;            // arc length
    float bedHalf = 0, bankHalf = 0, waterHalf = 0;   // carve: the three half widths there
    float rock = 0;             // 0..1, hardened ground
    bool any = false;
};

// The ground's change at `p` from one instance's operations, given the ground
// there before any feature. `stride` is how far apart the samples being
// worked out are: an operation narrower than that is left out, as the
// height field leaves out valleys narrower than a sample.
struct OpTotals {
    Fixed lift{}, cut{}, add{};
    [[nodiscard]] Fixed total() const { return lift + cut + add; }
};
void applyOps(std::span<const CompiledOp> ops, const FeatureInstance& instance, WorldPos p, Fixed base,
              std::int64_t strideMetres, OpTotals& totals, OpGeometry* geometry = nullptr);

// Fixed-point value noise, 0..1, one feature per `cellMetres`.
Fixed fixedNoise(std::uint64_t seed, Fixed x, Fixed y, std::int64_t cellMetres);
Fixed fixedSmoothstep(Fixed edge0, Fixed edge1, Fixed x);

} // namespace engine::environment
