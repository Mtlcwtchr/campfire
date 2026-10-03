#pragma once
// Feature meshes (doc/plan_procedural_environment_2026-10-03.md, part F).
//
// The heightfield carries everything that is one height per point. What is
// not - an overhanging cliff lip, a shelf, a bank the water has cut under, the
// root plate of a fallen tree, and for later the spires and arches - is a
// mesh, made here from a signed distance field and meshed by surface nets.
//
// A form is described in the feature's own frame: x along its line or yaw,
// y across (positive into the slope, away from the open side), z up from the
// ground at the anchor. The game turns the mesh into whatever its renderer
// draws (a runtime model) and stands it where placeMeshes says.
//
// The order a feature is built in (spec §5): deform the ground, add the mesh,
// override the material, push back or bring in the foliage, lay the talus and
// debris over the join, then the microdetail - so no rock is ever simply
// stuck into the ground. The masks a mesh rule's base needs are the recipe's
// own; this file only makes and places the shapes.
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/environment/catalogue.hpp"
#include "engine/environment/scatter.hpp"

namespace engine::environment {

struct TriangleMesh {
    std::vector<std::array<float, 3>> positions;
    std::vector<std::array<float, 3>> normals;
    std::vector<std::uint32_t> indices;
    std::array<float, 3> min{}, max{};
    [[nodiscard]] bool empty() const { return indices.empty(); }
};

// Metres, in the form's frame. Negative inside.
using Sdf = std::function<float(float x, float y, float z)>;

struct FormSize {
    float length = 12, depth = 3, height = 4, roughness = 0.3f;
};
// The distance field of a procedural form, roughened by seeded noise.
Sdf formField(ProceduralForm form, const FormSize& size, std::uint64_t seed);

// Surface nets over the box [min, max] at `cell` metres.
TriangleMesh meshField(const Sdf& field, std::array<float, 3> min, std::array<float, 3> max, float cell);

// The mesh of a recipe's mesh rule for one instance, or empty for a catalogue
// model (which the game draws as it draws any model). Pure: the same rule and
// seed make the same mesh, so it can be cached by (recipe, rule, seed).
TriangleMesh formMesh(const MeshRule& rule, std::uint64_t seed, float cell = 0.5f);

// Where a feature's meshes stand: one placement per mesh, spread along the
// spline or set at the anchor. `model` is the catalogue id for a catalogue
// model, or 0xffffffff for a procedural form (whose mesh formMesh makes with
// the placement's `seed`).
struct MeshPlacement {
    std::uint64_t seed = 0;
    std::uint32_t recipe = 0;
    std::uint16_t rule = 0;
    std::uint32_t model = 0xffffffffu;
    double x = 0, y = 0, z = 0;
    float yaw = 0, scale = 1, sink = 0;
    bool alignToNormal = false;
};
void placeMeshes(const Catalogue& catalogue, const FeatureInstance& instance, const GroundAt& ground,
                 std::vector<MeshPlacement>& out);

} // namespace engine::environment
