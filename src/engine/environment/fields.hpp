#pragma once
// The continuous fields an environment is read from
// (doc/plan_procedural_environment_2026-10-03.md, part A).
//
// The engine does not know what a world's climate, soil or ecology are made
// of - the game does. What the engine needs is to ask "how wet, how steep, how
// deep is the soil here" by name, and to work out the shape of the ground
// (slope, curvature, convergence, topographic position, exposure) itself from
// a height function, because that arithmetic is the same for every world.
//
// A sample is a flat array of floats indexed by FieldId. The registry of names
// is process-wide and append-only: the engine's own names come first, a game
// may add more, and an id never changes meaning while the process runs.
//
// Presentation numbers, in float. Nothing here decides where a person walks;
// the terrain operations that do are in terrain_ops.hpp, in fixed point.
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace engine::environment {

using FieldId = std::uint8_t;
inline constexpr std::size_t kMaxFields = 32;

// The engine's own fields, in registry order. Names are what content says.
namespace field {
inline constexpr FieldId Elevation = 0;    // metres above sea
inline constexpr FieldId Slope = 1;        // rise over run
inline constexpr FieldId Aspect = 2;       // radians, direction the ground faces, 0 = +x
inline constexpr FieldId Curvature = 3;    // profile curvature, 1/m; + convex (ridge), - concave (hollow)
inline constexpr FieldId PlanCurvature = 4;// plan curvature, 1/m; - converging (gully), + diverging (spur)
inline constexpr FieldId Convergence = 5;  // 0..1, how much the ground around drains towards here
inline constexpr FieldId TpiSmall = 6;     // metres above the mean of a 32 m ring
inline constexpr FieldId TpiLarge = 7;     // metres above the mean of a 128 m ring
inline constexpr FieldId Exposure = 8;     // 0..1, facing the configured wind / sun vector
inline constexpr FieldId Wetness = 9;      // 0..1, standing moisture of the ground
inline constexpr FieldId Moisture = 10;    // 0..1, climate moisture
inline constexpr FieldId Fertility = 11;   // 0..1
inline constexpr FieldId SoilDepth = 12;   // 0..1
inline constexpr FieldId Drainage = 13;    // 0..1, 1 drains freely
inline constexpr FieldId Temperature = 14; // degrees C
inline constexpr FieldId Canopy = 15;      // 0..1
inline constexpr FieldId Rockiness = 16;   // 0..1
inline constexpr FieldId Disturbance = 17; // 0..1
inline constexpr FieldId FlowAccum = 18;   // 0..1, log-scaled upstream area
inline constexpr FieldId DistWater = 19;   // metres to the nearest standing or flowing water
inline constexpr FieldId Age = 20;         // 0..1, how long the ground has been left alone
inline constexpr FieldId Sand = 21;        // 0..1, loose sand share of the ground
inline constexpr FieldId Snow = 22;        // 0..1
inline constexpr FieldId Water = 23;       // 0..1, under water here
inline constexpr std::size_t kBuiltIn = 24;
} // namespace field

// The name of a field, or empty for an id nobody registered.
std::string_view fieldName(FieldId id);
// The id of a name, registering it if `add` and it is new. Nothing when the
// name is unknown and not added, or when the registry is full.
std::optional<FieldId> fieldId(std::string_view name, bool add = false);
std::size_t fieldCount();

struct FieldSample {
    std::array<float, kMaxFields> value{};
    std::uint32_t known = 0;   // bit per field that the source answered

    [[nodiscard]] float operator[](FieldId id) const { return value[id]; }
    [[nodiscard]] bool has(FieldId id) const { return (known >> id) & 1u; }
    void set(FieldId id, float v) { value[id] = v; known |= 1u << id; }
    // The value, or `fallback` when no source answered for it.
    [[nodiscard]] float get(FieldId id, float fallback) const { return has(id) ? value[id] : fallback; }
};

// Where the fields come from. The game implements one over its own world.
//
// `sampleGrid` fills `out` row-major, `columns` x `rows` samples `step` metres
// apart starting at (x0, y0). A page asks for hundreds of points that share
// their coarse lookups, so a source that can answer a grid at once should.
class FieldSource {
public:
    virtual ~FieldSource() = default;
    virtual void sample(double x, double y, FieldSample& out) const = 0;
    virtual void sampleGrid(double x0, double y0, double step, int columns, int rows,
                            std::span<FieldSample> out) const;
};

// The height of the ground, metres, anywhere.
using HeightAt = std::function<double(double x, double y)>;

// Settings for the shape fields worked out from height.
struct ShapeSettings {
    double step = 4.0;              // metres between the samples the derivatives are taken over
    double smallRing = 32.0;        // TpiSmall radius
    double largeRing = 128.0;       // TpiLarge radius
    double exposureX = -0.857, exposureY = 0.516;   // unit vector the exposure faces into
};

// Slope, aspect, both curvatures, convergence, both TPIs and exposure, worked
// out from the height function and written into `out` (Elevation too).
void deriveShape(const HeightAt& height, double x, double y, const ShapeSettings& s, FieldSample& out);

// A FieldSource that is the game's source with the shape fields filled in
// from height wherever the game left them out.
class ShapedSource final : public FieldSource {
public:
    ShapedSource(const FieldSource* base, HeightAt height, ShapeSettings settings = {});
    void sample(double x, double y, FieldSample& out) const override;
private:
    const FieldSource* base_;
    HeightAt height_;
    ShapeSettings settings_;
};

} // namespace engine::environment
