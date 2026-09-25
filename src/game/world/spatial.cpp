#include "game/world/spatial.hpp"

#include <algorithm>

#include "game/world/terrain_mesh.hpp"

namespace world {
namespace {

using core::Fixed;
using core::WorldPos;

constexpr std::int32_t kLeafSize = 8;      // triangles a leaf may hold
constexpr int kMaxDepth = 24;

// Twice the signed area of the triangle abc, seen from above. Positive one way
// round, negative the other; zero when the three are in a line. Everything
// about flat containment is this function three times.
Fixed cross(WorldPos a, WorldPos b, WorldPos c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

} // namespace

Bounds Bounds::around(WorldPos centre, Fixed radius) {
    Bounds b;
    b.min = {centre.x - radius, centre.y - radius};
    b.max = {centre.x + radius, centre.y + radius};
    b.low = Fixed::fromInt(-(1 << 20));
    b.high = Fixed::fromInt(1 << 20);
    return b;
}

void Bounds::add(WorldPos p, Fixed z) {
    if (!valid()) {
        min = max = p;
        low = high = z;
        return;
    }
    min.x = Fixed::fromRaw(std::min(min.x.raw, p.x.raw));
    min.y = Fixed::fromRaw(std::min(min.y.raw, p.y.raw));
    max.x = Fixed::fromRaw(std::max(max.x.raw, p.x.raw));
    max.y = Fixed::fromRaw(std::max(max.y.raw, p.y.raw));
    low = Fixed::fromRaw(std::min(low.raw, z.raw));
    high = Fixed::fromRaw(std::max(high.raw, z.raw));
}

void Bounds::add(const Bounds& other) {
    if (!other.valid()) return;
    add(other.min, other.low);
    add(other.max, other.high);
}

bool Bounds::overlaps(const Bounds& other) const {
    if (!valid() || !other.valid()) return false;
    return !(other.min.x.raw > max.x.raw || other.max.x.raw < min.x.raw ||
             other.min.y.raw > max.y.raw || other.max.y.raw < min.y.raw);
}

bool Bounds::containsFlat(WorldPos p) const {
    return valid() && p.x.raw >= min.x.raw && p.x.raw <= max.x.raw && p.y.raw >= min.y.raw &&
           p.y.raw <= max.y.raw;
}

Bounds Triangle::bounds() const {
    Bounds b;
    b.add(a, za);
    b.add(this->b, zb);
    b.add(c, zc);
    return b;
}

bool Triangle::coversFlat(WorldPos p) const {
    // The three edge signs have to agree. A point exactly on an edge belongs to
    // both triangles that share it, which is what a caller wants: a body
    // standing on the seam between two triangles is standing on the ground.
    const Fixed ab = cross(a, b, p);
    const Fixed bc = cross(b, c, p);
    const Fixed ca = cross(c, a, p);
    const bool anyNegative = ab.raw < 0 || bc.raw < 0 || ca.raw < 0;
    const bool anyPositive = ab.raw > 0 || bc.raw > 0 || ca.raw > 0;
    return !(anyNegative && anyPositive);
}

Fixed Triangle::heightAt(WorldPos p) const {
    // Barycentric, which is the plane through the three corners.
    const Fixed total = cross(a, b, c);
    if (total.raw == 0) return za;
    const Fixed wa = cross(b, c, p) / total;
    const Fixed wb = cross(c, a, p) / total;
    const Fixed wc = core::kOne - wa - wb;
    return za * wa + zb * wb + zc * wc;
}

// --- the tree ------------------------------------------------------------

void TriangleTree::build(std::vector<Triangle> triangles) {
    triangles_ = std::move(triangles);
    nodes_.clear();
    bounds_ = Bounds{};
    if (triangles_.empty()) return;
    nodes_.reserve(triangles_.size() * 2 / kLeafSize + 4);
    buildNode(0, static_cast<std::uint32_t>(triangles_.size()), 0);
    bounds_ = nodes_[0].box;
}

std::uint32_t TriangleTree::buildNode(std::uint32_t first, std::uint32_t count, int depth) {
    const std::uint32_t self = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Bounds box;
    for (std::uint32_t i = first; i < first + count; ++i) box.add(triangles_[i].bounds());

    if (count <= kLeafSize || depth >= kMaxDepth) {
        nodes_[self].box = box;
        nodes_[self].first = first;
        nodes_[self].count = count;
        return self;
    }

    // Split down the middle of the longer side, by the centre of each triangle.
    // Median rather than a surface-area heuristic: the geometry here is a
    // terrain mesh, evenly spread by construction, so the clever split has
    // nothing to be clever about.
    const bool alongX = box.spanX().raw >= box.spanY().raw;
    const auto centre = [alongX](const Triangle& t) {
        return alongX ? (t.a.x.raw + t.b.x.raw + t.c.x.raw) : (t.a.y.raw + t.b.y.raw + t.c.y.raw);
    };
    const auto begin = triangles_.begin() + first;
    const auto end = begin + count;
    // Sorted rather than merely partitioned. Partitioning puts the right
    // triangles either side of the split but leaves their order to whatever
    // order they arrived in, so two runs over the same ground build trees that
    // answer alike and look nothing alike - which cannot be compared, and
    // therefore cannot be trusted to be the same. Sorting costs nothing at a
    // few hundred triangles to a chunk.
    std::sort(begin, end, [&](const Triangle& l, const Triangle& r) {
        const std::int64_t a = centre(l), b = centre(r);
        return a != b ? a < b : l.id < r.id;
    });

    const std::uint32_t half = count / 2;
    const std::uint32_t left = buildNode(first, half, depth + 1);
    const std::uint32_t right = buildNode(first + half, count - half, depth + 1);
    nodes_[self].box = box;
    nodes_[self].left = left;
    nodes_[self].right = right;
    nodes_[self].count = 0;
    return self;
}

void TriangleTree::coveringFlat(WorldPos p, std::vector<const Triangle*>& out) const {
    if (nodes_.empty()) return;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!node.box.containsFlat(p)) continue;
        if (node.leaf()) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i)
                if (triangles_[i].coversFlat(p)) out.push_back(&triangles_[i]);
            continue;
        }
        stack.push_back(node.left);
        stack.push_back(node.right);
    }
}

