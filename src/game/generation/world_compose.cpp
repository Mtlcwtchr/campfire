#include "game/generation/world_compose.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <memory>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>

#include "engine/biomes/category_field.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/core/progress.hpp"
#include "engine/core/rng.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_import.hpp"

namespace generation {
namespace {
using Clock = std::chrono::steady_clock;
using core::Fixed;
using core::TilePos;

constexpr std::int64_t kRegionM = kRegionMetres;
constexpr std::int32_t kSeaFloorDm = -600;   // the foundation's floor, where nothing was made
constexpr std::int32_t kHeightScale = 64;     // the drainage flood's sixty-fourths of a step

double millis(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }

// Every field the generator writes per macro cell, for copying a cell from a
// run into the composite. Anything added to WorldMapData per cell belongs here.
#define ASR_CELL_FIELDS(F)                                                                                   \
    F(primaryHeightField) F(continentalField) F(distanceToCoast) F(initialLandMask) F(upliftField)            \
    F(riftField) F(faultField) F(geologyRegion) F(macroHeightField) F(rockTypeField) F(erosionResistanceField) \
    F(soilParentMaterialField) F(permeabilityField) F(thermallyRelaxedHeightField)                           \
    F(hydrologicallyCorrectedHeightField) F(basinIdField) F(spillPointField) F(flowDirectionField)           \
    F(flowAccumulationField) F(riverDischargeField) F(riverSourceField) F(lakeRegionField) F(lakeLevelField) \
    F(lakeDepthField) F(provinceField) F(waterfallField) F(sedimentPotentialField) F(erosionField)           \
    F(floodplainPotentialField) F(deltaPotentialField) F(erodedHeightField) F(baseTemperatureField)          \
    F(prevailingWindField) F(windStrengthField) F(windVariabilityField) F(precipitationField)                \
    F(airMoistureField) F(rainShadowField) F(oceanCurrentField) F(seaSurfaceTemperatureBiasField)           \
    F(coastalClimateBiasField) F(temperatureField) F(annualRainfallField) F(humidityField)                  \
    F(seasonalityField) F(winterRainField) F(summerRainField) F(drySeasonStrengthField) F(soilTypeField)     \
    F(soilFertilityField) F(soilDrainageField) F(soilOrganicPotentialField)                                 \
    F(primaryBiomeSuitabilityField) F(secondaryBiomeSuitabilityField) F(biomeTransitionField)               \
    F(materialSuitabilityField) F(waterPaintField)

// One run of the generator, and where its world lies in this one.
struct Run {
    Generation generation;            // what it was run as
    std::int32_t originX = 0, originY = 0;   // cells
    std::shared_ptr<const WorldMapData> map; // shared with the run cache
    // How far it was taken: whether its drainage was worked out (not the
    // primary stage), and whether its water was (neither primary nor relief).
    // Composing adds none of either to ground whose run had none.
    bool drains = true, wet = true;
};

// --- the run cache -----------------------------------------------------------
//
// A run is a pure function of what it was run with: its generation's layout
// (its rectangle, its regions as they are held, the paint over it), where it
// lies on the planet and how far it goes. An edit in one region changes one
// run's inputs and no other's, so every other run is taken from here as it
// was - the country a thousand kilometres away is not made again because a
// river was painted somewhere else.
//
// Held in memory under a budget (ASR_RUN_CACHE_MB, default 512): a region run
// at 64 m is a hundred-odd megabytes of foundation, and the machine is small.
// Least recently used goes first. Its maps are shared, not copied, with the
// composed world's runs.
struct RunKey {
    WorldLayout sub;
    Latitude latitude;
    std::int32_t originX = 0, originY = 0;
    GenerationStage stage = GenerationStage::Full;
    bool authored = false;
    std::uint64_t imported = 0;          // the skeleton's key, for an imported region
    std::int32_t foundationStep = 0;
    bool operator==(const RunKey&) const = default;
};
struct CachedRun {
    RunKey key;
    std::shared_ptr<const WorldMapData> map;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
};
std::mutex cacheGuard;
std::vector<CachedRun> cache;
std::uint64_t cacheClock = 0;

std::size_t cacheBudget() {
    if (const char* mb = std::getenv("ASR_RUN_CACHE_MB")) return std::size_t(std::max(0L, std::atol(mb))) << 20;
    return std::size_t(512) << 20;
}

std::size_t bytesOf(const WorldMapData& map) {
    std::size_t bytes = map.cells.size() * sizeof(WorldCell);
#define ASR_BYTES(f) bytes += map.f.bytes();
    ASR_CELL_FIELDS(ASR_BYTES)
#undef ASR_BYTES
    if (const auto* f = map.terrainFoundation.get()) {
        for (const auto& plane : f->heightDm) bytes += plane.bytes();
        bytes += f->receiver.bytes() + f->accumulation.bytes() + f->detailPages.size();
    }
    return bytes;
}

std::shared_ptr<const WorldMapData> cachedRun(const RunKey& key) {
    const std::lock_guard<std::mutex> lock(cacheGuard);
    for (auto& entry : cache)
        if (entry.key == key) {
            entry.used = ++cacheClock;
            return entry.map;
        }
    return nullptr;
}

void keepRun(RunKey key, std::shared_ptr<const WorldMapData> map) {
    const std::size_t budget = cacheBudget();
    const std::size_t bytes = bytesOf(*map);
    if (bytes > budget) return;
    const std::lock_guard<std::mutex> lock(cacheGuard);
    std::size_t held = bytes;
    for (const auto& entry : cache) held += entry.bytes;
    while (held > budget && !cache.empty()) {
        auto oldest = std::min_element(cache.begin(), cache.end(),
                                       [](const CachedRun& a, const CachedRun& b) { return a.used < b.used; });
        held -= oldest->bytes;
        cache.erase(oldest);
    }
    cache.push_back({std::move(key), std::move(map), bytes, ++cacheClock});
}

// How far a generation's run goes: a region made by hand under the staged
// pipeline says; everything else is run in full.
void stageOf(const Generation& g, WorldMapParams& params) {
    if (g.regions.size() != 1) return;
    switch (g.regions.front().stage) {
        case RegionStage::Primary: params.stage = GenerationStage::Primary; params.authored = true; break;
        case RegionStage::Relief: params.stage = GenerationStage::Relief; params.authored = true; break;
        case RegionStage::Water: params.authored = true; break;
        case RegionStage::Full:
        case RegionStage::Sketch: break;
    }
}

// The composite, every field sized, holding what an untouched sea holds.
void allocate(WorldMapData& world, std::size_t count) {
#define ASR_ASSIGN(f) world.f.assign(count, {});
    ASR_CELL_FIELDS(ASR_ASSIGN)
#undef ASR_ASSIGN
    world.cells.assign(count, WorldCell{});
    // What the open sea holds, as each field's fill: nothing is kept for a
    // cell of it (cell_field.hpp).
    world.basinIdField.assign(count, -1);
    world.flowDirectionField.assign(count, std::int8_t(-1));
    world.lakeRegionField.assign(count, -1);
    world.provinceField.assign(count, std::int32_t(Province::Highlands));
    world.distanceToCoast.assign(count, 64);   // the open sea is far from any coast
    world.rockTypeField.assign(count, RockType::SoftSediment);
    world.soilTypeField.assign(count, SoilType::Mineral);
}

// Open sea where nothing was made: its temperature from where it lies, and
// nothing else worth a pass.
void seaCell(WorldMapData& world, std::size_t i, std::int32_t cellY, const Latitude& latitude) {
    std::int32_t band = 115;
    if (latitude.fixed && latitude.legacyRows > 0)
        band = std::clamp(latitude.legacyFrom + cellY * latitude.legacySpan / std::max(1, latitude.legacyRows - 1), 0, 230);
    else if (latitude.fixed)
        band = std::clamp(std::int32_t(std::lround(230.0 * (1.0 - std::min(90.0, std::abs(latitude.at(
                                                                (double(cellY) + 0.5) * kMetresPerCell))) / 90.0))),
                          0, 230);
    // The cell alone: every field of it stays the field's fill, and nothing
    // is kept for it (cell_field.hpp). What the sea says about itself - its
    // warmth, its wetness - is on the cell.
    WorldCell& c = world.cells[i];
    c = WorldCell{};
    c.sea = true;
    c.elevation = 0;
    c.temperature = std::uint8_t(std::clamp((25 + band) * 4 / 5 + 25, 0, 255));
    c.moisture = 200;
}

// A cell of a run into the composite, with every index the run made (lake and
// basin labels are cell indices, spill points cell coordinates) moved to where
// the run lies.
void copyCell(WorldMapData& to, std::size_t ti, const Run& run, std::size_t fi) {
    const WorldMapData& from = *run.map;
    to.cells[ti] = from.cells[fi];
#define ASR_COPY(f) if (fi < from.f.size()) to.f[ti] = from.f[fi];
    ASR_CELL_FIELDS(ASR_COPY)
#undef ASR_COPY
    const auto moved = [&](std::int32_t index) -> std::int32_t {
        if (index < 0) return index;
        const std::int32_t lx = index % from.width, ly = index / from.width;
        return (ly + run.originY) * to.width + (lx + run.originX);
    };
    to.basinIdField[ti] = moved(to.basinIdField[ti]);
    to.lakeRegionField[ti] = moved(to.lakeRegionField[ti]);
    auto& spill = to.spillPointField[ti];
    if (fi < from.basinIdField.size() && from.basinIdField[fi] >= 0)
        spill = {std::int16_t(spill.x + run.originX), std::int16_t(spill.y + run.originY)};
}

// The foundation lattice of the composite, and a run's value at a point of it.
struct Lattice {
    int step = 64, columns = 0, rows = 0;
};
std::int32_t runSample(const Run& run, std::size_t stage, std::int64_t worldX, std::int64_t worldY) {
    const TerrainFoundation* f = run.map->terrainFoundation.get();
    if (!f) return kSeaFloorDm;
    const std::int64_t lx = worldX - std::int64_t(run.originX) * kMetresPerCell;
    const std::int64_t ly = worldY - std::int64_t(run.originY) * kMetresPerCell;
    const int cx = int(std::clamp<std::int64_t>(lx / f->step, 0, f->columns - 1));
    const int cy = int(std::clamp<std::int64_t>(ly / f->step, 0, f->rows - 1));
    return f->heightDm[stage][std::size_t(cy) * std::size_t(f->columns) + std::size_t(cx)];
}

// Drainage over the foundation, as buildTerrainFoundation routes its own:
// every sample above the sea drains to its steepest lower neighbour, and what
// drains through it is added up. In the middle of a run this is the run's own
// graph again, sample for sample; it differs only where water now crosses a
// seam that was the edge of a run.
void routeFoundation(TerrainFoundation& f) {
    static constexpr int dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static constexpr int dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    const auto& slopes = f.heightDm[4];
    const std::size_t count = slopes.size();
    f.receiver.assign(count, -1);
    f.accumulation.assign(count, 1);
    // Only land drains: the open sea floor is the fill of every field here,
    // and a pass over it writes nothing (cell_field.hpp).
    std::vector<std::uint32_t> land;
    for (int y = 0; y < f.rows; ++y)
        for (int x = 0; x < f.columns; ++x) {
            const auto i = std::size_t(y) * f.columns + x;
            if (slopes[i] <= 0) continue;
            land.push_back(std::uint32_t(i));
            int sink = -1, best = 0, length = 1;
            for (int d = 0; d < 8; ++d) {
                const int nx = x + dx[d], ny = y + dy[d];
                if (nx < 0 || ny < 0 || nx >= f.columns || ny >= f.rows) continue;
                const auto j = std::size_t(ny) * f.columns + nx;
                const int drop = slopes[i] - slopes[j];
                const int distance = dx[d] && dy[d] ? int(f.step * 1.41421356 + 0.5) : f.step;
                if (drop > 0 && drop * length > best * distance) { best = drop; length = distance; sink = int(j); }
            }
            f.receiver.set(i, sink);
        }
    // Topological accumulation over the land, in the order it drains; a sea
    // sample receives and passes nothing on.
    std::vector<std::uint8_t> incoming(count, 0);
    for (const auto i : land) if (const int sink = f.receiver[i]; sink >= 0) ++incoming[std::size_t(sink)];
    std::vector<std::uint32_t> order;
    order.reserve(land.size());
    for (const auto i : land) if (!incoming[i]) order.push_back(i);
    for (std::size_t n = 0; n < order.size(); ++n) {
        const auto i = order[n];
        const int sink = f.receiver[i];
        if (sink < 0) continue;
        f.accumulation.set(std::size_t(sink), f.accumulation[std::size_t(sink)] + f.accumulation[i]);
        if (--incoming[std::size_t(sink)] == 0 && slopes[std::size_t(sink)] > 0) order.push_back(std::uint32_t(sink));
    }
    CellField<std::uint32_t> basin(count, std::uint32_t(-1));
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const int sink = f.receiver[*it];
        // Draining into the sea is the basin of that sea sample, as it was
        // when the sea was walked too.
        basin.set(*it, sink < 0 ? *it : slopes[std::size_t(sink)] <= 0 ? std::uint32_t(sink) : basin[std::size_t(sink)]);
    }
    f.divides.clear();
    for (const auto i : land) {
        const int x = int(i % std::uint32_t(f.columns)), y = int(i / std::uint32_t(f.columns));
        for (int d : {4, 6}) {
            const int nx = x + dx[d], ny = y + dy[d];
            if (nx >= f.columns || ny >= f.rows || nx < 0 || ny < 0) continue;
            const auto j = std::size_t(ny) * f.columns + nx;
            if (slopes[j] > 0 && basin[i] != basin[j]) f.divides.push_back({i, std::uint32_t(j)});
        }
    }
}

// The rivers of one land mass worked out again over the composite, as PASS G6
// works them out over a run: hollows flooded from the sea inwards, every cell
// draining to its steepest lower neighbour, the rain added up down those
// paths, a river where one cell in a hundred of the land mass is passed.
// Heights are not cut again - the runs cut them, and the seams joined them.
//
// The whole-map arrays are kept between land masses and given back clean
// (only what a land mass touched is reset): a continent of many regions has
// thousands of islands, and allocating the world's worth of cells for each
// of them was most of what composing cost.
struct DrainScratch {
    std::vector<std::uint8_t> mine, seen;
    std::vector<std::int32_t> filled, flow;
    std::vector<std::int8_t> downhill;
    void ready(std::size_t count) {
        if (mine.size() == count) return;
        mine.assign(count, 0);
        seen.assign(count, 0);
        filled.assign(count, 0);
        flow.assign(count, 0);
        downhill.assign(count, -1);
    }
};
void drainLandmass(WorldMapData& world, const std::vector<std::size_t>& cells, DrainScratch& scratch) {
    const std::int32_t W = world.width, H = world.height;
    const std::size_t count = world.cells.size();
    scratch.ready(count);
    auto& mine = scratch.mine;
    for (const auto i : cells) mine[i] = 1;
    const auto own = [&](std::size_t i) { return std::int32_t(world.cells[i].elevation) * kHeightScale; };
    struct Front {
        std::int32_t level;
        std::size_t index;
        bool operator>(const Front& o) const { return level != o.level ? level > o.level : index > o.index; }
    };
    std::priority_queue<Front, std::vector<Front>, std::greater<Front>> queue;
    auto& filled = scratch.filled;
    auto& seen = scratch.seen;
    for (const auto i : cells) {
        const std::int32_t x = std::int32_t(i % std::size_t(W)), y = std::int32_t(i / std::size_t(W));
        bool outlet = x == 0 || y == 0 || x == W - 1 || y == H - 1;
        for (int dir : core::kCardinalDirections) {
            const TilePos n = core::neighbour({x, y}, dir);
            if (world.inBounds(n) && world.at(n).sea) outlet = true;
        }
        if (!outlet) continue;
        filled[i] = own(i);
        seen[i] = 1;
        queue.push({filled[i], i});
    }
    while (!queue.empty()) {
        const Front here = queue.top();
        queue.pop();
        const TilePos p{std::int32_t(here.index % std::size_t(W)), std::int32_t(here.index / std::size_t(W))};
        for (int dir : core::kCardinalDirections) {
            const TilePos n = core::neighbour(p, dir);
            if (!world.inBounds(n)) continue;
            const std::size_t j = std::size_t(n.y) * W + n.x;
            if (!mine[j] || seen[j]) continue;
            seen[j] = 1;
            filled[j] = std::max(own(j), here.level + 1);
            queue.push({filled[j], j});
        }
    }
    std::vector<std::size_t> order = cells;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return filled[a] != filled[b] ? filled[a] > filled[b] : a < b;
    });
    auto& flow = scratch.flow;
    auto& downhill = scratch.downhill;
    for (const auto i : order) {
        const TilePos p{std::int32_t(i % std::size_t(W)), std::int32_t(i / std::size_t(W))};
        flow[i] += 1 + world.cells[i].moisture / 64;
        std::int32_t bestFall = 0;
        int best = -1;
        for (int dir = 0; dir < core::kNeighbourCount; ++dir) {
            const TilePos n = core::neighbour(p, dir);
            if (!world.inBounds(n)) continue;
            const std::size_t j = std::size_t(n.y) * W + n.x;
            const std::int32_t e = world.cells[j].sea ? -1 : mine[j] ? filled[j] : std::numeric_limits<std::int32_t>::max();
            if (e >= filled[i]) continue;
            const std::int32_t fall = filled[i] - e;
            const std::int32_t steepness = core::isDiagonal(dir) ? fall * 10 / 14 : fall;
            if (steepness > bestFall) { bestFall = steepness; best = dir; }
        }
        if (best < 0) continue;
        downhill[i] = std::int8_t(best);
        const TilePos n = core::neighbour(p, best);
        flow[std::size_t(n.y) * W + n.x] += flow[i];
    }
    // One cell in a hundred of the land mass, as the generator draws rivers.
    std::vector<std::int32_t> flows;
    flows.reserve(cells.size());
    for (const auto i : cells) flows.push_back(flow[i]);
    std::sort(flows.begin(), flows.end());
    const std::int32_t riverFlow = std::max(2, flows.empty() ? 2 : flows[std::min(flows.size() - 1, flows.size() * 99 / 100)]);
    for (const auto i : cells) {
        WorldCell& c = world.cells[i];
        world.hydrologicallyCorrectedHeightField[i] = filled[i];
        world.flowDirectionField[i] = downhill[i];
        world.flowAccumulationField[i] = flow[i];
        world.riverDischargeField[i] = flow[i];
        c.drainOut = downhill[i];
        c.drainSize = 0;
        if (downhill[i] >= 0)
            for (std::int32_t f = flow[i]; f > 1 && c.drainSize < 15; f /= 2) ++c.drainSize;
        c.river = flow[i] >= riverFlow && downhill[i] >= 0;
        c.riverOut = c.river ? downhill[i] : std::int8_t(-1);
        c.riverSize = 0;
        if (c.river) {
            std::uint8_t size = 0;
            for (std::int32_t f = flow[i]; f > riverFlow && size < 15; f /= 2) ++size;
            c.riverSize = std::uint8_t(size + 1);
        }
    }
    for (const auto i : cells) {
        if (!world.cells[i].river) { world.riverSourceField[i] = 0; continue; }
        const TilePos p{std::int32_t(i % std::size_t(W)), std::int32_t(i / std::size_t(W))};
        bool upstream = false;
        for (int dir = 0; dir < core::kNeighbourCount && !upstream; ++dir) {
            const TilePos n = core::neighbour(p, dir);
            if (!world.inBounds(n)) continue;
            const WorldCell& o = world.at(n);
            upstream = o.river && o.riverOut >= 0 && core::neighbour(n, o.riverOut) == p;
        }
        world.riverSourceField[i] = upstream ? 0u : 1u;
    }
    // Clean for the next land mass: what this one wrote, and only that - its
    // own cells, and whatever each of them passed its water to.
    for (const auto i : cells) {
        if (const int d = downhill[i]; d >= 0) {
            const TilePos p{std::int32_t(i % std::size_t(W)), std::int32_t(i / std::size_t(W))};
            const TilePos n = core::neighbour(p, d);
            if (world.inBounds(n)) flow[std::size_t(n.y) * W + n.x] = 0;
        }
    }
    for (const auto i : cells) {
        mine[i] = 0;
        seen[i] = 0;
        filled[i] = 0;
        flow[i] = 0;
        downhill[i] = -1;
    }
}

