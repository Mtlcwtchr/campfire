// asr_mesh_lab - side by side: what Unreal's Nanite bake makes of a mesh, and
// what our cluster DAG ("smart mesh") makes of the same mesh.
//
// Nothing here opens a window or touches the GPU. The comparison is a picture
// and a table, both written to disk:
//
//     tools/ue_nanite_reference.py      (headless UE: source + Nanite DAG cuts)
//     ./cmake-build-relwithdebinfo/asr_mesh_lab assets/generated/ue_reference
//
// For every exported mesh and every Nanite relative error, the same ABSOLUTE
// error is handed to our DAG's cut (Nanite's unit converted with Nanite's own
// formula), and both results are measured against the source with one metric:
// a sampled two-way surface distance. Triangles at equal error is the number
// that decides how many trees a frame can afford; the distance says whether the
// error each bake claims is the error it delivers.
//
// Output (default assets/generated/mesh_lab):
//   <name>.png      rows = error, columns = source | UE Nanite | ours per profile
//   report.md       the table
//   report.json     the same, for tools
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/geometry/cluster_dag.hpp"

namespace fs = std::filesystem;
using engine::geometry::ClusterDag;
using engine::geometry::ClusterDagOptions;

namespace {

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V3 a) { return std::sqrt(dot(a, a)); }

// A triangle soup: what both bakes are reduced to for measuring and drawing.
// `group` colours a triangle: the cluster it came from, or 0 for none.
struct Mesh {
    std::vector<float> positions;   // metres, Z up, three per vertex
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> group;   // one per triangle
    // Exact non-position attributes per vertex (game meshes only): what keeps
    // a UV seam from being welded, exactly as scene_model_clusters builds it.
    std::vector<std::uint64_t> keys;
    std::vector<std::uint64_t> materials;
    std::size_t triangles() const { return indices.size() / 3; }
    V3 vertex(std::uint32_t i) const {
        return {positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2]};
    }
};

// .uem, as tools/ue_nanite_reference.py writes it: Unreal centimetres, X
// forward, Y right. Metres here, Y mirrored into a right-handed frame, and the
// winding flipped with it so front faces stay front.
bool loadUem(const fs::path& path, Mesh& mesh) {
    std::ifstream in(path, std::ios::binary);
    char magic[4];
    std::uint32_t vertices = 0, triangles = 0;
    if (!in.read(magic, 4) || std::string(magic, 4) != "UEM1") return false;
    in.read(reinterpret_cast<char*>(&vertices), 4);
    in.read(reinterpret_cast<char*>(&triangles), 4);
    mesh.positions.resize(std::size_t(vertices) * 3);
    mesh.indices.resize(std::size_t(triangles) * 3);
    in.read(reinterpret_cast<char*>(mesh.positions.data()), std::streamsize(mesh.positions.size() * 4));
    in.read(reinterpret_cast<char*>(mesh.indices.data()), std::streamsize(mesh.indices.size() * 4));
    if (!in) return false;
    for (std::size_t v = 0; v < vertices; ++v) {
        mesh.positions[v * 3] *= 0.01f;
        mesh.positions[v * 3 + 1] *= -0.01f;
        mesh.positions[v * 3 + 2] *= 0.01f;
    }
    for (std::size_t t = 0; t < triangles; ++t) std::swap(mesh.indices[t * 3 + 1], mesh.indices[t * 3 + 2]);
    for (const auto i : mesh.indices)
        if (i >= vertices) return false;
    mesh.group.assign(triangles, 0);
    return true;
}

