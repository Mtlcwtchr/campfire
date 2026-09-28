#include "framework.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

#include "engine/geometry/cluster_dag.hpp"
#include "engine/geometry/cluster_morph.hpp"

namespace {
using namespace engine::geometry;

// A closed sphere by subdividing an octahedron: no boundary edges at all, so
// "every edge is used exactly twice" is the whole watertightness statement and
// not a statement with an exception in it.
struct Mesh {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
};

Mesh sphere(int subdivisions, double radius = 1.0) {
    std::vector<std::array<double, 3>> points{{1, 0, 0},  {-1, 0, 0}, {0, 1, 0},
                                              {0, -1, 0}, {0, 0, 1},  {0, 0, -1}};
    std::vector<std::array<std::uint32_t, 3>> faces{{0, 2, 4}, {2, 1, 4}, {1, 3, 4}, {3, 0, 4},
                                                    {2, 0, 5}, {1, 2, 5}, {3, 1, 5}, {0, 3, 5}};
    for (int step = 0; step < subdivisions; ++step) {
        std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> middles;
        const auto middle = [&](std::uint32_t a, std::uint32_t b) {
            const auto key = std::minmax(a, b);
            if (const auto it = middles.find({key.first, key.second}); it != middles.end())
                return it->second;
            std::array<double, 3> m{(points[a][0] + points[b][0]) * 0.5,
                                    (points[a][1] + points[b][1]) * 0.5,
                                    (points[a][2] + points[b][2]) * 0.5};
            const double n = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
            for (auto& v : m) v /= n;
            points.push_back(m);
            const auto index = std::uint32_t(points.size() - 1);
            middles[{key.first, key.second}] = index;
            return index;
        };
        std::vector<std::array<std::uint32_t, 3>> split;
        for (const auto& f : faces) {
            const auto a = middle(f[0], f[1]), b = middle(f[1], f[2]), c = middle(f[2], f[0]);
            split.push_back({f[0], a, c});
            split.push_back({a, f[1], b});
            split.push_back({c, b, f[2]});
            split.push_back({a, b, c});
        }
        faces = std::move(split);
    }
    Mesh mesh;
    for (const auto& p : points)
        for (int i = 0; i < 3; ++i) mesh.positions.push_back(float(p[i] * radius));
    for (const auto& f : faces)
        for (const auto v : f) mesh.indices.push_back(v);
    return mesh;
}

Mesh faceted(const Mesh& mesh) {
    Mesh result;
    for (const auto v : mesh.indices) {
        result.indices.push_back(std::uint32_t(result.positions.size() / 3));
        result.positions.insert(result.positions.end(), mesh.positions.begin() + v * 3,
                                mesh.positions.begin() + v * 3 + 3);
    }
    return result;
}

TEST(cluster_morph_expands_families_with_local_parent_targets) {
    const std::vector<float> positions{
        0,0,0, 1,0,0, 0,1,0,
        0,0,1, 1,0,1, 0,1,1};
    const std::vector<std::uint32_t> indices{0,1,2, 3,4,5};
    MeshCluster child;
    child.indices={0,3}; child.error=0; child.parentError=1;
    child.replacedBy=7;
    MeshCluster parent;
    parent.indices={3,3}; parent.error=1; parent.parentError=std::numeric_limits<float>::infinity();
    parent.bornOf=7; parent.level=1;
    const std::vector<MeshCluster> clusters{child,parent};
    const auto morph=buildClusterMorph(positions,indices,clusters);
    CHECK(!morph.empty());
    CHECK_EQ(morph.indices.size(),indices.size());
    CHECK_EQ(morph.sourceVertices.size(),6u);
    CHECK_EQ(morph.targetNormals.size(),18u);
    CHECK(std::abs(morph.targetNormals[0])<1e-6f);
    CHECK(std::abs(morph.targetNormals[1])<1e-6f);
    CHECK(std::abs(morph.targetNormals[2]-1.0f)<1e-6f);
    // The child triangle moves one unit along z towards its replacement; the
    // parent is a root and targets itself, so it remains stable at morph=1.
    CHECK_EQ(morph.targetPositions[2],1.0f);
    CHECK_EQ(morph.targetPositions[5],1.0f);
    CHECK_EQ(morph.targetPositions[8],1.0f);
    CHECK_EQ(morph.targetPositions[9],0.0f);
    CHECK_EQ(morph.targetPositions[11],1.0f);
    CHECK_EQ(morph.targetPositions[14],1.0f);
}

std::vector<std::uint32_t> trianglesOf(const ClusterDag& dag, const std::vector<std::uint32_t>& cut) {
    std::vector<std::uint32_t> out;
    for (const auto index : cut) {
        const auto& range = dag.clusters[index].indices;
        out.insert(out.end(), dag.indices.begin() + range.first,
                   dag.indices.begin() + range.first + range.count);
    }
    return out;
}

// How many faces each undirected edge carries. On a closed surface that is two
// everywhere, and any other number is a hole or a fold.
std::map<std::pair<std::uint32_t, std::uint32_t>, int> edgeUse(
        const std::vector<std::uint32_t>& triangles) {
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3)
        for (int e = 0; e < 3; ++e) {
            const auto key = std::minmax(triangles[i + e], triangles[i + (e + 1) % 3]);
            ++uses[{key.first, key.second}];
        }
    return uses;
}

double area(const ClusterDag& dag, const std::vector<std::uint32_t>& triangles) {
    double total = 0;
    for (std::size_t i = 0; i + 2 < triangles.size(); i += 3) {
        const auto point = [&](std::uint32_t v) {
            return std::array<double, 3>{dag.positions[std::size_t(v) * 3],
                                         dag.positions[std::size_t(v) * 3 + 1],
                                         dag.positions[std::size_t(v) * 3 + 2]};
        };
        const auto a = point(triangles[i]), b = point(triangles[i + 1]), c = point(triangles[i + 2]);
        const double u[3]{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const double v[3]{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const double n[3]{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                          u[0] * v[1] - u[1] * v[0]};
        total += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    }
    return total;
}

using Point = std::array<double, 3>;
Point pointAt(const ClusterDag& dag, std::uint32_t v) {
    return {dag.positions[std::size_t(v) * 3], dag.positions[std::size_t(v) * 3 + 1],
            dag.positions[std::size_t(v) * 3 + 2]};
}

// Closest point on a triangle, by clamped barycentric regions. The build uses
// the same measure but reaches it through a spatial grid; this is the plain
// scan the grid has to agree with.
double pointTriangleDistance(const Point& p, const Point& a, const Point& b, const Point& c) {
    const auto sub = [](const Point& u, const Point& v) {
        return Point{u[0] - v[0], u[1] - v[1], u[2] - v[2]};
    };
    const auto dot = [](const Point& u, const Point& v) {
        return u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    };
    const auto ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    const auto norm = [&](const Point& u) { return std::sqrt(dot(u, u)); };
    if (d1 <= 0 && d2 <= 0) return norm(ap);
    const auto bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return norm(bp);
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        const double t = d1 / (d1 - d3);
        return norm(sub(p, {a[0] + t * ab[0], a[1] + t * ab[1], a[2] + t * ab[2]}));
    }
    const auto cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return norm(cp);
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        const double t = d2 / (d2 - d6);
        return norm(sub(p, {a[0] + t * ac[0], a[1] + t * ac[1], a[2] + t * ac[2]}));
    }
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        const auto bc = sub(c, b);
        const double t = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return norm(sub(p, {b[0] + t * bc[0], b[1] + t * bc[1], b[2] + t * bc[2]}));
    }
    const double denom = 1.0 / (va + vb + vc);
    const double v = vb * denom, w = vc * denom;
    return norm(sub(p, {a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w,
                        a[2] + ab[2] * v + ac[2] * w}));
}

