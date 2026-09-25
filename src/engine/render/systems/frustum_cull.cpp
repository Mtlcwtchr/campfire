#include "engine/render/systems/frustum_cull.hpp"

#include <cmath>

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
}

CulledInstances cullToFrustum(const GatheredInstances& gathered,
                              std::span<const MeshDescription> assets,
                              const ScreenScale& screen, const Horizon* horizon) {
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