void TriangleTree::overlapping(const Bounds& box, std::vector<const Triangle*>& out) const {
    if (nodes_.empty()) return;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        const Node& node = nodes_[stack.back()];
        stack.pop_back();
        if (!node.box.overlaps(box)) continue;
        if (node.leaf()) {
            for (std::uint32_t i = node.first; i < node.first + node.count; ++i)
                if (triangles_[i].bounds().overlaps(box)) out.push_back(&triangles_[i]);
            continue;
        }
        stack.push_back(node.left);
        stack.push_back(node.right);
    }
}

std::optional<RayHit> TriangleTree::raycast(WorldPos from, Fixed fromZ, WorldPos to,
                                            Fixed toZ) const {
    if (nodes_.empty()) return std::nullopt;
    // The ray's own box, which is all the pruning a short ray needs. A proper
    // slab test would pay off for rays that cross the world; these are a mouse
    // pick and a line of sight, both a few dozen metres.
    Bounds ray;
    ray.add(from, fromZ);
    ray.add(to, toZ);

    std::vector<const Triangle*> candidates;
    overlapping(ray, candidates);

    std::optional<RayHit> best;
    const Fixed dx = to.x - from.x, dy = to.y - from.y, dz = toZ - fromZ;
    for (const Triangle* t : candidates) {
        // Where the ray crosses the triangle's plane, then whether that point is
        // inside the triangle. Two steps rather than one, because on a surface
        // world the second is the flat containment that is already written.
        const Fixed nx = (t->b.y - t->a.y) * (t->zc - t->za) - (t->zb - t->za) * (t->c.y - t->a.y);
        const Fixed ny = (t->zb - t->za) * (t->c.x - t->a.x) - (t->b.x - t->a.x) * (t->zc - t->za);
        const Fixed nz = (t->b.x - t->a.x) * (t->c.y - t->a.y) - (t->b.y - t->a.y) * (t->c.x - t->a.x);
        const Fixed denominator = nx * dx + ny * dy + nz * dz;
        if (denominator.raw == 0) continue;                  // parallel
        const Fixed numerator = nx * (t->a.x - from.x) + ny * (t->a.y - from.y) + nz * (t->za - fromZ);
        const Fixed along = numerator / denominator;
        if (along.raw < 0 || along.raw > core::kOne.raw) continue;   // outside the segment
        const WorldPos where{from.x + dx * along, from.y + dy * along};
        if (!t->coversFlat(where)) continue;
        const Fixed distance = core::hypot(dx, dy) * along;
        if (!best || distance.raw < best->distance.raw)
            best = RayHit{t->id, t->surface, where, fromZ + dz * along, distance};
    }
    return best;
}

