#include "engine/geometry/cluster_dag.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace engine::geometry {
namespace {

using Tri = std::array<std::uint32_t, 3>;
constexpr double kInfinity = std::numeric_limits<double>::infinity();
// A collapse that turns a triangle this far away from where it faced has
// folded it rather than simplified it. The same number the offline chain
// builder uses, for the same reason.
constexpr double kMaxFold = 0.2;

// Attribute-split vertices are intentionally distinct in the output, but
// their positions still describe one geometric edge for manifold/rim tests.
// Without this second identity every UV/normal seam looks like an open edge,
// all of its vertices get pinned, and a tree with imported attributes stalls
// after the first level even when its underlying surface is closed.
struct PositionKey {
    std::uint32_t x = 0, y = 0, z = 0;
    bool operator==(const PositionKey&) const = default;
};
struct PositionKeyHash {
    std::size_t operator()(const PositionKey& key) const {
        std::uint64_t value = 1469598103934665603ull;
        for (const auto bits : {key.x, key.y, key.z}) {
            value = (value ^ bits) * 1099511628211ull;
        }
        return static_cast<std::size_t>(value);
    }
};

PositionKey positionKey(std::span<const float> positions, std::uint32_t vertex) {
    PositionKey key;
    for (int axis = 0; axis < 3; ++axis) {
        std::memcpy(&(&key.x)[axis], &positions[std::size_t(vertex) * 3 + axis], sizeof(float));
        if ((&key.x)[axis] == 0x80000000u) (&key.x)[axis] = 0;
    }
    return key;
}

struct Vector3 {
    double x = 0, y = 0, z = 0;
};
Vector3 at(std::span<const float> positions, std::uint32_t v) {
    return {positions[std::size_t(v) * 3], positions[std::size_t(v) * 3 + 1],
            positions[std::size_t(v) * 3 + 2]};
}
Vector3 operator-(const Vector3& a, const Vector3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vector3 cross(const Vector3& a, const Vector3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(const Vector3& a, const Vector3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const Vector3& a) { return std::sqrt(dot(a, a)); }

double pointTriangleDistanceSquared(const Vector3& point, const Vector3& a,
                                    const Vector3& b, const Vector3& c) {
    const Vector3 ab = b - a, ac = c - a, ap = point - a;
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return dot(ap, ap);

    const Vector3 bp = point - b;
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return dot(bp, bp);

    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double t = d1 / (d1 - d3);
        const Vector3 projection{a.x + t * ab.x, a.y + t * ab.y, a.z + t * ab.z};
        const Vector3 delta = point - projection;
        return dot(delta, delta);
    }

    const Vector3 cp = point - c;
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return dot(cp, cp);

    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double t = d2 / (d2 - d6);
        const Vector3 projection{a.x + t * ac.x, a.y + t * ac.y, a.z + t * ac.z};
        const Vector3 delta = point - projection;
        return dot(delta, delta);
    }

    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const Vector3 bc = c - b;
        const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        const Vector3 projection{b.x + t * bc.x, b.y + t * bc.y, b.z + t * bc.z};
        const Vector3 delta = point - projection;
        return dot(delta, delta);
    }

    const Vector3 normal = cross(ab, ac);
    const double normalLengthSquared = dot(normal, normal);
    if (normalLengthSquared <= 0)
        return std::min({dot(ap, ap), dot(bp, bp), dot(cp, cp)});
    const double distanceToPlane = dot(point - a, normal);
    return distanceToPlane * distanceToPlane / normalLengthSquared;
}

struct Bounds {
    Vector3 low{kInfinity, kInfinity, kInfinity};
    Vector3 high{-kInfinity, -kInfinity, -kInfinity};
    void add(const Vector3& p) {
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    [[nodiscard]] Vector3 unit(const Vector3& p) const {
        const auto axis = [](double v, double lo, double hi) {
            return hi > lo ? (v - lo) / (hi - lo) : 0.0;
        };
        return {axis(p.x, low.x, high.x), axis(p.y, low.y, high.y), axis(p.z, low.z, high.z)};
    }
};

// Nearest-triangle queries over a uniform grid. The bound below samples four
// points of every triangle against every triangle of the other surface, which
// is the measured error and cannot be approximated away - but scanning the
// whole target per point made it cost the triangle count squared, and that was
// the single largest phase of building a region aggregate. The grid returns the
// same nearest distance: a triangle nearer than the current best must overlap a
// cell nearer than the current best, so the ring sweep stops only once no
// unvisited cell can still improve it.
class TriangleGrid {
public:
    TriangleGrid(std::span<const Tri> triangles, std::span<const float> positions)
        : triangles_(triangles), positions_(positions) {
        if (triangles.empty()) return;
        for (const auto& triangle : triangles)
            for (const auto vertex : triangle) bounds_.add(at(positions, vertex));
        const double span = std::max({bounds_.high.x - bounds_.low.x,
                                      bounds_.high.y - bounds_.low.y,
                                      bounds_.high.z - bounds_.low.z, 0.0});
        // Roughly one triangle per cell, bounded so that a flat or tiny surface
        // cannot ask for an enormous sparse grid.
        const auto perAxis = std::clamp(int(std::cbrt(double(triangles.size()))) + 1, 1, 48);
        side_ = perAxis;
        cell_ = span > 0 ? span / double(perAxis) : 1.0;
        if (!(cell_ > 0) || !std::isfinite(cell_)) cell_ = 1.0;
        cells_.assign(std::size_t(side_) * side_ * side_, {});
        visited_.assign(triangles.size(), 0);
        for (std::uint32_t t = 0; t < triangles.size(); ++t) {
            Bounds box;
            for (const auto vertex : triangles[t]) box.add(at(positions, vertex));
            const auto low = index(box.low), high = index(box.high);
            const std::size_t spanned = std::size_t(high[0] - low[0] + 1) *
                                        std::size_t(high[1] - low[1] + 1) *
                                        std::size_t(high[2] - low[2] + 1);
            // A triangle smeared across the grid would cost more to register
            // than to test; those are simply always tested.
            if (spanned > 64) { everywhere_.push_back(t); continue; }
            for (int z = low[2]; z <= high[2]; ++z)
                for (int y = low[1]; y <= high[1]; ++y)
                    for (int x = low[0]; x <= high[0]; ++x)
                        cells_[cellIndex(x, y, z)].push_back(t);
        }
    }

