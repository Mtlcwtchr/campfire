#include "engine/render/systems/level_select.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {
double row(const float r[4], const float p[3]) {
    return double(r[0]) * p[0] + double(r[1]) * p[1] + double(r[2]) * p[2] + double(r[3]);
}
}

double ScreenScale::pixelsPerMetreAt(const float position[3], double& depth) const {
    depth = row(rowW, position);
    if (focal <= 0) return scale;                 // orthographic: one scale everywhere
    return focal / std::max(depth, 1e-3);
}

SelectedLevels selectLevels(const GatheredInstances& gathered,
                            std::span<const MeshDescription> assets,
                            const ScreenScale& screen) {
    SelectedLevels result;

    // One pass to decide, then one stable regroup. Deciding and emitting in the
    // same pass would put an instance in whichever batch it happened to be
    // visited from, and the batches would stop being contiguous runs.
    struct Chosen {
        std::uint32_t mesh = 0, material = 0, level = 0;
        std::uint32_t source = 0;
    };
    std::vector<Chosen> chosen;
    chosen.reserve(gathered.instances.size());

    for (const InstanceBatch& batch : gathered.batches) {
        const MeshDescription* description =
                batch.mesh < assets.size() ? &assets[batch.mesh] : nullptr;
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const std::uint32_t at = batch.first + i;
            const GatheredInstance& instance = gathered.instances[at];
            // Measured at the model's middle, not at its feet. The cull uses
            // the same point, and a frame that disagreed about which one it
            // meant would size a thing by a depth it was not culled at.
            const float centre[3]{instance.position[0], instance.position[1],
                                  instance.position[2] +
                                          float((description ? description->bounds.rise : 0) *
                                                instance.scale)};
            double depth = 0;
            const double perMetre = screen.pixelsPerMetreAt(centre, depth);
            if (screen.focal > 0 && depth <= 0) {
                ++result.behind;
                continue;
            }
            std::uint32_t level = 0;
            if (description && !description->errors.empty()) {
                // The instance's own size on screen: the model's extent grown
                // by this instance's scale. A sapling is not a small tree's
                // worth of triangles because it is small; it is because it
                // covers fewer pixels, and that is the same number.
                const double pixels = description->extent * instance.scale * perMetre;
                level = std::uint32_t(levelFor(description->errors, pixels, description->extent,
                                               screen.allowance));
                // Never finer than what is actually there. Asking for a page
                // that has not arrived draws nothing; the coarser one it has is
                // the honest answer and the same fallback terrain pages use.
                if (description->resident)
                    level = std::min<std::uint32_t>(level, std::uint32_t(description->resident - 1));
            }
            chosen.push_back({batch.mesh, batch.material, level, at});
        }
    }

    // Asset, material, level, then the order the gather already settled. The
    // last key is what keeps an instance in the same place inside its batch
    // from frame to frame, which the gather went to trouble to establish.
    std::stable_sort(chosen.begin(), chosen.end(), [](const Chosen& a, const Chosen& b) {
        if (a.mesh != b.mesh) return a.mesh < b.mesh;
        if (a.material != b.material) return a.material < b.material;
        if (a.level != b.level) return a.level < b.level;
        return a.source < b.source;
    });

    result.instances.reserve(chosen.size());
    result.owners.reserve(chosen.size());
    for (const Chosen& entry : chosen) {
        if (result.batches.empty() || result.batches.back().mesh != entry.mesh ||
            result.batches.back().material != entry.material ||
            result.batches.back().level != entry.level)
            result.batches.push_back({entry.mesh, entry.material, entry.level,
                                      std::uint32_t(result.instances.size()), 0});
        ++result.batches.back().count;
        result.instances.push_back(gathered.instances[entry.source]);
        result.owners.push_back(gathered.owners[entry.source]);
    }
    return result;
}

} // namespace engine::render
