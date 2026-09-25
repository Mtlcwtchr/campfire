#pragma once
// Read-only snapshots. Percentages count pages/target tiles, never elapsed time.
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace world::terrain {

struct PreparationProgress {
    double ready = 0;
    std::size_t total = 0;
    bool known = false;

    int percent() const {
        if (!known) return -1;
        if (!total) return 100; // an empty ocean view needs no terrain pages
        if (ready >= static_cast<double>(total)) return 100;
        return std::clamp(static_cast<int>(100.0 * std::max(0.0, ready) / total), 0, 99);
    }
};

// A finer tile covers a fraction of a requested tile. Its reverse morph still
// meets that request until it reaches a level coarser than the target.
inline double detailContribution(int drawnLod, float parentMorph, int targetLod) {
    if (drawnLod > targetLod) return 0;
    if (drawnLod == targetLod) return 1.0 - std::clamp(double(parentMorph), 0.0, 1.0);
    return std::ldexp(1.0, 2 * (drawnLod - targetLod));
}

struct StreamingProgress {
    PreparationProgress ramPrepared, ramPublished, persistentGpu, viewPages, viewMesh;
    bool initialized = false, gridReady = false, frozen = false;
    bool backgroundRunning = false, capacityLimited = false, uploadFailed = false;
    int targetLod = 0;
    int meshStepMin = 0, meshStepMax = 0, dataStepMin = 0, dataStepMax = 0;
    int chunkMetresMin = 0, chunkMetresMax = 0, chunkCells = 0;
    std::size_t meshesBuilt = 0, deferredRegions = 0;
    int cameraMode = 0, gridMode = 0;
    double flightSpeed = 0;
    std::size_t ramBytes = 0, cacheBytes = 0, gpuBytes = 0;
    std::size_t fineResident = 0, fineCapacity = 0;
    std::size_t h8Resident = 0, h8Capacity = 0;
    std::size_t workers = 0, busy = 0, queued = 0, ready = 0, staged = 0, speculative = 0;
    std::size_t visibleBusy = 0, preloadBusy = 0, preparationBusy = 0, inspectionBusy = 0;
    std::size_t failed = 0, backgroundFailed = 0;
    std::size_t plansSubmitted = 0, plansCompleted = 0, plansDiscarded = 0;
    double lastBuildMs = 0, longestBuildMs = 0, uploadMs = 0;
    double longestFetchMs = 0, longestPackMs = 0;
    int longestPageX = 0, longestPageY = 0, longestPageStep = 0;
    double planBuildMs = 0, planLatencyMs = 0;
};

} // namespace world::terrain

