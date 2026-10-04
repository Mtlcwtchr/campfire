// environment_probe - what the procedural environment makes of a world, and
// what it costs (doc/plan_procedural_environment_2026-10-03.md, part I).
//
//   environment_probe [--seed N] [--area X Y SIZE] [--determinism]
//
// Generates a one-region world, raises its snapshot (which builds the
// environment from content/config/environment), and over the area:
//   - the zones: how much of the area each type holds
//   - the features: instances per recipe, planned and timed
//   - the masks: one page rasterised and timed, the share of it each channel covers
//   - the ground: how far the features move it, at most and on average
// --determinism builds the environment a second time and compares every
// instance and every mask byte.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>

#include "engine/environment/environment.hpp"
#include "engine/environment/scatter.hpp"
#include "game/environment/world_environment.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"
#include "game/world/world_builder.hpp"

namespace {
using Clock = std::chrono::steady_clock;
namespace env = engine::environment;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

std::filesystem::path contentDirectory() {
    std::filesystem::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 6 && !std::filesystem::exists(content, ec); ++up) content = ".." / content;
    return content;
}

core::WorldPos at(double x, double y) {
    return {core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y)};
}
} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 4242;
    double x0 = -1, y0 = -1, size = 2048;
    bool determinism = false, fields = false, bake = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--seed") seed = std::strtoull(next().c_str(), nullptr, 10);
        else if (a == "--area") { x0 = std::atof(next().c_str()); y0 = std::atof(next().c_str()); size = std::atof(next().c_str()); }
        else if (a == "--determinism") determinism = true;
        else if (a == "--fields") fields = true;
        else if (a == "--bake") bake = true;
        else { std::cerr << "usage: environment_probe [--seed N] [--area X Y SIZE] [--determinism]\n"; return 2; }
    }
    const auto presets = generation::loadWorldPresets(contentDirectory() / "config" / "world_presets.json");
    if (presets.empty()) { std::cerr << "no world presets\n"; return 1; }
    auto layout = generation::emptyLayout(1, 1, seed);
    generation::generateAll(layout, presets.front());
    auto map = generation::generateWorldMap(generation::paramsFor(layout));
    auto config = world::WorldBuilder::defaultPageConfig();
    config.diskCacheRoot.clear();
    std::cout << std::fixed << std::setprecision(1);
    const auto t0 = Clock::now();
    world::WorldSnapshot snapshot(std::move(map), config);
    const auto t1 = Clock::now();
    const auto& environment = snapshot.environment();
    if (!environment) { std::cerr << "no environment\n"; return 1; }
    const auto& cat = environment->catalogue();
    std::cout << "world seed " << seed << ", snapshot " << ms(t0, t1) << " ms\n";
    std::cout << "catalogue: " << cat.recipes().size() << " recipe(s), " << cat.masks().size() << " mask channel(s), "
              << cat.zones().size() << " zone type(s), " << cat.cover().rules.size() << " cover rule(s); "
              << (cat.movesGround() ? "moves the ground" : "leaves the ground") << ", "
              << (environment->writesMasks() ? "writes masks" : "writes no masks") << "\n";
    if (x0 < 0) {
        // Round the world's starting point, which is on land.
        const auto start = snapshot.startingPoint();
        x0 = start.x.toDouble() - size * 0.5;
        y0 = start.y.toDouble() - size * 0.5;
    }
    std::cout << "area " << x0 << ", " << y0 << " + " << size << " m\n";

    // What baking a page costs at each level, round the area's middle.
    if (bake) {
        for (std::uint8_t level = 0; level <= 6; ++level) {
            const double metres = world::streaming::pageMetresAtLevel(level);
            if (metres <= 0) continue;
            const world::streaming::TileKey key{std::int32_t(std::floor((x0 + size * 0.5) / metres)),
                                                std::int32_t(std::floor((y0 + size * 0.5) / metres)), level};
            const auto t = Clock::now();
            const auto page = snapshot.pages().page(key);
            std::cout << "bake level " << int(level) << " (" << world::streaming::PageStore::sampleMetresAtLevel(level) << " m, "
                      << metres << " m page): " << ms(t, Clock::now()) << " ms" << (page ? "" : " FAILED")
                      << (page && !page->envMasks.empty() ? ", masks" : "") << "\n";
        }
    }

    if (bake) {
        const auto cx = std::int64_t(x0 + size * 0.5), cy = std::int64_t(y0 + size * 0.5);
        const auto t = Clock::now();
        const auto scattered = snapshot.scatter(world::decor::ScatterBounds{cx - 768, cy - 768, cx + 768, cy + 768});
        std::cout << "scatter 1536 m region: " << ms(t, Clock::now()) << " ms, " << scattered.objects.size() << " objects\n";
    }

    // The fields the zones are classified from: percentiles over the area.
    if (fields && environment->setup().fields) {
        const int side = 24;
        std::vector<env::FieldSample> samples(std::size_t(side) * side);
        environment->setup().fields->sampleGrid(x0, y0, size / side, side, side, samples);
        {
            env::FieldSample one;
            environment->setup().fields->sample(x0 + size / 2, y0 + size / 2, one);
            std::cout << "centre: height " << snapshot.field().heightAt(at(x0 + size / 2, y0 + size / 2)).toDouble()
                      << ", field elevation " << one.get(env::field::Elevation, -999) << ", slope "
                      << one.get(env::field::Slope, -999) << ", world " << snapshot.worldMap().width << "x"
                      << snapshot.worldMap().height << " cells\n";
        }
        std::cout << "fields (p5 / p25 / p50 / p75 / p95):\n";
        for (std::size_t id = 0; id < env::fieldCount(); ++id) {
            std::vector<float> v;
            for (const auto& s : samples) if (s.has(env::FieldId(id))) v.push_back(s[env::FieldId(id)]);
            if (v.empty()) continue;
            std::sort(v.begin(), v.end());
            const auto q = [&](double p) { return v[std::min(v.size() - 1, std::size_t(p * double(v.size())))]; };
            std::cout << "  " << std::setw(16) << env::fieldName(env::FieldId(id)) << std::setprecision(3) << "  " << q(0.05)
                      << " / " << q(0.25) << " / " << q(0.5) << " / " << q(0.75) << " / " << q(0.95) << "\n";
        }
        std::cout << std::setprecision(1);
    }

    // Zones.
    if (const auto* zones = environment->zones()) {
        const auto t = Clock::now();
        std::map<std::string, int> histogram;
        int samples = 0;
        for (double y = y0; y < y0 + size; y += 32)
            for (double x = x0; x < x0 + size; x += 32) {
                const auto z = zones->at(x, y);
                histogram[cat.zones()[z.type()].name]++;
                ++samples;
            }
        std::cout << "zones (" << ms(t, Clock::now()) << " ms):\n";
        for (const auto& [name, count] : histogram)
            std::cout << "  " << std::setw(6) << 100.0 * count / samples << "%  " << name << "\n";
    } else {
        std::cout << "zones: none (the classifier has only \"unclassified\")\n";
    }

    // The historical drainage of the planning cell in the middle of the area.
    {
        const double cell = env::kPlanningCell[int(env::FeatureScale::Local)];
        const auto cx = std::int64_t(std::floor((x0 + size * 0.5) / cell)), cy = std::int64_t(std::floor((y0 + size * 0.5) / cell));
        const auto t = Clock::now();
        const auto net = environment->planner().drainage(env::FeatureScale::Local, cx, cy);
        std::map<std::string, std::pair<int, double>> classes;
        for (const auto& r : net->reaches) {
            auto& c = classes[env::kChannelClassNames[int(r.kind)]];
            ++c.first;
            c.second += r.length;
        }
        std::cout << "drainage (one local cell and its halo, " << ms(t, Clock::now()) << " ms): " << net->reaches.size()
                  << " reach(es), " << net->meanders.size() << " meander(s)\n";
        for (const auto& [name, c] : classes)
            std::cout << "  " << std::setw(6) << c.first << "  " << std::setw(8) << c.second << " m  " << name << "\n";
    }

    // Features.
    const core::WorldRect area{at(x0, y0), at(x0 + size, y0 + size)};
    const auto t2 = Clock::now();
    std::vector<env::FeatureInstance> instances;
    environment->planner().instancesIn(area, instances);
    const auto t3 = Clock::now();
    std::map<std::string, int> perRecipe;
    for (const auto& in : instances) perRecipe[cat.recipes()[in.recipe].name]++;
    std::cout << "features: " << instances.size() << " instance(s), planned in " << ms(t2, t3) << " ms\n";
    for (const auto& [name, count] : perRecipe) std::cout << "  " << std::setw(6) << count << "  " << name << "\n";
    for (std::size_t i = 0; i < instances.size() && i < 8; ++i)
        std::cout << "    at " << instances[i].anchor.x.toDouble() << ", " << instances[i].anchor.y.toDouble() << "  "
                  << cat.recipes()[instances[i].recipe].name << (instances[i].spline.empty() ? "" : " (line)") << "\n";

    // The ground.
    if (cat.movesGround()) {
        double most = 0, sum = 0;
        int n = 0;
        for (double y = y0; y < y0 + size; y += 8)
            for (double x = x0; x < x0 + size; x += 8) {
                const double d = environment->features().at(at(x, y), core::kZero, 4).toDouble();
                most = std::max(most, std::abs(d));
                sum += std::abs(d);
                ++n;
            }
        std::cout << "ground: moved at most " << most << " m, " << sum / n << " m on average\n";
    }

    // What dresses the features.
    if (!instances.empty()) {
        std::vector<env::PlacedObject> placed;
        const auto t = Clock::now();
        for (const auto& in : instances)
            env::dress(cat, in, [&](double x, double y) { return snapshot.field().heightAt(at(x, y)).toDouble(); }, placed);
        std::cout << "dressing: " << placed.size() << " object(s) in " << ms(t, Clock::now()) << " ms\n";
    }

    // One page of masks, round the first feature if there is one.
    if (environment->writesMasks()) {
        double mx = x0, my = y0;
        if (!instances.empty()) {
            mx = instances.front().anchor.x.toDouble() - 256;
            my = instances.front().anchor.y.toDouble() - 256;
        }
        const auto t = Clock::now();
        const auto page = environment->masks(mx, my, 8, 66);
        const auto took = ms(t, Clock::now());
        std::cout << "masks: one 512 m page at 8 m in " << took << " ms\n";
        for (std::size_t k = 0; k < cat.masks().size(); ++k) {
            int covered = 0;
            for (int r = 0; r < page.side; ++r)
                for (int c = 0; c < page.side; ++c) covered += page.channel(c, r, int(k)) > 0.1f;
            std::cout << "  " << std::setw(6) << 100.0 * covered / (page.side * page.side) << "%  " << cat.masks()[k].name << "\n";
        }
    }

    if (determinism) {
        auto again = world::environment::buildWorldEnvironment(snapshot.worldMap(), nullptr, snapshot.climate());
        std::vector<env::FeatureInstance> second;
        again->planner().instancesIn(area, second);
        bool same = second.size() == instances.size();
        for (std::size_t i = 0; same && i < instances.size(); ++i)
            same = instances[i].id == second[i].id && instances[i].anchor == second[i].anchor &&
                   instances[i].spline.points == second[i].spline.points;
        const bool masks = !environment->writesMasks() ||
                           environment->masks(x0, y0, 8, 66).channels == again->masks(x0, y0, 8, 66).channels;
        std::cout << "determinism: instances " << (same ? "identical" : "DIFFER") << ", masks "
                  << (masks ? "identical" : "DIFFER") << "\n";
        if (!same || !masks) return 1;
    }
    return 0;
}