// A game mesh (tools/prepare_scene_models.py, SCM2): position, normal, uv,
// colour, layer - twelve floats. The finest level's solid part, i.e. what
// buildClusterAsset hands the DAG: components of more than two triangles.
bool loadScm(const fs::path& path, Mesh& mesh) {
    constexpr std::size_t kFloats = 12;
    std::ifstream in(path, std::ios::binary);
    char magic[4];
    std::uint32_t vertices = 0, levels = 0;
    if (!in.read(magic, 4) || std::string(magic, 4) != "SCM2") return false;
    in.read(reinterpret_cast<char*>(&vertices), 4);
    in.read(reinterpret_cast<char*>(&levels), 4);
    if (!in || vertices == 0 || levels == 0 || levels > 8) return false;
    std::vector<std::uint32_t> counts(levels);
    for (auto& c : counts) in.read(reinterpret_cast<char*>(&c), 4);
    std::vector<float> all(std::size_t(vertices) * kFloats);
    in.read(reinterpret_cast<char*>(all.data()), std::streamsize(all.size() * 4));
    std::vector<std::uint32_t> finest(counts[0]);
    in.read(reinterpret_cast<char*>(finest.data()), std::streamsize(finest.size() * 4));
    if (!in) return false;
    mesh.positions.resize(std::size_t(vertices) * 3);
    mesh.keys.resize(vertices);
    mesh.materials.resize(vertices);
    for (std::uint32_t v = 0; v < vertices; ++v) {
        for (int a = 0; a < 3; ++a) mesh.positions[v * 3 + a] = all[std::size_t(v) * kFloats + a];
        std::uint32_t material = 0;
        std::memcpy(&material, &all[std::size_t(v) * kFloats + 11], 4);
        mesh.materials[v] = material;
        std::uint64_t key = 1469598103934665603ull;
        for (std::size_t f = 3; f < kFloats; ++f) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &all[std::size_t(v) * kFloats + f], 4);
            if (bits == 0x80000000u) bits = 0;
            key = (key ^ bits) * 1099511628211ull;
        }
        mesh.keys[v] = key;
    }
    const auto welded = engine::geometry::weldPositions(mesh.positions);
    std::vector<std::uint32_t> parent(vertices);
    for (std::uint32_t v = 0; v < vertices; ++v) parent[v] = v;
    const auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    for (std::size_t t = 0; t + 2 < finest.size(); t += 3) {
        const auto a = find(welded[finest[t]]);
        parent[find(welded[finest[t + 1]])] = a;
        parent[find(welded[finest[t + 2]])] = a;
    }
    std::map<std::uint32_t, std::size_t> size;
    for (std::size_t t = 0; t + 2 < finest.size(); t += 3) ++size[find(welded[finest[t]])];
    for (std::size_t t = 0; t + 2 < finest.size(); t += 3)
        if (size[find(welded[finest[t]])] > 2) mesh.indices.insert(mesh.indices.end(), &finest[t], &finest[t] + 3);
    mesh.group.assign(mesh.triangles(), 0);
    return !mesh.indices.empty();
}

struct Box {
    V3 low{1e30, 1e30, 1e30}, high{-1e30, -1e30, -1e30};
    void add(V3 p) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    V3 size() const { return high - low; }
    V3 centre() const { return (low + high) * 0.5; }
};

Box boundsOf(const Mesh& mesh) {
    Box box;
    for (const auto i : mesh.indices) box.add(mesh.vertex(i));
    return box;
}

double areaOf(const Mesh& mesh) {
    double area = 0;
    for (std::size_t t = 0; t < mesh.triangles(); ++t) {
        const auto a = mesh.vertex(mesh.indices[t * 3]), b = mesh.vertex(mesh.indices[t * 3 + 1]),
                   c = mesh.vertex(mesh.indices[t * 3 + 2]);
        area += 0.5 * length(cross(b - a, c - a));
    }
    return area;
}

// Nanite's relative error to an absolute one, exactly as NaniteBuilder.cpp
// states it: percent of sqrt(min(2 * surface area, bounds surface area)).
double absoluteError(double percent, const Mesh& source) {
    const auto s = boundsOf(source).size();
    const double boundsArea = 2 * (s.x * s.y + s.y * s.z + s.z * s.x);
    return percent * 0.01 * std::sqrt(std::min(2 * areaOf(source), boundsArea));
}

// --- measuring ------------------------------------------------------------

