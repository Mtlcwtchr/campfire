// What the bounded composition actually costs and contains, at several world
// sizes. Measures the source and the land mask; it does not generate terrain,
// build a mesh or open a window, and it never allocates anything proportional
// to a world's area.
#include "game/generation/land_coverage.hpp"
#include "game/generation/world_macro.hpp"
#include "game/generation/world_scale_policy.hpp"
#include "game/world/terrain_source.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace generation;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

double millis(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// How many distinct systems a transect meets, counted as runs above a
// threshold. Crude and order-independent on purpose: this is a composition
// statistic, not an artistic verdict about mountains.
std::size_t runs(const MacroField& field, double side, double v, double threshold, bool uplift) {
    std::size_t count = 0;
    bool inside = false;
    for (int i = 0; i <= 3000; ++i) {
        const auto sample = field.at(side * (i / 3000.0), side * v);
        const double value = uplift ? sample.uplift : sample.elevationMetres;
        const bool now = value > threshold;
        count += now && !inside;
        inside = now;
    }
    return count;
}

// A hillshaded look at the composition, straight from the overview's own
// samples. Netpbm because it needs no library and no decisions: this is a
// picture of the source for judging continents and belts by eye, not a
// screenshot of the renderer, and it never stands in for one.
void writeImage(const MacroOverview& overview, const std::string& path) {
    const double sea = overview.seaLevelMetres;
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("cannot write composition image: " + path);
    out << "P6\n" << overview.columns << ' ' << overview.rows << "\n255\n";
    const auto at = [&](std::int64_t column, std::int64_t row) {
        column = std::clamp<std::int64_t>(column, 0, overview.columns - 1);
        row = std::clamp<std::int64_t>(row, 0, overview.rows - 1);
        return double(overview.elevation[std::size_t(row) * overview.columns + std::size_t(column)]);
    };
    std::string pixels;
    pixels.reserve(std::size_t(overview.columns) * overview.rows * 3);
    for (std::int64_t row = 0; row < overview.rows; ++row)
        for (std::int64_t column = 0; column < overview.columns; ++column) {
            const double h = at(column, row);
            const double dx = (at(column + 1, row) - at(column - 1, row)) / (2 * overview.cellWidthMetres);
            const double dy = (at(column, row + 1) - at(column, row - 1)) / (2 * overview.cellHeightMetres);
            // The gain has to follow the terrain, not a constant: at a cell of
            // two hundred metres a slope of a tenth is ordinary ground, and a
            // fixed gain saturates every pixel into black or white and hides
            // exactly the shape this picture exists to show.
            const double light = std::clamp((-dx - dy) * 6 + 0.78, 0.30, 1.30);
            double r, g, b;
            if (h < sea) {
                const double depth = std::clamp((sea - h) / 3200.0, 0.0, 1.0);
                r = 0.05 + 0.10 * (1 - depth); g = 0.18 + 0.30 * (1 - depth); b = 0.35 + 0.35 * (1 - depth);
            } else {
                const double up = std::clamp((h - sea) / 2600.0, 0.0, 1.0);
                r = std::lerp(0.30, 0.86, up); g = std::lerp(0.45, 0.84, up); b = std::lerp(0.26, 0.82, up);
                r *= light; g *= light; b *= light;
            }
            for (const double channel : {r, g, b})
                pixels.push_back(char(std::clamp(int(channel * 255), 0, 255)));
        }
    out.write(pixels.data(), std::streamsize(pixels.size()));
    if (!out) throw std::runtime_error("composition image write failed: " + path);
}

// What a camera crossing the world costs the source, which is the question a
// large world actually asks: not "can it be generated" but "does looking at it
// stay inside a budget". The walk is deliberately longer than any cache: if the
// resident bytes do not hold, nothing below this matters.
Json stream(const generation::WorldDescriptor& descriptor, std::int64_t side) {
    using world::terrain::ScaleTerrainSource;
    constexpr std::size_t kBudget = 64u << 20;
    ScaleTerrainSource source(descriptor, {kBudget, 2});
    const auto overviewBytes = source.bytes();

    // Near ground at 512 m pages plus a coarse horizon, which is what a frame
    // of this world would want: the fine ring around the camera and a pyramid
    // level that can carry twenty kilometres of distance.
    const std::uint8_t horizon = ScaleTerrainSource::levelFor(64);
    std::size_t peakResident = 0, asked = 0, teleports = 0;
    const auto begin = Clock::now();
    double x = double(side) * 0.12, y = double(side) * 0.37;
    for (int step = 0; step < 200; ++step) {
        // A pan, and every fiftieth step a jump: a teleport is the case that
        // breaks a cache that grows with where it has been rather than with
        // what it is holding.
        if (step % 50 == 49) {
            x = double(side) * (0.17 + 0.21 * (step / 50));
            y = double(side) * (0.29 + 0.17 * (step / 50));
            ++teleports;
        } else {
            x += 900;
            y += 380;
        }
        for (const auto key : source.keysOverlapping(x - 2600, y - 2600, x + 2600, y + 2600, 0)) {
            (void)source.page(key);
            ++asked;
        }
        for (const auto key : source.keysOverlapping(x - 22000, y - 22000, x + 22000, y + 22000, horizon)) {
            (void)source.page(key);
            ++asked;
        }
        peakResident = std::max(peakResident, source.stats().residentBytes);
    }
    const auto stats = source.stats();
    return {{"budget_bytes", kBudget},
            {"peak_resident_page_bytes", peakResident},
            {"held_within_budget", peakResident <= kBudget},
            {"overview_and_pyramid_bytes", overviewBytes},
            {"total_bytes", source.bytes()},
            {"steps", 200}, {"teleports", teleports}, {"page_requests", asked},
            {"pages_derived", stats.derived}, {"pages_evicted", stats.evicted},
            {"pages_hit", stats.hits},
            {"pages_refused_open_water", stats.refusedOpenWater},
            {"field_samples", stats.fieldSamples},
            {"horizon_level", horizon},
            {"horizon_page_extent_m", world::terrain::SourcePageLayout::extentMetres(horizon)},
            {"milliseconds", millis(begin, Clock::now())},
            {"domain", "surface pages only; no mesh, no materials, no hydrology"}};
}

Json measure(std::int64_t side, std::uint64_t seed, std::uint32_t overviewLimit,
             const std::string& imagePrefix) {
    WorldScalePolicy policy;
    policy.overviewSideLimit = overviewLimit;
    const auto descriptor = policy.describe(WorldDomain(side, side), seed);

    const auto fieldAt = Clock::now();
    const MacroField field(descriptor);
    const auto overviewAt = Clock::now();
    auto overview = std::make_shared<const MacroOverview>(buildMacroOverview(field));
    const auto coverageAt = Clock::now();
    const LandCoverage coverage(overview);
    const auto builtAt = Clock::now();

    // Land share from the overview's own cells, and the share of the world the
    // mask can already refuse before a single terrestrial job is queued.
    std::size_t water = 0, land = 0, mixed = 0;
    const double cellW = overview->cellWidthMetres, cellH = overview->cellHeightMetres;
    for (std::uint32_t row = 0; row < overview->rows; ++row)
        for (std::uint32_t column = 0; column < overview->columns; ++column) {
            const WorldRect rect{column * cellW, row * cellH, (column + 1) * cellW, (row + 1) * cellH};
            switch (coverage.classify(rect)) {
            case Coverage::OpenWater: ++water; break;
            case Coverage::Land: ++land; break;
            default: ++mixed; break;
            }
        }
    const auto classifiedAt = Clock::now();

    std::size_t coasts = 0, systems = 0;
    for (const double v : {0.17, 0.33, 0.51, 0.68, 0.84}) {
        coasts += runs(field, double(side), v, overview->seaLevelMetres, false);
        systems += runs(field, double(side), v, 0.62, true);
    }

    // One local query, the thing a streaming world does constantly: it must not
    // depend on the world's size.
    const auto localAt = Clock::now();
    std::size_t localLand = 0;
    for (int i = 0; i < 20000; ++i) {
        const double x = double(side) * 0.5 + (i % 200) * 512.0;
        const double y = double(side) * 0.5 + (i / 200) * 512.0;
        localLand += coverage.mayHaveLand({x, y, x + 512, y + 512});
    }
    const auto doneAt = Clock::now();

    // Composition statistics from the overview's own centre samples. Land
    // share and the height distribution are what an artistic look at the map
    // argues about, so they belong in the machine-readable record too.
    auto heights = overview->elevation;
    auto crust = overview->continent;
    std::sort(heights.begin(), heights.end());
    std::sort(crust.begin(), crust.end());
    const auto at = [](const std::vector<float>& sorted, double q) {
        return double(sorted[std::size_t(q * double(sorted.size() - 1))]);
    };
    const auto above = std::size_t(std::count_if(overview->elevation.begin(),
        overview->elevation.end(),
        [sea = overview->seaLevelMetres](float h) { return h > sea; }));

    std::string image;
    if (!imagePrefix.empty()) {
        image = imagePrefix + "-" + std::to_string(side / 1000) + "km.ppm";
        writeImage(*overview, image);
    }
    return {{"side_km", double(side) / 1000.0},
            {"composition_image", image},
            {"seed", seed},
            {"coarse_step_m", descriptor.coarseStepMetres},
            {"overview", {{"columns", overview->columns}, {"rows", overview->rows},
                          {"cell_m", {cellW, cellH}}, {"bytes", overview->bytes()}}},
            {"coverage_bytes", coverage.bytes()},
            {"total_resident_bytes", overview->bytes() + coverage.bytes()},
            {"belt_lipschitz_m_per_m", field.elevationLipschitz()},
            {"elevation_ceiling_m", field.elevationCeiling()},
            {"sea_level_raw_m", overview->seaLevelMetres},
            {"cells", {{"open_water", water}, {"land", land}, {"mixed", mixed}}},
            {"open_water_share", double(water) / double(overview->cells())},
            {"land_share", double(above) / double(overview->cells())},
            {"elevation_percentiles_m", {{"p01", at(heights, 0.01)}, {"p25", at(heights, 0.25)},
                                         {"p50", at(heights, 0.50)}, {"p75", at(heights, 0.75)},
                                         {"p95", at(heights, 0.95)}, {"p999", at(heights, 0.999)}}},
            {"continent_percentiles", {{"p05", at(crust, 0.05)}, {"p50", at(crust, 0.50)},
                                       {"p95", at(crust, 0.95)}}},
            {"transects", {{"coast_crossings", coasts}, {"uplift_systems", systems},
                           {"lines", 5}, {"samples_per_line", 3001}}},
            {"milliseconds", {{"field", millis(fieldAt, overviewAt)},
                              {"overview", millis(overviewAt, coverageAt)},
                              {"coverage_pyramid", millis(coverageAt, builtAt)},
                              {"classify_every_cell", millis(builtAt, classifiedAt)},
                              {"twenty_thousand_local_queries", millis(localAt, doneAt)}}},
            {"local_queries_answering_land", localLand},
            {"streaming", stream(descriptor, side)},
            {"descriptor_fingerprint", descriptor.fingerprint()}};
}
}

