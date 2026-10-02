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
        // A foundation point stands for the ground a step either side of it
        // (the lattice is read bilinearly), in world metres - its own step,
        // which is 512 m on the reference world, not the 64 m this once
        // assumed: land there was marked an eighth of the way to the origin.
        const auto& f=*world.terrainFoundation;
        const std::int32_t step=f.step;
        // Chunk by chunk of what each stage holds: the open sea floor is
        // never above the water, and a stage holds nothing else there.
        std::vector<std::uint8_t> above(std::size_t(f.columns)*f.rows,0);
        for (const auto& plane:f.heightDm)
            plane.visitChunks([&](std::size_t c,const auto& chunk) {
                for (std::size_t k=0;k<chunk.size();++k) {
                    const auto i=c*chunk.size()+k;
                    if (i<above.size() && chunk[k]>0) above[i]=1;
                }
            },[](std::size_t) {});
        for (int y=0;y<f.rows;++y) for (int x=0;x<f.columns;++x)
            if (above[std::size_t(y)*f.columns+x])
                mask.markWorldRect(x*step-step,y*step-step,x*step+step+1,y*step+step+1);
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

