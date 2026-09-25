#pragma once
// Terrain potential and mutable soil are separate. No renderer-owned fertility.
#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"

namespace world {
enum class MapView { Natural, Temperature, Fertility, Moisture, Travel, Foundation, Flood, Relief, Wind,
    Grey, Slope, StageDelta, Erosion, Catchment, Geology, Flow, Count };
inline constexpr std::array<const char*, 16> kMapNames{
    "none", "temperature", "fertility", "moisture", "travel", "foundation", "flood", "relief", "wind",
    "grey", "slope", "stage-delta", "erosion", "catchment", "geology", "flow"};
inline constexpr std::array<const char*, 16> kMapTitles{
    "NATURAL", "TEMPERATURE / model Celsius", "SOIL FERTILITY", "MOISTURE / drainage",
    "MOVEMENT / terrain cost", "BUILDING / terrain suitability", "FLOOD / connected scenario",
    "RELIEF / height contours (50m)", "WIND / relative speed + direction",
    "GREY / geometry only", "SLOPE / green=flat red=45deg+", "STAGE DELTA / blue=cut red=rise (100m)",
    "EROSION / removed height (150m)", "CATCHMENT / slope-stage log2 H64 cells", "GEOLOGY / rock type",
    "FLOW / slope-stage H64 receiver arrows"};
struct EnvironmentalConditions {
    core::Fixed temperatureOffset;
    core::Fixed rainfall = core::kOne;
    core::Fixed windStrength = core::kOne;
};

// Sparse persistent-in-memory changes on a GLOBAL 8 m lattice. Sampling is
// continuous across plot/chunk boundaries; maps/meshes never own this state.
// Settlement integration must feed actual work events, not frame time.
class SoilState {
public:
    enum class Treatment { Harvest, Pasture, Ash };
    static constexpr int kStep = 8;
    void treat(core::WorldPos centre, int radius, Treatment treatment, core::Fixed amount) {
        if (radius <= 0 || amount <= core::kZero) return;
        radius = std::min(radius, 512);
        amount = std::min(amount, core::kOne);
        const auto cx = centre.x.toInt(), cy = centre.y.toInt();
        for (auto y = floorDiv(cy - radius, kStep); y <= floorDiv(cy + radius, kStep) + 1; ++y)
            for (auto x = floorDiv(cx - radius, kStep); x <= floorDiv(cx + radius, kStep) + 1; ++x) {
                const auto dx = core::Fixed::fromInt(x * kStep) - centre.x;
                const auto dy = core::Fixed::fromInt(y * kStep) - centre.y;
                const auto r = core::Fixed::fromInt(radius);
                const auto weight = std::clamp(core::kOne - (dx * dx + dy * dy) / (r * r), core::kZero, core::kOne);
                if (weight <= core::kZero) continue;
                auto& cell = cells_[{x, y}];
                const auto change = amount * weight * weight;
                if (treatment == Treatment::Ash) cell.ash = std::min(core::Fixed::ratio(1, 3), cell.ash + change);
                else cell.balance = std::clamp(cell.balance + (treatment == Treatment::Harvest ? -change : change),
                                               -core::kOne, core::Fixed::ratio(1, 4));
            }
        ++revision_;
    }
    void advanceDays(int days) {
        if (days <= 0) return;
        const auto decay = core::Fixed::ratio(std::min(days, 365), 180);
        for (auto& [key, cell] : cells_) cell.ash = std::max(core::kZero, cell.ash - decay);
        ++revision_;
    }
    core::Fixed deltaAt(core::WorldPos p) const {
        using core::Fixed;
        const auto side = Fixed::fromInt(kStep);
        const auto x = floorDiv(p.x.raw, side.raw), y = floorDiv(p.y.raw, side.raw);
        const auto u = Fixed::fromRaw(floorMod(p.x.raw, side.raw)) / side;
        const auto v = Fixed::fromRaw(floorMod(p.y.raw, side.raw)) / side;
        const auto at = [&](auto ax, auto ay) {
            const auto it = cells_.find({ax, ay});
            return it == cells_.end() ? core::kZero : it->second.balance + it->second.ash;
        };
        const auto a = at(x,y), b = at(x+1,y), c = at(x,y+1), d = at(x+1,y+1);
        return (a + (b-a)*u) * (core::kOne-v) + (c + (d-c)*u)*v;
    }
    core::Fixed fertilityAt(core::WorldPos p, core::Fixed potential) const {
        return std::clamp(potential + deltaAt(p), core::kZero, core::kOne);
    }
    std::uint64_t revision() const { return revision_; }
    void clear() { cells_.clear(); ++revision_; }
private:
    struct Cell { core::Fixed balance, ash; };
    std::map<std::pair<std::int64_t, std::int64_t>, Cell> cells_;
    std::uint64_t revision_ = 0;
};
} // namespace world