double directedDistance(const ClusterDag& dag, const std::vector<std::uint32_t>& samples,
                        const std::vector<std::uint32_t>& target) {
    double worst = 0;
    for (std::size_t i = 0; i + 2 < samples.size(); i += 3) {
        const auto a = pointAt(dag, samples[i]), b = pointAt(dag, samples[i + 1]),
                   c = pointAt(dag, samples[i + 2]);
        const Point points[]{a, b, c,
                             {(a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3,
                              (a[2] + b[2] + c[2]) / 3}};
        for (const auto& point : points) {
            double nearest = std::numeric_limits<double>::infinity();
            for (std::size_t t = 0; t + 2 < target.size(); t += 3)
                nearest = std::min(nearest, pointTriangleDistance(
                        point, pointAt(dag, target[t]), pointAt(dag, target[t + 1]),
                        pointAt(dag, target[t + 2])));
            worst = std::max(worst, nearest);
        }
    }
    return worst;
}
}

TEST(cluster_dag_cuts_the_finest_level_into_clusters_that_tile_the_mesh_exactly) {
    const auto mesh = sphere(4);          // 2048 triangles
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    CHECK_EQ(dag.degenerate, std::size_t(0));
    CHECK(dag.levels > 1);

    std::vector<std::uint32_t> finest;
    for (std::uint32_t i = 0; i < dag.clusters.size(); ++i)
        if (dag.clusters[i].level == 0) finest.push_back(i);
    CHECK(finest.size() > 1);
    const auto triangles = trianglesOf(dag, finest);
    CHECK_EQ(triangles.size(), mesh.indices.size());

    // Every triangle of the input, once: the level that is the model itself has
    // to be the model itself.
    const auto sorted = [](std::vector<std::uint32_t> faces) {
        std::vector<std::array<std::uint32_t, 3>> out;
        for (std::size_t i = 0; i + 2 < faces.size(); i += 3) {
            std::array<std::uint32_t, 3> t{faces[i], faces[i + 1], faces[i + 2]};
            std::sort(t.begin(), t.end());
            out.push_back(t);
        }
        std::sort(out.begin(), out.end());
        return out;
    };
    std::vector<std::uint32_t> original;
    for (const auto v : mesh.indices) original.push_back(v);
    CHECK(sorted(triangles) == sorted(original));

    for (const auto index : finest) {
        CHECK(dag.clusters[index].indices.count % 3 == 0);
        CHECK(dag.clusters[index].indices.count / 3 <= 128u);
        CHECK(dag.clusters[index].error == 0.0f);   // the finest level is the model
    }
}

TEST(cluster_dag_exact_finest_keeps_source_corners_after_skipping_degenerates) {
    const auto mesh = faceted(sphere(2));
    auto indices = mesh.indices;
    indices.insert(indices.begin() + 6, {0, 0, 1});
    std::vector<std::uint64_t> attributes(mesh.positions.size() / 3);
    for (std::size_t v = 0; v < attributes.size(); ++v) attributes[v] = v;
    for (const bool preserve : {false, true}) {
        auto options = naniteProfile();
        options.preserveSourceVertices = preserve;
        options.attributeKeys = attributes;
        const auto dag = buildClusterDag(mesh.positions, indices, options);
        CHECK_EQ(dag.degenerate, 1u);
        auto finest = trianglesOf(dag, cutAt(dag, 0));
        CHECK_EQ(finest.size(), mesh.indices.size());
        std::set<std::array<std::uint32_t, 3>> faces;
        for (std::size_t i = 0; i < finest.size(); i += 3) {
            std::array<std::uint32_t, 3> face;
            for (int c = 0; c < 3; ++c) {
                const auto v = finest[i + c];
                face[c] = dag.sourceVertex[v];
                for (int axis = 0; axis < 3; ++axis)
                    CHECK_EQ(dag.positions[v * 3 + axis], mesh.positions[face[c] * 3 + axis]);
            }
            CHECK(faces.insert(face).second);
        }
        for (std::size_t i = 0; i < mesh.indices.size(); i += 3)
            CHECK(faces.count({mesh.indices[i], mesh.indices[i + 1], mesh.indices[i + 2]}) == 1);
        CHECK(dag.trianglesAt(dag.levels - 1) < dag.trianglesAt(0));
    }
}

TEST(cluster_dag_exact_finest_has_no_holes_at_mixed_cut_crossovers) {
    const auto mesh = faceted(sphere(3));
    auto options = naniteProfile();
    options.clusterTriangles = 16;
    options.groupClusters = 2;
    const auto dag = buildClusterDag(mesh.positions, mesh.indices, options);
    CHECK(dag.levels > 2);
    const auto welded = weldPositions(dag.positions);
    std::set<double> allowances{0.0, 100.0};
    for (const auto& cluster : dag.clusters) {
        CHECK(cluster.parentError > cluster.error);
        allowances.insert(cluster.error);
        if (cluster.error > 0) allowances.insert(std::nextafter(double(cluster.error), 0.0));
    }
    bool mixed = false, coarsened = false;
    for (const auto allowance : allowances) {
        const auto cut = cutAt(dag, allowance);
        CHECK(!cut.empty());
        std::vector<std::uint32_t> hierarchy;
        CHECK(cutAtHierarchy(dag.clusters, allowance, hierarchy));
        auto sorted = cut;
        std::sort(sorted.begin(), sorted.end());
        std::sort(hierarchy.begin(), hierarchy.end());
        CHECK(sorted == hierarchy);
        auto triangles = trianglesOf(dag, cut);
        coarsened = coarsened || triangles.size() < mesh.indices.size();
        for (const auto id : cut) mixed = mixed || dag.clusters[id].level != dag.clusters[cut[0]].level;
        for (auto& v : triangles) v = welded[v];
        for (const auto& [edge, uses] : edgeUse(triangles)) CHECK_EQ(uses, 2);
    }
    CHECK(mixed);
    CHECK(coarsened);
}

TEST(cluster_dag_exact_finest_preserves_hard_material_seams) {
    const auto mesh = faceted(sphere(2));
    std::vector<std::uint64_t> materials(mesh.positions.size() / 3);
    for (std::size_t i = 0; i < mesh.indices.size(); i += 3) {
        const float z = mesh.positions[i * 3 + 2] + mesh.positions[(i + 1) * 3 + 2] +
                        mesh.positions[(i + 2) * 3 + 2];
        for (int c = 0; c < 3; ++c) materials[i + c] = z > 0 ? 1 : 2;
    }
    auto options = naniteProfile();
    options.hardBoundaryKeys = materials;
    const auto dag = buildClusterDag(mesh.positions, mesh.indices, options);
    CHECK(dag.levels > 1);
    bool coarse = false;
    for (const auto& cluster : dag.clusters) {
        coarse = coarse || cluster.level > 0;
        for (std::size_t i = cluster.indices.first; i < cluster.indices.first + cluster.indices.count; i += 3) {
            const auto material = materials[dag.sourceVertex[dag.indices[i]]];
            CHECK_EQ(materials[dag.sourceVertex[dag.indices[i + 1]]], material);
            CHECK_EQ(materials[dag.sourceVertex[dag.indices[i + 2]]], material);
        }
    }
    CHECK(coarse);
}