double pointTriangle(V3 p, V3 a, V3 b, V3 c) {
    // Ericson, Real-Time Collision Detection 5.1.5.
    const V3 ab = b - a, ac = c - a, ap = p - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return length(ap);
    const V3 bp = p - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return length(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return length(p - (a + ab * (d1 / (d1 - d3))));
    const V3 cp = p - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return length(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return length(p - (a + ac * (d2 / (d2 - d6))));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) {
        const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return length(p - (b + (c - b) * w));
    }
    const double denom = 1 / (va + vb + vc);
    return length(p - (a + ab * (vb * denom) + ac * (vc * denom)));
}

// Nearest-surface queries against one mesh through a uniform grid of triangle
// bounds. Rings grow until the ring's distance exceeds the best found, so the
// answer is the nearest triangle and not merely a near one.
class Nearest {
public:
    explicit Nearest(const Mesh& mesh) : mesh_(mesh) {
        box_ = boundsOf(mesh);
        const auto s = box_.size();
        const double volume = std::max(1e-9, (s.x + 1e-6) * (s.y + 1e-6) * (s.z + 1e-6));
        cell_ = std::cbrt(volume / std::max<double>(1, double(mesh.triangles()) / 4));
        cell_ = std::max(cell_, 1e-4);
        for (int k = 0; k < 3; ++k) {
            const double extent = k == 0 ? s.x : k == 1 ? s.y : s.z;
            dims_[k] = std::clamp(int(extent / cell_) + 1, 1, 512);
        }
        cells_.resize(std::size_t(dims_[0]) * dims_[1] * dims_[2]);
        for (std::uint32_t t = 0; t < mesh.triangles(); ++t) {
            Box b;
            for (int c = 0; c < 3; ++c) b.add(mesh.vertex(mesh.indices[t * 3 + c]));
            const auto lo = cellOf(b.low), hi = cellOf(b.high);
            for (int z = lo[2]; z <= hi[2]; ++z)
                for (int y = lo[1]; y <= hi[1]; ++y)
                    for (int x = lo[0]; x <= hi[0]; ++x) cells_[index(x, y, z)].push_back(t);
        }
    }

    double distance(V3 p) const {
        const auto c = cellOf(p);
        double best = 1e30;
        const int maxRing = std::max({dims_[0], dims_[1], dims_[2]});
        for (int ring = 0; ring <= maxRing; ++ring) {
            // Anything in ring r is at least (r - 1) cells away.
            if (ring > 0 && double(ring - 1) * cell_ > best) break;
            for (int z = c[2] - ring; z <= c[2] + ring; ++z)
                for (int y = c[1] - ring; y <= c[1] + ring; ++y)
                    for (int x = c[0] - ring; x <= c[0] + ring; ++x) {
                        if (std::max({std::abs(x - c[0]), std::abs(y - c[1]), std::abs(z - c[2])}) != ring)
                            continue;
                        if (x < 0 || y < 0 || z < 0 || x >= dims_[0] || y >= dims_[1] || z >= dims_[2])
                            continue;
                        for (const auto t : cells_[index(x, y, z)])
                            best = std::min(best, pointTriangle(p, mesh_.vertex(mesh_.indices[t * 3]),
                                                                mesh_.vertex(mesh_.indices[t * 3 + 1]),
                                                                mesh_.vertex(mesh_.indices[t * 3 + 2])));
                    }
        }
        return best;
    }

private:
    std::array<int, 3> cellOf(V3 p) const {
        return {std::clamp(int((p.x - box_.low.x) / cell_), 0, dims_[0] - 1),
                std::clamp(int((p.y - box_.low.y) / cell_), 0, dims_[1] - 1),
                std::clamp(int((p.z - box_.low.z) / cell_), 0, dims_[2] - 1)};
    }
    std::size_t index(int x, int y, int z) const {
        return (std::size_t(z) * dims_[1] + y) * dims_[0] + x;
    }
    const Mesh& mesh_;
    Box box_;
    double cell_ = 1;
    std::array<int, 3> dims_{1, 1, 1};
    std::vector<std::vector<std::uint32_t>> cells_;
};

// Points spread over a surface by area, from a fixed seed so two runs agree.
std::vector<V3> samplesOf(const Mesh& mesh, std::size_t count) {
    std::vector<double> cumulative(mesh.triangles());
    double total = 0;
    for (std::size_t t = 0; t < mesh.triangles(); ++t) {
        const auto a = mesh.vertex(mesh.indices[t * 3]), b = mesh.vertex(mesh.indices[t * 3 + 1]),
                   c = mesh.vertex(mesh.indices[t * 3 + 2]);
        total += 0.5 * length(cross(b - a, c - a));
        cumulative[t] = total;
    }
    std::vector<V3> out;
    if (!(total > 0)) return out;
    std::mt19937_64 rng(1234);
    std::uniform_real_distribution<double> unit(0, 1);
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto t = std::size_t(std::lower_bound(cumulative.begin(), cumulative.end(), unit(rng) * total) -
                                   cumulative.begin());
        const auto a = mesh.vertex(mesh.indices[std::min(t, mesh.triangles() - 1) * 3]);
        const auto b = mesh.vertex(mesh.indices[std::min(t, mesh.triangles() - 1) * 3 + 1]);
        const auto c = mesh.vertex(mesh.indices[std::min(t, mesh.triangles() - 1) * 3 + 2]);
        double u = unit(rng), v = unit(rng);
        if (u + v > 1) { u = 1 - u; v = 1 - v; }
        out.push_back(a + (b - a) * u + (c - a) * v);
    }
    return out;
}

