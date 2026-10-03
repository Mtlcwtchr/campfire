#include "engine/environment/feature_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "engine/environment/random.hpp"

namespace engine::environment {

namespace {

float box(float x, float y, float z, float hx, float hy, float hz) {
    const float qx = std::abs(x) - hx, qy = std::abs(y) - hy, qz = std::abs(z) - hz;
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f), oz = std::max(qz, 0.0f);
    return std::sqrt(ox * ox + oy * oy + oz * oz) + std::min(std::max(qx, std::max(qy, qz)), 0.0f);
}
float noise3(std::uint64_t seed, float x, float y, float z) {
    // Two value-noise slices blended by z: rough, cheap, deterministic.
    const float fz = std::floor(z);
    const float t = z - fz;
    const auto a = float(valueNoise(seed + std::uint64_t(std::int64_t(fz)), x, y));
    const auto b = float(valueNoise(seed + std::uint64_t(std::int64_t(fz) + 1), x, y));
    return a + (b - a) * (t * t * (3 - 2 * t));
}

} // namespace

Sdf formField(ProceduralForm form, const FormSize& s, std::uint64_t seed) {
    const float L = s.length, D = s.depth, H = s.height, R = s.roughness;
    const auto rough = [seed, R, D](float x, float y, float z) {
        const float k = std::max(0.5f, D * 0.6f);
        return R * k * (noise3(seed, x / k, y / k, z / k) - 0.5f) * 2 +
               R * k * 0.4f * (noise3(seed + 7, x / (k * 0.35f), y / (k * 0.35f), z / (k * 0.35f)) - 0.5f) * 2;
    };
    switch (form) {
        case ProceduralForm::Shelf:
            // A ledge standing out of the slope, thinning towards its lip.
            return [=](float x, float y, float z) {
                const float lip = 1 - std::clamp(-y / D, 0.0f, 1.0f) * 0.5f;
                return box(x, y + D * 0.5f, z - H * 0.5f, L * 0.5f, D * 0.5f, H * 0.5f * lip) + rough(x, y, z);
            };
        case ProceduralForm::Overhang:
            // A lip of rock leaning out over the low side: a slab whose top
            // reaches further out than its foot.
            return [=](float x, float y, float z) {
                const float lean = std::clamp(z / H, 0.0f, 1.0f) * D;
                return box(x, y + lean * 0.5f, z - H * 0.5f, L * 0.5f, D * 0.5f + lean * 0.5f, H * 0.5f) + rough(x, y, z);
            };
        case ProceduralForm::Undercut:
            // A bank with the water's notch cut under it.
            return [=](float x, float y, float z) {
                const float bank = box(x, y - D * 0.25f, z - H * 0.5f, L * 0.5f, D * 0.75f, H * 0.5f);
                const float notch = box(x, y + D * 0.3f, z - H * 0.2f, L * 0.6f, D * 0.6f, H * 0.22f);
                return std::max(bank, -notch) + rough(x, y, z) * 0.6f;
            };
        case ProceduralForm::RootPlate:
            // The upturned disc of earth and roots at the foot of a fallen trunk.
            return [=](float x, float y, float z) {
                const float r = std::hypot(y, z - H * 0.5f);
                const float disc = std::max(r - H * 0.5f, std::abs(x) - D * 0.25f);
                return disc + rough(x, y, z) * 1.4f;
            };
        case ProceduralForm::Spire:
            // A leaning needle, wide at the foot.
            return [=](float x, float y, float z) {
                const float t = std::clamp(z / H, 0.0f, 1.0f);
                const float lean = t * t * D * 0.6f;
                const float radius = (L * 0.5f) * (1 - t * 0.85f);
                return std::max(std::hypot(x - lean, y) - radius, std::max(-z, z - H)) + rough(x, y, z);
            };
        case ProceduralForm::Arch:
            return [=](float x, float y, float z) {
                const float ring = std::abs(std::hypot(x, z) - L * 0.4f) - H * 0.15f;
                return std::max(ring, std::max(std::abs(y) - D * 0.5f, -z)) + rough(x, y, z);
            };
        case ProceduralForm::None: break;
    }
    return [](float, float, float) { return 1.0f; };
}