    [[nodiscard]] double nearestSquared(const Vector3& point) {
        if (cells_.empty() && everywhere_.empty()) return kInfinity;
        double best = kInfinity;
        ++stamp_;
        const auto test = [&](std::uint32_t t) {
            if (visited_[t] == stamp_) return;
            visited_[t] = stamp_;
            best = std::min(best, pointTriangleDistanceSquared(
                    point, at(positions_, triangles_[t][0]), at(positions_, triangles_[t][1]),
                    at(positions_, triangles_[t][2])));
        };
        for (const auto t : everywhere_) test(t);
        const auto centre = index(point);
        for (int r = 0; r < side_ * 2; ++r) {
            // Nothing in a ring this far out can beat what is already found.
            if (r > 0 && best < kInfinity) {
                const double reach = double(r - 1) * cell_;
                if (reach > 0 && reach * reach > best) break;
            }
            bool any = false;
            for (int z = centre[2] - r; z <= centre[2] + r; ++z) {
                if (z < 0 || z >= side_) continue;
                for (int y = centre[1] - r; y <= centre[1] + r; ++y) {
                    if (y < 0 || y >= side_) continue;
                    const bool edgeZY = std::abs(z - centre[2]) == r || std::abs(y - centre[1]) == r;
                    for (int x = centre[0] - r; x <= centre[0] + r; ++x) {
                        if (x < 0 || x >= side_) continue;
                        // Only the shell of the box: the inside was swept by a
                        // smaller radius already.
                        if (!edgeZY && std::abs(x - centre[0]) != r) continue;
                        any = true;
                        for (const auto t : cells_[cellIndex(x, y, z)]) test(t);
                    }
                }
            }
            if (!any && r > side_) break;
        }
        return best;
    }

private:
    [[nodiscard]] std::array<int, 3> index(const Vector3& point) const {
        const double origin[3]{bounds_.low.x, bounds_.low.y, bounds_.low.z};
        const double value[3]{point.x, point.y, point.z};
        std::array<int, 3> out{};
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = std::clamp(int(std::floor((value[axis] - origin[axis]) / cell_)), 0,
                                   side_ - 1);
        return out;
    }
    [[nodiscard]] std::size_t cellIndex(int x, int y, int z) const {
        return (std::size_t(z) * side_ + std::size_t(y)) * side_ + std::size_t(x);
    }
    std::span<const Tri> triangles_;
    std::span<const float> positions_;
    Bounds bounds_;
    double cell_ = 1;
    int side_ = 1;
    std::vector<std::vector<std::uint32_t>> cells_;
    std::vector<std::uint32_t> everywhere_;
    std::vector<std::uint32_t> visited_;
    std::uint32_t stamp_ = 0;
};

double directedSurfaceError(std::span<const Tri> samples, std::span<const Tri> target,
                            std::span<const float> positions) {
    double worst = 0;
    TriangleGrid grid(target, positions);
    for (const auto& triangle : samples) {
        const Vector3 a = at(positions, triangle[0]);
        const Vector3 b = at(positions, triangle[1]);
        const Vector3 c = at(positions, triangle[2]);
        const Vector3 samplePoints[]{
                a, b, c, {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3,
                          (a.z + b.z + c.z) / 3}};
        for (const auto& point : samplePoints)
            worst = std::max(worst, std::sqrt(grid.nearestSquared(point)));
    }
    return worst;
}

double surfaceError(std::span<const Tri> original, std::span<const Tri> simplified,
                    std::span<const float> positions) {
    if (original.empty() || simplified.empty()) return kInfinity;
    return std::max(directedSurfaceError(original, simplified, positions),
                    directedSurfaceError(simplified, original, positions));
}

// Errors and culling bounds must never get smaller when stored as floats.
float upperFloat(double value) {
    const float rounded = float(value);
    return double(rounded) < value
        ? std::nextafter(rounded, std::numeric_limits<float>::infinity()) : rounded;
}

// A symmetric 4x4 quadric as its ten distinct entries: the sum of squared
// distances to a set of planes, area-weighted so that a large face counts for
// more than the sliver beside it.
struct Quadric {
    double m[10]{};
    void addPlane(const Vector3& n, double d, double weight) {
        const double p[4]{n.x, n.y, n.z, d};
        int k = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i; j < 4; ++j) m[k++] += weight * p[i] * p[j];
    }
    void operator+=(const Quadric& other) {
        for (int i = 0; i < 10; ++i) m[i] += other.m[i];
    }
    [[nodiscard]] double evaluate(const Vector3& v) const {
        const double p[4]{v.x, v.y, v.z, 1};
        double total = 0;
        int k = 0;
        for (int i = 0; i < 4; ++i)
            for (int j = i; j < 4; ++j) total += (i == j ? 1.0 : 2.0) * m[k++] * p[i] * p[j];
        return total;
    }
};

// Minimise a summed quadric in R3. Boundary vertices never call this path, so
// a singular interior system can safely fall back to the edge midpoint.
Vector3 optimalQuadricPosition(const Quadric& q, const Vector3& fallback) {
    double a[3][4]{{q.m[0], q.m[1], q.m[2], -q.m[3]},
                   {q.m[1], q.m[4], q.m[5], -q.m[6]},
                   {q.m[2], q.m[5], q.m[7], -q.m[8]}};
    for (int column = 0; column < 3; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 3; ++row)
            if (std::abs(a[row][column]) > std::abs(a[pivot][column])) pivot = row;
        if (std::abs(a[pivot][column]) < 1e-12) return fallback;
        if (pivot != column)
            for (int j = column; j < 4; ++j) std::swap(a[column][j], a[pivot][j]);
        const double divisor = a[column][column];
        for (int j = column; j < 4; ++j) a[column][j] /= divisor;
        for (int row = 0; row < 3; ++row) {
            if (row == column) continue;
            const double factor = a[row][column];
            for (int j = column; j < 4; ++j) a[row][j] -= factor * a[column][j];
        }
    }
    return {a[0][3], a[1][3], a[2][3]};
}

std::uint64_t edgeKey(std::uint32_t a, std::uint32_t b) {
    return a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a;
}

std::vector<std::uint32_t> geometricVertexIds(std::span<const float> positions) {
    std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> ids;
    std::vector<std::uint32_t> result(positions.size() / 3);
    std::uint32_t next = 0;
    for (std::uint32_t vertex = 0; vertex < result.size(); ++vertex) {
        const auto [entry, fresh] = ids.try_emplace(positionKey(positions, vertex), next);
        if (fresh) ++next;
        result[vertex] = entry->second;
    }
    return result;
}

std::uint64_t geometricEdgeKey(std::span<const std::uint32_t> geometricVertices,
                               std::uint32_t a, std::uint32_t b) {
    return edgeKey(geometricVertices[a], geometricVertices[b]);
}
std::uint64_t faceKey(Tri t) {
    std::sort(t.begin(), t.end());
    return (std::uint64_t(t[0]) << 42) ^ (std::uint64_t(t[1]) << 21) ^ std::uint64_t(t[2]);
}

// Interleaving the three coordinates gives neighbouring squares neighbouring
// numbers, which is all that is wanted here: a deterministic sweep that starts
// clusters near each other rather than at opposite ends of the mesh.
std::uint64_t morton(double x, double y, double z) {
    const auto part = [](double value) {
        std::uint64_t bits = std::uint64_t(std::clamp(value, 0.0, 1.0) * 1023.0);
        std::uint64_t spread = 0;
        for (int i = 0; i < 10; ++i) spread |= ((bits >> i) & 1ull) << (3 * i);
        return spread;
    };
    return part(x) | (part(y) << 1) | (part(z) << 2);
}