struct Deviation {
    double mean = 0, p95 = 0, max = 0;
};

// Two-way: source points to the result catch holes and eroded silhouettes,
// result points to the source catch geometry that bulges or floats.
Deviation deviation(const std::vector<V3>& sourceSamples, const Nearest& source, const Mesh& result) {
    Deviation d;
    if (result.triangles() == 0) {
        d.mean = d.p95 = d.max = 1e30;
        return d;
    }
    const Nearest toResult(result);
    std::vector<double> all;
    all.reserve(sourceSamples.size() * 2);
    for (const auto& p : sourceSamples) all.push_back(toResult.distance(p));
    for (const auto& p : samplesOf(result, sourceSamples.size())) all.push_back(source.distance(p));
    double sum = 0;
    for (const auto v : all) sum += v;
    d.mean = sum / double(all.size());
    std::sort(all.begin(), all.end());
    d.p95 = all[std::size_t(double(all.size() - 1) * 0.95)];
    d.max = all.back();
    return d;
}

// --- our bake -------------------------------------------------------------

struct Profile {
    std::string name;
    ClusterDagOptions options;
};

// Count geometric, not attribute edges: flat-shaded faces have private
// indices even when they form a closed surface. Open rims in the source are
// legitimate; a closed source acquiring any open edge is not.
nlohmann::json topologyOf(std::span<const float> positions, std::span<const std::uint32_t> indices) {
    const auto welded = engine::geometry::weldPositions(positions);
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> uses;
    std::size_t degenerate = 0, open = 0, nonmanifold = 0;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::array<std::uint32_t, 3> t{welded[indices[i]], welded[indices[i + 1]], welded[indices[i + 2]]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) { ++degenerate; continue; }
        for (int e = 0; e < 3; ++e) ++uses[std::minmax(t[e], t[(e + 1) % 3])];
    }
    for (const auto& [edge, count] : uses) {
        if (count == 1) ++open;
        else if (count > 2) ++nonmanifold;
    }
    return {{"open_edges", open}, {"nonmanifold_edges", nonmanifold}, {"degenerate_triangles", degenerate}};
}

Mesh cutOf(const ClusterDag& dag, double allowance, std::size_t& clusters) {
    Mesh mesh;
    mesh.positions = dag.positions;
    const auto chosen = engine::geometry::cutAt(dag, allowance);
    clusters = chosen.size();
    for (const auto id : chosen) {
        const auto& cluster = dag.clusters[id];
        for (std::uint32_t i = 0; i < cluster.indices.count; ++i)
            mesh.indices.push_back(dag.indices[cluster.indices.first + i]);
        mesh.group.insert(mesh.group.end(), cluster.indices.count / 3, id + 1);
    }
    return mesh;
}

// --- drawing --------------------------------------------------------------

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;   // 0xAARRGGBB
    std::vector<float> depth;
    void clear(std::uint32_t colour) {
        std::fill(pixels.begin(), pixels.end(), colour);
        std::fill(depth.begin(), depth.end(), 1e30f);
    }
};

