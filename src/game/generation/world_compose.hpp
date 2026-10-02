#pragma once
// A world made of its generations (world_layout.hpp), put together.
//
// Each generation is run as a world of its own size - its rectangle of
// regions, its seed, its plates, its regions as it holds them - placed where
// it is on the planet (its latitude is its place's). A region painted but
// never generated is run the same way, on its own, as a region made by hand:
// land where the continents layer is painted and nowhere else. A region that
// is neither is open sea, and nothing is worked out for it at all.
//
// The runs are then laid into one map, each region from the run it belongs
// to. Where two runs meet on land, the ground of both is pulled to one surface
// across the region blend band - a coast or a pass, not a cliff - and the
// rivers are worked out again over every land mass that now reaches across
// such a seam, and only over those: a continent from one run and an island
// from another beside it keep the rivers they were made with.
//
// A layout that is one generation over the whole world is the generator run
// once over it, exactly as it always was.
#include <cstdint>
#include <string>
#include <vector>

#include "game/generation/world_layout.hpp"
#include "game/generation/world_map_gen.hpp"

namespace generation {

struct ComposeReport {
    bool wholeWorld = false;          // one run over everything: nothing was composed
    std::int32_t runs = 0;            // generations and hand-made regions run
    std::int32_t seaRegions = 0;      // regions left as open sea, not computed
    std::int32_t seams = 0;           // region borders where two runs meet on land
    std::int32_t landmasses = 0;      // reaching across a seam, their rivers worked out again
    std::int32_t sketchRegions = 0;   // painted and not pinned: drawn, not computed (also sea regions)
    std::int32_t cachedRuns = 0;      // runs whose inputs had not changed, taken as they were
    double runMs = 0, composeMs = 0;
};

WorldMapData generateLayoutWorld(const WorldLayout& layout, ComposeReport* report = nullptr);

} // namespace generation