Vector3 centroid(const Tri& t, std::span<const float> positions) {
    const Vector3 a = at(positions, t[0]), b = at(positions, t[1]), c = at(positions, t[2]);
    return {(a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3};
}

// Cut a set of triangles into runs of at most `target`, growing each from a
// seed across shared edges. Growing across edges rather than by proximity is
// what keeps a cluster a connected patch, which is what makes its boundary
// short enough to be worth locking later.
std::vector<std::vector<std::uint32_t>> partition(const std::vector<Tri>& triangles,
                                                  std::span<const std::uint32_t> subset,
                                                  std::span<const float> positions,
                                                  std::size_t target) {
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byEdge;
    byEdge.reserve(subset.size() * 3);
    for (const auto t : subset)
        for (int e = 0; e < 3; ++e)
            byEdge[edgeKey(triangles[t][e], triangles[t][(e + 1) % 3])].push_back(t);

    // Pack complete connected components whenever they fit. The previous
    // sweep filled the last few slots of a cluster with the beginning of a
    // new branch, then put the rest of that branch in the next cluster. At
    // the next DAG level both clusters saw the branch boundary as an external
    // rim and the simplifier could not collapse it at all. Component-aware
    // packing keeps small disconnected vegetation pieces intact; oversized
    // components still split normally at the cluster budget.
    std::vector<std::uint32_t> componentParent(triangles.size());
    for (const auto t : subset) componentParent[t] = t;
    const auto findTriangle = [&](std::uint32_t triangle) {
        while (componentParent[triangle] != triangle) {
            componentParent[triangle] = componentParent[componentParent[triangle]];
            triangle = componentParent[triangle];
        }
        return triangle;
    };
    const auto joinTriangles = [&](std::uint32_t a, std::uint32_t b) {
        a = findTriangle(a);
        b = findTriangle(b);
        if (a != b) componentParent[b] = a;
    };
    for (const auto& [edge, uses] : byEdge)
        for (std::size_t i = 1; i < uses.size(); ++i) joinTriangles(uses[0], uses[i]);
    std::unordered_map<std::uint32_t, std::size_t> componentSize;
    for (const auto t : subset) ++componentSize[findTriangle(t)];

    Bounds bounds;
    for (const auto t : subset) bounds.add(centroid(triangles[t], positions));
    std::vector<std::uint32_t> order(subset.begin(), subset.end());
    std::vector<std::uint64_t> code(triangles.size(), 0);
    for (const auto t : subset) {
        const auto unit = bounds.unit(centroid(triangles[t], positions));
        code[t] = morton(unit.x, unit.y, unit.z);
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return code[a] != code[b] ? code[a] < code[b] : a < b;
    });

    std::unordered_set<std::uint32_t> taken;
    taken.reserve(subset.size());
    std::vector<std::vector<std::uint32_t>> parts;
    std::size_t placed = 0;
    while (placed < order.size()) {
        std::vector<std::uint32_t> part;
        std::queue<std::uint32_t> pending;
        // From the start each time. Triangles queued but not reached are put
        // back, and one of them may sit BEFORE wherever the sweep had got to -
        // a cursor that only moved forward would leave them in no cluster at
        // all, which is a hole in the finest level and a seam at every cut.
        std::size_t next = 0;
        // Keep filling this cluster until it is full. Growing across shared
        // edges keeps a cluster a connected patch where the mesh IS connected,
        // but an imported prop is rarely one shell - a pine is a few hundred
        // separate pieces - and a cluster per piece leaves clusters a tenth of
        // the size asked for. Those are the clusters that then cannot be
        // simplified: a group of four separate shells is four rims and nothing
        // in the middle, so the level stalls and the cut at distance ends up
        // more expensive than the chain it replaced. When a piece runs out, the
        // nearest unused triangle continues the same cluster.
        while (part.size() < target) {
            if (pending.empty()) {
                while (next < order.size() && taken.count(order[next])) ++next;
                if (next >= order.size()) break;
                const auto component = findTriangle(order[next]);
                if (!part.empty() && componentSize[component] > target - part.size()) break;
                pending.push(order[next]);
                taken.insert(order[next]);
            }
            const auto t = pending.front();
            pending.pop();
            part.push_back(t);
            for (int e = 0; e < 3 && part.size() + pending.size() < target; ++e)
                for (const auto neighbour :
                     byEdge[edgeKey(triangles[t][e], triangles[t][(e + 1) % 3])])
                    if (taken.insert(neighbour).second) pending.push(neighbour);
        }
        // Whatever was queued but not reached stays for the next cluster.
        while (!pending.empty()) {
            taken.erase(pending.front());
            pending.pop();
        }
        if (part.empty()) break;
        placed += part.size();
        parts.push_back(std::move(part));
    }
    return parts;
}

struct Simplified {
    std::vector<Tri> triangles;
    std::unordered_map<std::uint32_t, Vector3> positions;
    double error = 0;
};

