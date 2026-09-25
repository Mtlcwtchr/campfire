#include "game/world/surface_reconstruction.hpp"
#include <cstdlib>

namespace world {
void SurfaceReconstruction::configure(const terrain::TerrainConfig& config) {
    auto policy = std::getenv("ASR_TERRAIN_STABLE_FEATURES") ? terrain::kCameraFeatureDataLodPolicy :
        terrain::kRegionalDataLodPolicy;
    policy.chunkCells = config.chunkCells;
    policy.chunkMetres = config.chunkMetres;
    const auto& map = world_->worldMap();
    constexpr int rootLevel = int(terrain::kGeometryLevels) - 1;
    const int side = static_cast<int>(policy.metresAt(rootLevel));
    std::vector<TileId> roots;
    for (int y = 0; y * side < map.height * generation::kMetresPerCell; ++y)
        for (int x = 0; x * side < map.width * generation::kMetresPerCell; ++x)
            if (world_->pages().landMask().anyLandInWorldRect(x * side, y * side, (x + 1) * side, (y + 1) * side))
                roots.push_back({x, y, rootLevel});
    planner_.reset(); // cancel/drain the old channel before replacing its caches
    published_.reset();
    planner_ = std::make_unique<terrain::TerrainPlanner>(map, world_->pages(), std::move(roots), policy);
}
} // namespace world
