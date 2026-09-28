#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace world::ecology {
inline constexpr int kCellMetres = 64;
using Key = std::pair<std::int64_t, std::int64_t>;
inline Key key(double x, double y, int metres = kCellMetres) {
    return {std::int64_t(std::floor(x / metres)), std::int64_t(std::floor(y / metres))};
}
// Biomes describe the fields, never supply them. Species not in the catalogue
// remain a content task; vegetationSet chooses the available broadleaf/conifer mix.
enum class Biome : std::uint8_t {
    TemperateForest, MixedForest, Taiga, ForestSteppe, UplandMeadow, Steppe,
    DrySteppe, Mediterranean, Riparian, Floodplain, Marsh, AridDelta, WetDelta,
    Swamp, Bog, RockyDesert, SandyDesert, Wadi, Savanna, SeasonalForest,
    Rainforest, Foothills, MountainForest, AlpineMeadow, Tundra, PolarDesert
};
enum Mask : std::uint16_t {
    Forest = 1, OpenForest = 2, ForestEdge = 4, Meadow = 8, Shrubland = 16,
    Wetland = 32, Floodplain = 64, Riparian = 128, Rock = 256, Cliff = 512,
    Coast = 1024, Disturbed = 2048, Deer = 4096, Boar = 8192, Grazer = 16384, Waterfowl = 32768
};
struct Physical {
    float temperature = 12, moisture = 0.6f, naturalFertility = 0.6f;
    float soilDepth = 1, drainage = 0.7f, groundwater = 0, salinity = 0;
    float slope = 0, elevation = 0, wind = 0, flood = 0, sand = 0, rock = 0;
    float seasonality = 0.5f, conifer = 0.2f;
    bool coast = false;
};
struct Cell {
    float potentialForest = 0, canopy = 0, grass = 0, shrubs = 0, deadwood = 0;
    float fertility = 0, moisture = 0, wetland = 0, disturbance = 0, youngGrowth = 0;
    float agriculture = 0;
    std::uint16_t masks = 0;
    Biome biome = Biome::Steppe;
    std::uint8_t succession = 0;
};
inline float unit(float value) { return std::clamp(value, 0.0f, 1.0f); }
inline void classify(Cell& c, const Physical& p) {
    c.masks = 0;
    if (c.canopy > 0.6f) c.masks |= Forest;
    else if (c.canopy > 0.12f) c.masks |= OpenForest;
    if (c.canopy > 0.08f && c.canopy < 0.65f) c.masks |= ForestEdge;
    if (c.grass > 0.25f && c.canopy < 0.45f) c.masks |= Meadow;
    if (c.shrubs > 0.2f) c.masks |= Shrubland;
    if (c.wetland > 0.5f) c.masks |= Wetland;
    if (p.flood > 0.25f && p.slope < 0.15f) c.masks |= Floodplain;
    if (p.groundwater > 0.6f) c.masks |= Riparian;
    if (p.rock > 0.5f) c.masks |= Rock;
    if (p.slope > 1.2f) c.masks |= Cliff;
    if (p.coast) c.masks |= Coast;
    if (c.disturbance > 0.2f) c.masks |= Disturbed;
    if (!(c.masks & Cliff) && c.disturbance < 0.65f) {
        if (c.masks & (ForestEdge | Meadow)) c.masks |= Deer;
        if (c.canopy > 0.4f && c.moisture > 0.5f) c.masks |= Boar;
        if (c.masks & Meadow) c.masks |= Grazer;
        if (c.masks & Wetland) c.masks |= Waterfowl;
    }
    c.agriculture = unit(c.fertility * (1 - p.salinity) * (1 - unit(p.slope * 2)) *
        (1 - c.wetland * 0.85f) * unit(c.moisture * 2));
    c.succession = c.canopy > 0.6f ? 4 : c.canopy > 0.2f ? 3 : c.shrubs > 0.2f ? 2 : c.grass > 0.15f ? 1 : 0;
    if (p.temperature < -12) c.biome = Biome::PolarDesert;
    else if (p.temperature < -3) c.biome = p.elevation > 1600 ? Biome::AlpineMeadow : Biome::Tundra;
    else if (c.masks & Wetland) c.biome = p.naturalFertility < 0.3f ? Biome::Bog :
        c.canopy > 0.3f ? Biome::Swamp : Biome::Marsh;
    else if (p.coast && p.flood > 0.5f) c.biome = p.moisture < 0.35f ? Biome::AridDelta : Biome::WetDelta;
    else if (c.masks & Floodplain) c.biome = Biome::Floodplain;
    else if (c.masks & Riparian) c.biome = Biome::Riparian;
    else if (p.moisture < 0.12f) c.biome = p.sand > 0.5f ? Biome::SandyDesert : Biome::RockyDesert;
    else if (p.moisture < 0.22f) c.biome = p.groundwater > 0.2f ? Biome::Wadi : Biome::DrySteppe;
    else if (p.elevation > 1700 && c.canopy < 0.2f) c.biome = Biome::AlpineMeadow;
    else if (p.elevation > 1000 && c.canopy > 0.3f) c.biome = Biome::MountainForest;
    else if (p.slope > 0.3f) c.biome = c.canopy > 0.2f ? Biome::Foothills : Biome::UplandMeadow;
    else if (p.temperature > 22) c.biome = c.canopy < 0.4f ? Biome::Savanna :
        p.seasonality > 0.5f ? Biome::SeasonalForest : Biome::Rainforest;
    else if (p.temperature > 15 && p.seasonality > 0.65f) c.biome = Biome::Mediterranean;
    else if (c.canopy < 0.12f) c.biome = Biome::Steppe;
    else if (c.canopy < 0.4f) c.biome = Biome::ForestSteppe;
    else if (p.temperature < 3) c.biome = Biome::Taiga;
    else c.biome = p.conifer > 0.35f ? Biome::MixedForest : Biome::TemperateForest;
}
inline Cell initial(const Physical& p, float patch) {
    Cell c;
    c.fertility = unit(p.naturalFertility);
    c.moisture = unit(std::max(p.moisture, p.groundwater * 0.85f));
    c.wetland = unit((c.moisture - 0.55f) * 2 + p.flood * 0.6f) * (1 - unit(p.drainage));
    c.potentialForest = unit((p.temperature + 6) / 12) * unit((c.moisture - 0.18f) * 2.4f) *
        unit(p.soilDepth * 1.5f) * (0.35f + c.fertility * 0.65f) * (1 - unit(p.salinity)) *
        (1 - unit(p.slope / 1.2f)) * (1 - unit(p.wind) * 0.5f) *
        // Loose sand and bare rock hold no forest, whatever the rain.
        (1 - unit((p.sand - 0.2f) * 2.2f)) * (1 - unit((p.rock - 0.55f) * 2.2f));
    c.canopy = c.potentialForest * unit(patch);
    c.grass = unit(c.moisture * 2) * (1 - c.canopy * 0.8f) * (1 - unit(p.rock)) *
        (1 - unit((p.sand - 0.15f) * 1.6f)) * (0.45f + 0.55f * c.fertility);
    c.shrubs = c.potentialForest * (0.12f + 0.4f * (1 - c.canopy));
    c.deadwood = c.canopy * c.moisture * 0.35f;
    classify(c, p);
    return c;
}
// Called by simulation batches, not render frames. Potential/natural fertility
// survive clearing. Grazing/farming keep succession suppressed.
inline void advance(Cell& c, const Physical& p, float days) {
    if (!std::isfinite(days) || days <= 0) return;
    const float recovery = 1 - std::exp(-days / 3650.0f);
    c.canopy = unit(c.canopy + (c.potentialForest - c.canopy) * recovery * (1 - c.disturbance));
    c.youngGrowth = unit((c.potentialForest - c.canopy) * (1 - c.disturbance));
    c.grass = unit(c.grass + (unit(c.moisture * 2) * (1 - c.canopy) - c.grass) * (1 - std::exp(-days / 60)));
    c.shrubs = unit(c.shrubs + (c.youngGrowth * 0.5f - c.shrubs) * recovery);
    c.fertility = unit(c.fertility + (p.naturalFertility - c.fertility) * recovery);
    classify(c, p);
}
struct Delta {
    std::uint64_t revision = 0;
    std::map<Key, Cell> cells;
    std::map<std::uint64_t, Key> removed;
    std::map<Key, std::uint64_t> regions; // 128 m ownership regions
    std::uint64_t region(double x, double y) const {
        const auto it = regions.find(key(x, y, 128));
        return it == regions.end() ? 0 : it->second;
    }
};
// Copy-on-write event state. Workers retain one coherent immutable revision.
// Only edits are stored: deterministic unvisited cells require no allocation.
class Store {
public:
    std::shared_ptr<const Delta> read() const { std::lock_guard lock(mutex_); return delta_; }
    void set(double x, double y, Cell cell) {
        std::lock_guard lock(mutex_);
        auto next = std::make_shared<Delta>(*delta_);
        next->cells[key(x, y)] = cell;
        next->regions[key(x, y, 128)] = ++next->revision;
        delta_ = std::move(next);
    }
    void remove(std::uint64_t id, double x, double y) {
        std::lock_guard lock(mutex_);
        if (delta_->removed.contains(id)) return;
        auto next = std::make_shared<Delta>(*delta_);
        next->removed[id] = key(x, y);
        next->regions[key(x, y, 128)] = ++next->revision;
        delta_ = std::move(next);
    }
private:
    mutable std::mutex mutex_;
    std::shared_ptr<const Delta> delta_ = std::make_shared<const Delta>();
};
} // namespace world::ecology