TriangleMesh meshField(const Sdf& f, std::array<float, 3> lo, std::array<float, 3> hi, float cell) {
    TriangleMesh mesh;
    const int nx = std::max(2, int(std::ceil((hi[0] - lo[0]) / cell)) + 1);
    const int ny = std::max(2, int(std::ceil((hi[1] - lo[1]) / cell)) + 1);
    const int nz = std::max(2, int(std::ceil((hi[2] - lo[2]) / cell)) + 1);
    std::vector<float> d(std::size_t(nx) * ny * nz);
    const auto at = [&](int x, int y, int z) -> float& { return d[(std::size_t(z) * ny + y) * nx + x]; };
    for (int z = 0; z < nz; ++z)
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                at(x, y, z) = f(lo[0] + x * cell, lo[1] + y * cell, lo[2] + z * cell);
    // One vertex per cell the surface crosses, at the mean of its edge crossings.
    std::vector<std::int32_t> vertexOf(std::size_t(nx - 1) * (ny - 1) * (nz - 1), -1);
    const auto cellIndex = [&](int x, int y, int z) { return (std::size_t(z) * (ny - 1) + y) * (nx - 1) + x; };
    static constexpr int kEdges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int z = 0; z + 1 < nz; ++z)
        for (int y = 0; y + 1 < ny; ++y)
            for (int x = 0; x + 1 < nx; ++x) {
                float v[8];
                int inside = 0;
                for (int k = 0; k < 8; ++k) {
                    v[k] = at(x + (k & 1), y + ((k >> 1) & 1), z + ((k >> 2) & 1));
                    inside += v[k] < 0;
                }
                if (inside == 0 || inside == 8) continue;
                float px = 0, py = 0, pz = 0;
                int crossings = 0;
                for (const auto& e : kEdges) {
                    const float a = v[e[0]], b = v[e[1]];
                    if ((a < 0) == (b < 0)) continue;
                    const float t = a / (a - b);
                    const float ax = float(e[0] & 1), ay = float((e[0] >> 1) & 1), az = float((e[0] >> 2) & 1);
                    const float bx = float(e[1] & 1), by = float((e[1] >> 1) & 1), bz = float((e[1] >> 2) & 1);
                    px += ax + (bx - ax) * t;
                    py += ay + (by - ay) * t;
                    pz += az + (bz - az) * t;
                    ++crossings;
                }
                vertexOf[cellIndex(x, y, z)] = std::int32_t(mesh.positions.size());
                mesh.positions.push_back({lo[0] + (x + px / crossings) * cell, lo[1] + (y + py / crossings) * cell,
                                          lo[2] + (z + pz / crossings) * cell});
            }
    // A quad across every grid edge the surface crosses, wound by its sign.
    const auto quad = [&](std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t e, bool flip) {
        if (a < 0 || b < 0 || c < 0 || e < 0) return;
        if (flip) std::swap(b, e);
        mesh.indices.insert(mesh.indices.end(), {std::uint32_t(a), std::uint32_t(b), std::uint32_t(c),
                                                 std::uint32_t(a), std::uint32_t(c), std::uint32_t(e)});
    };
    for (int z = 1; z + 1 < nz; ++z)
        for (int y = 1; y + 1 < ny; ++y)
            for (int x = 0; x + 1 < nx; ++x) {
                const bool a = at(x, y, z) < 0, b = at(x + 1, y, z) < 0;
                if (a == b) continue;
                quad(vertexOf[cellIndex(x, y - 1, z - 1)], vertexOf[cellIndex(x, y, z - 1)],
                     vertexOf[cellIndex(x, y, z)], vertexOf[cellIndex(x, y - 1, z)], a);
            }
    for (int z = 1; z + 1 < nz; ++z)
        for (int y = 0; y + 1 < ny; ++y)
            for (int x = 1; x + 1 < nx; ++x) {
                const bool a = at(x, y, z) < 0, b = at(x, y + 1, z) < 0;
                if (a == b) continue;
                quad(vertexOf[cellIndex(x - 1, y, z - 1)], vertexOf[cellIndex(x - 1, y, z)],
                     vertexOf[cellIndex(x, y, z)], vertexOf[cellIndex(x, y, z - 1)], a);
            }
    for (int z = 0; z + 1 < nz; ++z)
        for (int y = 1; y + 1 < ny; ++y)
            for (int x = 1; x + 1 < nx; ++x) {
                const bool a = at(x, y, z) < 0, b = at(x, y, z + 1) < 0;
                if (a == b) continue;
                quad(vertexOf[cellIndex(x - 1, y - 1, z)], vertexOf[cellIndex(x, y - 1, z)],
                     vertexOf[cellIndex(x, y, z)], vertexOf[cellIndex(x - 1, y, z)], a);
            }
    // Normals from the field's gradient: smooth across the faces.
    mesh.normals.resize(mesh.positions.size());
    const float h = cell * 0.5f;
    mesh.min = {1e30f, 1e30f, 1e30f};
    mesh.max = {-1e30f, -1e30f, -1e30f};
    for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
        const auto& p = mesh.positions[i];
        float gx = f(p[0] + h, p[1], p[2]) - f(p[0] - h, p[1], p[2]);
        float gy = f(p[0], p[1] + h, p[2]) - f(p[0], p[1] - h, p[2]);
        float gz = f(p[0], p[1], p[2] + h) - f(p[0], p[1], p[2] - h);
        const float len = std::sqrt(gx * gx + gy * gy + gz * gz);
        if (len > 1e-9f) { gx /= len; gy /= len; gz /= len; } else { gz = 1; }
        mesh.normals[i] = {gx, gy, gz};
        for (int k = 0; k < 3; ++k) { mesh.min[k] = std::min(mesh.min[k], p[k]); mesh.max[k] = std::max(mesh.max[k], p[k]); }
    }
    return mesh;
}