int main(int argc, char** argv) {
    try {
        std::uint64_t seed = 42;
        std::uint32_t overviewLimit = 1024;
        std::vector<std::int64_t> sides{100000, 250000, 1000000, 10000000};
        std::string imagePrefix;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
            else if (arg == "--overview" && i + 1 < argc) overviewLimit = std::uint32_t(std::stoul(argv[++i]));
            else if (arg == "--sides" && i + 1 < argc) {
                sides.clear();
                std::string list = argv[++i];
                std::size_t at = 0;
                while (at <= list.size()) {
                    const auto comma = list.find(',', at);
                    sides.push_back(std::stoll(list.substr(at, comma - at)) * 1000);
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            } else if (arg == "--image" && i + 1 < argc) imagePrefix = argv[++i];
            else throw std::runtime_error("usage: world_scale_probe [--seed N] "
                "[--overview 2..1024] [--sides km,km,...] [--image PREFIX]");
        }
        Json rows = Json::array();
        for (const auto side : sides) rows.push_back(measure(side, seed, overviewLimit, imagePrefix));
        std::cout << Json{{"overview_side_limit", overviewLimit},
                          {"domain", "bounded macro composition and land coverage only"},
                          {"not_measured", "terrain generation, erosion, meshes, streaming"},
                          {"measurements", rows}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