double fall(double t) {
    t = std::clamp(t, 0.0, 1.0);
    const double c = std::cos(t * 1.5707963267948966);
    return c * c;
}

// Imported ground at its primary stage stands under a stand-in climate - the
// bare, alpine one (world_map_gen.cpp bareGround) where nothing is said. Where
// the import painted its terrain categories it says a good deal: the moisture
// map is how wet the place is, and the category its climate zone, how warm
// and how fertile (categories.json "climate"). The grass, the forest and what
// the ground is made of follow from these before the world works out its own.
void authoredStandIn(WorldMapData& world, const WorldLayout& layout) {
    const auto registry = engine::biomes::active();
    if (!registry || !world.categories || !layout.imported) return;
    const auto zoneOf = [](const std::string& z) -> std::optional<Climate> {
        if (z == "steppe") return Climate::Steppe;
        if (z == "taiga") return Climate::Taiga;
        if (z == "temperate_forest") return Climate::TemperateForest;
        if (z == "tropical_forest") return Climate::TropicalForest;
        if (z == "mediterranean") return Climate::Mediterranean;
        if (z == "savanna") return Climate::Savanna;
        if (z == "tundra") return Climate::Tundra;
        if (z == "alpine") return Climate::Alpine;
        if (z == "desert") return Climate::Desert;
        if (z == "ice") return Climate::Ice;
        return std::nullopt;
    };
    const std::int32_t cellsPerRegion = std::int32_t(kRegionMetres / kMetresPerCell);
    std::size_t authored = 0;
    for (std::int32_t y = 0; y < world.height; ++y)
        for (std::int32_t x = 0; x < world.width; ++x) {
            WorldCell& c = world.cells[std::size_t(y) * std::size_t(world.width) + std::size_t(x)];
            if (c.sea) continue;
            const std::int32_t rx = x / cellsPerRegion, ry = y / cellsPerRegion;
            if (!importedIn(layout, rx, ry) ||
                std::uint8_t(layout.at(rx, ry).stage) >= std::uint8_t(RegionStage::Relief))
                continue;
            const double mx = (double(x) + 0.5) * kMetresPerCell, my = (double(y) + 0.5) * kMetresPerCell;
            const auto ids = world.categories->at(mx, my);
            const auto* category = ids[0] ? registry->category(ids[0]) : nullptr;
            if (!category) continue;
            const auto zone = zoneOf(category->climateZone);
            if (!zone) continue;
            double moisture = category->controls.moisture.value_or(0.5);
            if (const auto ground = layout.imported->ground(rx, ry, false); ground && !ground->moisture.empty())
                moisture = double(ground->maskAt(ground->moisture, std::int64_t(mx) - std::int64_t(rx) * kRegionMetres,
                                                 std::int64_t(my) - std::int64_t(ry) * kRegionMetres, 128)) / 255.0;
            c.climate = *zone;
            // The generator's own scale: forty is dust, a hundred and forty
            // holds the grass on the hillsides, two hundred and thirty a bog.
            c.moisture = std::uint8_t(std::clamp(std::lround(30.0 + moisture * 215.0), 0L, 255L));
            c.temperature = std::uint8_t(std::clamp(std::lround(40.0 + category->warmth.value_or(0.55) * 200.0), 0L, 255L));
            c.fertility = std::uint8_t(std::clamp(std::lround(category->fertility.value_or(0.5) * 255.0), 0L, 255L));
            // And the soil fields the ecology reads (world_builder ecologyAt):
            // at the primary stage they are held, and all nought.
            const std::size_t i = std::size_t(y) * std::size_t(world.width) + std::size_t(x);
            if (world.humidityField.size() == world.cells.size()) world.humidityField.set(i, c.moisture);
            if (world.soilFertilityField.size() == world.cells.size()) world.soilFertilityField.set(i, c.fertility);
            if (world.soilDrainageField.size() == world.cells.size() && world.soilDrainageField[i] == 0)
                world.soilDrainageField.set(i, 128);
            ++authored;
        }
    if (authored) std::cerr << "imported ground at its primary stage: " << authored << " cells under the categories' climate\n";
}

} // namespace

