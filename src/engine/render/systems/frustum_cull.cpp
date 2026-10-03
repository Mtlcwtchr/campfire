#include "engine/render/systems/frustum_cull.hpp"

#include <cmath>
#include <limits>
#include <unordered_map>

namespace engine::render {
namespace {
// A clip plane as four numbers. `x*p.x + y*p.y + z*p.z + w >= 0` is inside, and
// the sphere of radius r around p is wholly outside when that sum is below
// `-r * length(xyz)`.
struct Plane {
    double x = 0, y = 0, z = 0, w = 0;
    [[nodiscard]] double extent() const { return std::sqrt(x * x + y * y + z * z); }
    [[nodiscard]] double at(const double p[3]) const { return x * p[0] + y * p[1] + z * p[2] + w; }
};

Plane combine(const float a[4], const float b[4], double sign) {
    return {double(a[0]) + sign * b[0], double(a[1]) + sign * b[1], double(a[2]) + sign * b[2],
            double(a[3]) + sign * b[3]};
}

// The grouped test bounds the highest possible slope and the entire angular
// wedge of all child spheres. The individual test's centre-distance formula
// cannot safely be used for a large group that contains many smaller objects.
bool hidesGroup(const Horizon& horizon, const InstanceCullIndex::Group& group) {
    if (!horizon.active) return false;
    const double dx = group.centre[0] - horizon.eyeX, dy = group.centre[1] - horizon.eyeY;
    const double distance = std::hypot(dx, dy), radius = group.radius;
    if (!(distance > radius + 1.0)) return false;
    const double height = group.centre[2] + radius - horizon.eyeZ;
    const double top = height / (height >= 0 ? distance - radius : distance + radius);
    const int spread = int(std::ceil(std::asin(radius / distance) *
                           (Horizon::kBins / 6.28318530717958647692))) + 2;
    const int middle = Horizon::binOf(dx, dy);
    for (int offset = -spread; offset <= spread; ++offset) {
        const int bin = (middle + offset + Horizon::kBins) % Horizon::kBins;
        if (horizon.reach[bin] <= 0 || horizon.reach[bin] >= distance - radius ||
            top >= horizon.rise[bin]) return false;
    }
    return true;
}
}

InstanceCullIndex buildInstanceCullIndex(const GatheredInstances& gathered,
                                         std::span<const MeshDescription> assets) {
    InstanceCullIndex result;
    result.source = gathered.instances.data();
    result.count = gathered.instances.size();
    result.members.reserve(result.count);
    struct Cell {
        std::int64_t x, y;
        bool operator==(const Cell&) const = default;
    };
    struct Hash {
        std::size_t operator()(Cell cell) const {
            return std::uint64_t(cell.x) * 0x9e3779b97f4a7c15ull ^
                   std::uint64_t(cell.y) * 0xc2b2ae3d27d4eb4full;
        }
    };
    struct Bounds {
        double low[3]{INFINITY, INFINITY, INFINITY};
        double high[3]{-INFINITY, -INFINITY, -INFINITY};
        std::uint32_t count = 0, next = 0;
    };
    // Two passes and no vector a group: each instance's group (first
    // appearance order) and the bounds, then the members laid out by prefix
    // sums in instance order. The same groups and members as one vector per
    // group, which spent most of this in allocating them.
    std::vector<std::uint32_t> groupOf;
    std::vector<Bounds> groups;
    std::unordered_map<Cell, std::uint32_t, Hash> cells;
    for (const auto& batch : gathered.batches) {
        const MeshBounds shape = batch.mesh < assets.size() ? assets[batch.mesh].bounds : MeshBounds{};
        cells.clear();
        cells.reserve(batch.count / 4 + 16);
        groups.clear();
        groupOf.resize(batch.count);
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const auto at = batch.first + i;
            const auto& instance = gathered.instances[at];
            const Cell cell{std::int64_t(std::floor(double(instance.position[0]) / 32)),
                            std::int64_t(std::floor(double(instance.position[1]) / 32))};
            auto [entry, added] = cells.try_emplace(cell, std::uint32_t(groups.size()));
            if (added) groups.emplace_back();
            groupOf[i] = entry->second;
            auto& group = groups[entry->second];
            ++group.count;
            const double centre[3]{instance.position[0], instance.position[1],
                                  instance.position[2] + shape.rise * instance.scale};
            const double radius = std::abs(shape.radius * instance.scale);
            const double extent = std::max(radius, double(float(radius)));
            for (int axis = 0; axis < 3; ++axis) {
                // Horizon::hides reads floats while the frustum reads doubles.
                // Enclose both, including rounding at large world coordinates.
                group.low[axis] = std::min(group.low[axis],
                    std::min(centre[axis], double(float(centre[axis]))) - extent);
                group.high[axis] = std::max(group.high[axis],
                    std::max(centre[axis], double(float(centre[axis]))) + extent);
            }
        }
        std::uint32_t at = std::uint32_t(result.members.size());
        for (auto& bounds : groups) {
            InstanceCullIndex::Group group;
            double radiusSquared = 0;
            for (int axis = 0; axis < 3; ++axis) {
                group.centre[axis] = (bounds.low[axis] + bounds.high[axis]) * 0.5;
                const double extent = (bounds.high[axis] - bounds.low[axis]) * 0.5;
                radiusSquared += extent * extent;
            }
            group.radius = std::sqrt(radiusSquared) + 1e-6;
            group.first = at;
            group.count = bounds.count;
            bounds.next = at;
            at += bounds.count;
            result.groups.push_back(group);
        }
        result.members.resize(at);
        for (std::uint32_t i = 0; i < batch.count; ++i)
            result.members[groups[groupOf[i]].next++] = batch.first + i;
    }
    return result;
}