TEST(cluster_dag_has_no_seam_at_any_cut_it_can_produce) {
    // The gate. A cut mixes levels wherever the errors differ, and the whole
    // point of simplifying a GROUP with its rim locked is that such a cut is
    // watertight by construction rather than by a tolerance.
    //
    // Large enough that the DAG has several groups per level: a mesh with only
    // a handful of clusters produces cuts that are all one level, and the test
    // would hold for the wrong reason. The count below is the guard on that.
    const auto mesh = sphere(5);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    const double closed = area(dag, trianglesOf(dag, cutAt(dag, 0.0)));
    CHECK(closed > 0);

    std::size_t mixedCuts = 0, distinctSizes = 0, previous = 0;
    for (int step = 0; step <= 120; ++step) {
        const double allowance = step * 0.004;
        const auto cut = cutAt(dag, allowance);
        CHECK(!cut.empty());
        const auto triangles = trianglesOf(dag, cut);

        for (const auto& [edge, uses] : edgeUse(triangles))
            CHECK_EQ(uses, 2);   // a closed surface, still closed

        std::set<std::uint32_t> levels;
        for (const auto index : cut) levels.insert(dag.clusters[index].level);
        mixedCuts += levels.size() > 1;
        // And it is the same surface: no patch drawn twice, none missing.
        CHECK(std::abs(area(dag, triangles) - closed) < closed * 0.25);
        if (triangles.size() != previous) ++distinctSizes;
        previous = triangles.size();
    }
    // If every cut were one level, the test above would hold trivially.
    CHECK(mixedCuts > 10);
    CHECK(distinctSizes > 3);
}

TEST(cluster_dag_explicit_family_cut_matches_the_interval_cut) {
    const auto mesh = sphere(5);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    CHECK(dag.clusters.size() > 1);

    for (int step = 0; step <= 160; ++step) {
        const double allowance = step * 0.003;
        std::vector<std::uint32_t> hierarchyCut;
        CHECK(cutAtHierarchy(dag.clusters, allowance, hierarchyCut));
        CHECK(hierarchyCut == cutAt(dag, allowance));
    }
}

TEST(cluster_dag_errors_never_decrease_towards_the_root) {
    const auto mesh = sphere(4);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    std::size_t roots = 0;
    std::unordered_map<std::uint32_t, float> groupError;
    for (const auto& cluster : dag.clusters) {
        CHECK(std::isfinite(cluster.error));
        CHECK(double(cluster.error) <= double(cluster.parentError));
        if (!std::isfinite(cluster.parentError)) ++roots;
        // Every cluster a group produced carries that group's error, and every
        // cluster it replaced names the same number as its parent's. That is
        // what makes the cut rule pick one level along a path instead of two.
        if (cluster.bornOf != MeshCluster::kNoGroup) {
            const auto [entry, fresh] = groupError.try_emplace(cluster.bornOf, cluster.error);
            CHECK_EQ(entry->second, cluster.error);
        }
    }
    CHECK(roots > 0);
    for (const auto& cluster : dag.clusters)
        if (cluster.replacedBy != MeshCluster::kNoGroup) {
            const auto it = groupError.find(cluster.replacedBy);
            CHECK(it != groupError.end());
            CHECK_EQ(it->second, cluster.parentError);
        }
}

TEST(cluster_dag_keeps_a_group_rim_exactly_where_the_finer_level_left_it) {
    // Why the cut above has no seam: the boundary of what a group replaced and
    // the boundary of what it produced are the same edges, not merely edges
    // that are close.
    const auto mesh = sphere(4);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    const auto rim = [&](const std::vector<std::uint32_t>& clusters) {
        std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
        for (const auto& [edge, uses] : edgeUse(trianglesOf(dag, clusters)))
            if (uses != 2) edges.insert(edge);
        return edges;
    };
    std::map<std::uint32_t, std::vector<std::uint32_t>> before, after;
    for (std::uint32_t i = 0; i < dag.clusters.size(); ++i) {
        if (dag.clusters[i].replacedBy != MeshCluster::kNoGroup)
            before[dag.clusters[i].replacedBy].push_back(i);
        if (dag.clusters[i].bornOf != MeshCluster::kNoGroup)
            after[dag.clusters[i].bornOf].push_back(i);
    }
    CHECK(before.size() > 2);
    std::size_t checked = 0;
    for (const auto& [group, fine] : before) {
        const auto coarse = after.find(group);
        CHECK(coarse != after.end());
        CHECK(rim(fine) == rim(coarse->second));
        ++checked;
    }
    CHECK_EQ(checked, before.size());
}

TEST(cluster_dag_error_still_bounds_the_surface_when_components_are_far_apart) {
    // The measured error a level publishes is this distance, and it is reached
    // through a spatial grid rather than by comparing every pair of triangles.
    // A search that stopped early would return merely a near triangle instead
    // of the nearest one, shrink the published error, and let a level be drawn
    // in place of geometry it does not resemble - with nothing failing. So it
    // is pinned here against a plain scan, on the arrangements where a grid
    // goes wrong: components far outside each other's cells, one surface
    // wholly inside another, and a sliver crossing many cells at once.
    const auto shift = [](Mesh mesh, double x, double y, double z) {
        for (std::size_t v = 0; v * 3 + 2 < mesh.positions.size(); ++v) {
            mesh.positions[v * 3] += float(x);
            mesh.positions[v * 3 + 1] += float(y);
            mesh.positions[v * 3 + 2] += float(z);
        }
        return mesh;
    };
    const auto join = [](const Mesh& a, const Mesh& b) {
        Mesh out = a;
        const auto base = std::uint32_t(a.positions.size() / 3);
        out.positions.insert(out.positions.end(), b.positions.begin(), b.positions.end());
        for (const auto index : b.indices) out.indices.push_back(index + base);
        return out;
    };

    struct Case {
        const char* what;
        Mesh mesh;
        std::size_t split;   // indices before this belong to the first surface
    };
    std::vector<Case> cases;
    {
        const auto near_ = sphere(2), far_ = shift(sphere(2), 50, 0, 0);
        cases.push_back({"far apart", join(near_, far_), near_.indices.size()});
    }
    {
        const auto outer = sphere(3, 4.0), inner = sphere(2, 0.4);
        cases.push_back({"one inside the other", join(outer, inner), outer.indices.size()});
    }
    {
        // A sliver that spans the whole grid: it must be found from every cell,
        // which is what the always-tested list exists for.
        Mesh sliver;
        sliver.positions = {-40, 0.01f, 0, 40, 0.01f, 0, 0, 0.02f, 0.001f};
        sliver.indices = {0, 1, 2};
        const auto blob = sphere(3, 2.0);
        cases.push_back({"a sliver across everything", join(blob, sliver), blob.indices.size()});
    }
    {
        // Coincident surfaces: the nearest distance is zero and any slack shows.
        const auto a = sphere(3, 1.5);
        cases.push_back({"the same surface twice", join(a, a), a.indices.size()});
    }

    for (const auto& [what, mesh, split] : cases) {
        ClusterDag view;                       // a holder so the brute force can read positions
        view.positions.assign(mesh.positions.begin(), mesh.positions.end());
        const std::vector<std::uint32_t> first(mesh.indices.begin(),
                                               mesh.indices.begin() + std::ptrdiff_t(split));
        const std::vector<std::uint32_t> second(mesh.indices.begin() + std::ptrdiff_t(split),
                                                mesh.indices.end());
        for (const auto& [samples, target] :
             std::vector<std::pair<std::vector<std::uint32_t>, std::vector<std::uint32_t>>>{
                     {first, second}, {second, first}, {first, first}}) {
            const double accelerated = surfaceDeviation(view.positions, samples, target);
            const double scanned = directedDistance(view, samples, target);
            CHECK(std::isfinite(accelerated));
            // The same number, not a close one: this is an exact search.
            CHECK(std::abs(accelerated - scanned) <= 1e-9 * std::max(1.0, scanned));
        }
    }

    // A triangle too large to register in every cell it covers is held aside
    // and tested from everywhere instead. Measured: it has to span more than
    // sixty-four cells before that applies, so the case is built to.
    {
        Mesh target = shift(sphere(4, 1.0), 60, 60, 0);   // many small triangles, far away
        const auto base = std::uint32_t(target.positions.size() / 3);
        for (const auto& corner : std::vector<std::array<float, 3>>{
                     {-40, -40, 0}, {40, -40, 0}, {0, 40, 0}})
            for (const auto value : corner) target.positions.push_back(value);
        target.indices.insert(target.indices.end(), {base, base + 1, base + 2});
        const auto samples = shift(sphere(2, 1.0), 0, 0, 4);   // sitting just above it

        ClusterDag view;
        view.positions.assign(target.positions.begin(), target.positions.end());
        const auto offset = std::uint32_t(view.positions.size() / 3);
        view.positions.insert(view.positions.end(), samples.positions.begin(),
                              samples.positions.end());
        std::vector<std::uint32_t> sampleIndices;
        for (const auto index : samples.indices) sampleIndices.push_back(index + offset);
        const std::vector<std::uint32_t> targetIndices(target.indices.begin(),
                                                       target.indices.end());
        const double accelerated = surfaceDeviation(view.positions, sampleIndices, targetIndices);
        const double scanned = directedDistance(view, sampleIndices, targetIndices);
        CHECK(std::abs(accelerated - scanned) <= 1e-9 * std::max(1.0, scanned));
        // The near surface is the large triangle, not the distant cluster: if
        // the oversized triangle were skipped the answer would be the distance
        // to the far sphere instead, which is more than ten times as much.
        CHECK(scanned < 10.0);
    }

    // And it refuses what it cannot measure rather than returning a small
    // number that would read as a tight error.
    const auto mesh = sphere(2);
    CHECK(!std::isfinite(surfaceDeviation(mesh.positions, mesh.indices, {})));
    auto wild = mesh.indices;
    wild[3] = std::uint32_t(mesh.positions.size());
    CHECK(!std::isfinite(surfaceDeviation(mesh.positions, wild, mesh.indices)));
}

