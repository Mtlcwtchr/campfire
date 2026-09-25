#include "game/world/terrain_plan.hpp"

#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace world::terrain {
namespace {
using Point = std::pair<double, double>;
using Rank = std::tuple<int, float, int, int>;
struct EdgePoint {
    std::array<float, 3> height{};
    Rank rank{-1, 0, 0, 0};
    EdgePoint* a = nullptr;
    EdgePoint* b = nullptr;
    const TerrainPlan::Block* owner = nullptr;
    bool shared = false, support = false;
    double fraction = 0, span = 0;
    int state = 0;
};
bool boundary(const AdaptiveVertex& v, int cells) {
    return v.x == 0 || v.y == 0 || v.x == cells || v.y == cells;
}
std::array<float, 3> rendered(const AdaptiveMesh& mesh, const AdaptiveVertex& v, float morph) {
    const auto i = std::size_t(v.y) * (mesh.cells + 1) + v.x;
    return {std::lerp(mesh.bed[i], v.parentBed, morph),
            std::lerp(mesh.head[i], v.parentHead, morph),std::lerp(v.priorBed,v.priorParent,morph)};
}
const std::array<float, 3>& resolve(EdgePoint& point) {
    if (point.state == 2) return point.height;
    if (point.state == 1) throw std::logic_error("non-dyadic terrain edge constraints");
    point.state = 1;
    if (point.a) {
        const auto a = resolve(*point.a), b = resolve(*point.b);
        for (int i = 0; i < 3; ++i) point.height[i] = float(std::lerp(double(a[i]), double(b[i]), point.fraction));
    }
    point.state = 2;
    return point.height;
}
}