// Simplify one group as a single mesh, with every vertex on its outer boundary
// pinned. Interior collapses use the optimal position of the combined quadric;
// a pinned boundary vertex remains exactly where it was so mixed cuts stay
// watertight.
Simplified simplifyLocked(const std::vector<Tri>& input, std::span<const float> positions,
                          std::size_t target, bool conservativeError,
                          std::span<const std::uint64_t> boundaryEdges,
                          std::span<const std::uint32_t> boundaryVertices,
                          std::span<const std::uint32_t> geometricVertices) {
    Simplified result;
    result.triangles = input;
    if (input.size() <= target) return result;

    // Simplification state belongs to this group. The old implementation
    // shared these accumulators between every group and every DAG level. A
    // collapse in one parent then changed the cost and the reported error in
    // an unrelated parent that merely happened to reuse a boundary vertex.
    // Apart from making the result order-dependent, that let geometry from a
    // neighbouring branch steer this branch's collapses. Build the quadrics
    // from this group's current surface and keep all accumulated state local.
    std::unordered_map<std::uint32_t, Quadric> quadric;
    std::unordered_map<std::uint32_t, double> weight;
    std::unordered_map<std::uint32_t, double> displacement;
    std::unordered_map<std::uint32_t, Vector3> localPosition;
    quadric.reserve(input.size());
    weight.reserve(input.size());
    displacement.reserve(input.size());
    localPosition.reserve(input.size());
    for (const auto& t : input) {
        // Candidate edges are collected from the topology below even when a
        // face has zero area in the current representation. Keep its endpoint
        // positions available so a stale/degenerate edge can be rejected
        // cleanly instead of throwing from the cost queue.
        for (const auto v : t) localPosition.try_emplace(v, at(positions, v));
        const Vector3 a = at(positions, t[0]), b = at(positions, t[1]), c = at(positions, t[2]);
        const Vector3 normal = cross(b - a, c - a);
        const double twice = length(normal);
        if (twice <= 0) continue;
        const Vector3 unit{normal.x / twice, normal.y / twice, normal.z / twice};
        const double area = twice * 0.5;
        for (const auto v : t) {
            quadric[v].addPlane(unit, -dot(unit, a), area);
            weight[v] += area;
        }
    }

    std::unordered_set<std::uint64_t> candidateEdges;
    for (const auto& t : input)
        for (int e = 0; e < 3; ++e) {
            const auto next = (e + 1) % 3;
            const auto topological = edgeKey(t[e], t[next]);
            candidateEdges.insert(topological);
        }

    // The rim, as edges and not only as vertices. Pinning the vertices alone is
    // not enough: collapsing an interior vertex ONTO a rim vertex deletes the
    // triangle they share, and if that triangle carried a rim edge the rim
    // loses a face - the crack the whole lock exists to prevent, arriving from
    // the inside.
    std::unordered_set<std::uint64_t> rim;
    std::unordered_set<std::uint32_t> locked;
    rim.insert(boundaryEdges.begin(), boundaryEdges.end());
    // Lock the actual attribute vertices on the geometric rim. A seam with
    // two topological copies is locked; a smooth shared edge is not.
    for (const auto& t : input)
        for (int e = 0; e < 3; ++e) {
            const auto next = (e + 1) % 3;
            if (rim.count(geometricEdgeKey(geometricVertices, t[e], t[next]))) {
                locked.insert(t[e]);
                locked.insert(t[next]);
            }
        }
    // Two groups can touch at a single vertex without sharing an edge. That
    // point is still part of the mixed-cut boundary and must be pinned; edge
    // locks alone would duplicate/move it and open a crack in the cut.
    locked.insert(boundaryVertices.begin(), boundaryVertices.end());

    // The local quadrics are merged at every collapse. The inherited error is
    // carried by the DAG edge, so rebuilding the local state from this
    // group's current surface does not reset the representation's total
    // error; it prevents an unrelated group from contributing planes here.
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> incident;
    std::vector<Tri> live = input;
    std::vector<char> dead(live.size(), 0);
    std::unordered_set<std::uint64_t> faces;
    for (std::uint32_t i = 0; i < live.size(); ++i) {
        const Tri& t = live[i];
        const Vector3 a = at(positions, t[0]), b = at(positions, t[1]), c = at(positions, t[2]);
        if (length(cross(b - a, c - a)) <= 0) {
            dead[i] = 1;
            continue;
        }
        for (const auto v : t) incident[v].push_back(i);
        faces.insert(faceKey(t));
    }

    std::unordered_map<std::uint32_t, std::uint32_t> version;
    struct Candidate {
        double cost;
        std::uint32_t from, onto, stamp;
        bool operator<(const Candidate& other) const { return cost > other.cost; }   // min-heap
    };
    const auto stampOf = [&](std::uint32_t a, std::uint32_t b) { return version[a] + version[b]; };
    const auto costOf = [&](std::uint32_t from, std::uint32_t onto) {
        Quadric combined = quadric[from];
        combined += quadric[onto];
        const double mass = weight[from] + weight[onto];
        const auto fromPosition = localPosition.at(from), ontoPosition = localPosition.at(onto);
        const auto fallback=Vector3{(fromPosition.x+ontoPosition.x)*0.5,
                                    (fromPosition.y+ontoPosition.y)*0.5,
                                    (fromPosition.z+ontoPosition.z)*0.5};
        const auto candidate=locked.count(onto) ? ontoPosition
                                                : optimalQuadricPosition(combined,fallback);
        const double squared = combined.evaluate(candidate);
        return mass > 0 ? std::sqrt(std::max(0.0, squared) / mass) : 0.0;
    };
    std::priority_queue<Candidate> heap;
    const auto offer = [&](std::uint32_t from, std::uint32_t onto) {
        if (locked.count(from) || from == onto) return;
        heap.push({costOf(from, onto), from, onto, stampOf(from, onto)});
    };
    for (const auto key : candidateEdges) {
        const auto a = std::uint32_t(key >> 32), b = std::uint32_t(key & 0xffffffffu);
        offer(a, b);
        offer(b, a);
    }

    std::unordered_set<std::uint32_t> gone;
    std::size_t alive = std::size_t(std::count(dead.begin(), dead.end(), 0));
    while (alive > target && !heap.empty()) {
        const Candidate candidate = heap.top();
        heap.pop();
        const auto from = candidate.from, onto = candidate.onto;
        if (gone.count(from) || gone.count(onto)) continue;
        if (candidate.stamp != stampOf(from, onto)) continue;   // stale: one end has moved on

        // Work the collapse out fully before applying any of it: a fold or a
        // duplicated face found halfway through would leave the mesh in a state
        // no later step could describe.
        std::vector<std::uint32_t> removed, rewritten;
        bool refuse = false;
        // The link condition, and it is not optional. Two ends of an edge may
        // share only the vertices opposite that edge's own faces; any other
        // shared neighbour means the collapse folds the surface onto itself and
        // leaves an edge with one face or three. On a closed mesh that is a
        // hole, and a hole in one level of a group is a crack at every cut that
        // draws it beside another. Refusing identical faces afterwards catches
        // some of these and not the ones that matter.
        {
            const auto neighbours = [&](std::uint32_t v) {
                std::unordered_set<std::uint32_t> out;
                for (const auto index : incident[v])
                    if (!dead[index])
                        for (const auto other : live[index])
                            if (other != v) out.insert(other);
                return out;
            };
            const auto near = neighbours(from), far = neighbours(onto);
            std::size_t shared = 0, opposite = 0;
            for (const auto v : near)
                if (far.count(v)) {
                    ++shared;
                    for (const auto index : incident[from])
                        if (!dead[index]) {
                            const Tri& face = live[index];
                            const bool hasOnto = face[0] == onto || face[1] == onto || face[2] == onto;
                            const bool hasV = face[0] == v || face[1] == v || face[2] == v;
                            if (hasOnto && hasV) { ++opposite; break; }
                        }
                }
            if (shared != opposite || shared == 0) continue;
        }
        for (const auto index : incident[from]) {
            if (dead[index]) continue;
            Tri face = live[index];
            if (face[0] == onto || face[1] == onto || face[2] == onto) {
                for (int e = 0; e < 3 && !refuse; ++e)
                    refuse = rim.count(geometricEdgeKey(geometricVertices, face[e],
                                                        face[(e + 1) % 3])) != 0;
                if (refuse) break;
                removed.push_back(index);
                continue;
            }
            const Vector3 a0 = localPosition.at(face[0]), b0 = localPosition.at(face[1]),
                          c0 = localPosition.at(face[2]);
            const Vector3 before = cross(b0 - a0, c0 - a0);
            for (auto& v : face)
                if (v == from) v = onto;
            const Vector3 a1 = localPosition.at(face[0]), b1 = localPosition.at(face[1]),
                          c1 = localPosition.at(face[2]);
            const Vector3 after = cross(b1 - a1, c1 - a1);
            const double scale = length(before) * length(after);
            if (scale <= 0 || dot(before, after) / scale < kMaxFold) { refuse = true; break; }
            if (faces.count(faceKey(face))) { refuse = true; break; }   // would double a face
            rewritten.push_back(index);
        }
        if (refuse || removed.empty()) continue;   // an edge with no shared face is not an edge

        for (const auto index : removed) {
            faces.erase(faceKey(live[index]));
            dead[index] = 1;
            --alive;
        }
        for (const auto index : rewritten) {
            faces.erase(faceKey(live[index]));
            for (auto& v : live[index])
                if (v == from) v = onto;
            faces.insert(faceKey(live[index]));
            incident[onto].push_back(index);
        }
        quadric[onto] += quadric[from];
        weight[onto] += weight[from];
        const auto fromPosition=localPosition.at(from), ontoPosition=localPosition.at(onto);
        const auto fallback=Vector3{(fromPosition.x+ontoPosition.x)*0.5,
                                    (fromPosition.y+ontoPosition.y)*0.5,
                                    (fromPosition.z+ontoPosition.z)*0.5};
        const auto target=locked.count(onto) ? ontoPosition
                                             : optimalQuadricPosition(quadric[onto],fallback);
        displacement[onto] = std::max(displacement[onto], displacement[from] +
            length(fromPosition - target));
        localPosition[onto]=target;
        gone.insert(from);
        ++version[onto];

        // Everything still touching the moved vertex is worth reconsidering:
        // its quadric has grown, so its neighbours cost more than they did.
        std::unordered_set<std::uint32_t> around;
        for (const auto index : incident[onto])
            if (!dead[index])
                for (const auto v : live[index])
                    if (v != onto && !gone.count(v)) around.insert(v);
        for (const auto v : around) {
            offer(v, onto);
            offer(onto, v);
        }
    }

    result.triangles.clear();
    std::unordered_set<std::uint32_t> survivors;
    for (std::size_t i = 0; i < live.size(); ++i)
        if (!dead[i]) {
            result.triangles.push_back(live[i]);
            for (const auto v : live[i]) survivors.insert(v);
        }
    // The error is measured on what is LEFT, not on what each collapse cost.
    // A vertex now stands in for every original face merged into it, and how
    // far it sits from those faces is exactly what a viewer would see. Summing
    // collapse costs instead reports the worst single step, which stops growing
    // once the expensive steps are behind - and a level whose error equals its
    // parent's is a level no allowance can ever select.
    for (const auto v : survivors) {
        if (conservativeError) result.error = std::max(result.error, displacement[v]);
        if (weight[v] <= 0) continue;
        const double squared = quadric[v].evaluate(localPosition.at(v));
        result.error = std::max(result.error, std::sqrt(std::max(0.0, squared) / weight[v]));
    }
    // QEM is a useful collapse ordering, but its plane residual is not a
    // Hausdorff/silhouette bound. Measure both directions against the actual
    // original group surface so a narrow feature or a coplanar displacement
    // cannot report a deceptively tiny parent error.
    if (conservativeError)
        result.error = std::max(result.error, surfaceError(input, result.triangles, positions));
    result.positions=std::move(localPosition);
    return result;
}