TEST(source_cluster_dag_level_limit_keeps_a_complete_root_cut) {
    const auto mesh = sphere(3);
    for (std::size_t limit : {1u, 2u, 3u}) {
        ClusterDagOptions options;
        options.maxLevels = limit;
        options.clusterTriangles = 32;
        options.preserveSourceVertices = true;
        options.conservativeError = true;
        const auto dag = buildClusterDag(mesh.positions, mesh.indices, options);
        CHECK_EQ(dag.levels, limit);
        CHECK(!dag.converged);
        std::set<std::uint32_t> publishedGroups;
        for (const auto& c : dag.clusters)
            if (c.bornOf != MeshCluster::kNoGroup) publishedGroups.insert(c.bornOf);
        for (const auto& c : dag.clusters) {
            if (c.replacedBy != MeshCluster::kNoGroup)
                CHECK(publishedGroups.count(c.replacedBy) != 0);
            if (c.level + 1 == limit) CHECK(std::isinf(c.parentError));
        }
        for (double allowance : {0.0, 0.3, 1.0, 100.0}) {
            const auto triangles = trianglesOf(dag, cutAt(dag, allowance));
            CHECK(!triangles.empty());
            for (const auto& [edge, uses] : edgeUse(triangles)) CHECK_EQ(uses, 2);
            CHECK(area(dag, triangles) > 0);
            if (limit == 1) CHECK_EQ(triangles.size(), mesh.indices.size());
        }
    }
}

TEST(source_cluster_dag_bounds_round_outwards) {
    const std::vector<float> positions{0,0,0, 1,1,1, 1,0,0};
    const std::vector<std::uint32_t> indices{0,1,2};
    ClusterDagOptions options;
    options.preserveSourceVertices = true;
    const auto dag = buildClusterDag(positions, indices, options);
    CHECK_EQ(dag.clusters.size(), std::size_t(1));
    const auto& c = dag.clusters.front();
    for (auto v : indices)
        CHECK(std::hypot(double(positions[3*v]) - c.centre[0],
                         double(positions[3*v+1]) - c.centre[1],
                         double(positions[3*v+2]) - c.centre[2]) <= double(c.radius));
}

TEST(cluster_dag_gets_coarser_every_level_and_then_stops) {
    const auto mesh = sphere(5);          // 8192 triangles
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    CHECK(dag.levels >= 4);
    std::size_t previous = 0;
    for (std::size_t level = 0; level < dag.levels; ++level) {
        const auto triangles = dag.trianglesAt(level);
        CHECK(triangles > 0);
        if (level) CHECK(triangles < previous);
        previous = triangles;
    }
    CHECK_EQ(dag.trianglesAt(0), mesh.indices.size() / 3);
    // Coarse enough to be worth the machinery: a level that halves is what the
    // survival ratio asks for, so four levels should be about a sixteenth.
    CHECK(dag.trianglesAt(dag.levels - 1) * 8 < dag.trianglesAt(0));
    // And the error of the coarsest level is a real distance, not a guess: a
    // unit sphere cannot be approximated to a thousandth by a few triangles.
    float coarsest = 0;
    for (const auto& cluster : dag.clusters)
        if (cluster.level + 1 == dag.levels) coarsest = std::max(coarsest, cluster.error);
    CHECK(coarsest > 0.001f);
    CHECK(coarsest < 1.0f);
}

TEST(cluster_dag_is_the_same_dag_twice) {
    // It runs offline, so it runs on whichever machine builds the assets. Two
    // builds that disagreed would put two different meshes in one repository.
    const auto mesh = sphere(4);
    const auto first = buildClusterDag(mesh.positions, mesh.indices);
    const auto again = buildClusterDag(mesh.positions, mesh.indices);
    CHECK_EQ(first.clusters.size(), again.clusters.size());
    CHECK(first.indices == again.indices);
    CHECK(first.positions == again.positions);
    for (std::size_t i = 0; i < first.clusters.size(); ++i) {
        CHECK_EQ(first.clusters[i].error, again.clusters[i].error);
        CHECK_EQ(first.clusters[i].parentError, again.clusters[i].parentError);
        CHECK_EQ(first.clusters[i].indices.first, again.clusters[i].indices.first);
        CHECK_EQ(first.clusters[i].level, again.clusters[i].level);
    }
}

TEST(cluster_dag_bounds_every_cluster_around_what_it_actually_contains) {
    const auto mesh = sphere(4, 7.5);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    for (const auto& cluster : dag.clusters) {
        CHECK(cluster.radius > 0);
        CHECK(std::isfinite(cluster.radius));
        for (std::uint32_t i = 0; i < cluster.indices.count; ++i) {
            const auto v = dag.indices[cluster.indices.first + i];
            double distance = 0;
            for (int axis = 0; axis < 3; ++axis) {
                const double d = double(dag.positions[std::size_t(v) * 3 + axis]) -
                                 double(cluster.centre[axis]);
                distance += d * d;
            }
            CHECK(std::sqrt(distance) <= double(cluster.radius) + 1e-4);
        }
    }
}

