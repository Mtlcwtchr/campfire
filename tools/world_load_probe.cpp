#include "engine/biomes/category_field.hpp"
#include "engine/biomes/registry.hpp"
#include <optional>
// What opening a world costs, pass by pass: the world made from a preset over
// N x N regions (as the client's "new world" makes it), then generated, then
// published as a snapshot (drainage graph, climate, landmarks), then the
// pages prebaked - everything the loading screen waits for before the first
// frame of ground can be drawn.
//
//   world_load_probe [--regions N] [--preset NAME] [--seed S] [--no-prebake] [--disk]
//   world_load_probe --import <height.png | package dir> WxH [--drain | --water] [--rivers]
//       an empty world of W x H regions, the picture (black -100 m, white
//       3000 m) or package imported over all of it, and the world built on it
//
// The disk cache of pages is off unless --disk: a cold open is what is being
// measured, and a warm one only measures the disk.
#include <chrono>
#include <map>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <mach/mach.h>

#include "game/generation/world_layout.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/generation/world_brush.hpp"
#include "game/generation/world_compose.hpp"
#include "game/generation/world_import.hpp"
#include "engine/world_source/transfer.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/world_sketch.hpp"
#include "game/world/world_builder.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::filesystem::path contentDirectory() {
    std::filesystem::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 5 && !std::filesystem::exists(content, ec); ++up) content = ".." / content;
    return content;
}
} // namespace

// What the process holds now, in megabytes.
std::size_t residentMb() {
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS)
        return 0;
    return std::size_t(info.resident_size / (1024 * 1024));
}