// Gather neighbouring clusters, by shared vertices rather than shared edges: two
// clusters that meet only at a corner still have to agree about that corner.
std::vector<std::vector<std::uint32_t>> gather(const std::vector<std::vector<std::uint32_t>>& parts,
                                               const std::vector<Tri>& triangles,
                                               std::span<const float> positions, std::size_t target) {
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> byVertex;
    for (std::uint32_t p = 0; p < parts.size(); ++p) {
        std::unordered_set<std::uint32_t> seen;
        for (const auto t : parts[p])
            for (const auto v : triangles[t])
                if (seen.insert(v).second) byVertex[v].push_back(p);
    }
    std::vector<std::unordered_set<std::uint32_t>> unique(parts.size());
    {
        for (const auto& [vertex, sharing] : byVertex)
            for (const auto a : sharing)
                for (const auto b : sharing)
                    if (a != b) unique[a].insert(b);
    }

    Bounds bounds;
    std::vector<Vector3> middle(parts.size());
    for (std::size_t p = 0; p < parts.size(); ++p) {
        Vector3 sum;
        for (const auto t : parts[p]) {
            const auto c = centroid(triangles[t], positions);
            sum = {sum.x + c.x, sum.y + c.y, sum.z + c.z};
        }
        const double n = double(std::max<std::size_t>(1, parts[p].size()));
        middle[p] = {sum.x / n, sum.y / n, sum.z / n};
        bounds.add(middle[p]);
    }
    std::vector<std::uint32_t> order(parts.size());
    for (std::uint32_t p = 0; p < parts.size(); ++p) order[p] = p;
    std::vector<std::uint64_t> code(parts.size());
    for (std::size_t p = 0; p < parts.size(); ++p) {
        const auto unit = bounds.unit(middle[p]);
        code[p] = morton(unit.x, unit.y, unit.z);
    }

    // A foliage/vegetation mesh is often a collection of disconnected
    // surfaces: branch tubes, needle fans, and authored leaf patches. Shared
    // topology is still the strongest signal, but refusing to group anything
    // without it leaves every spatially separate piece as a singleton parent.
    // Add the nearest spatial neighbour as a weak edge so those pieces can be
    // retopologised together, while connected surface patches retain all their
    // topological neighbours as the primary growth frontier.
    for (std::uint32_t p = 0; p < parts.size(); ++p) {
        std::uint32_t nearest = p;
        double distance = kInfinity;
        for (std::uint32_t q = 0; q < parts.size(); ++q) {
            if (q == p) continue;
            const Vector3 d = middle[q] - middle[p];
            const double candidate = dot(d, d);
            if (candidate < distance) {
                distance = candidate;
                nearest = q;
            }
        }
        // A connected patch already has the right topological frontier. The
        // spatial edge is only the fallback for an actually disconnected
        // part; adding it to every patch perturbs otherwise stable manifold
        // grouping and can create avoidable dominated families.
        if (nearest != p && unique[p].empty()) unique[p].insert(nearest);
    }
    std::vector<std::vector<std::uint32_t>> neighbours(parts.size());
    for (std::size_t p = 0; p < parts.size(); ++p) {
        neighbours[p].assign(unique[p].begin(), unique[p].end());
        std::sort(neighbours[p].begin(), neighbours[p].end());
    }
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return code[a] != code[b] ? code[a] < code[b] : a < b;
    });

    // Few enough left that they all belong together. A group of one has its
    // whole boundary locked and can remove nothing, so a build that kept making
    // them would stall with a dozen middling clusters as roots - and a cut at a
    // coarse allowance would then be the union of all of them, which is how a
    // DAG ends up more expensive than the chain it replaced at distance.
    if (parts.size() <= target) {
        std::vector<std::uint32_t> all(parts.size());
        for (std::uint32_t p = 0; p < parts.size(); ++p) all[p] = p;
        return {all};
    }

    std::vector<char> taken(parts.size(), 0);
    std::vector<std::vector<std::uint32_t>> groups;
    for (const auto seed : order) {
        if (taken[seed]) continue;
        std::vector<std::uint32_t> group{seed};
        taken[seed] = 1;
        std::queue<std::uint32_t> pending;
        pending.push(seed);
        while (!pending.empty() && group.size() < target) {
            const auto p = pending.front();
            pending.pop();
            for (const auto next : neighbours[p]) {
                if (group.size() >= target) break;
                if (taken[next]) continue;
                taken[next] = 1;
                group.push_back(next);
                pending.push(next);
            }
        }
        groups.push_back(std::move(group));
    }
    // A leftover of one joins the nearest group that will have it, for the same
    // reason: alone it can only be a root.
    for (std::size_t g = 0; g < groups.size();) {
        if (groups[g].size() > 1 || groups.size() < 2) { ++g; continue; }
        const auto lonely = groups[g].front();
        std::size_t best = g == 0 ? 1 : 0;
        double nearest = kInfinity;
        for (std::size_t other = 0; other < groups.size(); ++other) {
            if (other == g || groups[other].size() >= target * 2) continue;
            const Vector3 d = middle[groups[other].front()] - middle[lonely];
            if (const double distance = dot(d, d); distance < nearest) {
                nearest = distance;
                best = other;
            }
        }
        if (nearest == kInfinity) { ++g; continue; }
        groups[best].push_back(lonely);
        groups.erase(groups.begin() + std::ptrdiff_t(g));
        if (best < g) continue;
    }
    return groups;
}

std::unordered_set<std::uint64_t> boundarySignature(std::span<const Tri> triangles,
                                                     std::span<const std::uint32_t> geometricVertex) {
    std::unordered_map<std::uint64_t, std::uint32_t> uses;
    for (const auto& triangle : triangles)
        for (int edge = 0; edge < 3; ++edge)
            ++uses[geometricEdgeKey(geometricVertex, triangle[edge], triangle[(edge + 1) % 3])];
    std::unordered_set<std::uint64_t> boundary;
    for (const auto& [edge, count] : uses)
        if (count != 2) boundary.insert(edge);
    return boundary;
}

}   // namespace

std::size_t ClusterDag::trianglesAt(std::size_t level) const {
    std::size_t total = 0;
    for (const auto& cluster : clusters)
        if (cluster.level == level) total += cluster.indices.count / 3;
    return total;
}

std::vector<std::uint32_t> weldPositions(std::span<const float> positions) {
    return weldPositions(positions, {});
}