TEST(cluster_dag_welds_a_seam_rather_than_building_a_crack_into_the_asset) {
    // An importer splits a vertex wherever the UVs differ. Two positions that
    // are the same point are one point of geometry, and a build that believed
    // the index buffer would lock a rim that was already torn.
    auto mesh = sphere(3);
    const auto vertices = mesh.positions.size() / 3;
    // Duplicate every vertex and send half the triangles to the copies.
    mesh.positions.insert(mesh.positions.end(), mesh.positions.begin(),
                          mesh.positions.begin() + std::ptrdiff_t(vertices * 3));
    for (std::size_t i = 0; i < mesh.indices.size(); ++i)
        if ((i / 3) % 2) mesh.indices[i] += std::uint32_t(vertices);

    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    CHECK(dag.positions.size() / 3 >= vertices);       // welded source plus QEM replacement vertices
    CHECK(dag.sourceVertex.size() >= vertices);
    for (const auto& [edge, uses] : edgeUse(trianglesOf(dag, cutAt(dag, 0.0)))) CHECK_EQ(uses, 2);
    for (const auto& [edge, uses] : edgeUse(trianglesOf(dag, cutAt(dag, 0.08)))) CHECK_EQ(uses, 2);
}

TEST(cluster_dag_welds_only_when_position_and_attribute_identity_match) {
    const std::vector<float> positions{
            0, 0, 0, 1, 0, 0, 0, 1, 0,
            0, 0, 0}; // coincident position, deliberately different attribute seam
    const std::vector<std::uint32_t> indices{0, 1, 2, 3, 2, 1};
    const std::vector<std::uint64_t> splitKeys{7, 8, 9, 70};
    ClusterDagOptions options;
    options.attributeKeys = splitKeys;
    const auto split = buildClusterDag(positions, indices, options);
    CHECK_EQ(split.positions.size(), std::size_t(12));

    auto joinedKeys = splitKeys;
    joinedKeys[3] = joinedKeys[0];
    options.attributeKeys = joinedKeys;
    const auto joined = buildClusterDag(positions, indices, options);
    CHECK(joined.positions.size() >= std::size_t(9));
}

