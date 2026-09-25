#include "game/world/terrain_streaming/water_tile.hpp"

#include <algorithm>

#include "game/world/coords.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::streaming {

std::vector<RiverId> reachesAround(const HydrologyGraph& graph, TileKey key,
                                   std::int32_t haloMetres) {
    std::vector<RiverId> found;
    if (haloMetres < 0) return found;
    const auto grown = tileBounds(key, haloMetres);
    const auto first = tileAt(grown.min, key.level);
    const auto last = tileAt(grown.max, key.level);
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) {
            const auto* page = findHydrologySpatialPage(graph, {x, y, key.level});
            if (page == nullptr) continue;
            found.insert(found.end(), page->segments.begin(), page->segments.end());
        }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

} // namespace world::streaming