double surfaceDeviation(std::span<const float> positions, std::span<const std::uint32_t> samples,
                        std::span<const std::uint32_t> target) {
    if (positions.size() < 3 || positions.size() % 3 || samples.size() < 3 || target.size() < 3 ||
        samples.size() % 3 || target.size() % 3)
        return kInfinity;
    const auto vertices = positions.size() / 3;
    const auto faces = [&](std::span<const std::uint32_t> indices) {
        std::vector<Tri> out;
        out.reserve(indices.size() / 3);
        for (std::size_t i = 0; i + 2 < indices.size(); i += 3) out.push_back(
                {indices[i], indices[i + 1], indices[i + 2]});
        return out;
    };
    for (const auto index : samples) if (index >= vertices) return kInfinity;
    for (const auto index : target) if (index >= vertices) return kInfinity;
    return directedSurfaceError(faces(samples), faces(target), positions);
}

std::vector<std::uint32_t> weldPositions(std::span<const float> positions,
                                         std::span<const std::uint64_t> attributeKeys) {
    if (positions.size() < 3 || positions.size() % 3) return {};
    const bool keyed = attributeKeys.size() == positions.size() / 3;
    std::unordered_map<std::uint64_t, std::uint32_t> seen;
    std::vector<std::uint32_t> remap(positions.size() / 3, 0);
    std::uint32_t next = 0;
    for (std::uint32_t v = 0; v < remap.size(); ++v) {
        const float* p = &positions[std::size_t(v) * 3];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return {};
        std::uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < 3; ++i) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &p[i], 4);
            if (bits == 0x80000000u) bits = 0;   // minus zero is zero
            key = (key ^ bits) * 1099511628211ull;
        }
        if (keyed) key = (key ^ attributeKeys[v]) * 1099511628211ull;
        const auto [entry, fresh] = seen.try_emplace(key, next);
        if (fresh) ++next;
        remap[v] = entry->second;
    }
    return remap;
}