void TerrainPlan::stitchEdges(const std::vector<Block>& previous) {
    // Every segment endpoint is first registered globally. A corner constrained
    // by a coarser perpendicular edge must also move on the other incident edges.
    // Resolving the dyadic constraint DAG handles arbitrary LOD gaps, not just 2:1.
    std::map<Point, EdgePoint> points;
    const auto coordinate = [](const Block& block, const AdaptiveVertex& v) -> Point {
        const double side = block.metres();
        return {block.tile.x * side + v.x * block.mesh->step,
                block.tile.y * side + v.y * block.mesh->step};
    };
    for (auto& block : coverage) {
        if (!block.mesh) continue;
        if (block.mesh->unstitched) block.mesh = block.mesh->unstitched;
        const Rank rank{block.tile.lod, block.parentMorph, -block.tile.y, -block.tile.x};
        for (const auto& v : block.mesh->vertices) if (boundary(v, block.mesh->cells)) {
            auto& point = points[coordinate(block, v)];
            point.shared = point.shared || (point.owner && point.owner != &block);
            point.owner = &block;
            if (rank > point.rank) {
                point.rank = rank;
                point.height = rendered(*block.mesh, v, block.parentMorph);
            }
        }
    }
    std::map<Point, EdgePoint*> rows, columns;
    for (auto& [p, value] : points) {
        columns.emplace(p, &value);
        rows.emplace(Point{p.second, p.first}, &value);
    }
    for (const auto& block : coverage) {
        if (!block.mesh) continue;
        const auto& mesh = *block.mesh;
        const double side = block.metres();
        const double ox = block.tile.x * side, oy = block.tile.y * side;
        const auto edge = [&](bool vertical, double fixed, double start) {
            auto& line = vertical ? columns : rows;
            std::vector<double> endpoints;
            for (const auto& v:mesh.vertices) {
                const auto p=coordinate(block,v);
                if ((vertical?p.first:p.second)==fixed)
                    endpoints.push_back((vertical?p.second:p.first)-start);
            }
            std::sort(endpoints.begin(),endpoints.end());
            endpoints.erase(std::unique(endpoints.begin(),endpoints.end()),endpoints.end());
            // Probe spacing is not edge spacing. A planar boundary may now be
            // one long segment next to several fine ridge/valley segments.
            for (std::size_t i=1;i<endpoints.size();++i) {
                const double lo=start+endpoints[i-1],hi=start+endpoints[i],span=hi-lo;
                auto* a = line.at({fixed, lo});
                auto* b = line.at({fixed, hi});
                for (auto it = line.upper_bound({fixed, lo});
                     it != line.end() && it->first.first == fixed && it->first.second < hi; ++it) {
                    auto& p = *it->second;
                    if (span <= p.span) continue;
                    p.a = a; p.b = b; p.span = span;
                    p.fraction = (it->first.second - lo) / span;
                    a->support = b->support = true;
                }
            }
        };
        edge(true, ox, oy); edge(true, ox + side, oy);
        edge(false, oy, ox); edge(false, oy + side, ox);
    }
    for (auto& [p, point] : points) resolve(point);
    std::unordered_map<std::int64_t, const Block*> old;
    for (const auto& block : previous) old.emplace(tileKeyOf(block.tile), &block);
    for (auto& block : coverage) {
        if (!block.mesh) continue;
        const auto& mesh = *block.mesh;
        std::shared_ptr<AdaptiveMesh> changed;
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            const auto& v = mesh.vertices[i];
            if (!boundary(v, mesh.cells)) continue;
            const auto& point = points.at(coordinate(block, v));
            const auto height = point.height;
            // Both sides must use the same floats, not CPU interpolation on
            // one side and a separately rounded GPU UNORM decode on the other.
            if (!point.shared && !point.support && !point.a && height == rendered(mesh, v, block.parentMorph)) continue;
            if (!changed) {
                changed = std::make_shared<AdaptiveMesh>(mesh);
                changed->unstitched = block.mesh;
            }
            auto& target = changed->vertices[i];
            target.skirt |= 2;
            target.edgeBed = height[0]; target.edgeHead = height[1];
            target.priorEdge = height[2];
            // A wet neighbour can invalidate a local dry-page proof at the seam.
            if (height[1] > height[0]) block.mayHaveWater = true;
        }
        if (!changed) continue;
        // Repeated page arrivals/reculls must not re-upload identical seams.
        if (const auto it = old.find(tileKeyOf(block.tile)); it != old.end()) {
            const auto& prior = it->second->mesh;
            if (prior && prior->unstitched == block.mesh && prior->vertices.size() == changed->vertices.size() &&
                std::equal(prior->vertices.begin(), prior->vertices.end(), changed->vertices.begin(),
                    [](const auto& a, const auto& b) {
                        return a.skirt == b.skirt && a.edgeBed == b.edgeBed && a.edgeHead == b.edgeHead && a.priorEdge == b.priorEdge;
                    })) {
                block.mesh = prior;
                continue;
            }
        }
        block.mesh = std::move(changed);
    }
}
void TerrainPlan::interpolateFrom(const std::vector<Block>& previous) {
    using Address = std::tuple<std::int64_t, int, int>;
    struct Source {
        const Block* block = nullptr;
        std::unique_ptr<AdaptiveMesh> surface;
    };
    std::map<Address, Source> sources;
    std::vector<std::int64_t> extents;
    for (const auto& block : previous) if (block.mesh) {
        sources.emplace(Address{block.metres(), block.tile.x, block.tile.y}, Source{&block, {}});
        extents.push_back(block.metres());
    }
    std::sort(extents.begin(), extents.end());
    extents.erase(std::unique(extents.begin(), extents.end()), extents.end());
    interpolated = false;
    for (auto& block : coverage) {
        if (!block.mesh) continue;
        const auto same = sources.find({block.metres(), block.tile.x, block.tile.y});
        if (same != sources.end() && same->second.block->mesh == block.mesh &&
            same->second.block->parentMorph == block.parentMorph) continue;
        auto mesh = std::make_shared<AdaptiveMesh>(*block.mesh);
        bool changed = false;
        for (auto& v : mesh->vertices) {
            const double x = double(block.tile.x * block.metres()) + v.x * mesh->step;
            const double y = double(block.tile.y * block.metres()) + v.y * mesh->step;
            Source* source = nullptr;
            // Boundaries have two owners; always choose the same side, including
            // at corners. No world-sized index and no triangle scan per vertex.
            for (auto extent : extents) {
                const int ix = int(std::floor(x / double(extent))), iy = int(std::floor(y / double(extent)));
                for (int dy = 0; !source && dy <= int(y == double(iy * extent)); ++dy)
                    for (int dx = 0; dx <= int(x == double(ix * extent)); ++dx)
                        if (auto it = sources.find({extent, ix-dx, iy-dy}); it != sources.end()) {
                            source = &it->second;
                            break;
                        }
                if (source) break;
            }
            if (!source) continue; // newly visible coverage has no old surface
            const auto& old = *source->block;
            if (!source->surface) {
                source->surface = std::make_unique<AdaptiveMesh>();
                auto& s = *source->surface;
                s.cells = old.mesh->cells; s.step = old.mesh->step;
                s.splits = old.mesh->splits;
                s.bed = old.mesh->bed; s.head = old.mesh->head; s.prior = old.mesh->prior;
                if (s.prior.empty()) s.prior = s.bed;
                for (const auto& p : old.mesh->vertices) {
                    const auto i = std::size_t(p.y)*(s.cells+1)+p.x;
                    const auto h = rendered(*old.mesh, p, old.parentMorph);
                    s.bed[i] = (p.skirt & 2) ? p.edgeBed : h[0];
                    s.head[i] = (p.skirt & 2) ? p.edgeHead : h[1];
                    s.prior[i] = (p.skirt & 2) ? p.priorEdge : h[2];
                }
            }
            const double ox = x - double(old.tile.x*old.metres()), oy = y - double(old.tile.y*old.metres());
            const auto h = source->surface->sample(ox, oy);
            v.displayFrom = {h[0], h[1], source->surface->samplePrior(ox, oy)};
            v.skirt |= 8;
            changed = true;
        }
        if (changed) { block.mesh = std::move(mesh); interpolated = true; }
    }
}
} // namespace world::terrain

