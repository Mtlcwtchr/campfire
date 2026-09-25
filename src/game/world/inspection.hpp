#pragma once
// Immutable requests/results for per-ring inspection work. No renderer or UI references.
#include <functional>
#include <memory>
#include <optional>
#include <vector>
#include "game/world/environment.hpp"
#include "game/world/flood_preview.hpp"
#include "game/world/navmesh.hpp"

namespace world {
struct FloodRequest {
    core::WorldPos centre;
    double span = 0;
};
struct InspectionSnapshot {
    std::uint64_t revision = 0;
    MapView mode = MapView::Natural;
    std::shared_ptr<const SoilState> soil;
    std::shared_ptr<const FloodPreview> flood;
    std::optional<FloodRequest> floodRequest;
};
struct InspectionOrder {
    // RingSpec::key(), including epoch; zero is reserved for the connected flood solve.
    std::uint64_t key = 0;
    std::shared_ptr<const std::vector<core::WorldPos>> positions;
    double urgency = 0;
};
struct InspectionSample {
    float soil = 0, travel = 1, windX = 0, windY = 0, flood = -1;
    float fertilityPotential = 0, bearingStrength = 0, mudPotential = 0;
};
struct InspectionResult {
    std::uint64_t key = 0, revision = 0;
    std::vector<InspectionSample> samples;
    std::shared_ptr<const FloodPreview> flood;
};
inline bool needsInspection(MapView mode) {
    return mode == MapView::Fertility || mode == MapView::Travel ||
           mode == MapView::Wind || mode == MapView::Flood || mode == MapView::Foundation;
}
inline std::vector<InspectionSample> sampleInspection(const HeightField& field,
        const std::vector<core::WorldPos>& positions, const InspectionSnapshot& state,
        const std::function<bool()>& cancelled = {}) {
    std::vector<InspectionSample> samples(positions.size());
    for (std::size_t i = 0; i < positions.size(); ++i) {
        if (i % 64 == 0 && cancelled && cancelled()) return {};
        auto& sample = samples[i];
        const auto p = positions[i];
        switch (state.mode) {
        case MapView::Fertility:
        case MapView::Foundation: {
            const auto properties = field.soilAt(p).properties;
            sample.fertilityPotential = static_cast<float>(properties.fertility.toDouble());
            sample.bearingStrength = static_cast<float>(properties.bearingStrength.toDouble());
            sample.mudPotential = static_cast<float>(properties.mudPotential.toDouble());
            if (state.mode == MapView::Fertility && state.soil)
                sample.soil = static_cast<float>(state.soil->deltaAt(p).toDouble());
            break;
        }
        case MapView::Travel:
            sample.travel = static_cast<float>(travelCost(field.travelAt(p)).toDouble());
            break;
        case MapView::Wind: {
            const auto wind = field.windAt(p);
            sample.windX = static_cast<float>(wind[0].toDouble());
            sample.windY = static_cast<float>(wind[1].toDouble());
            break;
        }
        case MapView::Flood:
            if (state.flood) sample.flood = state.flood->at(p);
            break;
        default: break;
        }
    }
    return samples;
}
} // namespace world