WorldMapData generateLayoutWorld(const WorldLayout& layout, ComposeReport* report) {
    ComposeReport local;
    ComposeReport& out = report ? *report : local;
    out = ComposeReport{};
    // One run over the whole world - unless there is nothing to run: a world
    // of open sea is laid down as sea, whatever its size, and costs a moment.
    // Nor when any region is made by hand under the staged pipeline: each of
    // those is its own run, taken only as far as its stage (and a sketch not
    // at all).
    bool staged = false;
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx) staged |= authored(layout, rx, ry);
    if (!staged && isWholeWorld(layout) && (layout.anyGenerated() || layout.anyPainted())) {
        out.wholeWorld = true;
        out.runs = 1;
        const auto began = Clock::now();
        auto map = generateWorldMap(paramsFor(layout));
        out.runMs = millis(began);
        return map;
    }

    const std::int32_t W = layout.widthCells(), H = layout.heightCells();
    const std::size_t count = std::size_t(W) * std::size_t(H);
    const std::size_t regionCount = layout.regions.size();

    // --- what each region is made from -----------------------------------
    // Its own generation; or, painted and never generated, a run of its own
    // as a region made by hand; or, sea inside some generation's rectangle,
    // that generation's sea (its sea floor meets its coast); or open sea.
    std::vector<Run> runs;
    std::vector<std::int32_t> runOfGeneration(layout.generations.size(), -1);
    std::vector<std::int32_t> regionRun(regionCount, -1);
    const auto begin = Clock::now();
    // How many runs there will be, for the progress line.
    std::int64_t expectedRuns = 0;
    {
        std::vector<char> seen(layout.generations.size(), 0);
        for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
            for (std::int32_t rx = 0; rx < layout.regionsX; ++rx) {
                const Region& region = layout.at(rx, ry);
                if (region.source >= 0 && std::size_t(region.source) < seen.size()) {
                    expectedRuns += seen[std::size_t(region.source)] ? 0 : 1;
                    seen[std::size_t(region.source)] = 1;
                } else if (region.stage != RegionStage::Sketch && (importedIn(layout, rx, ry) || paintedIn(layout, rx, ry))) {
                    ++expectedRuns;
                }
            }
    }
    const auto runGeneration = [&](const Generation& g, std::shared_ptr<const ImportedGround> ground) {
        WorldLayout sub = generationLayout(layout, g);
        // The offset is folded into the run's place below; a run shifted with
        // the world is the same run (and the same key in the cache).
        sub.latitude.rowOffset = 0;
        WorldMapParams params = paramsFor(sub);
        params.latitude = sub.latitude;
        params.originX = g.x * kCellsPerRegion;
        // Where it is for its latitude: the rows the world grew by in the
        // north since the latitude was set do not move it (Latitude::rowOffset).
        params.originY = g.y * kCellsPerRegion - layout.latitude.rowOffset;
        stageOf(g, params);
        // An imported region is made by hand whatever stage it says: its
        // ground is the skeleton's, not a preset's.
        // Built at the step the composite keeps: a sixteen-region world holds
        // its ground at a kilometre, and a run at sixty-four metres would be
        // two hundred and fifty times the memory for what is sampled down.
        if (ground) {
            params.authored = true;
            params.imported = ground;
            params.foundationStep = compositeFoundationStepFor(std::int64_t(layout.widthCells()) * kMetresPerCell,
                                                               std::int64_t(layout.heightCells()) * kMetresPerCell);
        }
        Run run;
        run.generation = g;
        run.originX = params.originX;
        run.originY = g.y * kCellsPerRegion;
        run.drains = params.stage != GenerationStage::Primary;
        run.wet = params.stage == GenerationStage::Full;
        RunKey key{std::move(sub), params.latitude, params.originX, params.originY, params.stage, params.authored,
                   ground ? ground->key : 0, params.foundationStep};
        core::progress("working out regions", std::int64_t(runs.size()), expectedRuns);
        run.map = cachedRun(key);
        if (run.map) {
            ++out.cachedRuns;
        } else {
            run.map = std::make_shared<const WorldMapData>(generateWorldMap(params));
            keepRun(std::move(key), run.map);
        }
        runs.push_back(std::move(run));
        return std::int32_t(runs.size() - 1);
    };
    for (std::size_t r = 0; r < regionCount; ++r) {
        const Region& region = layout.regions[r];
        if (region.source < 0 || std::size_t(region.source) >= layout.generations.size()) continue;
        auto& index = runOfGeneration[std::size_t(region.source)];
        if (index < 0) index = runGeneration(layout.generations[std::size_t(region.source)], nullptr);
        regionRun[r] = index;
    }
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx) {
            const std::size_t r = layout.indexOf(rx, ry);
            if (regionRun[r] >= 0) continue;
            // A sketch is drawn, not computed: until it is pinned its region
            // is open sea to everything that is worked out.
            if (layout.regions[r].stage == RegionStage::Sketch) {
                ++out.sketchRegions;
                ++out.seaRegions;
                continue;
            }
            const bool imported = importedIn(layout, rx, ry);
            if (imported || paintedIn(layout, rx, ry)) {
                Generation hand;
                hand.x = rx; hand.y = ry; hand.w = hand.h = 1;
                hand.seed = regionSeed(layout.seed ^ 0x4A11D5EEDull, rx, ry);
                Region made;
                made.generated = true;
                made.settings = layout.regions[r].settings;
                made.settings.seed = hand.seed;
                made.settings.manual = true;
                made.stage = layout.regions[r].stage;
                if (imported && (made.stage == RegionStage::Full || made.stage == RegionStage::Sketch))
                    made.stage = RegionStage::Primary;
                hand.regions = {made};
                // Imported ground is drained only once the region has been
                // taken to its drainage: before that it is the skeleton as
                // it was drawn.
                const bool drained = made.stage == RegionStage::Relief || made.stage == RegionStage::Water;
                regionRun[r] = runGeneration(hand, imported ? layout.imported->ground(rx, ry, drained) : nullptr);
                continue;
            }
            for (std::size_t g = 0; g < layout.generations.size(); ++g)
                if (runOfGeneration[g] >= 0 && layout.generations[g].contains(rx, ry)) {
                    regionRun[r] = runOfGeneration[g];
                    break;
                }
            if (regionRun[r] < 0) ++out.seaRegions;
        }
    out.runs = std::int32_t(runs.size());
    out.runMs = millis(begin);
    core::progress("putting the regions together");
    const auto composing = Clock::now();

    // --- the macro map ------------------------------------------------------
    WorldMapData world;
    world.width = W;
    world.height = H;
    world.seed = layout.seed;
    world.terrainStage = TerrainStage::Final;
    allocate(world, count);
    const auto regionOfCell = [&](std::int32_t x, std::int32_t y) {
        return layout.indexOf(std::min(layout.regionsX - 1, x / kCellsPerRegion),
                              std::min(layout.regionsY - 1, y / kCellsPerRegion));
    };
    for (std::int32_t y = 0; y < H; ++y)
        for (std::int32_t x = 0; x < W; ++x) {
            const std::size_t i = std::size_t(y) * W + x;
            const std::int32_t r = regionRun[regionOfCell(x, y)];
            if (r < 0) { seaCell(world, i, y - layout.latitude.rowOffset, layout.latitude); continue; }
            const Run& run = runs[std::size_t(r)];
            const std::int32_t lx = x - run.originX, ly = y - run.originY;
            copyCell(world, i, run, std::size_t(ly) * run.map->width + lx);
        }
    if (!runs.empty()) {
        world.constants = runs.front().map->constants;
        world.constants.worldSeed = layout.seed;
        world.constants.worldWidth = W;
        world.constants.worldHeight = H;
    }
    // The peoples each run settled, on the ground now taken from that run; the
    // played community from the first run that has one.
    bool played = false;
    for (std::size_t r = 0; r < runs.size(); ++r)
        for (WorldSite site : runs[r].map->sites) {
            site.cell = {site.cell.x + runs[r].originX, site.cell.y + runs[r].originY};
            if (!world.inBounds(site.cell) || regionRun[regionOfCell(site.cell.x, site.cell.y)] != std::int32_t(r)) continue;
            if (site.played && played) site.played = false;
            if (site.played) {
                played = true;
                world.playedCell = site.cell;
                const std::int32_t half = kCellsPerLocalMap / 2;
                world.playedBlock = {std::clamp(site.cell.x - half, 0, std::max(0, W - kCellsPerLocalMap)),
                                     std::clamp(site.cell.y - half, 0, std::max(0, H - kCellsPerLocalMap))};
            }
            world.sites.push_back(std::move(site));
        }
    // The rivers dial of land made by hand, which the drainage graph reads
    // again: the world has one, and a run that was made with it carries it.
    for (const Run& run : runs)
        if (run.map->riverShare != 1.0f) world.riverShare = run.map->riverShare;
    // Large continents require substantial independent catchments. Smaller
    // tributaries are retained inside the river trees selected by the graph.
    if (std::int64_t(layout.regionsX) * layout.regionsY > 16) world.graphRiverFlow = 11;
    for (const Run& run : runs) {
        world.lakesKept += run.map->lakesKept;
        world.basinsDried += run.map->basinsDried;
    }

    // --- the foundation ---------------------------------------------------
    auto foundation = std::make_shared<TerrainFoundation>();
    TerrainFoundation& f = *foundation;
    f.step = compositeFoundationStepFor(std::int64_t(W) * kMetresPerCell, std::int64_t(H) * kMetresPerCell);
    f.stepShift = 0;
    while ((1 << f.stepShift) < f.step) ++f.stepShift;
    f.columns = int((std::int64_t(W) * kMetresPerCell + f.step - 1) / f.step + 1);
    f.rows = int((std::int64_t(H) * kMetresPerCell + f.step - 1) / f.step + 1);
    const std::size_t lattice = std::size_t(f.columns) * std::size_t(f.rows);
    for (auto& stage : f.heightDm) stage.assign(lattice, kSeaFloorDm);
    f.pageColumns = W;
    f.pageRows = H;
    f.detailPages.assign(std::size_t(W) * std::size_t(H), 0);
    // Which run each lattice point is taken from: its region's.
    const auto pointRun = [&](std::size_t p) {
        const std::int64_t mx = std::int64_t(p % std::size_t(f.columns)) * f.step;
        const std::int64_t my = std::int64_t(p / std::size_t(f.columns)) * f.step;
        return regionRun[regionOfCell(std::int32_t(std::min<std::int64_t>(W - 1, mx / kMetresPerCell)),
                                      std::int32_t(std::min<std::int64_t>(H - 1, my / kMetresPerCell)))];
    };
    for (int j = 0; j < f.rows; ++j)
        for (int i = 0; i < f.columns; ++i) {
            const std::size_t p = std::size_t(j) * f.columns + i;
            const std::int32_t r = pointRun(p);
            if (r < 0) continue;
            const std::int64_t mx = std::int64_t(i) * f.step, my = std::int64_t(j) * f.step;
            for (std::size_t s = 0; s < f.heightDm.size(); ++s) f.heightDm[s].set(p, runSample(runs[std::size_t(r)], s, mx, my));
        }
    for (std::int32_t y = 0; y < H; ++y)
        for (std::int32_t x = 0; x < W; ++x) {
            const std::int32_t r = regionRun[regionOfCell(x, y)];
            if (r < 0) continue;
            const Run& run = runs[std::size_t(r)];
            const TerrainFoundation* rf = run.map->terrainFoundation.get();
            if (!rf || rf->detailPages.empty()) continue;
            const std::int32_t lx = x - run.originX, ly = y - run.originY;
            if (lx < rf->pageColumns && ly < rf->pageRows)
                f.detailPages[std::size_t(y) * W + x] = rf->detailPages[std::size_t(ly) * rf->pageColumns + lx];
        }
    for (const Run& run : runs)
        if (const TerrainFoundation* rf = run.map->terrainFoundation.get()) {
            f.thermalCells += rf->thermalCells;
            f.thermalMetres += rf->thermalMetres;
            for (std::size_t s = 0; s < f.milliseconds.size(); ++s) f.milliseconds[s] += rf->milliseconds[s];
        }

    // --- seams ------------------------------------------------------------
    // Where two runs meet, both are pulled to their average at the border and
    // back to themselves a band's width away: the ground is one surface across
    // the seam. Each stage of the foundation on its own.
    const int band = std::max(2, int(std::clamp(layout.blendMetres, 2 * f.step, kMaxRegionBlendMetres) / f.step));
    const int perRegion = int(kRegionM / f.step);
    std::vector<std::uint8_t> nearSeam(lattice, 0);
    const auto blendSeam = [&](bool vertical, int line, int from, int to, std::int32_t before, std::int32_t after) {
        // `line` is the lattice column (or row) of the border; it belongs to
        // the run after it. `from`..`to` is its extent along the border.
        const Run& a = runs[std::size_t(before)];
        for (int t = from; t <= to && t < (vertical ? f.rows : f.columns); ++t) {
            const std::int64_t bx = std::int64_t(vertical ? line : t) * f.step;
            const std::int64_t by = std::int64_t(vertical ? t : line) * f.step;
            const std::size_t seamPoint = vertical ? std::size_t(t) * f.columns + line : std::size_t(line) * f.columns + t;
            for (std::size_t s = 0; s < f.heightDm.size(); ++s) {
                const std::int32_t hb = runSample(a, s, bx, by);
                const std::int32_t ha = f.heightDm[s][seamPoint];
                const double d = double(ha - hb);
                for (int k = -band; k <= band; ++k) {
                    const int u = line + k;
                    if (u < 0 || u >= (vertical ? f.columns : f.rows)) continue;
                    const std::size_t p = vertical ? std::size_t(t) * f.columns + u : std::size_t(u) * f.columns + t;
                    const std::int32_t owner = pointRun(p);
                    const bool mineAfter = k >= 0 && owner == after;
                    const bool mineBefore = k < 0 && owner == before;
                    if (!mineAfter && !mineBefore) continue;
                    const double w = 0.5 * fall(double(std::abs(k)) / band);
                    const auto moved = std::int32_t(std::lround(mineAfter ? -d * w : d * w));
                    if (moved == 0) continue;
                    f.heightDm[s].set(p, f.heightDm[s][p] + moved);
                    nearSeam[p] = 1;
                }
            }
        }
    };
    for (std::int32_t ry = 0; ry < layout.regionsY; ++ry)
        for (std::int32_t rx = 0; rx < layout.regionsX; ++rx) {
            const std::int32_t here = regionRun[layout.indexOf(rx, ry)];
            if (here < 0) continue;
            if (rx > 0) {
                const std::int32_t left = regionRun[layout.indexOf(rx - 1, ry)];
                if (left >= 0 && left != here) {
                    blendSeam(true, rx * perRegion, ry * perRegion, (ry + 1) * perRegion, left, here);
                    ++out.seams;
                }
            }
            if (ry > 0) {
                const std::int32_t up = regionRun[layout.indexOf(rx, ry - 1)];
                if (up >= 0 && up != here) {
                    blendSeam(false, ry * perRegion, rx * perRegion, (rx + 1) * perRegion, up, here);
                    ++out.seams;
                }
            }
        }
    // The macro cells over a seam follow the joined ground; every other cell
    // is its run's, as it was.
    for (std::int32_t y = 0; y < H; ++y)
        for (std::int32_t x = 0; x < W; ++x) {
            const int i0 = int(std::int64_t(x) * kMetresPerCell / f.step), j0 = int(std::int64_t(y) * kMetresPerCell / f.step);
            const std::size_t p = std::size_t(j0) * f.columns + i0;
            if (!nearSeam[p]) continue;
            WorldCell& c = world.cells[std::size_t(y) * W + x];
            const std::int32_t h = f.heightDm[4][p];
            const bool wasSea = c.sea;
            c.sea = h <= 0;
            c.elevation = c.sea ? 0 : std::uint8_t(std::clamp<std::int64_t>(std::lround(h / 10.0 / kMetresPerElevationStep), 1, 255));
            if (!c.sea) f.detailPages[std::size_t(y) * W + x] = 1;
            if (wasSea && !c.sea) c.climate = Climate::TemperateForest;
        }
    for (std::size_t s = 0; s + 1 < f.heightDm.size(); ++s) f.heightDm[s].shareEqualChunks(f.heightDm[4]);
    routeFoundation(f);
    f.seal();
    world.terrainFoundation = foundation;

    // --- landforms ----------------------------------------------------------
    {
        const Run* first = nullptr;
        for (const Run& run : runs) if (run.map->hybridTerrain) { first = &run; break; }
        if (first) {
            auto hybrid = std::make_shared<HybridTerrain>();
            hybrid->width = W;
            hybrid->height = H;
            hybrid->version = first->map->hybridTerrain->version;
            hybrid->macroDelta.assign(count, 0);
            for (std::int32_t y = 0; y < H; ++y)
                for (std::int32_t x = 0; x < W; ++x) {
                    const std::int32_t r = regionRun[regionOfCell(x, y)];
                    if (r < 0) continue;
                    const Run& run = runs[std::size_t(r)];
                    if (!run.map->hybridTerrain) continue;
                    const auto& delta = run.map->hybridTerrain->macroDelta;
                    const std::size_t li = std::size_t(y - run.originY) * run.map->width + std::size_t(x - run.originX);
                    if (li < delta.size()) hybrid->macroDelta[std::size_t(y) * W + x] = delta[li];
                }
            for (std::size_t r = 0; r < runs.size(); ++r) {
                const Run& run = runs[r];
                if (!run.map->hybridTerrain) continue;
                const std::int64_t ox = std::int64_t(run.originX) * kMetresPerCell, oy = std::int64_t(run.originY) * kMetresPerCell;
                for (LandformPatch patch : run.map->hybridTerrain->patches) {
                    const std::int64_t cx = ox + patch.originX + patch.radius, cy = oy + patch.originY + patch.radius;
                    if (cx < 0 || cy < 0 || cx >= std::int64_t(W) * kMetresPerCell || cy >= std::int64_t(H) * kMetresPerCell)
                        continue;
                    if (regionRun[regionOfCell(std::int32_t(cx / kMetresPerCell), std::int32_t(cy / kMetresPerCell))] !=
                        std::int32_t(r))
                        continue;
                    patch.originX = std::int32_t(ox + patch.originX);
                    patch.originY = std::int32_t(oy + patch.originY);
                    if (std::int64_t(patch.originX) + 2 * patch.radius >= std::int64_t(W) * kMetresPerCell ||
                        std::int64_t(patch.originY) + 2 * patch.radius >= std::int64_t(H) * kMetresPerCell)
                        continue;
                    hybrid->patches.push_back(std::move(patch));
                }
                for (const auto& note : run.map->hybridTerrain->diagnostics) hybrid->diagnostics.push_back(note);
            }
            std::int32_t incision = 0;
            for (const Run& run : runs)
                if (run.map->hybridTerrain) incision = std::max(incision, run.map->hybridTerrain->incisionStep);
            hybrid->incisionStep = incision;
            if (incision > 0) {
                const auto columns = (std::int64_t(W) * kMetresPerCell + incision - 1) / incision + 1;
                const auto rows = (std::int64_t(H) * kMetresPerCell + incision - 1) / incision + 1;
                hybrid->incision.assign(std::size_t(columns * rows), 0);
                for (std::int64_t j = 0; j < rows; ++j)
                    for (std::int64_t i = 0; i < columns; ++i) {
                        const std::int64_t mx = i * incision, my = j * incision;
                        const std::int32_t cx = std::int32_t(std::min<std::int64_t>(W - 1, mx / kMetresPerCell));
                        const std::int32_t cy = std::int32_t(std::min<std::int64_t>(H - 1, my / kMetresPerCell));
                        const std::int32_t r = regionRun[regionOfCell(cx, cy)];
                        if (r < 0) continue;
                        const Run& run = runs[std::size_t(r)];
                        if (!run.map->hybridTerrain) continue;
                        const Fixed metres = run.map->hybridTerrain->incisionAt(
                                Fixed::fromInt(mx - std::int64_t(run.originX) * kMetresPerCell),
                                Fixed::fromInt(my - std::int64_t(run.originY) * kMetresPerCell));
                        hybrid->incision[std::size_t(j * columns + i)] =
                                std::uint16_t(std::clamp<std::int64_t>((metres * Fixed::fromInt(10)).roundToInt(), 0, 3000));
                    }
            }
            hybrid->prepare();
            world.hybridTerrain = hybrid;
        }
    }

    // --- rivers across the seams -------------------------------------------
    // Land masses that now take ground from more than one run: their water
    // runs across what were the edges of those runs, and is worked out again
    // over the whole of each. Every other land mass keeps its run's rivers.
    // Only water that was asked for: a land mass none of whose runs has its
    // water (imported or pinned ground not yet taken to the Water stage)
    // gets none here either, and cells of such runs on a land mass that does
    // are left as dry as their run made them.
    {
        std::vector<std::int32_t> label(count, -1);
        std::int32_t next = 0;
        DrainScratch scratch;
        for (std::size_t start = 0; start < count; ++start) {
            if (label[start] >= 0 || world.cells[start].sea) continue;
            std::vector<std::size_t> members, pending{start};
            label[start] = next;
            std::int32_t firstRun = -2;
            bool mixed = false, wet = false;
            while (!pending.empty()) {
                const std::size_t i = pending.back();
                pending.pop_back();
                members.push_back(i);
                const std::int32_t x = std::int32_t(i % std::size_t(W)), y = std::int32_t(i / std::size_t(W));
                const std::int32_t r = regionRun[regionOfCell(x, y)];
                if (firstRun == -2) firstRun = r;
                else if (r != firstRun) mixed = true;
                wet |= r >= 0 && runs[std::size_t(r)].wet;
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour({x, y}, dir);
                    if (!world.inBounds(n)) continue;
                    const std::size_t j = std::size_t(n.y) * W + n.x;
                    if (label[j] >= 0 || world.cells[j].sea) continue;
                    label[j] = next;
                    pending.push_back(j);
                }
            }
            ++next;
            if (!mixed || !wet) continue;
            drainLandmass(world, members, scratch);
            // What the runs did not work out stays not worked out.
            for (const auto i : members) {
                const std::int32_t x = std::int32_t(i % std::size_t(W)), y = std::int32_t(i / std::size_t(W));
                const std::int32_t r = regionRun[regionOfCell(x, y)];
                if (r < 0 || runs[std::size_t(r)].wet) continue;
                WorldCell& c = world.cells[i];
                c.river = false;
                c.riverOut = -1;
                c.riverSize = 0;
                world.flowDirectionField[i] = std::int8_t(-1);
                world.riverDischargeField[i] = 0;
                world.riverSourceField[i] = 0;
                if (runs[std::size_t(r)].drains) continue;
                c.drainOut = -1;
                c.drainSize = 0;
                // As the run left them.
                const Run& run = runs[std::size_t(r)];
                const std::size_t li = std::size_t(y - run.originY) * std::size_t(run.map->width) + std::size_t(x - run.originX);
                if (li < run.map->flowAccumulationField.size())
                    world.flowAccumulationField[i] = run.map->flowAccumulationField[li];
                if (li < run.map->hydrologicallyCorrectedHeightField.size())
                    world.hydrologicallyCorrectedHeightField[i] = run.map->hydrologicallyCorrectedHeightField[li];
            }
            ++out.landmasses;
        }
    }
    // The categories painted with the import (engine/biomes), read once by
    // the source and shared.
    if (layout.imported) {
        world.categories = layout.imported->categories();
        world.details = layout.imported->details();
        world.features = layout.imported->features();
        authoredStandIn(world, layout);
    }
    out.composeMs = millis(composing);
    return world;
}

} // namespace generation
