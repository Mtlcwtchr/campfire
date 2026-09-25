#include "game/world/terrain_lod.hpp"

#include <algorithm>
#include <cstdint>

#include "game/generation/world_map_gen.hpp"

namespace world::terrain {

LandMask64 makeLandMask64(const generation::WorldMapData& world) {
    const auto worldWidth = static_cast<std::int64_t>(std::max(0, world.width)) *
                            generation::kMetresPerCell;
    const auto worldHeight = static_cast<std::int64_t>(std::max(0, world.height)) *
                             generation::kMetresPerCell;
    LandMask64 mask(static_cast<std::int32_t>(worldWidth),
                    static_cast<std::int32_t>(worldHeight));
    const auto expected = static_cast<std::size_t>(std::max(0, world.width)) *
                          static_cast<std::size_t>(std::max(0, world.height));
    if (world.cells.size() != expected) return mask;
    if (world.terrainFoundation) {
        const auto& f=*world.terrainFoundation;
        for (int y=0;y<f.rows;++y) for (int x=0;x<f.columns;++x) {
            const auto i=std::size_t(y)*f.columns+x;
            if (std::any_of(f.heightDm.begin(),f.heightDm.end(),[&](const auto& h){return h[i]>0;}))
                mask.markWorldRect(x*64-64,y*64-64,x*64+65,y*64+65);
        }
        return mask;
    }
    for (std::int32_t y = 0; y < world.height; ++y)
        for (std::int32_t x = 0; x < world.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * world.width + x;
            if (world.cells[at].sea) continue;
            const auto left = x * generation::kMetresPerCell;
            const auto top = y * generation::kMetresPerCell;
            // A macro cell is a spline CONTROL POINT, not the footprint of
            // the rendered land. Catmull-Rom reads two cells away and the
            // lookup is warped by up to 0.45 cell in either axis. Reject only
            // beyond that support; sea flags are not coastline geometry.
            constexpr int support = (generation::kMetresPerCell * 5 + 1) / 2;
            mask.markWorldRect(left - support, top - support,
                               left + generation::kMetresPerCell + support,
                               top + generation::kMetresPerCell + support);
        }
    return mask;
}

} // namespace world::terrain

