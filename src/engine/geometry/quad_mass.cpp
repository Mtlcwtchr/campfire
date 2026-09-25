#include "engine/geometry/quad_mass.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

#include "engine/geometry/cluster_dag.hpp"

namespace engine::geometry {
namespace {
constexpr double kInfinity = std::numeric_limits<double>::infinity();

struct Point {
    double x = 0, y = 0, z = 0;
};
Point operator-(const Point& a, const Point& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Point operator+(const Point& a, const Point& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Point operator*(const Point& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
Point cross(const Point& a, const Point& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double dot(const Point& a, const Point& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double length(const Point& a) { return std::sqrt(dot(a, a)); }
Point corner(const Quad& quad, int i) {
    return {quad.corner[i][0], quad.corner[i][1], quad.corner[i][2]};
}
bool sane(const Quad& quad) {
    for (int i = 0; i < 4; ++i)
        for (int axis = 0; axis < 3; ++axis)
            if (!std::isfinite(quad.corner[i][axis])) return false;
    return std::isfinite(quad.coverage) && quad.coverage > 0;
}

// The grid the density lives on. Samples, not cells: a cell is eight samples,
// and the surface is extracted from cells.
struct Grid {
    Point low;
    double step = 1;
    int nx = 0, ny = 0, nz = 0;
    std::vector<float> density;
    // Material IDs are categorical. Empty space filled for topology contributes
    // no material; averaging numeric IDs would invent unrelated texture layers.
    std::vector<float> materials;
    std::vector<std::vector<float>> materialDensity;
    [[nodiscard]] std::size_t at(int i, int j, int k) const {
        return (std::size_t(k) * ny + j) * nx + i;
    }
    [[nodiscard]] float sample(int i, int j, int k) const {
        if (i < 0 || j < 0 || k < 0 || i >= nx || j >= ny || k >= nz) return 0;
        return density[at(i, j, k)];
    }
    [[nodiscard]] Point pointOf(double i, double j, double k) const {
        return {low.x + i * step, low.y + j * step, low.z + k * step};
    }
};

// Spread one sample of leaf area over the eight grid samples around it, by how
// close it is to each. A nearest-sample splat gives a field with the grid's own
// staircase in it, and the surface then has the staircase too.
void splat(Grid& grid, const Point& p, double weight, std::size_t material) {
    const double fx = (p.x - grid.low.x) / grid.step;
    const double fy = (p.y - grid.low.y) / grid.step;
    const double fz = (p.z - grid.low.z) / grid.step;
    const int i = int(std::floor(fx)), j = int(std::floor(fy)), k = int(std::floor(fz));
    const double tx = fx - i, ty = fy - j, tz = fz - k;
    for (int dk = 0; dk < 2; ++dk)
        for (int dj = 0; dj < 2; ++dj)
            for (int di = 0; di < 2; ++di) {
                const int x = i + di, y = j + dj, z = k + dk;
                if (x < 0 || y < 0 || z < 0 || x >= grid.nx || y >= grid.ny || z >= grid.nz)
                    continue;
                const double w = (di ? tx : 1 - tx) * (dj ? ty : 1 - ty) * (dk ? tz : 1 - tz);
                const auto cell = grid.at(x, y, z);
                grid.density[cell] += float(weight * w);
                grid.materialDensity[material][cell] += float(weight * w);
            }
}
}

std::vector<Quad> cardsOf(std::span<const float> positions, std::span<const std::uint32_t> indices,
                          std::vector<std::array<std::uint32_t, 4>>* corners) {
    std::vector<Quad> cards;
    if (positions.size() < 3 || positions.size() % 3 || indices.size() < 3 || indices.size() % 3)
        return cards;
    const auto vertices = std::uint32_t(positions.size() / 3);
    const auto welded = weldPositions(positions);
    if (welded.empty()) return cards;

    // Which triangles hang together, over welded vertices - a card split by an
    // importer for its UVs is one card, not two.
    std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> byEdge;
    std::vector<std::array<std::uint32_t, 3>> faces;
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertices || indices[i + 1] >= vertices || indices[i + 2] >= vertices)
            return {};
        const std::array<std::uint32_t, 3> t{welded[indices[i]], welded[indices[i + 1]],
                                             welded[indices[i + 2]]};
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        const auto index = std::uint32_t(faces.size());
        faces.push_back(t);
        for (int e = 0; e < 3; ++e) {
            const auto a = t[e], b = t[(e + 1) % 3];
            byEdge[a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a]
                    .push_back(index);
        }
    }

    if (corners) corners->clear();
    // The source vertex behind each welded one, so a caller can ask what a card
    // is made of and not only where it is.
    std::vector<std::uint32_t> origin(welded.size() ? *std::max_element(welded.begin(), welded.end()) + 1 : 0, 0);
    for (std::uint32_t v = 0; v < welded.size(); ++v) origin[welded[v]] = v;
    std::vector<char> seen(faces.size(), 0);
    for (std::uint32_t f = 0; f < faces.size(); ++f) {
        if (seen[f]) continue;
        // Walk the WHOLE component, and keep it only if it is exactly two.
        //
        // Not "walk until it is clearly too big": abandoning a component
        // half-walked leaves its remaining triangles marked as visited but
        // unclaimed, and the next sweep reads pieces of one large shell as
        // separate little cards. A pine with no cards at all came out with a
        // crown that way.
        std::vector<std::uint32_t> part{f};
        seen[f] = 1;
        for (std::size_t at = 0; at < part.size(); ++at)
            for (int e = 0; e < 3; ++e) {
                const auto a = faces[part[at]][e], b = faces[part[at]][(e + 1) % 3];
                for (const auto next :
                     byEdge[a < b ? (std::uint64_t(a) << 32) | b : (std::uint64_t(b) << 32) | a])
                    if (!seen[next]) {
                        seen[next] = 1;
                        part.push_back(next);
                    }
            }
        if (part.size() != 2) continue;

        // The four distinct corners, in an order that walks the quad's rim:
        // the shared edge's two ends, with each triangle's odd vertex between.
        std::array<std::uint32_t, 3> a = faces[part[0]], b = faces[part[1]];
        std::uint32_t shared[2]{}, count = 0, oddA = 0, oddB = 0;
        for (const auto v : a) {
            if (std::find(b.begin(), b.end(), v) != b.end()) {
                if (count < 2) shared[count] = v;
                ++count;
            } else {
                oddA = v;
            }
        }
        if (count != 2) continue;
        for (const auto v : b)
            if (v != shared[0] && v != shared[1]) oddB = v;
        const std::uint32_t order[4]{shared[0], oddA, shared[1], oddB};
        Quad quad;
        for (int i = 0; i < 4; ++i)
            for (int axis = 0; axis < 3; ++axis)
                quad.corner[i][axis] = positions[std::size_t(origin[order[i]]) * 3 + axis];
        cards.push_back(quad);
        if (corners)
            corners->push_back({origin[order[0]], origin[order[1]], origin[order[2]],
                                origin[order[3]]});
    }
    return cards;
}

QuadMass mergeQuadMass(std::span<const Quad> quads, const QuadMassOptions& options) {
    QuadMass result;
    if (quads.empty() || !(options.cellMetres > 0) || !(options.solidDensity > 0) ||
        options.maxSamples < 64)
        return result;

    Point low{kInfinity, kInfinity, kInfinity}, high{-kInfinity, -kInfinity, -kInfinity};
    for (const Quad& quad : quads) {
        if (!sane(quad)) continue;
        ++result.quads;
        for (int i = 0; i < 4; ++i) {
            const Point p = corner(quad, i);
            low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
    }
    if (result.quads == 0) return result;

    // One cell of margin either side, so the mass is enclosed rather than cut
    // off at the box: a shell that ends at the bounding box is not closed, and
    // an open shell is a hole seen from the other side.
    Grid grid;
    grid.step = options.cellMetres;
    for (int attempt = 0; attempt < 24; ++attempt) {
        grid.low = {low.x - grid.step * 2, low.y - grid.step * 2, low.z - grid.step * 2};
        grid.nx = int(std::ceil((high.x - low.x) / grid.step)) + 5;
        grid.ny = int(std::ceil((high.y - low.y) / grid.step)) + 5;
        grid.nz = int(std::ceil((high.z - low.z) / grid.step)) + 5;
        if (std::size_t(grid.nx) * grid.ny * grid.nz <= options.maxSamples) break;
        grid.step *= 2;
        result.coarsened = true;
    }
    if (std::size_t(grid.nx) * grid.ny * grid.nz > options.maxSamples) return {};
    result.cellMetres = grid.step;
    grid.density.assign(std::size_t(grid.nx) * grid.ny * grid.nz, 0.0f);
    for (const auto& quad : quads)
        if (sane(quad) && std::isfinite(quad.layer)) grid.materials.push_back(quad.layer);
    std::sort(grid.materials.begin(), grid.materials.end());
    grid.materials.erase(std::unique(grid.materials.begin(), grid.materials.end()), grid.materials.end());
    if (grid.materials.empty()) return {};
    grid.materialDensity.assign(grid.materials.size(), std::vector<float>(grid.density.size(), 0.0f));

    // Every quad's own area, spread over the grid. Sampled on a lattice half a
    // cell across, so a card smaller than a cell still lands in it and a card
    // many cells across lands in all of them.
    const double cellVolume = grid.step * grid.step * grid.step;
    for (const Quad& quad : quads) {
        if (!sane(quad) || !std::isfinite(quad.layer)) continue;
        const auto material = std::size_t(std::lower_bound(grid.materials.begin(), grid.materials.end(),
                                                         quad.layer) - grid.materials.begin());
        const Point a = corner(quad, 0), b = corner(quad, 1), c = corner(quad, 2),
                    d = corner(quad, 3);
        const double across = std::max(length(b - a), length(c - d));
        const double along = std::max(length(d - a), length(c - b));
        const int steps = std::max(1, int(std::ceil(std::max(across, along) / (grid.step * 0.5))));
        const double area = 0.5 * (length(cross(b - a, c - a)) + length(cross(c - a, d - a)));
        // Leaf area per lattice point, as a fraction of a cell's volume: a
        // density, so the threshold means the same thing at every cell size.
        const double weight = area * double(quad.coverage) / (double(steps + 1) * (steps + 1)) *
                              grid.step / cellVolume;
        for (int v = 0; v <= steps; ++v)
            for (int u = 0; u <= steps; ++u) {
                const double s = double(u) / steps, t = double(v) / steps;
                const Point top = a + (b - a) * s;
                const Point bottom = d + (c - d) * s;
                splat(grid, top + (bottom - top) * t, weight, material);
            }
    }

    const double threshold = options.solidDensity;
    // What the outside can reach, walked from the border over everything below
    // the threshold. Whatever is left below it and unreached is inside the
    // mass, and filling it is what stops a crown being drawn twice.
    if (options.fillCavities) {
        std::vector<char> reached(grid.density.size(), 0);
        std::vector<std::size_t> pending;
        const auto offer = [&](int i, int j, int k) {
            if (i < 0 || j < 0 || k < 0 || i >= grid.nx || j >= grid.ny || k >= grid.nz) return;
            const auto at = grid.at(i, j, k);
            if (reached[at] || grid.density[at] >= threshold) return;
            reached[at] = 1;
            pending.push_back(at);
        };
        for (int k = 0; k < grid.nz; ++k)
            for (int j = 0; j < grid.ny; ++j)
                for (int i = 0; i < grid.nx; ++i)
                    if (i == 0 || j == 0 || k == 0 || i + 1 == grid.nx || j + 1 == grid.ny ||
                        k + 1 == grid.nz)
                        offer(i, j, k);
        for (std::size_t head = 0; head < pending.size(); ++head) {
            const auto at = pending[head];
            const int i = int(at % grid.nx);
            const int j = int((at / grid.nx) % grid.ny);
            const int k = int(at / (std::size_t(grid.nx) * grid.ny));
            offer(i - 1, j, k); offer(i + 1, j, k);
            offer(i, j - 1, k); offer(i, j + 1, k);
            offer(i, j, k - 1); offer(i, j, k + 1);
        }
        for (std::size_t at = 0; at < grid.density.size(); ++at)
            if (!reached[at] && grid.density[at] < threshold) {
                // Comfortably above rather than exactly at it: the threshold is
                // a double and the field is floats, so a cell set to the
                // threshold reads as below it and the fill does nothing at all.
                grid.density[at] = float(threshold * 1.5);
                ++result.filledCells;
            }
    }

    // Surface nets. One vertex per cell that straddles the threshold, at the
    // average of the crossings on its twelve edges.
    const auto inside = [&](int i, int j, int k) { return grid.sample(i, j, k) >= threshold; };
    std::vector<std::uint32_t> cellVertex(std::size_t(grid.nx) * grid.ny * grid.nz, 0xffffffffu);
    static constexpr int kCorner[8][3]{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0},
                                       {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    static constexpr int kEdge[12][2]{{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3},
                                      {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k + 1 < grid.nz; ++k)
        for (int j = 0; j + 1 < grid.ny; ++j)
            for (int i = 0; i + 1 < grid.nx; ++i) {
                double value[8];
                bool solid[8];
                int inCount = 0;
                for (int c = 0; c < 8; ++c) {
                    value[c] = grid.sample(i + kCorner[c][0], j + kCorner[c][1], k + kCorner[c][2]);
                    solid[c] = value[c] >= threshold;
                    inCount += solid[c];
                }
                if (inCount == 0 || inCount == 8) continue;
                Point sum;
                int crossings = 0;
                for (const auto& edge : kEdge) {
                    if (solid[edge[0]] == solid[edge[1]]) continue;
                    const double a = value[edge[0]], b = value[edge[1]];
                    const double t = std::abs(b - a) > 1e-12
                                             ? std::clamp((threshold - a) / (b - a), 0.0, 1.0)
                                             : 0.5;
                    const Point p0 = grid.pointOf(i + kCorner[edge[0]][0], j + kCorner[edge[0]][1],
                                                  k + kCorner[edge[0]][2]);
                    const Point p1 = grid.pointOf(i + kCorner[edge[1]][0], j + kCorner[edge[1]][1],
                                                  k + kCorner[edge[1]][2]);
                    sum = sum + (p0 + (p1 - p0) * t);
                    ++crossings;
                }
                if (!crossings) continue;
                cellVertex[grid.at(i, j, k)] = std::uint32_t(result.positions.size() / 3);
                const Point p = sum * (1.0 / crossings);
                result.positions.insert(result.positions.end(),
                                        {float(p.x), float(p.y), float(p.z)});
                ++result.solidCells;
            }
    if (result.positions.empty()) return result;

    // One quad per grid edge whose ends differ, joining the four cells that
    // share it. The winding follows the direction the surface faces, so the
    // shell is consistently outward without a second pass to work it out.
    const auto emit = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d,
                          bool flip) {
        if (a == 0xffffffffu || b == 0xffffffffu || c == 0xffffffffu || d == 0xffffffffu) return;
        if (flip) {
            result.indices.insert(result.indices.end(), {a, b, c, a, c, d});
        } else {
            result.indices.insert(result.indices.end(), {a, c, b, a, d, c});
        }
    };
    for (int k = 1; k + 1 < grid.nz; ++k)
        for (int j = 1; j + 1 < grid.ny; ++j)
            for (int i = 1; i + 1 < grid.nx; ++i) {
                const bool here = inside(i, j, k);
                if (here != inside(i + 1, j, k))
                    emit(cellVertex[grid.at(i, j - 1, k - 1)], cellVertex[grid.at(i, j, k - 1)],
                         cellVertex[grid.at(i, j, k)], cellVertex[grid.at(i, j - 1, k)], here);
                if (here != inside(i, j + 1, k))
                    emit(cellVertex[grid.at(i - 1, j, k - 1)], cellVertex[grid.at(i, j, k - 1)],
                         cellVertex[grid.at(i, j, k)], cellVertex[grid.at(i - 1, j, k)], !here);
                if (here != inside(i, j, k + 1))
                    emit(cellVertex[grid.at(i - 1, j - 1, k)], cellVertex[grid.at(i, j - 1, k)],
                         cellVertex[grid.at(i, j, k)], cellVertex[grid.at(i - 1, j, k)], here);
            }

    // Averaging towards the neighbours, which is what takes the cell staircase
    // out without moving the silhouette anywhere a viewer would see.
    const auto vertices = result.positions.size() / 3;
    for (int pass = 0; pass < std::max(0, options.relax); ++pass) {
        std::vector<Point> sum(vertices);
        std::vector<int> count(vertices, 0);
        for (std::size_t t = 0; t + 2 < result.indices.size(); t += 3)
            for (int e = 0; e < 3; ++e) {
                const auto a = result.indices[t + e], b = result.indices[t + (e + 1) % 3];
                sum[a] = sum[a] + Point{result.positions[std::size_t(b) * 3],
                                        result.positions[std::size_t(b) * 3 + 1],
                                        result.positions[std::size_t(b) * 3 + 2]};
                ++count[a];
            }
        for (std::size_t v = 0; v < vertices; ++v) {
            if (!count[v]) continue;
            for (int axis = 0; axis < 3; ++axis) {
                const double mine = result.positions[v * 3 + axis];
                const double theirs = (axis == 0 ? sum[v].x : axis == 1 ? sum[v].y : sum[v].z) /
                                      count[v];
                result.positions[v * 3 + axis] = float(mine + (theirs - mine) * 0.5);
            }
        }
    }

    // Normals from the field's own gradient, and coverage from its value: how
    // solid the mass is where this vertex sits, which is what keeps a lacy
    // crown lacy once it has become one surface.
    result.normals.assign(vertices * 3, 0.0f);
    result.coverage.assign(vertices, 0.0f);
    result.layer.assign(vertices, 0.0f);
    for (std::size_t v = 0; v < vertices; ++v) {
        const double fx = (result.positions[v * 3] - grid.low.x) / grid.step;
        const double fy = (result.positions[v * 3 + 1] - grid.low.y) / grid.step;
        const double fz = (result.positions[v * 3 + 2] - grid.low.z) / grid.step;
        const int i = std::clamp(int(std::lround(fx)), 0, grid.nx - 1);
        const int j = std::clamp(int(std::lround(fy)), 0, grid.ny - 1);
        const int k = std::clamp(int(std::lround(fz)), 0, grid.nz - 1);
        Point n{grid.sample(i - 1, j, k) - grid.sample(i + 1, j, k),
                grid.sample(i, j - 1, k) - grid.sample(i, j + 1, k),
                grid.sample(i, j, k - 1) - grid.sample(i, j, k + 1)};
        const double size = length(n);
        if (size > 1e-12) n = n * (1.0 / size); else n = {0, 0, 1};
        result.normals[v * 3] = float(n.x);
        result.normals[v * 3 + 1] = float(n.y);
        result.normals[v * 3 + 2] = float(n.z);
        // Coverage is measured over the neighbourhood, not at the vertex. The
        // vertex sits ON the level set by construction, so the density there is
        // the threshold for every mass however dense - which reads as "one" for
        // a lacy crown and a solid one alike. What differs between them is how
        // much leaf is packed just inside, and that is what a shader needs to
        // know to keep a lacy crown lacy.
        double around = 0;
        for (int dk = -1; dk <= 1; ++dk)
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) around += grid.sample(i + di, j + dj, k + dk);
        result.coverage[v] = float(std::clamp(around / 27.0 / (threshold * 3), 0.0, 1.0));
        double strongest = -1;
        for (std::size_t material = 0; material < grid.materials.size(); ++material) {
            double weight = 0;
            for (int dk = -1; dk <= 1; ++dk)
                for (int dj = -1; dj <= 1; ++dj)
                    for (int di = -1; di <= 1; ++di) {
                        const int x = i + di, y = j + dj, z = k + dk;
                        if (x >= 0 && y >= 0 && z >= 0 && x < grid.nx && y < grid.ny && z < grid.nz)
                            weight += grid.materialDensity[material][grid.at(x, y, z)];
                    }
            if (weight > strongest) {
                strongest = weight;
                result.layer[v] = grid.materials[material];
            }
        }
    }
    return result;
}

} // namespace engine::geometry