// --- all the chunks together ---------------------------------------------

void StaticGeometry::set(ChunkId chunk, std::vector<Triangle> triangles) {
    TriangleTree tree;
    tree.build(std::move(triangles));
    trees_[chunk] = std::move(tree);
}

void StaticGeometry::clear(ChunkId chunk) { trees_.erase(chunk); }

std::vector<const Triangle*> StaticGeometry::coveringFlat(WorldPos p) const {
    std::vector<const Triangle*> out;
    // The chunks a point could belong to: its own, and its neighbours, because
    // a triangle whose corner is in the next chunk still covers ground here.
    for (ChunkId c : withNeighbours(chunkOf(p))) {
        const auto it = trees_.find(c);
        if (it != trees_.end()) it->second.coveringFlat(p, out);
    }
    return out;
}

std::vector<const Triangle*> StaticGeometry::overlapping(const Bounds& box) const {
    std::vector<const Triangle*> out;
    if (!box.valid()) return out;
    // Every chunk the box touches, and the ring around it: a query is a region
    // of the world, not a region of one chunk.
    const ChunkId first = chunkOf(box.min);
    const ChunkId last = chunkOf(box.max);
    for (std::int32_t y = first.y - 1; y <= last.y + 1; ++y)
        for (std::int32_t x = first.x - 1; x <= last.x + 1; ++x) {
            const auto it = trees_.find(ChunkId{x, y});
            if (it != trees_.end()) it->second.overlapping(box, out);
        }
    return out;
}

std::optional<RayHit> StaticGeometry::raycast(WorldPos from, Fixed fromZ, WorldPos to,
                                              Fixed toZ) const {
    std::optional<RayHit> best;
    Bounds ray;
    ray.add(from, fromZ);
    ray.add(to, toZ);
    const ChunkId first = chunkOf(ray.min);
    const ChunkId last = chunkOf(ray.max);
    for (std::int32_t y = first.y - 1; y <= last.y + 1; ++y)
        for (std::int32_t x = first.x - 1; x <= last.x + 1; ++x) {
            const auto it = trees_.find(ChunkId{x, y});
            if (it == trees_.end()) continue;
            const std::optional<RayHit> hit = it->second.raycast(from, fromZ, to, toZ);
            if (hit && (!best || hit->distance.raw < best->distance.raw)) best = hit;
        }
    return best;
}

std::optional<Fixed> StaticGeometry::groundHeight(WorldPos p) const {
    std::optional<Fixed> highest;
    for (const Triangle* t : coveringFlat(p)) {
        if (t->surface == Surface::Cliff) continue;   // a face is a wall, not a floor
        const Fixed here = t->heightAt(p);
        if (!highest || here.raw > highest->raw) highest = here;
    }
    return highest;
}

// --- things that move ----------------------------------------------------

EntityIndex::Cell EntityIndex::cellOf(WorldPos p) {
    return {static_cast<std::int32_t>(floorDiv(p.x.toInt(), kBucketMetres)),
            static_cast<std::int32_t>(floorDiv(p.y.toInt(), kBucketMetres))};
}

void EntityIndex::insert(std::uint32_t id, WorldPos at) {
    const auto known = where_.find(id);
    if (known != where_.end()) {
        move(id, at);
        return;
    }
    where_[id] = at;
    buckets_[cellOf(at)].push_back(id);
}

void EntityIndex::move(std::uint32_t id, WorldPos to) {
    const auto known = where_.find(id);
    if (known == where_.end()) {
        insert(id, to);
        return;
    }
    const Cell was = cellOf(known->second);
    const Cell now = cellOf(to);
    known->second = to;
    if (was == now) return;      // the common case: a step inside one bucket

    // Out of the old bucket and into the new. Getting this wrong is how an
    // entity ends up listed in two places, or in none, and the symptom is a
    // person nobody can see standing next to them.
    auto& old = buckets_[was];
    old.erase(std::remove(old.begin(), old.end(), id), old.end());
    if (old.empty()) buckets_.erase(was);
    buckets_[now].push_back(id);
}