ClusterDag buildClusterDag(std::span<const float> positions, std::span<const std::uint32_t> indices,
                           const ClusterDagOptions& options) {
    ClusterDag dag;
    if (positions.size() < 3 || positions.size() % 3 || indices.size() < 3 || indices.size() % 3 ||
        options.clusterTriangles == 0 || options.groupClusters == 0 ||
        !(options.survival > 0 && options.survival < 1))
        return dag;

    // Weld first. Two vertices at the same place with different UVs are one
    // point of geometry, and a build that treated them as two would leave a
    // seam that no amount of locked boundary could close.
    const auto vertices = positions.size() / 3;
    if (std::any_of(positions.begin(), positions.end(), [](float v) { return !std::isfinite(v); }))
        return dag;
    auto remap = options.preserveSourceVertices ? std::vector<std::uint32_t>(vertices)
                                               : weldPositions(positions, options.attributeKeys);
    if (options.preserveSourceVertices)
        for (std::uint32_t v = 0; v < vertices; ++v) remap[v] = v;
    if (remap.empty()) return dag;
    for (std::uint32_t v = 0; v < vertices; ++v)
        if (remap[v] == dag.sourceVertex.size()) {
            dag.positions.insert(dag.positions.end(), &positions[std::size_t(v) * 3],
                                 &positions[std::size_t(v) * 3] + 3);
            dag.sourceVertex.push_back(v);
        }

    std::vector<Tri> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices || indices[i + 1] >= vertices || indices[i + 2] >= vertices)
            return {};
        const Tri t{remap[indices[i]], remap[indices[i + 1]], remap[indices[i + 2]]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
            ++dag.degenerate;
            continue;
        }
        triangles.push_back(t);
    }
    if (triangles.empty()) return dag;

    std::vector<std::uint32_t> all(triangles.size());
    for (std::uint32_t i = 0; i < triangles.size(); ++i) all[i] = i;
    auto parts = partition(triangles, all, dag.positions, options.clusterTriangles);
    std::vector<double> errorOf(parts.size(), 0.0);
    std::vector<std::uint32_t> bornOf(parts.size(), MeshCluster::kNoGroup);
    std::uint32_t groups = 0;

    // A source-open edge is part of the silhouette only when it belongs to a
    // surface component that also has interior manifold edges. This keeps the
    // rim of a sheet/trunk intact, while not pinning every triangle of a
    // completely open imported patch (a common representation for foliage or
    // branch cards). Such a component has no interior topology from which an
    // open boundary can be distinguished, so its retopology is allowed to
    // close/rebuild locally at the next representation.
    auto geometricVertex = geometricVertexIds(dag.positions);
    struct EdgeUse { std::uint32_t triangle = 0; };
    std::unordered_map<std::uint64_t, std::vector<EdgeUse>> sourceEdges;
    for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
        for (int e = 0; e < 3; ++e) {
            const auto next = (e + 1) % 3;
            sourceEdges[geometricEdgeKey(geometricVertex, triangles[triangle][e],
                                        triangles[triangle][next])].push_back({triangle});
        }
    std::vector<std::uint32_t> component(geometricVertex.size());
    for (std::uint32_t vertex = 0; vertex < component.size(); ++vertex) component[vertex] = vertex;
    const auto findComponent = [&](std::uint32_t vertex) {
        while (component[vertex] != vertex) {
            component[vertex] = component[component[vertex]];
            vertex = component[vertex];
        }
        return vertex;
    };
    const auto joinComponents = [&](std::uint32_t a, std::uint32_t b) {
        a = findComponent(a);
        b = findComponent(b);
        if (a != b) component[b] = a;
    };
    for (const auto& [edge, uses] : sourceEdges) {
        const auto a = std::uint32_t(edge >> 32), b = std::uint32_t(edge & 0xffffffffu);
        joinComponents(a, b);
    }
    std::unordered_map<std::uint32_t, std::size_t> componentTriangles;
    for (const auto& triangle : triangles)
        ++componentTriangles[findComponent(geometricVertex[triangle[0]])];
    std::unordered_set<std::uint32_t> manifoldComponents;
    for (const auto& [edge, uses] : sourceEdges)
        if (uses.size() == 2 &&
            componentTriangles[findComponent(std::uint32_t(edge >> 32))] >= options.clusterTriangles)
            manifoldComponents.insert(findComponent(std::uint32_t(edge >> 32)));
    std::unordered_set<std::uint64_t> sourceSilhouette;
    for (const auto& [edge, uses] : sourceEdges)
        if (uses.size() != 2 &&
            manifoldComponents.count(findComponent(std::uint32_t(edge >> 32))))
            sourceSilhouette.insert(edge);

    for (std::size_t level = 0; level < options.maxLevels && !parts.empty(); ++level) {
        dag.levels = level + 1;
        // Publish this level before simplifying it: a cluster's parent error is
        // not known until the group that replaces it has been built.
        std::vector<std::uint32_t> published(parts.size());
        for (std::size_t p = 0; p < parts.size(); ++p) {
            MeshCluster cluster;
            cluster.indices.first = std::uint32_t(dag.indices.size());
            Bounds box;
            for (const auto t : parts[p]) {
                for (const auto v : triangles[t]) {
                    dag.indices.push_back(v);
                    box.add(at(dag.positions, v));
                }
            }
            cluster.indices.count = std::uint32_t(dag.indices.size()) - cluster.indices.first;
            const Vector3 middle{(box.low.x + box.high.x) * 0.5, (box.low.y + box.high.y) * 0.5,
                                 (box.low.z + box.high.z) * 0.5};
            cluster.centre[0] = float(middle.x);
            cluster.centre[1] = float(middle.y);
            cluster.centre[2] = float(middle.z);
            // Enclose the box around the STORED centre, which may have rounded.
            const Vector3 reach{
                std::max(box.high.x - cluster.centre[0], cluster.centre[0] - box.low.x),
                std::max(box.high.y - cluster.centre[1], cluster.centre[1] - box.low.y),
                std::max(box.high.z - cluster.centre[2], cluster.centre[2] - box.low.z)};
            cluster.radius = upperFloat(length(reach));
            cluster.error = upperFloat(errorOf[p]);
            cluster.parentError = std::numeric_limits<float>::infinity();
            cluster.level = std::uint32_t(level);
            cluster.bornOf = bornOf[p];
            published[p] = std::uint32_t(dag.clusters.size());
            dag.clusters.push_back(cluster);
        }
        // Do not publish replacement errors for a level we will never store.
        // Otherwise a large allowance drops these clusters without any parents
        // to replace them. A build limit affects storage, never coverage.
        if (level + 1 == options.maxLevels) break;

        // Even a single cluster may simplify further, but its open rims and
        // imported seams remain locked. Disconnected leaf cards can stay exact
        // roots forever; there is no target requiring them to become a shell.

        const auto gathered = gather(parts, triangles, dag.positions, options.groupClusters);
        std::vector<Tri> coarser;
        std::vector<std::vector<std::uint32_t>> nextParts;
        std::vector<double> nextErrors;
        std::vector<std::uint32_t> nextBorn;
        // `allEdges` is rebuilt for the current representation because a
        // collapse changes which triangles are inside/outside a parent group.
        std::unordered_map<std::uint64_t, std::vector<EdgeUse>> allEdges;
        for (std::uint32_t triangle = 0; triangle < triangles.size(); ++triangle)
            for (int e = 0; e < 3; ++e) {
                const auto next = (e + 1) % 3;
                allEdges[geometricEdgeKey(geometricVertex, triangles[triangle][e],
                                          triangles[triangle][next])].push_back(
                        {triangle});
            }
        // Loop-invariant across groups: it maps a vertex to the hard material
        // key of the SOURCE vertex it came from, which no amount of grouping
        // changes. Rebuilding and re-zeroing it per group cost one pass over
        // every vertex per group, which is the whole vertex count squared over
        // a level - measurable as seconds once a region aggregate is large.
        std::vector<std::uint64_t> hardBoundary(dag.positions.size(), 0);
        if (options.hardBoundaryKeys.size() == positions.size() / 3)
            for (std::uint32_t vertex = 0; vertex < dag.sourceVertex.size(); ++vertex)
                hardBoundary[vertex] = options.hardBoundaryKeys[dag.sourceVertex[vertex]];
        // How many triangle corners of THIS level land on each geometric
        // vertex. A group's own corners are counted per group below, and a
        // vertex is shared with another branch exactly when the two counts
        // differ. That replaces a scan of every triangle per group - the
        // dominant cost of the build - with one scan for the whole level.
        std::vector<std::uint32_t> cornerUses(component.size(), 0);
        for (const auto& triangle : triangles)
            for (const auto vertex : triangle) ++cornerUses[geometricVertex[vertex]];
        std::vector<std::uint32_t> insideUses(component.size(), 0);
        std::vector<std::uint32_t> touchedGeometry;
        for (const auto& group : gathered) {
            std::vector<Tri> merged;
            double inherited = 0;
            std::unordered_set<std::uint32_t> groupTriangles;
            for (const auto p : group) {
                for (const auto t : parts[p]) {
                    merged.push_back(triangles[t]);
                    groupTriangles.insert(t);
                }
                inherited = std::max(inherited, errorOf[p]);
            }
            // Lock the boundary between this parent group and triangles that
            // remain in another branch. Keep the original open rim too: it is
            // part of the source silhouette, not an incidental split between
            // hierarchy branches. Attribute-split copies of one geometric edge
            // remain locked only when their hard material identity differs.
            std::unordered_map<std::uint64_t, std::vector<std::array<std::uint64_t, 2>>> insideEdges;
            for (const auto t : groupTriangles)
                for (int e = 0; e < 3; ++e) {
                    const auto next = (e + 1) % 3;
                    const auto geometric=geometricEdgeKey(geometricVertex,triangles[t][e],triangles[t][next]);
                    auto material = std::array<std::uint64_t, 2>{hardBoundary[triangles[t][e]],
                                                                  hardBoundary[triangles[t][next]]};
                    if (material[1] < material[0]) std::swap(material[0], material[1]);
                    insideEdges[geometric].push_back(material);
                }
            std::vector<std::uint64_t> boundaryEdges;
            // Corners this group contributes, so that a vertex shared with a
            // triangle outside it shows up as a shortfall against the level's
            // total. `touchedGeometry` keeps the reset proportional to the
            // group rather than to the whole level.
            touchedGeometry.clear();
            for (const auto triangle : groupTriangles)
                for (const auto vertex : triangles[triangle]) {
                    const auto geometric = geometricVertex[vertex];
                    if (!insideUses[geometric]) touchedGeometry.push_back(geometric);
                    ++insideUses[geometric];
                }
            const auto sharedOutside = [&](std::uint32_t geometric) {
                return cornerUses[geometric] > insideUses[geometric];
            };
            for (auto& [geometric, materials] : insideEdges) {
                bool outside = false;
                for (const auto use : allEdges[geometric])
                    if (!groupTriangles.count(use.triangle)) { outside = true; break; }
                const auto componentOf = findComponent(std::uint32_t(geometric >> 32));
                const bool currentOpen = allEdges[geometric].size() != 2 &&
                                         manifoldComponents.count(componentOf) != 0;
                const bool sourceOpen = sourceSilhouette.count(geometric) != 0;
                std::sort(materials.begin(), materials.end());
                materials.erase(std::unique(materials.begin(), materials.end()), materials.end());
                const bool hardMaterialSeam = materials.size() > 1;
                if (outside || currentOpen || sourceOpen || hardMaterialSeam)
                    boundaryEdges.push_back(geometric);
            }
            std::unordered_set<std::uint32_t> edgeBoundaryGeometry;
            for (const auto edge : boundaryEdges) {
                edgeBoundaryGeometry.insert(std::uint32_t(edge >> 32));
                edgeBoundaryGeometry.insert(std::uint32_t(edge & 0xffffffffu));
            }
            std::unordered_set<std::uint32_t> boundaryVerticesSet;
            for (const auto triangle : groupTriangles)
                for (const auto vertex : triangles[triangle])
                    if (sharedOutside(geometricVertex[vertex]) &&
                        !edgeBoundaryGeometry.count(geometricVertex[vertex]))
                        boundaryVerticesSet.insert(vertex);
            // Last read of the counters; clear them here rather than at the end
            // of the body, which the root and signature rejections below skip.
            for (const auto geometric : touchedGeometry) insideUses[geometric] = 0;
            std::vector<std::uint32_t> boundaryVertices(boundaryVerticesSet.begin(),
                                                         boundaryVerticesSet.end());
            const auto target = std::size_t(double(merged.size()) * options.survival);
            const auto simplified =
                    simplifyLocked(merged, dag.positions, std::max<std::size_t>(1, target),
                                   options.conservativeError, boundaryEdges, boundaryVertices,
                                   geometricVertex);
            if (simplified.triangles.size() >= merged.size()) continue;   // nothing moved: a root
            // The local link/fold checks protect each collapse, but the group
            // is the actual watertightness contract. Reject any result whose
            // open-edge signature differs from the externally visible rim;
            // internal cluster seams are allowed to disappear during
            // retopology, while a new external edge is never acceptable.
            // Imported open vegetation components intentionally allow their
            // internal cut fan to be retopologised; the source silhouette is
            // still locked by boundaryEdges. Closed/manifold components get
            // the stronger whole-signature check used by the mixed-cut gate.
            bool touchesSourceSilhouette = false;
            for (const auto edge : boundaryEdges)
                if (sourceSilhouette.count(edge)) { touchesSourceSilhouette = true; break; }
            if (!touchesSourceSilhouette &&
                boundarySignature(merged, geometricVertex) !=
                boundarySignature(simplified.triangles, geometricVertex))
                continue;

            const double error = std::max(inherited, simplified.error);
            const auto group_ = groups++;
            // A replacement with the same measured error is still a valid
            // representation, but an interval cut needs a strict boundary:
            // otherwise the finer cluster becomes permanently dominated and
            // the crossover can leave a hole exactly at that error. Keep the
            // conservative estimate and add only the next representable float
            // when rounding made the two levels equal.
            float parentError = upperFloat(error);
            for (const auto p : group)
                parentError = std::max(parentError,
                    std::nextafter(dag.clusters[published[p]].error,
                                   std::numeric_limits<float>::infinity()));
            for (const auto p : group) {
                dag.clusters[published[p]].parentError = parentError;
                dag.clusters[published[p]].replacedBy = group_;
            }
            // QEM is allowed to move interior survivors. Give every moved
            // parent-group vertex a real DAG vertex id so the position is
            // carried into later levels and, eventually, into the sidecar's
            // cluster-local morph targets. Boundary vertices are locked and
            // therefore remain shared with neighbouring groups.
            std::unordered_map<std::uint32_t, std::uint32_t> moved;
            for (const auto& [oldVertex, targetPosition] : simplified.positions) {
                if (oldVertex >= dag.sourceVertex.size()) continue;
                const auto originalPosition = at(dag.positions, oldVertex);
                const auto delta = targetPosition - originalPosition;
                if (length(delta) <= 1e-12) continue;
                const auto fresh = std::uint32_t(dag.sourceVertex.size());
                moved.emplace(oldVertex, fresh);
                dag.positions.push_back(float(targetPosition.x));
                dag.positions.push_back(float(targetPosition.y));
                dag.positions.push_back(float(targetPosition.z));
                dag.sourceVertex.push_back(dag.sourceVertex[oldVertex]);
                // A moved survivor is a private geometric point of this
                // replacement group. It must not accidentally become a source
                // silhouette/material seam or be looked up past the DSU.
                const auto freshGeometric = std::uint32_t(component.size());
                geometricVertex.push_back(freshGeometric);
                component.push_back(freshGeometric);
            }
            const auto remapMoved = [&](Tri triangle) {
                for (auto& vertex : triangle) {
                    const auto found = moved.find(vertex);
                    if (found != moved.end()) vertex = found->second;
                }
                return triangle;
            };
            std::vector<Tri> movedTriangles;
            movedTriangles.reserve(simplified.triangles.size());
            for (const auto& triangle : simplified.triangles)
                movedTriangles.push_back(remapMoved(triangle));

            const auto first = std::uint32_t(coarser.size());
            coarser.insert(coarser.end(), movedTriangles.begin(), movedTriangles.end());
            std::vector<std::uint32_t> subset(movedTriangles.size());
            for (std::uint32_t i = 0; i < subset.size(); ++i) subset[i] = first + i;
            for (auto& part : partition(coarser, subset, dag.positions, options.clusterTriangles)) {
                nextParts.push_back(std::move(part));
                // The strict crossover bound is part of the representation's
                // measured error. This matters when spatial fallback groups
                // branches that arrived with slightly different child
                // errors: otherwise the replacement names one error while
                // the produced cluster stores another, and the hierarchy cut
                // has an interval that belongs to neither side.
                nextErrors.push_back(double(parentError));
                nextBorn.push_back(group_);
            }
        }
        if (nextParts.empty()) {
            dag.converged = true;
            break;   // every group was a root: nothing left to coarsen
        }
        triangles = std::move(coarser);
        parts = std::move(nextParts);
        errorOf = std::move(nextErrors);
        bornOf = std::move(nextBorn);
    }
    for (const auto& cluster : dag.clusters)
        if (double(cluster.error) >= double(cluster.parentError)) ++dag.dominated;
    return dag;
}