std::uint32_t hashColour(std::uint32_t id) {
    std::uint32_t h = id * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const auto channel = [&](int shift) { return 90 + ((h >> shift) & 0xff) * 150 / 255; };
    return 0xff000000u | (channel(0) << 16) | (channel(8) << 8) | channel(16);
}

// The same camera for every tile of one mesh: an orthographic three-quarter
// view fitted to the SOURCE bounds, so a shrinking or bloating result shows.
struct View {
    V3 centre, right, up, forward;
    double scale = 1;
};

View viewFor(const Mesh& source, int tile) {
    const auto box = boundsOf(source);
    View view;
    view.centre = box.centre();
    const double yaw = 0.7, pitch = 0.35;
    view.forward = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), -std::sin(pitch)};
    view.right = cross(view.forward, {0, 0, 1});
    view.right = view.right * (1 / length(view.right));
    view.up = cross(view.right, view.forward);
    const double radius = std::max(1e-6, length(box.size()) * 0.5);
    view.scale = tile * 0.46 / radius;
    return view;
}

void draw(const Mesh& mesh, const View& view, Image& image, int ox, int oy, int tile, bool edges) {
    const V3 light = V3{0.4, -0.5, 0.75} * (1 / length(V3{0.4, -0.5, 0.75}));
    const auto project = [&](V3 p) {
        const V3 d = p - view.centre;
        return V3{ox + tile * 0.5 + dot(d, view.right) * view.scale,
                  oy + tile * 0.5 - dot(d, view.up) * view.scale, dot(d, view.forward)};
    };
    for (std::size_t t = 0; t < mesh.triangles(); ++t) {
        const V3 a = mesh.vertex(mesh.indices[t * 3]), b = mesh.vertex(mesh.indices[t * 3 + 1]),
                 c = mesh.vertex(mesh.indices[t * 3 + 2]);
        V3 n = cross(b - a, c - a);
        const double len = length(n);
        if (!(len > 0)) continue;
        n = n * (1 / len);
        const double shade = 0.35 + 0.65 * std::abs(dot(n, light));
        const std::uint32_t base = mesh.group[t] ? hashColour(mesh.group[t]) : 0xffb8b4acu;
        const auto scaleChannel = [&](int shift) {
            return std::uint32_t(std::clamp(double((base >> shift) & 0xff) * shade, 0.0, 255.0));
        };
        const std::uint32_t colour = 0xff000000u | (scaleChannel(16) << 16) | (scaleChannel(8) << 8) | scaleChannel(0);
        const std::uint32_t edge = 0xff000000u | ((scaleChannel(16) / 2) << 16) | ((scaleChannel(8) / 2) << 8) |
                                   (scaleChannel(0) / 2);
        const V3 p0 = project(a), p1 = project(b), p2 = project(c);
        const int x0 = std::max(ox, int(std::floor(std::min({p0.x, p1.x, p2.x}))));
        const int x1 = std::min(ox + tile - 1, int(std::ceil(std::max({p0.x, p1.x, p2.x}))));
        const int y0 = std::max(oy, int(std::floor(std::min({p0.y, p1.y, p2.y}))));
        const int y1 = std::min(oy + tile - 1, int(std::ceil(std::max({p0.y, p1.y, p2.y}))));
        const double area = (p1.x - p0.x) * (p2.y - p0.y) - (p2.x - p0.x) * (p1.y - p0.y);
        if (std::abs(area) < 1e-12) continue;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                double w0 = ((p1.x - px) * (p2.y - py) - (p2.x - px) * (p1.y - py)) / area;
                double w1 = ((p2.x - px) * (p0.y - py) - (p0.x - px) * (p2.y - py)) / area;
                double w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                const float z = float(w0 * p0.z + w1 * p1.z + w2 * p2.z);
                const auto i = std::size_t(y) * image.width + x;
                if (z >= image.depth[i]) continue;
                image.depth[i] = z;
                // Edge darkening in barycentric space, about a pixel wide.
                const double lim = 1.0 / std::max(1.0, std::sqrt(std::abs(area)));
                image.pixels[i] = edges && std::min({w0, w1, w2}) < lim ? edge : colour;
            }
    }
}

}   // namespace

