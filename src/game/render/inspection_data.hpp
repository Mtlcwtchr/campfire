#pragma once
// The mode-specific inspection payload shares existing GPU attributes. Natural
// terrain curvature/drainage stay intact; only the active map's fields change.
#include "game/render/terrain_data.hpp"
#include "game/world/inspection.hpp"

namespace game {
inline void packInspection(TerrainVertexGpu& gpu, const world::InspectionSample* sample,
                           world::MapView mode) {
    gpu.windAndTravel[1] = sample ? sample->travel : (world::needsInspection(mode) ? -1.0f : 1.0f);
    if (!sample) return;
    switch (mode) {
    case world::MapView::Fertility:
        gpu.environment[1] = sample->fertilityPotential;
        gpu.environment[3] = sample->soil;
        break;
    case world::MapView::Foundation:
        gpu.environment[3] = sample->bearingStrength;
        gpu.geography[0] = sample->mudPotential;
        break;
    case world::MapView::Wind:
        gpu.geography[0] = sample->windX;
        gpu.geography[1] = sample->windY;
        break;
    case world::MapView::Flood:
        gpu.geography[2] = sample->flood;
        break;
    default: break;
    }
}
} // namespace game

