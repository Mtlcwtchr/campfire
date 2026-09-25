#include "engine/render/systems/terrain_gather.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {
bool sane(const TerrainPatch& patch) {
    return std::isfinite(patch.originX) && std::isfinite(patch.originY) &&
           std::isfinite(patch.metres) && patch.metres > 0 && std::isfinite(patch.lowZ) &&
           std::isfinite(patch.highZ) && patch.highZ >= patch.lowZ && std::isfinite(patch.morph);
}
}

GatheredTerrain gatherTerrain(std::span<const TerrainPatch> cut, const ScreenScale& screen) {
    GatheredTerrain result;
    result.patches.reserve(cut.size());
    for (const TerrainPatch& patch : cut) {
        if (!sane(patch)) {
            ++result.rejected;
            continue;
        }
        result.patches.push_back(patch);
    }

    // Coarsest first, then a fixed sweep. Sorting by the patch's own footprint
    // rather than by where it sat in the cut is what makes the frame the same
    // frame on two machines whose quadtrees were walked in different orders.
    std::sort(result.patches.begin(), result.patches.end(),
              [](const TerrainPatch& a, const TerrainPatch& b) {
                  if (a.level != b.level) return a.level > b.level;
                  if (a.originY != b.originY) return a.originY < b.originY;
                  if (a.originX != b.originX) return a.originX < b.originX;
                  return a.source < b.source;
              });

    result.pixelsPerMetre.resize(result.patches.size());
    for (std::size_t i = 0; i < result.patches.size(); ++i) {
        const TerrainPatch& patch = result.patches[i];
        if (result.levels.empty() || result.levels.back().level != patch.level)
            result.levels.push_back({patch.level, std::uint32_t(i), 0});
        ++result.levels.back().count;
        if (patch.water) result.water.push_back(std::uint32_t(i));
        result.area += patch.metres * patch.metres;

        const float middle[3]{float(patch.originX + patch.metres * 0.5),
                              float(patch.originY + patch.metres * 0.5),
                              (patch.lowZ + patch.highZ) * 0.5f};
        double depth = 0;
        const double perMetre = screen.pixelsPerMetreAt(middle, depth);
        // A patch the camera has passed is still in the cut - the streaming
        // quadtree keeps a margin - and reporting a negative size for it would
        // put a nonsense number into whatever divides by it.
        result.pixelsPerMetre[i] =
                float(screen.focal > 0 && depth <= 0 ? 0.0 : std::max(0.0, perMetre));
    }
    return result;
}

} // namespace engine::render