void EntityIndex::remove(std::uint32_t id) {
    const auto known = where_.find(id);
    if (known == where_.end()) return;
    const Cell cell = cellOf(known->second);
    auto& bucket = buckets_[cell];
    bucket.erase(std::remove(bucket.begin(), bucket.end(), id), bucket.end());
    if (bucket.empty()) buckets_.erase(cell);
    where_.erase(known);
}

void EntityIndex::clear() {
    buckets_.clear();
    where_.clear();
}

std::vector<std::uint32_t> EntityIndex::near(WorldPos centre, Fixed radius) const {
    std::vector<std::uint32_t> out;
    const Bounds box = Bounds::around(centre, radius);
    const Fixed squared = radius * radius;
    const Cell first = cellOf(box.min);
    const Cell last = cellOf(box.max);
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) {
            const auto it = buckets_.find(Cell{x, y});
            if (it == buckets_.end()) continue;
            for (std::uint32_t id : it->second) {
                const WorldPos at = where_.at(id);
                const Fixed dx = at.x - centre.x, dy = at.y - centre.y;
                if ((dx * dx + dy * dy).raw <= squared.raw) out.push_back(id);
            }
        }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::uint32_t> EntityIndex::inBox(const Bounds& box) const {
    std::vector<std::uint32_t> out;
    if (!box.valid()) return out;
    const Cell first = cellOf(box.min);
    const Cell last = cellOf(box.max);
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) {
            const auto it = buckets_.find(Cell{x, y});
            if (it == buckets_.end()) continue;
            for (std::uint32_t id : it->second)
                if (box.containsFlat(where_.at(id))) out.push_back(id);
        }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<WorldPos> EntityIndex::positionOf(std::uint32_t id) const {
    const auto it = where_.find(id);
    if (it == where_.end()) return std::nullopt;
    return it->second;
}

// --- remembered answers --------------------------------------------------

LookupGrid::Cell LookupGrid::cellOf(WorldPos p) {
    return {static_cast<std::int32_t>(floorDiv(p.x.toInt(), kCellMetres)),
            static_cast<std::int32_t>(floorDiv(p.y.toInt(), kCellMetres))};
}

std::optional<LookupGrid::Answer> LookupGrid::hint(WorldPos p) const {
    const auto it = answers_.find(cellOf(p));
    if (it == answers_.end()) return std::nullopt;
    return it->second;
}

void LookupGrid::remember(WorldPos p, Answer answer) { answers_[cellOf(p)] = answer; }

void LookupGrid::forget(const Bounds& box) {
    if (!box.valid()) return;
    // A cell that so much as touches the region goes, and one ring beyond it:
    // an answer is about the ground around a point, so a change just outside a
    // cell can still be a change to what that cell was remembering.
    const Cell first = cellOf(box.min);
    const Cell last = cellOf(box.max);
    for (std::int32_t y = first.y - 1; y <= last.y + 1; ++y)
        for (std::int32_t x = first.x - 1; x <= last.x + 1; ++x) answers_.erase(Cell{x, y});
}

void LookupGrid::forgetAll() { answers_.clear(); }

// --- feeding the index ---------------------------------------------------

std::vector<Triangle> trianglesOf(const TerrainMesh& mesh, Surface surface) {
    std::vector<Triangle> out;
    out.reserve(mesh.triangleCount());
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const TerrainVertex& a = mesh.vertices[mesh.indices[i]];
        const TerrainVertex& b = mesh.vertices[mesh.indices[i + 1]];
        const TerrainVertex& c = mesh.vertices[mesh.indices[i + 2]];
        Triangle t;
        t.a = a.position;
        t.b = b.position;
        t.c = c.position;
        t.za = a.height;
        t.zb = b.height;
        t.zc = c.height;
        t.id = static_cast<std::uint32_t>(i / 3);
        t.surface = surface;
        out.push_back(t);
    }
    return out;
}

} // namespace world