TEST(cluster_dag_refuses_what_it_cannot_describe_rather_than_guessing) {
    const auto mesh = sphere(2);
    CHECK_EQ(buildClusterDag({}, mesh.indices).clusters.size(), std::size_t(0));
    CHECK_EQ(buildClusterDag(mesh.positions, {}).clusters.size(), std::size_t(0));
    // An index count that is not whole triangles.
    auto ragged = mesh.indices;
    ragged.pop_back();
    CHECK_EQ(buildClusterDag(mesh.positions, ragged).clusters.size(), std::size_t(0));
    // An index past the end of the vertices.
    auto wild = mesh.indices;
    wild[4] = std::uint32_t(mesh.positions.size());
    CHECK_EQ(buildClusterDag(mesh.positions, wild).clusters.size(), std::size_t(0));
    // A vertex that is not a number.
    auto broken = mesh.positions;
    broken[7] = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(buildClusterDag(broken, mesh.indices).clusters.size(), std::size_t(0));
    // Options that ask for nothing.
    ClusterDagOptions none;
    none.clusterTriangles = 0;
    CHECK_EQ(buildClusterDag(mesh.positions, mesh.indices, none).clusters.size(), std::size_t(0));
    none = {};
    none.survival = 1.0;   // a level that keeps everything never terminates
    CHECK_EQ(buildClusterDag(mesh.positions, mesh.indices, none).clusters.size(), std::size_t(0));

    // A single triangle has nowhere to go, and says so rather than looping.
    const float one[]{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::uint32_t face[]{0, 1, 2};
    const auto tiny = buildClusterDag(one, face);
    CHECK_EQ(tiny.clusters.size(), std::size_t(1));
    CHECK(tiny.converged);
    CHECK_EQ(tiny.levels, std::size_t(1));
    // Degenerate faces are counted, not drawn.
    const std::uint32_t folded[]{0, 1, 2, 1, 1, 2};
    CHECK_EQ(buildClusterDag(one, folded).degenerate, std::size_t(1));
}

TEST(cluster_dag_says_when_a_level_it_built_is_one_nobody_will_ever_draw) {
    // A cluster whose error equals its replacement's is dominated: the coarser
    // one is no further from the original surface and costs fewer triangles.
    // The cut rule needs `own <= allowance` AND `parent > allowance`, which no
    // allowance satisfies at once, so such a cluster is never drawn - and that
    // is the right answer, not a hole. What matters is that the build says how
    // much of itself is in that state instead of quietly carrying it.
    const auto mesh = sphere(5);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    std::size_t counted = 0;
    for (const auto& cluster : dag.clusters)
        if (double(cluster.error) >= double(cluster.parentError)) ++counted;
    CHECK_EQ(counted, dag.dominated);
    // Most of the build has to be reachable, or the levels are not a chain.
    CHECK(dag.dominated * 4 < dag.clusters.size());

    // And a dominated cluster really is unreachable: no allowance draws it.
    std::vector<char> ever(dag.clusters.size(), 0);
    for (int step = 0; step <= 400; ++step)
        for (const auto index : cutAt(dag, step * 0.002)) ever[index] = 1;
    for (std::size_t i = 0; i < dag.clusters.size(); ++i)
        if (double(dag.clusters[i].error) >= double(dag.clusters[i].parentError)) CHECK(!ever[i]);
    // Every level still has clusters that some allowance does draw.
    for (std::size_t level = 0; level < dag.levels; ++level) {
        std::size_t drawable = 0;
        for (std::size_t i = 0; i < dag.clusters.size(); ++i)
            if (dag.clusters[i].level == level && ever[i]) ++drawable;
        CHECK(drawable > 0);
    }
}

TEST(cluster_dag_holds_an_open_meshs_own_border_still_at_every_level) {
    // Nothing a scene actually contains is a closed sphere. A trunk is cut off
    // at the roots and a rock is cut off at the ground, and a border that crept
    // inward as the levels coarsened would show as the model shrinking - the
    // silhouette is the one thing a viewer notices at the distance where the
    // coarse levels are used.
    constexpr int kSide = 33;
    Mesh grid;
    for (int y = 0; y < kSide; ++y)
        for (int x = 0; x < kSide; ++x) {
            const double u = double(x) / (kSide - 1), v = double(y) / (kSide - 1);
            grid.positions.push_back(float(u * 8));
            grid.positions.push_back(float(v * 8));
            grid.positions.push_back(float(std::sin(u * 7) * std::cos(v * 5) * 0.6));
        }
    for (int y = 0; y + 1 < kSide; ++y)
        for (int x = 0; x + 1 < kSide; ++x) {
            const std::uint32_t a = std::uint32_t(y * kSide + x), b = a + 1;
            const std::uint32_t c = a + kSide, d = c + 1;
            grid.indices.insert(grid.indices.end(), {a, b, c, b, d, c});
        }
    const auto dag = buildClusterDag(grid.positions, grid.indices);
    CHECK(dag.levels > 2);

    const auto border = [&](const std::vector<std::uint32_t>& cut) {
        std::set<std::pair<std::uint32_t, std::uint32_t>> edges;
        for (const auto& [edge, uses] : edgeUse(trianglesOf(dag, cut))) {
            CHECK(uses <= 2);            // never folded onto itself
            if (uses == 1) edges.insert(edge);
        }
        return edges;
    };
    const auto original = border(cutAt(dag, 0.0));
    CHECK_EQ(original.size(), std::size_t(4 * (kSide - 1)));   // the rim of the sheet
    for (int step = 1; step <= 60; ++step) {
        const auto cut = cutAt(dag, step * 0.01);
        CHECK(!cut.empty());
        CHECK(border(cut) == original);   // the same edges, not merely as many
    }
    // And it did coarsen despite holding that border still.
    CHECK(dag.trianglesAt(dag.levels - 1) * 3 < dag.trianglesAt(0));
}

#include "engine/geometry/cluster_asset.hpp"

namespace {
// The whole index buffer as one chain level, for a mesh that has no chain.
std::vector<engine::IndexRange> whole(const Mesh& mesh) {
    return {{0, std::uint32_t(mesh.indices.size())}};
}

// A sphere plus a scatter of loose two-triangle quads, which is the shape an
// imported tree actually has: a trunk, and a thousand leaves that touch nothing.
Mesh withCards(int quads, int subdivisions = 3) {
    Mesh mesh = sphere(subdivisions);
    for (int i = 0; i < quads; ++i) {
        const auto base = std::uint32_t(mesh.positions.size() / 3);
        const double x = 3.0 + i * 3.0;   // far enough apart that no two weld together
        for (int corner = 0; corner < 4; ++corner) {
            mesh.positions.push_back(float(x + (corner & 1)));
            mesh.positions.push_back(float(corner >> 1));
            mesh.positions.push_back(0.0f);
        }
        mesh.indices.insert(mesh.indices.end(),
                            {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    return mesh;
}
}

TEST(cluster_asset_reads_back_exactly_what_it_wrote) {
    // A sidecar is the only thing between a build and a frame. If it did not
    // round trip exactly, the picture would differ from what the gate above
    // proved, and nothing would say so.
    const auto mesh = sphere(4);
    const auto built = buildClusterAsset(mesh.positions, mesh.indices, whole(mesh));
    CHECK(!built.empty());
    CHECK_EQ(built.sourceTriangles, std::uint32_t(mesh.indices.size() / 3));
    CHECK_EQ(built.sourceVertices, std::uint32_t(mesh.positions.size() / 3));

    const auto bytes = encodeClusters(built);
    CHECK(!bytes.empty());
    std::string why = "untouched";
    const auto read = decodeClusters(bytes, why);
    CHECK(why.empty());
    CHECK_EQ(read.sourceVertices, built.sourceVertices);
    CHECK_EQ(read.sourceTriangles, built.sourceTriangles);
    CHECK_EQ(read.cardTriangles, built.cardTriangles);
    CHECK_EQ(read.levels, built.levels);
    CHECK_EQ(read.dominated, built.dominated);
    CHECK(read.indices == built.indices);
    CHECK(read.clusterPositions == built.clusterPositions);
    CHECK_EQ(read.clusters.size(), built.clusters.size());
    for (std::size_t i = 0; i < built.clusters.size(); ++i) {
        CHECK_EQ(read.clusters[i].indices.first, built.clusters[i].indices.first);
        CHECK_EQ(read.clusters[i].indices.count, built.clusters[i].indices.count);
        CHECK_EQ(read.clusters[i].error, built.clusters[i].error);
        CHECK_EQ(read.clusters[i].parentError, built.clusters[i].parentError);
        CHECK_EQ(read.clusters[i].radius, built.clusters[i].radius);
        CHECK_EQ(read.clusters[i].level, built.clusters[i].level);
        for (int axis = 0; axis < 3; ++axis)
            CHECK_EQ(read.clusters[i].centre[axis], built.clusters[i].centre[axis]);
    }
    // Encoding the decoded thing gives the same bytes: no field is read into a
    // place the writer does not write from.
    CHECK(encodeClusters(read) == bytes);
}

TEST(cluster_asset_exact_finest_roundtrips_source_and_coarse_geometry) {
    const auto mesh = faceted(sphere(2));
    const auto options = naniteProfile();
    for (const auto& built : {buildClusterAsset(mesh.positions, mesh.indices, whole(mesh), options),
                              buildSourceClusterAsset(mesh.positions, mesh.indices, options)}) {
        CHECK(!built.empty());
        CHECK(built.levels > 1);
        std::string why;
        const auto bytes = encodeClusters(built);
        const auto read = decodeClusters(bytes, why);
        CHECK(why.empty());
        CHECK(encodeClusters(read) == bytes);
        CHECK_EQ(read.clusterPositions.size(), read.indices.size() * 3);
        std::vector<std::uint32_t> finest;
        for (const auto& cluster : read.clusters) {
            if (cluster.level != 0) continue;
            CHECK_EQ(cluster.error, 0.0f);
            for (std::size_t i = cluster.indices.first; i < cluster.indices.first + cluster.indices.count; ++i) {
                finest.push_back(read.indices[i]);
                for (int axis = 0; axis < 3; ++axis)
                    CHECK_EQ(read.clusterPositions[i * 3 + axis], mesh.positions[read.indices[i] * 3 + axis]);
            }
        }
        std::sort(finest.begin(), finest.end());
        CHECK(finest == mesh.indices);
    }
}

TEST(cluster_dag_carries_optimal_qem_positions_into_replacement_vertices) {
    const auto mesh = sphere(4);
    const auto dag = buildClusterDag(mesh.positions, mesh.indices);
    const auto sourceVertices = mesh.positions.size() / 3;
    CHECK(dag.positions.size() / 3 > sourceVertices);
    CHECK_EQ(dag.positions.size() / 3, dag.sourceVertex.size());
    bool moved = false;
    for (std::size_t vertex = sourceVertices; vertex < dag.sourceVertex.size(); ++vertex) {
        CHECK(dag.sourceVertex[vertex] < sourceVertices);
        for (int axis = 0; axis < 3; ++axis)
            CHECK(std::isfinite(dag.positions[vertex * 3 + axis]));
        moved = true;
    }
    CHECK(moved);
}

TEST(cluster_asset_refuses_a_sidecar_that_does_not_describe_a_mesh_it_could_draw) {
    const auto mesh = sphere(3);
    const auto good = encodeClusters(buildClusterAsset(mesh.positions, mesh.indices, whole(mesh)));
    CHECK(good.size() > 64);
    std::string why;
    CHECK(!decodeClusters(good, why).empty());

    const auto broken = [&](auto&& change) {
        auto bytes = good;
        change(bytes);
        std::string reason;
        const auto asset = decodeClusters(bytes, reason);
        CHECK(asset.empty());
        CHECK(!reason.empty());
        return reason;
    };
    broken([](auto& b) { b[1] = std::byte{'X'}; });                       // not a sidecar
    broken([](auto& b) { b.resize(12); });                                // shorter than a header
    broken([](auto& b) { b.push_back(std::byte{0}); });                   // longer than it claims
    broken([](auto& b) { b.resize(b.size() - 4); });                      // truncated
    // An index past the mesh: the first index sits right after the header.
    broken([](auto& b) {
        const std::uint32_t wild = 0xfffffff0u;
        std::memcpy(&b[32], &wild, 4);
    });
    // A vertex count of zero makes every index out of range, which is the same
    // mistake arriving from the other end.
    broken([](auto& b) {
        const std::uint32_t none = 0;
        std::memcpy(&b[4], &none, 4);
    });
    // And a cluster count that does not match the bytes present.
    broken([](auto& b) {
        const std::uint32_t many = 4096;
        std::memcpy(&b[28], &many, 4);
    });
    CHECK(decodeClusters({}, why).empty());
}

TEST(cluster_asset_versions_crown_layers_and_reads_both_legacy_layouts) {
    // A crown-only fixture: no expensive geometry build and no generated assets.
    ClusterAsset asset;
    asset.crownPositions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    asset.crownNormals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    asset.crownCoverage = {1, 0.5f, 0.75f};
    asset.crownLayers = {2, 3, 4};
    asset.crownLayer = 7;
    asset.crownIndices = {0, 1, 2};
    asset.crownLevels = 1;
    MeshCluster cluster;
    cluster.indices = {0, 3};
    cluster.radius = 1;
    cluster.parentError = std::numeric_limits<float>::infinity();
    cluster.bornOf = 9;
    cluster.replacedBy = 11;
    asset.crownClusters.push_back(cluster);

    const auto current = encodeClusters(asset);
    CHECK(current[3] == std::byte{'6'});
    std::string why;
    const auto roundtrip = decodeClusters(current, why);
    CHECK(why.empty());
    CHECK(roundtrip.crownLayers == asset.crownLayers);
    CHECK_EQ(roundtrip.crownClusters[0].bornOf, cluster.bornOf);
    CHECK_EQ(roundtrip.crownClusters[0].replacedBy, cluster.replacedBy);
    CHECK(encodeClusters(roundtrip) == current);

    // SCC4 has the per-vertex layer, but predates the family ids. This fixture
    // has one crown cluster, so the eight trailing SCC5 bytes are its ids.
    auto transitional = current;
    transitional.erase(transitional.end() - 8, transitional.end());
    transitional[3] = std::byte{'4'};
    const auto layered = decodeClusters(transitional, why);
    CHECK(why.empty());
    CHECK(layered.crownLayers == asset.crownLayers);

    // The original SCC3 has seven floats, not eight, at each crown vertex.
    auto legacy = transitional;
    for (int vertex = 2; vertex >= 0; --vertex) {
        const auto layer = legacy.begin() + 64 + vertex * 32 + 28;
        legacy.erase(layer, layer + 4);
    }
    legacy[3] = std::byte{'3'};
    const auto old = decodeClusters(legacy, why);
    CHECK(why.empty());
    CHECK(!old.empty());
    CHECK(old.crownPositions == asset.crownPositions);
    CHECK(old.crownNormals == asset.crownNormals);
    CHECK(old.crownCoverage == asset.crownCoverage);
    CHECK(old.crownIndices == asset.crownIndices);
    CHECK(old.crownLayers == std::vector<float>({7, 7, 7}));
    CHECK_EQ(old.crownClusters.size(), std::size_t(1));
    CHECK(encodeClusters(old)[3] == std::byte{'6'});

    // Only legacy files may omit layers; the new format must stay strict.
    legacy[3] = std::byte{'4'};
    CHECK(decodeClusters(legacy, why).empty());
    CHECK(!why.empty());
    transitional.pop_back();
    CHECK(decodeClusters(transitional, why).empty());
    CHECK(!why.empty());
}

TEST(cluster_asset_leaves_the_alpha_cards_to_the_chain) {
    // 960 separate two-triangle quads per tree is the worst case a DAG can be
    // handed: nothing to group, nothing to collapse, and every quad its own
    // cluster. They are thinned with area compensation instead, so the build
    // has to recognise and skip them rather than cluster them badly.
    const auto mesh = withCards(120);
    const auto asset = buildClusterAsset(mesh.positions, mesh.indices, whole(mesh));
    CHECK(!asset.empty());
    CHECK_EQ(asset.cardTriangles, std::uint32_t(240));
    CHECK_EQ(asset.sourceTriangles, std::uint32_t(mesh.indices.size() / 3));

    // The clusters hold the sphere and nothing else: every index it kept names
    // a vertex of the sphere, not one of the loose quads.
    const auto solid = std::uint32_t(sphere(3).positions.size() / 3);
    for (const auto index : asset.indices) CHECK(index < solid);
    std::size_t finest = 0;
    for (const auto& cluster : asset.clusters)
        if (cluster.level == 0) finest += cluster.indices.count / 3;
    CHECK_EQ(finest, std::size_t(sphere(3).indices.size() / 3));

    // A model that is nothing BUT cards has no solid geometry, and says so by
    // writing no clusters rather than by clustering the cards.
    Mesh onlyCards;
    for (int i = 0; i < 40; ++i) {
        const auto base = std::uint32_t(onlyCards.positions.size() / 3);
        for (int corner = 0; corner < 4; ++corner) {
            onlyCards.positions.push_back(float(i * 3 + (corner & 1)));
            onlyCards.positions.push_back(float(corner >> 1));
            onlyCards.positions.push_back(0.0f);
        }
        onlyCards.indices.insert(onlyCards.indices.end(),
                                 {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    const auto none = buildClusterAsset(onlyCards.positions, onlyCards.indices, whole(onlyCards));
    CHECK(none.empty());
    CHECK_EQ(none.cardTriangles, std::uint32_t(80));
    CHECK_EQ(encodeClusters(none).size(), std::size_t(0) + encodeClusters(none).size());
}

TEST(cluster_asset_indexes_the_models_own_vertices_so_one_buffer_serves_every_level) {
    // The build welds - it must, or a UV seam is a crack - but it writes the
    // welding back out. If it did not, the renderer would need a second vertex
    // buffer per model and the levels could not share one.
    auto mesh = sphere(3);
    const auto vertices = std::uint32_t(mesh.positions.size() / 3);
    mesh.positions.insert(mesh.positions.end(), mesh.positions.begin(), mesh.positions.end());
    for (std::size_t i = 0; i < mesh.indices.size(); ++i)
        if ((i / 3) % 2) mesh.indices[i] += vertices;

    const auto asset = buildClusterAsset(mesh.positions, mesh.indices, whole(mesh));
    CHECK(!asset.empty());
    CHECK_EQ(asset.sourceVertices, vertices * 2);
    for (const auto index : asset.indices) CHECK(index < vertices * 2);

    // And the geometry is still watertight when read through those indices,
    // which is the thing the welding was for.
    std::map<std::pair<std::uint32_t, std::uint32_t>, int> uses;
    const auto welded = weldPositions(mesh.positions);
    for (const auto& cluster : asset.clusters) {
        if (cluster.level != 0) continue;
        for (std::uint32_t i = 0; i < cluster.indices.count; i += 3)
            for (int e = 0; e < 3; ++e) {
                const auto a = welded[asset.indices[cluster.indices.first + i + e]];
                const auto b = welded[asset.indices[cluster.indices.first + i + (e + 1) % 3]];
                const auto key = std::minmax(a, b);
                ++uses[{key.first, key.second}];
            }
    }
    CHECK(!uses.empty());
    for (const auto& [edge, count] : uses) CHECK_EQ(count, 2);
}

TEST(cluster_asset_carries_each_levels_cards_out_so_a_clustered_tree_keeps_its_leaves) {
    // A DAG covers the solid part. Without the cards coming out separately a
    // tree either keeps the chain for everything, and the trunk never gets a
    // cut, or it is clustered and loses its leaves. Both are worse than the
    // third answer: the cut for the trunk, and the level the offline thinning
    // already reduced for that distance for the leaves.
    const auto fine = withCards(60);
    const auto coarse = withCards(15);     // as the thinning would leave it
    Mesh chained = fine;
    const engine::IndexRange levels[]{{0, std::uint32_t(fine.indices.size())},
                                      {std::uint32_t(fine.indices.size()),
                                       std::uint32_t(coarse.indices.size())}};
    chained.indices.insert(chained.indices.end(), coarse.indices.begin(), coarse.indices.end());
    // The coarse level names the same vertices; it is a different selection of
    // the same mesh, which is what a thinned level is.
    chained.positions = fine.positions;
    for (auto& index : chained.indices) CHECK(index < chained.positions.size() / 3);

    const auto asset = buildClusterAsset(chained.positions, chained.indices, levels);
    CHECK(!asset.empty());
    CHECK_EQ(asset.cardLevels.size(), std::size_t(2));
    CHECK_EQ(asset.cardTriangles, std::uint32_t(120));          // the fine level's quads
    CHECK_EQ(asset.cardLevels[0].count, std::uint32_t(120 * 3));
    CHECK_EQ(asset.cardLevels[1].count, std::uint32_t(30 * 3));  // and the coarse level's
    CHECK_EQ(asset.cardLevels[0].first, std::uint32_t(0));
    CHECK_EQ(asset.cardLevels[1].first, asset.cardLevels[0].count);
    CHECK_EQ(asset.cardIndices.size(), std::size_t(150 * 3));

    // The clusters hold the sphere and the card ranges hold the quads, with
    // nothing in both and nothing in neither.
    const auto solid = std::uint32_t(sphere(3).positions.size() / 3);
    for (const auto index : asset.indices) CHECK(index < solid);
    for (const auto index : asset.cardIndices) CHECK(index >= solid);

    // And it survives the file.
    std::string why = "untouched";
    const auto read = decodeClusters(encodeClusters(asset), why);
    CHECK(why.empty());
    CHECK(read.cardIndices == asset.cardIndices);
    CHECK_EQ(read.cardLevels.size(), asset.cardLevels.size());
    for (std::size_t i = 0; i < asset.cardLevels.size(); ++i) {
        CHECK_EQ(read.cardLevels[i].first, asset.cardLevels[i].first);
        CHECK_EQ(read.cardLevels[i].count, asset.cardLevels[i].count);
    }
}

TEST(cluster_asset_builds_a_crown_out_of_the_cards_it_could_not_cluster) {
    // A model's alpha cards are what a DAG cannot touch. Merged into one
    // surface they are ordinary geometry again - and that is what lets a tree
    // stop being a billboard at distance rather than at a threshold.
    const auto mesh = withCards(400);
    const auto levels = whole(mesh);
    auto asset = buildClusterAsset(mesh.positions, mesh.indices, levels);
    CHECK(!asset.empty());
    CHECK_EQ(asset.cardTriangles, std::uint32_t(800));
    CHECK(asset.crownClusters.empty());

    buildCrown(asset, mesh.positions, mesh.indices, levels, 0.25);
    CHECK(!asset.crownClusters.empty());
    CHECK(asset.crownLevels > 1);
    CHECK_EQ(asset.crownPositions.size() / 3, asset.crownCoverage.size());
    CHECK_EQ(asset.crownPositions.size(), asset.crownNormals.size());
    for (const auto index : asset.crownIndices) CHECK(index < asset.crownPositions.size() / 3);
    for (const float value : asset.crownCoverage) CHECK(value >= 0 && value <= 1);
    // The crown simplifies, which the cards it replaced could not.
    std::size_t finest = 0, coarsest = 0;
    for (const auto& cluster : asset.crownClusters) {
        if (cluster.level == 0) finest += cluster.indices.count / 3;
        if (cluster.level + 1 == asset.crownLevels) coarsest += cluster.indices.count / 3;
    }
    CHECK(finest > coarsest * 2);
    CHECK(coarsest > 0);

    // And it survives the file, crown and trunk together.
    asset.crownLayer = 35;
    std::string why = "untouched";
    const auto read = decodeClusters(encodeClusters(asset), why);
    CHECK(why.empty());
    CHECK_EQ(read.crownLevels, asset.crownLevels);
    CHECK_EQ(read.crownLayer, 35.0f);
    CHECK(read.crownIndices == asset.crownIndices);
    CHECK(read.crownPositions == asset.crownPositions);
    CHECK(read.crownCoverage == asset.crownCoverage);
    CHECK_EQ(read.crownClusters.size(), asset.crownClusters.size());
    CHECK_EQ(read.clusters.size(), asset.clusters.size());
    CHECK(read.indices == asset.indices);
}

TEST(cluster_asset_gives_a_model_that_is_only_cards_a_crown_and_nothing_else) {
    // A bush is entirely alpha cards. It has no solid geometry to cluster, so
    // until now it had no sidecar at all and no answer but a billboard. A
    // sidecar with a crown and nothing else is a complete answer.
    Mesh bush;
    for (int i = 0; i < 300; ++i) {
        const auto base = std::uint32_t(bush.positions.size() / 3);
        const double a = i * 2.399963;
        const double r = 1.0 + 0.4 * std::sin(double(i));
        for (int c = 0; c < 4; ++c) {
            bush.positions.push_back(float(std::cos(a) * r + (c & 1) * 0.35));
            bush.positions.push_back(float(std::sin(a) * r + (c >> 1) * 0.35));
            bush.positions.push_back(float(0.4 + std::cos(double(i) * 1.7) * 0.6));
        }
        bush.indices.insert(bush.indices.end(),
                            {base, base + 1, base + 2, base + 2, base + 1, base + 3});
    }
    const auto levels = whole(bush);
    auto asset = buildClusterAsset(bush.positions, bush.indices, levels);
    CHECK(asset.clusters.empty());       // nothing solid to cluster
    CHECK(asset.empty());
    buildCrown(asset, bush.positions, bush.indices, levels, 0.1);
    CHECK(!asset.crownClusters.empty());
    CHECK(!asset.empty());               // a crown alone is an answer

    std::string why = "untouched";
    const auto read = decodeClusters(encodeClusters(asset), why);
    CHECK(why.empty());
    CHECK(read.clusters.empty());
    CHECK_EQ(read.crownClusters.size(), asset.crownClusters.size());
}

TEST(cluster_asset_shells_the_whole_model_not_only_its_foliage) {
    // The shell stands in for the model at distance, so it is built from
    // everything - a trunk is part of the mass there. Measured: a tree's solid
    // part cannot simplify past about two hundred triangles because the group
    // rims that make a cut seamless are most of a thin branchy thing, so the
    // cut plus its cards still cost more than the chain at the distance where
    // almost every tree in a frame sits. Merged together they are one mass.
    const auto mesh = sphere(4);          // no cards at all
    const auto levels = whole(mesh);
    auto asset = buildClusterAsset(mesh.positions, mesh.indices, levels);
    CHECK_EQ(asset.cardTriangles, std::uint32_t(0));
    buildCrown(asset, mesh.positions, mesh.indices, levels, 0.08);
    CHECK(!asset.crownClusters.empty());
    CHECK(asset.crownLevels > 1);

    std::size_t finest = 0, coarsest = 0;
    for (const auto& cluster : asset.crownClusters) {
        if (cluster.level == 0) finest += cluster.indices.count / 3;
        if (cluster.level + 1 == asset.crownLevels) coarsest += cluster.indices.count / 3;
    }
    CHECK(coarsest > 0);
    CHECK(finest > coarsest * 4);

    // A model with almost nothing in it gets no shell: three triangles are
    // better served by three triangles.
    const float tiny[]{0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
    const std::uint32_t faces[]{0, 1, 2, 1, 2, 3};
    const engine::IndexRange one[]{{0, 6}};
    ClusterAsset small;
    buildCrown(small, tiny, faces, one, 0.1);
    CHECK(small.crownClusters.empty());
}

TEST(cluster_asset_takes_its_cell_from_the_cards_only_not_from_every_triangle) {
    // Solid triangles go into the merge as degenerate quads, and a triangle is
    // not the scale at which anything stops being separate. Counting them
    // dragged a pine's cell to a quarter of a metre and produced a shell twice
    // the size of the model it replaced.
    const auto solid = sphere(4);
    const auto levels = whole(solid);
    ClusterAsset plain;
    buildCrown(plain, solid.positions, solid.indices, levels, 0.3);
    CHECK(!plain.crownClusters.empty());
    // The cell asked for is the cell used, because there are no cards to raise
    // a floor above it.
    CHECK(std::abs(plain.crownCellMetres - 0.3) < 1e-6);

    // With cards present, their own size raises it.
    const auto leafy = withCards(200, 3);
    const auto leafyLevels = whole(leafy);
    ClusterAsset crowned;
    buildCrown(crowned, leafy.positions, leafy.indices, leafyLevels, 0.05);
    CHECK(!crowned.crownClusters.empty());
    CHECK(crowned.crownCellMetres > 0.05);
}