TriangleMesh formMesh(const MeshRule& rule, std::uint64_t seed, float cell) {
    if (rule.form == ProceduralForm::None) return {};
    FormSize size{float(rule.length), float(rule.depth), float(rule.height), float(rule.roughness)};
    const auto field = formField(rule.form, size, seed);
    const float pad = std::max(1.0f, size.depth * 0.6f) + cell * 2;
    const float reach = std::max({size.length, size.depth, size.height}) * 0.6f + pad;
    // The form sits on z = 0 and may reach a little below to meet the ground.
    return meshField(field, {-size.length * 0.5f - pad, -reach, -cell * 2}, {size.length * 0.5f + pad, reach, size.height + pad},
                     cell);
}

void placeMeshes(const Catalogue& catalogue, const FeatureInstance& in, const GroundAt& ground,
                 std::vector<MeshPlacement>& out) {
    const auto& recipe = catalogue.recipes()[in.recipe];
    const double s = in.scale.toDouble();
    for (std::uint16_t ri = 0; ri < recipe.meshes.size(); ++ri) {
        const auto& rule = recipe.meshes[ri];
        if (rule.model.name.empty() && rule.form == ProceduralForm::None) continue;
        if (!rule.model.name.empty() && rule.model.id == 0 && rule.form == ProceduralForm::None) continue;
        Rng rng(hashOf(in.seed, ri, 0, 0x3e5));
        const auto put = [&](double x, double y, double yaw, int k) {
            MeshPlacement m;
            m.seed = hashOf(in.seed, ri, k, 0x3e6);
            m.recipe = in.recipe;
            m.rule = ri;
            m.model = rule.form == ProceduralForm::None ? rule.model.id : 0xffffffffu;
            m.x = x; m.y = y;
            m.z = ground ? ground(x, y) : in.anchorHeight.toDouble();
            m.yaw = float(yaw);
            m.scale = float(s * rng.range(rule.scaleMin, rule.scaleMax));
            m.sink = float(rule.sink);
            m.alignToNormal = rule.alignToNormal;
            out.push_back(m);
        };
        if (rule.attach == MeshAttach::Anchor || in.spline.empty()) {
            const double yaw = in.yaw();
            for (int k = 0; k < std::max(1, rule.count); ++k) {
                const double off = rule.offset * s + (k ? rng.range(-1, 1) * rule.spacing * s : 0.0);
                put(in.anchor.x.toDouble() - std::sin(yaw) * off, in.anchor.y.toDouble() + std::cos(yaw) * off, yaw, k);
            }
            continue;
        }
        // Along the spline, at its brow (Edge) or on its line, facing across it.
        const double length = in.spline.length.toDouble();
        const double spacing = std::max(1.0, rule.spacing * s);
        const int n = std::min(rule.count > 0 ? rule.count : 1 << 20, int(length / spacing) + 1);
        double brow = 0;
        if (rule.attach == MeshAttach::Edge)
            for (const auto& op : recipe.terrain)
                if (op.kind == TerrainOpKind::Step || op.kind == TerrainOpKind::CarveProfile)
                    brow = std::max(brow, (op.kind == TerrainOpKind::Step ? op.width : op.outerWidth) * 0.5 * s);
        for (int k = 0; k < n; ++k) {
            const double along = std::min(length, (k + 0.5) * length / n + rng.range(-0.25, 0.25) * spacing);
            const auto at = in.spline.pointAt(quantised(along));
            const auto ahead = in.spline.pointAt(quantised(std::min(length, along + 1)));
            const auto behind = in.spline.pointAt(quantised(std::max(0.0, along - 1)));
            const double tx = (ahead.x - behind.x).toDouble(), ty = (ahead.y - behind.y).toDouble();
            const double tl = std::max(1e-9, std::hypot(tx, ty));
            const double side = brow + rule.offset * s;
            put(at.x.toDouble() - ty / tl * side, at.y.toDouble() + tx / tl * side, std::atan2(ty, tx), k);
        }
    }
}

} // namespace engine::environment