CulledInstances cullToFrustum(const GatheredInstances& gathered,
                              std::span<const MeshDescription> assets,
                              const ScreenScale& screen, const Horizon* horizon,
                              const InstanceCullIndex* index) {
    CulledInstances result;

    // The four side planes of the clip cube, each as the sum or difference of
    // the depth row and one screen row. Building them once per frame rather
    // than per instance is the whole reason this is a system and not a method.
    const Plane sides[4]{combine(screen.rowW, screen.rowX, +1), combine(screen.rowW, screen.rowX, -1),
                         combine(screen.rowW, screen.rowY, +1), combine(screen.rowW, screen.rowY, -1)};
    double reach[4];
    for (int i = 0; i < 4; ++i) reach[i] = sides[i].extent();
    const Plane eye{double(screen.rowW[0]), double(screen.rowW[1]), double(screen.rowW[2]),
                     double(screen.rowW[3])};

    std::vector<unsigned char> removed;
    if (index && index->source == gathered.instances.data() && index->count == gathered.instances.size()) {
        removed.resize(index->count, 0);
        const double eyeExtent = eye.extent();
        for (const auto& group : index->groups) {
            unsigned char reason = 0;
            const double depth = eye.at(group.centre), eyeRadius = group.radius * eyeExtent;
            if (depth + eyeRadius <= 0) reason = 1;
            else if (depth - eyeRadius > 0) {
                bool inside = true;
                for (int p = 0; p < 4; ++p) {
                    const double side = sides[p].at(group.centre), radius = group.radius * reach[p];
                    if (side + radius < 0) { reason = 2; break; }
                    if (side - radius < 0) inside = false;
                }
                if (!reason && inside && horizon && hidesGroup(*horizon, group)) reason = 3;
            }
            if (reason) for (std::uint32_t i = 0; i < group.count; ++i)
                removed[index->members[group.first + i]] = reason;
        }
    }

    result.kept.invisible = gathered.invisible;
    result.kept.rejected = gathered.rejected;
    result.kept.instances.reserve(gathered.instances.size());
    result.kept.owners.reserve(gathered.instances.size());

    for (const InstanceBatch& batch : gathered.batches) {
        const MeshBounds shape =
                batch.mesh < assets.size() ? assets[batch.mesh].bounds : MeshBounds{};
        const std::uint32_t begin = std::uint32_t(result.kept.instances.size());
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const std::uint32_t at = batch.first + i;
            if (!removed.empty() && removed[at]) {
                if (removed[at] == 1) ++result.behind;
                else if (removed[at] == 2) ++result.outside;
                else ++result.hidden;
                continue;
            }
            const GatheredInstance& instance = gathered.instances[at];
            const double centre[3]{instance.position[0], instance.position[1],
                                   instance.position[2] + shape.rise * instance.scale};
            const double radius = shape.radius * instance.scale;

            // The near plane on the centre alone, not on the sphere. Keeping a
            // model the eye sits inside would only hand the selection a depth
            // it is about to divide by and reject, and counting it here says
            // something truer about the frame than counting it there.
            if (eye.at(centre) <= 0) {
                ++result.behind;
                continue;
            }
            bool out = false;
            for (int p = 0; p < 4 && !out; ++p) out = sides[p].at(centre) + radius * reach[p] < 0;
            if (out) {
                ++result.outside;
                continue;
            }
            // In the view and in front of the eye - and possibly behind a hill.
            // Asked last because it is the only one of the three that has to
            // look anything up.
            if (horizon != nullptr &&
                horizon->hides(float(centre[0]), float(centre[1]), float(centre[2]),
                               float(radius))) {
                ++result.hidden;
                continue;
            }
            result.kept.instances.push_back(instance);
            result.kept.owners.push_back(gathered.owners[at]);
        }
        const std::uint32_t count = std::uint32_t(result.kept.instances.size()) - begin;
        if (count) result.kept.batches.push_back({batch.mesh, batch.material, begin, count});
    }
    return result;
}

} // namespace engine::render