int main(int argc, char** argv) {
    fs::path input = "assets/generated/ue_reference";
    fs::path output = "assets/generated/mesh_lab";
    std::string only;
    fs::path scm;   // a directory of game meshes instead of UE references
    int tile = 300;
    std::size_t sampleCount = 20000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) output = argv[++i];
        else if (a == "--only" && i + 1 < argc) only = argv[++i];
        else if (a == "--scm" && i + 1 < argc) scm = argv[++i];
        else if (a == "--tile" && i + 1 < argc) tile = std::atoi(argv[++i]);
        else if (a == "--samples" && i + 1 < argc) sampleCount = std::size_t(std::atoll(argv[++i]));
        else if (a == "--help") {
            std::cout << "usage: asr_mesh_lab [UE_REFERENCE_DIR] [--out DIR] [--only NAME[,NAME]] [--tile PX]"
                         " [--samples N] [--scm SCENE_MODEL_DIR]\n";
            return 0;
        } else input = a;
    }
    fs::create_directories(output);

    // The profiles compared against Nanite. "current" is what the game bakes
    // today (cluster_asset.cpp: conservative error on); "nanite" follows the
    // parameters NaniteBuilder uses (ClusterDAG.cpp).
    std::vector<Profile> profiles;
    {
        Profile current{"current", {}};
        current.options.conservativeError = true;
        profiles.push_back(current);
        Profile nanite{"nanite", engine::geometry::naniteProfile()};
        profiles.push_back(nanite);
    }

    std::vector<nlohmann::json> assets;
    if (!scm.empty()) {
        // No Unreal column: the two profiles side by side on the game's own
        // meshes, cut at the same ladder of relative errors.
        for (const auto& entry : fs::directory_iterator(scm)) {
            if (entry.path().extension() != ".mesh") continue;
            nlohmann::json a{{"name", entry.path().stem().string()}, {"category", "game"},
                             {"scm", entry.path().string()}};
            for (const double e : {0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0})
                a["cuts"].push_back({{"relative_error", e}, {"file", ""}});
            assets.push_back(a);
        }
    } else {
        std::ifstream index(input / "index.json");
        if (!index) {
            std::cerr << "no " << (input / "index.json") << " - run tools/ue_nanite_reference.py first\n";
            return 1;
        }
        const auto all = nlohmann::json::parse(index);
        for (const auto& a : all) assets.push_back(a);
    }

    nlohmann::json report = nlohmann::json::array();
    std::ofstream md(output / "report.md");
    md << "# Mesh lab: UE Nanite vs our cluster DAG\n\n"
          "Same absolute error for every column (Nanite relative error converted with Nanite's formula).\n"
          "`dev` is a sampled two-way surface distance to the source, mean / p95, in units of that error.\n\n";

    for (const auto& asset : assets) {
        const std::string name = asset.value("name", "");
        if (!only.empty() && ("," + only + ",").find("," + name + ",") == std::string::npos) continue;
        Mesh source;
        if (asset.contains("scm") ? !loadScm(asset["scm"].get<std::string>(), source)
                                  : !loadUem(input / (name + "@source.uem"), source)) {
            std::cerr << name << ": cannot read the source\n";
            continue;
        }
        std::cout << name << ": " << source.triangles() << " source triangles\n" << std::flush;
        const Nearest nearSource(source);
        const auto sourceSamples = samplesOf(source, sampleCount);

        std::vector<ClusterDag> dags;
        nlohmann::json row;
        row["name"] = name;
        row["category"] = asset.value("category", "");
        row["source_triangles"] = source.triangles();
        row["source_topology"] = topologyOf(source.positions, source.indices);
        row["nanite_triangles"] = asset.value("nanite_triangles", 0);
        for (const auto& profile : profiles) {
            const auto started = std::chrono::steady_clock::now();
            auto options = profile.options;
            if (!source.keys.empty()) options.attributeKeys = source.keys;
            options.hardBoundaryKeys = source.materials;
            dags.push_back(engine::geometry::buildClusterDag(source.positions, source.indices, options));
            const double seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            const auto& dag = dags.back();
            std::size_t roots = 0, rootTriangles = 0;
            for (const auto& c : dag.clusters)
                if (std::isinf(c.parentError)) { ++roots; rootTriangles += c.indices.count / 3; }
            row["profiles"][profile.name] = {{"build_seconds", seconds},   {"clusters", dag.clusters.size()},
                                             {"levels", dag.levels},       {"roots", roots},
                                             {"root_triangles", rootTriangles}, {"converged", dag.converged},
                                             {"dominated", dag.dominated}};
            if (std::getenv("MESH_LAB_LEVELS")) {
                std::map<std::uint32_t, std::array<double, 3>> perLevel;   // tris, min error, max error
                std::map<std::uint32_t, std::vector<std::uint32_t>> levelIndices;
                for (const auto& c : dag.clusters) {
                    auto [it, fresh] = perLevel.try_emplace(c.level, std::array<double, 3>{0, 1e30, 0});
                    it->second[0] += c.indices.count / 3;
                    it->second[1] = std::min<double>(it->second[1], c.error);
                    it->second[2] = std::max<double>(it->second[2], c.error);
                    auto& indices = levelIndices[c.level];
                    indices.insert(indices.end(), dag.indices.begin() + c.indices.first,
                                   dag.indices.begin() + c.indices.first + c.indices.count);
                }
                for (const auto& [level, v] : perLevel) {
                    const auto topology = topologyOf(dag.positions, levelIndices[level]);
                    row["profiles"][profile.name]["level_topology"].push_back({{"level", level}, {"topology", topology}});
                    std::cout << "    " << profile.name << " L" << level << ": " << v[0] << " tris, error "
                              << v[1] * 1000 << ".." << v[2] * 1000 << " mm, topology " << topology.dump() << "\n";
                }
            }
            std::cout << "  " << profile.name << ": " << dag.clusters.size() << " clusters, " << dag.levels
                      << " levels, " << roots << " roots holding " << rootTriangles << " triangles, "
                      << seconds << " s\n" << std::flush;
        }

        const auto cuts = asset.value("cuts", nlohmann::json::array());
        const int columns = 2 + int(profiles.size());
        const int rows = int(cuts.size());
        const int labelHeight = 30;
        Image image;
        image.width = columns * tile;
        image.height = rows * (tile + labelHeight) + labelHeight;
        image.pixels.resize(std::size_t(image.width) * image.height);
        image.depth.resize(image.pixels.size());
        image.clear(0xff1d2024u);
        const View view = viewFor(source, tile);
        struct Label {
            int x, y;
            std::string text;
            bool header;
        };
        std::vector<Label> labels;
        labels.push_back({6, 6, name + "  source " + std::to_string(source.triangles()) + " tris", true});
        std::vector<std::string> heads{"source", "UE Nanite"};
        for (const auto& p : profiles) heads.push_back("ours: " + p.name);
        for (std::size_t c = 0; c < heads.size(); ++c) labels.push_back({int(c) * tile + 6, 20, heads[c], true});

        md << "## " << name << " (" << asset.value("category", "") << ", " << source.triangles()
           << " source triangles)\n\n";
        md << "Source topology: `" << row["source_topology"].dump() << "`\n\n";
        for (std::size_t p = 0; p < profiles.size(); ++p) {
            const auto& stats = row["profiles"][profiles[p].name];
            md << "- ours/" << profiles[p].name << ": " << stats["clusters"] << " clusters, " << stats["levels"]
               << " levels, " << stats["roots"] << " roots (" << stats["root_triangles"]
               << " triangles never simplify further), build " << stats["build_seconds"].get<double>() << " s\n";
        }
        md << "\n| rel. error | abs. error (mm) | UE tris | UE dev |";
        for (const auto& p : profiles) md << " " << p.name << " tris | " << p.name << " dev | open/nonmanifold |";
        md << "\n|---|---|---|---|";
        for (std::size_t p = 0; p < profiles.size(); ++p) md << "---|---|---|";
        md << "\n";

        for (int r = 0; r < rows; ++r) {
            const auto& cut = cuts[r];
            const double percent = cut.value("relative_error", 0.0);
            const double allowance = absoluteError(percent, source);
            const int oy = labelHeight + r * (tile + labelHeight) + labelHeight;
            nlohmann::json entry{{"relative_error", percent}, {"absolute_error", allowance}};

            Mesh ue;
            if (!cut.value("file", "").empty() && !loadUem(input / cut.value("file", ""), ue)) {
                std::cerr << name << ": cannot read " << cut.value("file", "") << "\n";
                continue;
            }
            const auto ueDev = deviation(sourceSamples, nearSource, ue);
            entry["ue"] = {{"triangles", ue.triangles()}, {"mean", ueDev.mean}, {"p95", ueDev.p95}, {"max", ueDev.max}};
            draw(source, view, image, 0, oy, tile, source.triangles() < 4000);
            draw(ue, view, image, tile, oy, tile, ue.triangles() < 4000);
            char text[160];
            std::snprintf(text, sizeof text, "e=%g%% (%.1f mm)", percent, allowance * 1000);
            labels.push_back({6, oy - labelHeight + 4, text, true});
            std::snprintf(text, sizeof text, "%zu tris  dev %.2f/%.2f", ue.triangles(), ueDev.mean / allowance,
                          ueDev.p95 / allowance);
            labels.push_back({tile + 6, oy - labelHeight + 16, "UE Nanite: " + std::string(text), false});
            labels.push_back({6, oy - labelHeight + 16, "source", false});
            md << "| " << percent << "% | " << allowance * 1000 << " | " << ue.triangles() << " | "
               << ueDev.mean / allowance << " / " << ueDev.p95 / allowance << " |";

            for (std::size_t p = 0; p < profiles.size(); ++p) {
                std::size_t clusters = 0;
                const auto ours = cutOf(dags[p], allowance, clusters);
                const auto oursDev = deviation(sourceSamples, nearSource, ours);
                const auto topology = topologyOf(ours.positions, ours.indices);
                entry[profiles[p].name] = {{"triangles", ours.triangles()}, {"clusters", clusters},
                                           {"mean", oursDev.mean}, {"p95", oursDev.p95}, {"max", oursDev.max},
                                           {"topology", topology}};
                const int ox = int(2 + p) * tile;
                draw(ours, view, image, ox, oy, tile, ours.triangles() < 4000);
                std::snprintf(text, sizeof text, "%zu tris %zu cl  dev %.2f/%.2f", ours.triangles(), clusters,
                              oursDev.mean / allowance, oursDev.p95 / allowance);
                labels.push_back({ox + 6, oy - labelHeight + 16, profiles[p].name + ": " + text, false});
                md << " " << ours.triangles() << " | " << oursDev.mean / allowance << " / "
                   << oursDev.p95 / allowance << " | " << topology["open_edges"] << " / "
                   << topology["nonmanifold_edges"] << " |";
            }
            md << "\n";
            row["cuts"].push_back(entry);
            std::cout << "  e=" << percent << "%: UE " << ue.triangles();
            for (const auto& p : profiles) std::cout << ", " << p.name << " " << entry[p.name]["triangles"];
            std::cout << "\n" << std::flush;
        }
        md << "\n![" << name << "](" << name << ".png)\n\n";
        report.push_back(row);

        // Text through SDL's software renderer onto the finished picture:
        // no window, no GPU, and the same 8x8 debug font everywhere.
        SDL_Surface* surface = SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_ARGB8888,
                                                     image.pixels.data(), image.width * 4);
        if (surface) {
            if (SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface)) {
                for (const auto& label : labels) {
                    if (label.header) SDL_SetRenderDrawColor(renderer, 255, 220, 150, 255);
                    else SDL_SetRenderDrawColor(renderer, 210, 214, 220, 255);
                    SDL_RenderDebugText(renderer, float(label.x), float(label.y), label.text.c_str());
                }
                SDL_RenderPresent(renderer);
                SDL_DestroyRenderer(renderer);
            }
            const auto png = (output / (name + ".png")).string();
            if (!IMG_SavePNG(surface, png.c_str())) std::cerr << "cannot write " << png << ": " << SDL_GetError() << "\n";
            SDL_DestroySurface(surface);
        }
    }
    std::ofstream(output / "report.json") << report.dump(1);
    std::cout << "wrote " << (output / "report.md") << "\n";
    return 0;
}