std::vector<std::uint32_t> cutAt(const ClusterDag& dag, double allowance) {
    std::vector<std::uint32_t> chosen;
    for (std::uint32_t i = 0; i < dag.clusters.size(); ++i) {
        const auto& cluster = dag.clusters[i];
        if (double(cluster.error) <= allowance && double(cluster.parentError) > allowance)
            chosen.push_back(i);
    }
    return chosen;
}

bool cutAtHierarchy(std::span<const MeshCluster> clusters, double allowance,
                    std::vector<std::uint32_t>& chosen) {
    chosen.clear();
    if (!std::isfinite(allowance) || allowance < 0) return false;

    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> children;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> parents;
    bool hasFamilies = false;
    for (std::uint32_t i = 0; i < clusters.size(); ++i) {
        const auto& cluster = clusters[i];
        if (cluster.replacedBy != MeshCluster::kNoGroup) {
            children[cluster.replacedBy].push_back(i);
            hasFamilies = true;
        }
        if (cluster.bornOf != MeshCluster::kNoGroup) {
            parents[cluster.bornOf].push_back(i);
            hasFamilies = true;
        }
    }
    if (!hasFamilies) return false;
    for (const auto& [family, members] : children)
        if (members.empty() || !parents.count(family) || parents[family].empty()) return false;
    for (const auto& [family, members] : parents)
        if (members.empty() || !children.count(family) || children[family].empty()) return false;

    std::vector<std::uint32_t> roots;
    for (std::uint32_t i = 0; i < clusters.size(); ++i)
        if (std::isinf(clusters[i].parentError)) roots.push_back(i);
    if (roots.empty()) return false;

    std::unordered_set<std::uint32_t> expanded;
    std::unordered_set<std::uint32_t> visiting;
    const auto visitFamily = [&](auto&& self, std::uint32_t family) -> bool {
        if (expanded.count(family)) return true;
        if (!visiting.insert(family).second) return false;
        const auto it = children.find(family);
        if (it == children.end() || it->second.empty()) return false;
        bool complete = true;
        for (const auto child : it->second) {
            const auto& cluster = clusters[child];
            if (cluster.error <= allowance) {
                chosen.push_back(child);
                continue;
            }
            if (cluster.bornOf == MeshCluster::kNoGroup ||
                !self(self, cluster.bornOf)) {
                complete = false;
                break;
            }
        }
        visiting.erase(family);
        if (complete) expanded.insert(family);
        return complete;
    };
    const auto visitRoot = [&](std::uint32_t root) {
        const auto& cluster = clusters[root];
        if (cluster.error <= allowance) {
            chosen.push_back(root);
            return true;
        }
        if (cluster.bornOf == MeshCluster::kNoGroup) return false;
        return visitFamily(visitFamily, cluster.bornOf);
    };

    for (const auto root : roots) {
        if (!visitRoot(root)) {
            chosen.clear();
            return false;
        }
    }
    std::sort(chosen.begin(), chosen.end());
    return true;
}

} // namespace engine::geometry