int main(int argc, char** argv) {
    int regions = 1;
    std::string presetName;
    std::uint64_t seed = 4242;
    bool prebake = true, disk = false, grow = false, reference = false, stages = false;
    int empty = 0, emptyHigh = 0;
    std::string importFrom;
    bool water = false, rivers = false, drain = false;
    std::string saveTo;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--regions") regions = std::max(1, std::atoi(next().c_str()));
        else if (a == "--preset") presetName = next();
        else if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--no-prebake") prebake = false;
        else if (a == "--disk") disk = true;
        else if (a == "--grow") grow = true;
        else if (a == "--reference") reference = true;
        else if (a == "--stages") stages = true;
        else if (a == "--empty") {
            // N, or W x H in regions: --empty 6x15 is 786 x 1966 km.
            const std::string size = next();
            empty = std::max(1, std::atoi(size.c_str()));
            const auto cross = size.find('x');
            emptyHigh = cross == std::string::npos ? empty : std::max(1, std::atoi(size.c_str() + cross + 1));
        }
        else if (a == "--import") {
            importFrom = next();
            const std::string size = next();
            empty = std::max(1, std::atoi(size.c_str()));
            const auto cross = size.find('x');
            emptyHigh = cross == std::string::npos ? empty : std::max(1, std::atoi(size.c_str() + cross + 1));
        }
        else if (a == "--water") water = true;
        else if (a == "--drain") drain = true;
        else if (a == "--rivers") rivers = true;
        else if (a == "--save") saveTo = next();
        else { std::cerr << "unknown argument " << a << "\n"; return 2; }
    }
    if (!importFrom.empty()) {
        namespace ws = engine::world_source;
        auto layout = generation::emptyLayout(empty, emptyHigh, seed);
        layout.latitude.fixed = true;
        layout.latitude.northDegrees = 55;
        layout.latitude.kmPerDegree = 111;   // the Earth's: a continent spans ten degrees, not two hundred
        // Into a world of its own when asked (--save worlds/name.json: the
        // layout written there and the source in its history root, as the
        // client keeps them); a scratch one otherwise.
        const auto root = saveTo.empty() ? std::filesystem::temp_directory_path() / "campfire_probe" / "source"
                                         : std::filesystem::path(saveTo).parent_path() /
                                                   std::filesystem::path(saveTo).stem() / "source";
        std::filesystem::remove_all(root.parent_path());
        ws::ImportTarget target;
        target.world = ws::WorldExtent{double(layout.widthMetres()), double(layout.heightMetres()), 256, 32768};
        target.rect = std::array<double, 4>{0, 0, double(layout.widthMetres()), double(layout.heightMetres())};
        target.rastersOnly = true;
        // The categorical layers onto the engine's numbering, by name.
        if (const auto registry = engine::biomes::Registry::load(engine::biomes::defaultDirectory());
            !registry->categories().empty())
            for (std::size_t k = 0; k < engine::biomes::kLayers; ++k)
                target.legends[engine::biomes::kLayerNames[k]] = registry->legend(engine::biomes::Layer(k));
        std::string why;
        auto t = Clock::now();
        std::optional<ws::ImportReport> report;
        if (std::filesystem::is_directory(importFrom)) {
            report = ws::importPackage(importFrom, root, target, &why);
        } else {
            ws::LooseImages images;
            images.height = importFrom;
            report = ws::importImages(images, root, target, &why);
        }
        if (!report) { std::cerr << "import: " << why << "\n"; return 1; }
        std::cout << "import " << ms(t, Clock::now()) << " ms: " << report->chunksWritten << " chunks, coast at grey "
                  << report->seaGrey << ", "
                  << residentMb() << " MB\n";
        t = Clock::now();
        // Heights alone unless the drainage (--drain) or the water (--water)
        // is asked for: only then is the drained height baked.
        layout.imported = generation::ImportedSource::open(root, water || drain);
        std::cout << (water || drain ? "opened and baked " : "opened ") << ms(t, Clock::now()) << " ms, "
                  << residentMb() << " MB\n";
        const std::int32_t held = layout.imported ? layout.imported->regionsHeld() : 0;
        for (auto& r : layout.regions)
            r.stage = water ? generation::RegionStage::Water
                    : drain ? generation::RegionStage::Relief
                            : generation::RegionStage::Primary;
        std::cout << "regions with land: " << held << " of " << layout.regions.size() << "\n";
        t = Clock::now();
        generation::ComposeReport composed;
        const auto map = generation::generateLayoutWorld(layout, &composed);
        std::cout << "built " << ms(t, Clock::now()) << " ms: " << composed.runs << " runs (" << composed.runMs
                  << " ms), compose " << composed.composeMs << " ms, foundation step "
                  << (map.terrainFoundation ? map.terrainFoundation->step : 0) << " m, " << residentMb() << " MB\n";
        std::size_t land = 0;
        for (const auto& c : map.cells) land += !c.sea;
        std::cout << "land cells " << land << " of " << map.cells.size() << "\n";
        // The terrain categories the import painted (engine/biomes): the share
        // of the land each takes, by the registry's names.
        if (map.categories) {
            const auto registry = engine::biomes::Registry::load(engine::biomes::defaultDirectory());
            const auto counts = map.categories->groundCounts();
            std::size_t total = 0;
            for (const auto n : counts) total += n;
            std::cout << "categories (" << map.categories->chunksHeld() << " chunks, "
                      << map.categories->bytes() / (1024 * 1024) << " MB):";
            for (std::size_t id = 0; id < counts.size(); ++id) {
                if (!counts[id]) continue;
                const auto* c = registry->category(std::uint32_t(id));
                std::printf(" %s %.1f%%", c ? c->name.c_str() : std::to_string(id).c_str(),
                            100.0 * double(counts[id]) / double(std::max<std::size_t>(1, total)));
            }
            std::cout << "\n";
        } else {
            std::cout << "categories: none painted\n";
        }
        {
            std::array<std::size_t, 8> moist{}, warm{};
            for (const auto& c : map.cells) {
                if (c.sea) continue;
                ++moist[std::min<std::size_t>(7, c.moisture / 32)];
                ++warm[std::min<std::size_t>(7, c.temperature / 32)];
            }
            std::cout << "land moisture per 32:";
            for (const auto m : moist) std::cout << " " << m;
            std::map<int, std::size_t> climates;
            for (const auto& c : map.cells) if (!c.sea) ++climates[int(c.climate)];
            std::cout << "\nland climates:";
            for (const auto& [k, n] : climates) std::cout << " " << k << ":" << n;
            // What the ground is made of where it is land: the winning material,
            // one sample in eight cells, as the page baker asks it.
            {
                world::HeightField field(&map, map.seed);
                std::array<std::size_t, 6> wins{};
                std::array<double, 6> mean{};
                std::array<std::size_t, 6> heights{}, moistures{};
                std::size_t n = 0;
                for (std::int32_t y = 0; y < map.height; y += 8)
                    for (std::int32_t x = 0; x < map.width; x += 8) {
                        if (map.at({x, y}).sea) continue;
                        const std::int64_t stride = 64;
                        const std::int64_t sx = (std::int64_t(x) * generation::kMetresPerCell + 256) / world::kSampleMetres;
                        const std::int64_t sy = (std::int64_t(y) * generation::kMetresPerCell + 256) / world::kSampleMetres;
                        const auto w = field.materialsGiven(sx, sy, core::kZero, core::kZero, stride);
                        const double here = field.heightAt({core::Fixed::fromInt(sx * world::kSampleMetres), core::Fixed::fromInt(sy * world::kSampleMetres)}).toDouble();
                        heights[std::min<std::size_t>(5, std::size_t(std::max(0.0, here) / 50.0))] += 1;
                        moistures[std::min<std::size_t>(5, map.at({x, y}).moisture / 43)] += 1;
                        std::size_t best = 0;
                        for (std::size_t m = 0; m < 6; ++m) {
                            mean[m] += w.weight[m].toDouble();
                            if (w.weight[m] > w.weight[best]) best = m;
                        }
                        ++wins[best];
                        ++n;
                    }
                std::cout << "\nmaterials (grass dirt sand rock marsh snow) winning:";
                for (const auto v : wins) std::cout << " " << v;
                std::cout << "; mean:";
                for (const auto v : mean) std::cout << " " << int(100 * v / std::max<std::size_t>(1, n)) << "%";
                std::cout << "; heights per 50 m:";
                for (const auto v : heights) std::cout << " " << v;
                std::cout << "; moisture per 43:";
                for (const auto v : moistures) std::cout << " " << v;
            }
            std::cout << "\nland temperature per 32:";
            for (const auto m : warm) std::cout << " " << m;
            std::cout << "\n";
        }
        // What the ground came out as: the land's heights, in tenths of its range.
        if (map.terrainFoundation) {
            const auto& f = *map.terrainFoundation;
            std::array<std::size_t, 12> bins{};
            std::int32_t low = 1 << 30, high = -(1 << 30);
            for (std::size_t i = 0; i < f.heightDm[4].size(); i += 7) {
                const auto h = f.heightDm[4][i];
                if (h <= 0) continue;
                low = std::min(low, h); high = std::max(high, h);
                ++bins[std::size_t(std::clamp(h / 3000, 0, 11))];
            }
            std::cout << "land heights " << low / 10 << " .. " << high / 10 << " m; per 300 m:";
            for (const auto b : bins) std::cout << " " << b;
            std::cout << "\n";
        }
        if (rivers) {
            // The drainage graph the snapshot builds (ASR_HYDRO_TRACE=1 for its steps).
            t = Clock::now();
            const auto graph = world::streaming::buildHydrologyGraph(map);
            std::cout << "rivers " << ms(t, Clock::now()) << " ms: " << graph.segments.size() << " courses, "
                      << graph.nodes.size() << " nodes, " << residentMb() << " MB\n";
        }
        if (!saveTo.empty()) {
            std::cout << (generation::saveWorldLayout(layout, saveTo) ? "saved " : "could not save ") << saveTo << "\n";
            return 0;
        }
        std::filesystem::remove_all(root);
        std::filesystem::remove_all(generation::ImportedSource::bakedRootOf(root));
        return 0;
    }
    const auto presets = generation::loadWorldPresets(contentDirectory() / "config" / "world_presets.json");
    if (stages) {
        // The authoring pipeline on the reference world: two islands sketched
        // side by side, pinned, a range painted on one, the water asked for -
        // and then a stroke on one of them. What each step costs, and what an
        // edit in one region makes again.
        auto layout = generation::emptyLayout(6, 15, seed);
        layout.latitude.fixed = true;
        layout.latitude.northDegrees = 60;
        layout.latitude.kmPerDegree = layout.heightMetres() / 1000.0 / 25.0;
        const double R = double(generation::kRegionMetres);
        generation::Brush land;
        land.tool = generation::BrushTool::Paint;
        land.value = 900;
        land.strength = 1.0f;
        land.hardness = 0.7f;
        land.radius = generation::legalRadius(generation::LayerId::Continents, layout, 22000);
        for (const int rx : {2, 3}) {
            generation::beginSketch(layout, rx, 7);
            for (double d = -30000; d <= 30000; d += 4000)
                generation::dab(layout, generation::LayerId::Continents, land, (rx + 0.5) * R + d, 7.5 * R + d * 0.4, nullptr);
        }
        const auto step = [&](const char* what) {
            const auto t = Clock::now();
            generation::ComposeReport report;
            auto map = generation::generateLayoutWorld(layout, &report);
            const double composed = ms(t, Clock::now());
            const auto s = Clock::now();
            // What the step changed in the first island's region, cell by cell.
            static std::optional<generation::WorldMapData> before;
            if (before && before->cells.size() == map.cells.size()) {
                std::size_t changed = 0, moist = 0, high = 0;
                for (std::int32_t y = 7 * generation::kCellsPerRegion; y < 8 * generation::kCellsPerRegion; ++y)
                    for (std::int32_t x = 2 * generation::kCellsPerRegion; x < 3 * generation::kCellsPerRegion; ++x) {
                        const auto& a = before->at({x, y});
                        const auto& b = map.at({x, y});
                        high += a.elevation != b.elevation;
                        moist += a.moisture != b.moisture;
                        changed += a.elevation != b.elevation || a.moisture != b.moisture || a.drainSize != b.drainSize ||
                                   a.drainOut != b.drainOut || a.sea != b.sea || a.river != b.river;
                    }
                std::size_t fields = 0, ground = 0;
                for (std::int32_t y = 7 * generation::kCellsPerRegion; y < 8 * generation::kCellsPerRegion; ++y)
                    for (std::int32_t x = 2 * generation::kCellsPerRegion; x < 3 * generation::kCellsPerRegion; ++x) {
                        const std::size_t i = std::size_t(y) * map.width + x;
                        fields += before->riverDischargeField[i] != map.riverDischargeField[i] ||
                                  before->flowDirectionField[i] != map.flowDirectionField[i] ||
                                  before->lakeLevelField[i] != map.lakeLevelField[i] ||
                                  before->lakeRegionField[i] != map.lakeRegionField[i] ||
                                  before->lakeDepthField[i] != map.lakeDepthField[i];
                    }
                if (before->terrainFoundation && map.terrainFoundation) {
                    const auto& fa = *before->terrainFoundation;
                    const auto& fb = *map.terrainFoundation;
                    const int per = int(generation::kRegionMetres / fa.step);
                    for (int j = 7 * per; j < 8 * per; ++j)
                        for (int k = 2 * per; k < 3 * per; ++k) {
                            const std::size_t i = std::size_t(j) * fa.columns + k;
                            bool d = false;
                            for (std::size_t st = 0; st < 5; ++st) d |= fa.heightDm[st][i] != fb.heightDm[st][i];
                            d |= fa.receiver[i] != fb.receiver[i] || fa.accumulation[i] != fb.accumulation[i];
                            ground += d;
                        }
                }
                std::cout << "    first island's region: " << changed << " cells changed (" << high << " height, "
                          << moist << " moisture), " << fields << " water fields, " << ground << " foundation points\n";
            }
            before = map;
            auto config = world::WorldBuilder::defaultPageConfig();
            config.diskCacheRoot.clear();
            world::WorldSnapshot snapshot(std::move(map), config);
            std::cout << what << ": composed " << composed << " ms (" << report.runs << " runs, " << report.cachedRuns
                      << " from the cache, " << report.sketchRegions << " sketch), snapshot " << ms(s, Clock::now())
                      << " ms (graph " << snapshot.timings().graph << "), holding " << residentMb() << " MB\n";
        };
        const auto sketched = Clock::now();
        const auto mesh = generation::sketchMesh(layout);
        std::cout << "sketch: " << mesh.vertices.size() << " vertices in " << ms(sketched, Clock::now()) << " ms\n";
        step("  as sketched");
        generation::raiseStage(layout, {}, generation::RegionStage::Primary);
        step("  pinned");
        generation::Brush range = land;
        range.value = 700;
        range.radius = generation::legalRadius(generation::LayerId::Ranges, layout, 6000);
        for (double d = -20000; d <= 20000; d += 2000)
            generation::dab(layout, generation::LayerId::Ranges, range, 2.5 * R + d, 7.5 * R + d * 0.4, nullptr);
        generation::raiseStage(layout, {{2, 7}}, generation::RegionStage::Relief);
        step("  a range on the first");
        generation::raiseStage(layout, {}, generation::RegionStage::Water);
        step("  water");
        generation::dab(layout, generation::LayerId::Continents, land, 3.8 * R, 7.2 * R, nullptr);
        step("  a stroke on the second");
        rusage use{};
        getrusage(RUSAGE_SELF, &use);
        std::cout << "  peak memory " << use.ru_maxrss / (1024 * 1024) << " MB\n";
        return 0;
    }
    if (reference) {
        // The world the game is built for, 6 x 15 regions, empty, with a
        // stretch of land made in the middle of it on its own terms.
        auto layout = generation::emptyLayout(6, 15, seed);
        layout.latitude.fixed = true;
        layout.latitude.northDegrees = 60;
        layout.latitude.kmPerDegree = layout.heightMetres() / 1000.0 / 25.0;
        generation::generateRegions(layout, {{2, 7}, {3, 7}}, generation::regionSettingsFrom(presets.front(), seed), seed, true);
        const auto t0 = Clock::now();
        generation::ComposeReport report;
        auto map = generation::generateLayoutWorld(layout, &report);
        std::cout << "reference 6x15 with a 2x1 run: composed in " << ms(t0, Clock::now()) << " ms (" << report.runs
                  << " runs, " << report.seaRegions << " sea regions), holding " << residentMb() << " MB\n";
        std::size_t held = 0, cap = 0;
        held += map.macroHeightField.bytes() + map.temperatureField.bytes() + map.flowDirectionField.bytes();
        cap = map.macroHeightField.size() * 4 + map.temperatureField.size() * 4 + map.flowDirectionField.size();
        std::cout << "  three fields hold " << held / (1024 * 1024) << " MB of the " << cap / (1024 * 1024) << " MB dense\n";
        const auto t1 = Clock::now();
        auto config = world::WorldBuilder::defaultPageConfig();
        config.diskCacheRoot.clear();
        world::WorldSnapshot snapshot(std::move(map), config);
        std::cout << "  snapshot " << ms(t1, Clock::now()) << " ms (graph " << snapshot.timings().graph << ", climate "
                  << snapshot.timings().climate << "), holding " << residentMb() << " MB\n";
        rusage use{};
        getrusage(RUSAGE_SELF, &use);
        std::cout << "  peak memory " << use.ru_maxrss / (1024 * 1024) << " MB\n";
        return 0;
    }
    if (empty > 0) {
        // A world of nothing but sea, N regions a side: what making one costs.
        auto layout = generation::emptyLayout(empty, emptyHigh, seed);
        const auto t0 = Clock::now();
        generation::ComposeReport report;
        auto map = generation::generateLayoutWorld(layout, &report);
        const auto t1 = Clock::now();
        std::cout << "empty " << empty << "x" << emptyHigh << " regions (" << layout.widthMetres() / 1000 << " x "
                  << layout.heightMetres() / 1000 << " km): composed in " << ms(t0, t1) << " ms (" << report.runs
                  << " runs, " << report.seaRegions << " sea regions), holding " << residentMb() << " MB\n";
        if (std::getenv("PROBE_PARTS")) {
            // What the composed world holds, part by part.
            {
                std::size_t fields = 0, tables = 0;
                const auto count = [&](const auto& f) { fields += f.bytes(); tables += f.chunkCount() * 16; };
                const auto& m = map;
                count(m.primaryHeightField); count(m.continentalField); count(m.distanceToCoast); count(m.initialLandMask);
                count(m.upliftField); count(m.riftField); count(m.faultField); count(m.geologyRegion);
                count(m.macroHeightField); count(m.rockTypeField); count(m.erosionResistanceField);
                count(m.soilParentMaterialField); count(m.permeabilityField); count(m.thermallyRelaxedHeightField);
                count(m.hydrologicallyCorrectedHeightField); count(m.basinIdField); count(m.spillPointField);
                count(m.flowDirectionField); count(m.flowAccumulationField); count(m.riverDischargeField);
                count(m.riverSourceField); count(m.lakeRegionField); count(m.lakeLevelField); count(m.lakeDepthField);
                count(m.provinceField); count(m.waterfallField); count(m.sedimentPotentialField); count(m.erosionField);
                count(m.floodplainPotentialField); count(m.deltaPotentialField); count(m.erodedHeightField);
                count(m.baseTemperatureField); count(m.prevailingWindField); count(m.windStrengthField);
                count(m.windVariabilityField); count(m.precipitationField); count(m.airMoistureField);
                count(m.rainShadowField); count(m.oceanCurrentField); count(m.seaSurfaceTemperatureBiasField);
                count(m.coastalClimateBiasField); count(m.temperatureField); count(m.annualRainfallField);
                count(m.humidityField); count(m.seasonalityField); count(m.winterRainField); count(m.summerRainField);
                count(m.drySeasonStrengthField); count(m.soilTypeField); count(m.soilFertilityField);
                count(m.soilDrainageField); count(m.soilOrganicPotentialField); count(m.primaryBiomeSuitabilityField);
                count(m.secondaryBiomeSuitabilityField); count(m.biomeTransitionField); count(m.materialSuitabilityField);
                std::size_t foundation = 0;
                if (const auto* f = m.terrainFoundation.get()) {
                    for (const auto& plane : f->heightDm) foundation += plane.bytes();
                    foundation += f->receiver.bytes() + f->accumulation.bytes() + f->detailPages.capacity();
                }
                std::cout << "  cells " << m.cells.capacity() * sizeof(m.cells[0]) / (1024 * 1024) << " MB, fields "
                          << fields / (1024 * 1024) << " MB (of it chunk tables " << tables / (1024 * 1024)
                          << "), foundation " << foundation / (1024 * 1024) << " MB\n";
            }
            if (map.terrainFoundation) {
                const auto f = Clock::now();
                const auto print = map.terrainFoundation->fingerprint();
                std::cout << "  foundation fingerprint " << ms(f, Clock::now()) << " ms (" << std::hex << print
                          << std::dec << ", step " << map.terrainFoundation->step << " m)\n";
            }
            const auto s = Clock::now();
            (void)world::streaming::hydrologySourceFingerprint(map);
            std::cout << "  graph source fingerprint " << ms(s, Clock::now()) << " ms\n";
            const auto a = Clock::now();
            auto graph = world::streaming::buildHydrologyGraph(map);
            std::cout << "  graph alone " << ms(a, Clock::now()) << " ms, holding " << residentMb() << " MB\n";
            const auto b = Clock::now();
            const auto mask = world::terrain::makeLandMask64(map);
            std::cout << "  land mask " << ms(b, Clock::now()) << " ms, holding " << residentMb() << " MB\n";
            const auto c = Clock::now();
            world::HeightField field(&map, map.seed);
            std::cout << "  height field " << ms(c, Clock::now()) << " ms, holding " << residentMb() << " MB\n";
            world::ClimateField climate;
            climate.raise(map, field);
            std::cout << "  climate " << ms(c, Clock::now()) << " ms, " << climate.bytes() / (1024 * 1024) << " MB, holding "
                      << residentMb() << " MB\n";
        }
        auto config = world::WorldBuilder::defaultPageConfig();
        config.diskCacheRoot.clear();
        world::WorldSnapshot snapshot(std::move(map), config);
        const auto t2 = Clock::now();
        std::cout << "  snapshot " << ms(t1, t2) << " ms (graph " << snapshot.timings().graph << ", climate "
                  << snapshot.timings().climate << ", climate step " << snapshot.climate().metres() << " m), holding "
                  << residentMb() << " MB\n";
        while (snapshot.pages().prebakeProgress().running) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        rusage use{};
        getrusage(RUSAGE_SELF, &use);
        std::cout << "  prebaked " << ms(t2, Clock::now()) << " ms; peak memory " << use.ru_maxrss / (1024 * 1024)
                  << " MB\n";
        return 0;
    }
    if (grow) {
        // A world grows: one region, then a second beside it left empty, then
        // that one generated on its own. What happens to the first?
        auto layout = generation::emptyLayout(1, 1, seed);
        generation::generateAll(layout, presets.front());
        const auto one = generation::generateLayoutWorld(layout);
        generation::resizeLayout(layout, 2, 1);
        generation::ComposeReport report;
        const auto two = generation::generateLayoutWorld(layout, &report);
        const auto compare = [&](const generation::WorldMapData& a, const generation::WorldMapData& b, const char* what) {
            std::size_t cells = 0, differ = 0;
            std::int32_t nearest = 1 << 30, farthest = -1;
            for (std::int32_t y = 0; y < a.height; ++y)
                for (std::int32_t x = 0; x < a.width; ++x) {
                    const auto& ca = a.at({x, y});
                    const auto& cb = b.at({x, y});
                    ++cells;
                    if (ca.elevation != cb.elevation || ca.sea != cb.sea || ca.river != cb.river ||
                        ca.climate != cb.climate || ca.temperature != cb.temperature) {
                        ++differ;
                        const std::int32_t fromSeam = a.width - 1 - x;
                        nearest = std::min(nearest, fromSeam);
                        farthest = std::max(farthest, fromSeam);
                    }
                }
            std::size_t points = 0, moved = 0;
            const auto& fa = *a.terrainFoundation;
            const auto& fb = *b.terrainFoundation;
            if (fa.step == fb.step)
                for (int j = 0; j < fa.rows; ++j)
                    for (int i = 0; i < fa.columns - 1; ++i) {
                        ++points;
                        moved += fa.heightDm[4][std::size_t(j) * fa.columns + i] !=
                                 fb.heightDm[4][std::size_t(j) * fb.columns + i];
                    }
            std::cout << what << ": " << differ << " of " << cells << " cells differ";
            if (differ) std::cout << " (" << farthest << " cells from the seam at most)";
            std::cout << ", foundation " << moved << " of " << points << " points moved\n";
        };
        std::cout << "grown to 2x1, new region empty: " << report.runs << " run(s), " << report.seaRegions
                  << " sea region(s), " << report.runMs << " + " << report.composeMs << " ms\n";
        compare(one, two, "  the first region");
        auto filled = layout;
        generation::generateRegions(filled, {{1, 0}}, layout.at(0, 0).settings, seed + 99, true);
        const auto three = generation::generateLayoutWorld(filled, &report);
        std::cout << "the new region generated on its own: " << report.runs << " run(s), " << report.seams
                  << " seam(s), " << report.landmasses << " land mass(es) drained again, " << report.runMs << " + "
                  << report.composeMs << " ms\n";
        compare(one, three, "  the first region");
        {
            // Across the seam, one row: what the cells say and what the ground is.
            const auto& f = *three.terrainFoundation;
            for (const std::int32_t row : {100, 133, 160}) {
                std::cout << "  row " << row << ":";
                for (std::int32_t x = 240; x < 272; x += 2) {
                    const auto& c = three.at({x, row});
                    const auto h = f.heightDm[4][std::size_t(row * 512 / f.step) * f.columns + std::size_t(x * 512 / f.step)];
                    std::cout << " " << (c.sea ? "~" : "#") << h / 10;
                }
                std::cout << "\n";
            }
        }
        {
            // What the ground under that sea says of its water: what the
            // renderer and the forest read.
            auto copy = three;
            world::WorldSnapshot snap(std::move(copy), [] { auto c = world::WorldBuilder::defaultPageConfig(); c.diskCacheRoot.clear(); return c; }());
            auto field = snap.field();
            for (const double dx : {-6000.0, -2000.0, 1000.0, 2600.0, 6000.0}) {
                const core::WorldPos p{core::Fixed::fromDoubleForContent(double(generation::kRegionMetres) + dx),
                                       core::Fixed::fromDoubleForContent(68201.0)};
                std::cout << "  " << dx << " m from the seam: ground " << field.heightAt(p).toDouble() << ", water "
                          << field.waterLevelAt(p).toDouble() << (field.underWater(p) ? " (under water)" : " (dry)") << "\n";
            }
        }
        // And land painted across the border, a bridge from one run to the
        // other: the seam should close over it, and its rivers be worked out
        // again as one land mass.
        auto bridged = filled;
        generation::Brush paint;
        paint.tool = generation::BrushTool::Paint;
        paint.value = 900;
        paint.strength = 1.0f;
        paint.hardness = 0.8f;
        paint.radius = generation::legalRadius(generation::LayerId::Continents, bridged, 12000);
        const double border = double(generation::kRegionMetres);
        for (double x = border - 40000; x <= border + 40000; x += 4000)
            generation::dab(bridged, generation::LayerId::Continents, paint, x, border * 0.5, nullptr);
        const auto four = generation::generateLayoutWorld(bridged, &report);
        std::size_t land = 0;
        for (std::int32_t x = 250; x < 262; ++x) land += four.at({x, 128}).sea ? 0 : 1;
        std::cout << "a coast painted across the border: " << report.runs << " run(s), " << report.seams
                  << " seam(s), " << report.landmasses << " land mass(es) drained again, " << land
                  << " of 12 cells across the border are land\n";
        return 0;
    }
    auto preset = presets.front();
    for (const auto& p : presets) if (p.name == presetName) preset = p;
    auto layout = generation::emptyLayout(regions, regions, seed);
    generation::generateAll(layout, preset);
    const auto params = generation::paramsFor(layout);
    std::cout << "world: " << regions << "x" << regions << " regions (" << params.width << "x" << params.height
              << " cells), preset \"" << preset.name << "\", seed " << seed << "\n";

    generation::GenerationTimings timings;
    generation::setGenerationTimings(&timings);
    const auto t0 = Clock::now();
    auto map = generation::generateWorldMap(params);
    const auto t1 = Clock::now();
    generation::setGenerationTimings(nullptr);
    std::size_t sea = 0;
    for (const auto& c : map.cells) sea += c.sea ? 1 : 0;
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "generate: " << ms(t0, t1) << " ms, sea " << 100.0 * double(sea) / double(map.cells.size()) << "% of cells\n";
    for (const auto& [pass, took] : timings.passes)
        std::cout << "  " << std::setw(8) << took << " ms  " << pass << "\n";

    if (map.terrainFoundation) {
        const char* stages[] = {"primary", "macro", "thermal", "coast", "fluvial"};
        std::cout << "  H64 foundation stages (step " << map.terrainFoundation->step << " m):";
        for (std::size_t i = 0; i < 5; ++i) std::cout << " " << stages[i] << " " << map.terrainFoundation->milliseconds[i];
        std::cout << "\n  foundation fingerprint " << std::hex << map.terrainFoundation->fingerprint() << std::dec << "\n";
    }
    auto config = world::WorldBuilder::defaultPageConfig();
    if (!disk) config.diskCacheRoot.clear();
    const auto t2 = Clock::now();
    world::WorldSnapshot snapshot(std::move(map), config);
    const auto t3 = Clock::now();
    std::cout << "snapshot: " << ms(t2, t3) << " ms\n";
    std::cout << "  " << std::setw(8) << snapshot.timings().graph << " ms  drainage graph\n";
    std::cout << "  " << std::setw(8) << snapshot.timings().climate << " ms  climate\n";
    std::cout << "  " << std::setw(8) << snapshot.timings().landmarks << " ms  landmarks\n";
    if (!prebake) return 0;
    for (;;) {
        const auto progress = snapshot.pages().prebakeProgress();
        if (!progress.running) {
            const auto t4 = Clock::now();
            const auto stats = snapshot.pages().stats();
            std::cout << "prebake: " << ms(t3, t4) << " ms, " << progress.total << " pages, " << stats.baked
                      << " baked, " << stats.diskLoaded << " from disk\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::cout << "total: " << ms(t0, Clock::now()) << " ms\n";
    return 0;
}
