// Ground dug into a world is the ground that is drawn (D149).
//
// The edit layer holds differences from the generator; everything made from
// the ground - baked height pages, their disk cache, the meshes the planner
// builds from resident pages, the trees placed on it - remembers which ground
// it was made from and is made again when that ground moves, and only there.
#include "framework.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>
#include <thread>

#include "game/world/edit_layer.hpp"
#include "game/world/terrain_plan.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/world_delta.hpp"
#include "game/world/world_system.hpp"

namespace {
using namespace world;
using namespace world::streaming;
using core::Fixed;

core::WorldRect rect(double minX, double minY, double maxX, double maxY) {
    return {{Fixed::fromDoubleForContent(minX), Fixed::fromDoubleForContent(minY)},
            {Fixed::fromDoubleForContent(maxX), Fixed::fromDoubleForContent(maxY)}};
}

// Five metres dug up over samples [from, to) on both axes: 4 m samples.
void dig(EditLayer& layer, std::int64_t from, std::int64_t to, double metres = 5) {
    for (auto y = from; y < to; ++y)
        for (auto x = from; x < to; ++x) layer.add(x, y, Fixed::fromDoubleForContent(metres));
}

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("campfire_ground_" + std::to_string(rd()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};

// Four macro cells a side of flat land: 2 km, no rivers, no foundation.
struct Flat {
    generation::WorldMapData world;
    HydrologyGraph graph;
    std::shared_ptr<EditLayer> edits = std::make_shared<EditLayer>();
    std::unique_ptr<PageStore> pages;
    explicit Flat(std::filesystem::path cache = {}, std::shared_ptr<EditLayer> layer = {}) {
        if (layer) edits = std::move(layer);
        world.width = world.height = 4;
        world.seed = 17;
        world.cells.resize(16);
        for (auto& cell : world.cells) { cell.sea = false; cell.elevation = 40; }
        graph.macroWidth = graph.macroHeight = 4;
        graph.macroCellMetres = generation::kMetresPerCell;
        PageStore::Config config;
        config.workerCount = 1;
        config.diskCacheRoot = std::move(cache);
        config.edits = edits;
        pages = std::make_unique<PageStore>(world, graph, hsimQuantisationFor(world), config);
    }
};

// The height a page holds at a global sample of its own spacing.
double heightOf(const BakedPage& page, std::int64_t globalX, std::int64_t globalY) {
    const auto& base = page.base;
    const auto side = std::int64_t(base.width) + 2 * base.padding;
    const auto perPage = std::int64_t(kPageMetres / base.sampleMetres);
    const auto x = globalX - std::int64_t(base.key.x) * perPage + base.padding;
    const auto y = globalY - std::int64_t(base.key.y) * perPage + base.padding;
    const HsimQuantisation quant{base.elevationMin, base.elevationMax};
    return quant.dequantise(base.heightQuantized[std::size_t(y * side + x)]).toDouble();
}

template<class F> bool await(F done, int seconds = 20) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}
} // namespace

TEST(ground_edits_are_named_by_what_they_hold_not_how_they_got_there) {
    EditLayer a, b;
    const auto area = rect(0, 0, 512, 512);
    CHECK_EQ(a.fingerprint(area), std::uint64_t(0));
    // The same samples, written in opposite orders and in two steps.
    for (std::int64_t i = 0; i < 40; ++i) a.add(10 + i, 20, Fixed::fromInt(3));
    for (std::int64_t i = 39; i >= 0; --i) {
        b.add(10 + i, 20, Fixed::fromInt(1));
        b.add(10 + i, 20, Fixed::fromInt(2));
    }
    CHECK(a.fingerprint(area) != 0);
    CHECK_EQ(a.fingerprint(area), b.fingerprint(area));
    // Elsewhere the layer holds nothing, whatever it holds here.
    CHECK_EQ(a.fingerprint(rect(1024, 1024, 1536, 1536)), std::uint64_t(0));
    // Dug and filled back in is no dig at all.
    for (std::int64_t i = 0; i < 40; ++i) b.add(10 + i, 20, Fixed::fromInt(-3));
    CHECK_EQ(b.fingerprint(area), std::uint64_t(0));
    CHECK(b.revision() > a.revision());
}

TEST(ground_edits_know_to_the_patch_where_they_were_written) {
    EditLayer layer;
    CHECK_EQ(layer.revisionIn(rect(0, 0, 512, 512)), std::uint64_t(0));
    layer.add(2, 2, Fixed::fromInt(1));          // patch (0,0) of block (0,0)
    const auto first = layer.revision();
    layer.add(100, 100, Fixed::fromInt(1));      // patch (6,6): 400 m in
    const auto second = layer.revision();
    CHECK_EQ(layer.revisionIn(rect(0, 0, 60, 60)), first);
    CHECK_EQ(layer.revisionIn(rect(380, 380, 420, 420)), second);
    CHECK_EQ(layer.revisionIn(rect(0, 0, 512, 512)), second);
    // The same block, patches nobody wrote: nothing to rebake there.
    CHECK_EQ(layer.revisionIn(rect(130, 130, 250, 250)), std::uint64_t(0));

    std::vector<core::WorldRect> changed;
    CHECK_EQ(layer.changedSince(first, changed), second);
    CHECK_EQ(changed.size(), std::size_t(1));
    CHECK(changed[0].contains({Fixed::fromInt(400), Fixed::fromInt(400)}));
    CHECK(!changed[0].contains({Fixed::fromInt(8), Fixed::fromInt(8)}));

    // Cleared ground moved too, though nothing is left to say where.
    layer.clear();
    const auto cleared = layer.revision();
    CHECK_EQ(layer.revisionIn(rect(0, 0, 60, 60)), cleared);
    changed.clear();
    layer.changedSince(second, changed);
    CHECK(!changed.empty());
    CHECK_EQ(layer.fingerprint(rect(0, 0, 512, 512)), std::uint64_t(0));
}

TEST(ground_edits_a_page_is_baked_over_the_ground_as_dug_and_only_that_page) {
    Flat flat;
    auto& pages = *flat.pages;
    const auto near = pages.page({0, 0, 0});
    const auto far = pages.page({3, 3, 0});
    CHECK(near && far);
    if (!near || !far) return;
    const double before = heightOf(*near, 36, 36);
    CHECK_EQ(near->ground, std::uint64_t(0));

    dig(*flat.edits, 32, 40);   // 128..160 m, inside page (0,0)
    CHECK(!pages.current(*near));
    CHECK(pages.current(*far));
    CHECK(pages.resident({0, 0, 0}) == nullptr);   // never handed out stale
    const auto again = pages.page({0, 0, 0});
    CHECK(again && again != near);
    if (!again) return;
    CHECK(again->ground != 0);
    CHECK(pages.current(*again));
    const double after = heightOf(*again, 36, 36);
    CHECK(std::abs(after - before - 5) < 1.0);
    // Outside the dig the page is the ground it was.
    CHECK(std::abs(heightOf(*again, 100, 100) - heightOf(*near, 100, 100)) < 0.01);
    // A page whose reach the dig does not touch is the page it was.
    CHECK(pages.page({3, 3, 0}) == far);
    CHECK(pages.stats().stale >= 1);
    // Its coarser parents were baked afresh with it, and agree with it.
    const auto parent = pages.page({0, 0, 1});
    CHECK(parent && pages.current(*parent));
    if (parent) CHECK(std::abs(heightOf(*parent, 18, 18) - after) < 1.0);
}

TEST(ground_edits_a_pinned_page_is_replaced_in_the_pinned_set) {
    Flat flat;
    auto& pages = *flat.pages;
    pages.prebakeInBackground({2, 4});
    CHECK(await([&] { return !pages.prebakeProgress().running; }));
    const auto before = pages.resident({0, 0, 4});
    CHECK(before != nullptr);
    if (!before) return;
    dig(*flat.edits, 30, 36);   // over the H64 sample at 128 m
    CHECK(pages.resident({0, 0, 4}) == nullptr);
    const auto after = pages.page({0, 0, 4});
    CHECK(after && pages.current(*after));
    // And it is the pinned page now: found without baking, and kept there.
    const auto baked = pages.stats().baked;
    CHECK(pages.resident({0, 0, 4}) == after);
    CHECK(pages.page({0, 0, 4}) == after);
    CHECK_EQ(pages.stats().baked, baked);
    if (after) CHECK(std::abs(heightOf(*after, 2, 2) - heightOf(*before, 2, 2) - 5) < 1.0);
}

TEST(ground_edits_the_disk_cache_files_dug_ground_beside_the_generators) {
    TempDir dir;
    const TileKey key{0, 0, 4};
    double generated = 0, dug = 0;
    {
        Flat plain(dir.path);
        const auto page = plain.pages->page(key);
        CHECK(page != nullptr);
        if (page) generated = heightOf(*page, 2, 2);
        CHECK_EQ(plain.pages->stats().diskSaved, std::size_t(1));
    }
    auto layer = std::make_shared<EditLayer>();
    dig(*layer, 30, 36);
    {
        Flat edited(dir.path, layer);
        const auto page = edited.pages->page(key);
        CHECK(page != nullptr);
        if (page) dug = heightOf(*page, 2, 2);
        CHECK(std::abs(dug - generated - 5) < 1.0);
        CHECK_EQ(edited.pages->stats().diskLoaded, std::size_t(0));   // not the generator's file
        CHECK_EQ(edited.pages->stats().diskSaved, std::size_t(1));
    }
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path))
        files += entry.is_regular_file() && entry.path().extension() == ".bin";
    CHECK_EQ(files, std::size_t(2));   // side by side: nothing was overwritten
    {
        // The same dig in another session reads its own file back.
        Flat again(dir.path, layer);
        const auto page = again.pages->page(key);
        CHECK_EQ(again.pages->stats().diskLoaded, std::size_t(1));
        if (page) CHECK_EQ(heightOf(*page, 2, 2), dug);
    }
    {
        // Filled back in: the generator's page again, from its own file.
        dig(*layer, 30, 36, -5);
        Flat filled(dir.path, layer);
        const auto page = filled.pages->page(key);
        CHECK_EQ(filled.pages->stats().diskLoaded, std::size_t(1));
        if (page) CHECK_EQ(heightOf(*page, 2, 2), generated);
    }
}

TEST(ground_edits_the_planner_rebuilds_only_the_meshes_over_moved_ground) {
    using namespace world::terrain;
    Flat flat;
    const int wide = 4;
    auto residency = std::make_shared<TerrainResidency>();
    residency->revision = 1;
    residency->capacity[0] = 0;
    const auto surfaceFor = [&](TileKey key, double bump, std::uint64_t version) {
        auto surface = std::make_shared<SurfacePage>();
        surface->step = 4 << key.level; surface->padding = 2;
        surface->side = 512 / surface->step + 1 + 2 * surface->padding;
        surface->version = version;
        const auto count = std::size_t(surface->side) * surface->side;
        surface->bed.resize(count); surface->head.assign(count, 0);
        for (int y = 0; y < surface->side; ++y) for (int x = 0; x < surface->side; ++x) {
            const double wx = key.x * 512 + (x - surface->padding) * surface->step;
            const double wy = key.y * 512 + (y - surface->padding) * surface->step;
            const double rise = bump * std::max(0.0, 1.0 - std::hypot(wx - 200, wy - 200) / 120);
            // Rough everywhere, so the cut is fine everywhere and a mesh over
            // the dig is one of many.
            const double rough = 15 * std::sin(wx / 37) * std::cos(wy / 53);
            surface->bed[std::size_t(y) * surface->side + x] = float(100 + 0.05 * wy + rough + rise);
        }
        return surface;
    };
    for (std::uint8_t level : {1, 2, 4})
        for (int y = -2; y < wide + 2; ++y) for (int x = -2; x < wide + 2; ++x) {
            const TileKey key{x, y, level};
            if (!flat.pages->containsLand(key)) continue;
            residency->pages.insert(key);
            residency->surfaces.emplace(key, surfaceFor(key, 0, 0));
        }
    std::vector<TileId> roots;
    const auto side = kRegionalDataLodPolicy.metresAt(6);
    for (std::int64_t y = 0; y * side < 2048; ++y)
        for (std::int64_t x = 0; x * side < 2048; ++x) roots.push_back({int(x), int(y), 6});
    TerrainPlanner planner(flat.world, *flat.pages, roots, kRegionalDataLodPolicy);
    TerrainView view;
    view.target = 1; view.x = view.lookX = 1024; view.y = view.lookY = 1024;
    view.radius = 1600; view.width = view.height = 800;
    view.matrix[0] = view.matrix[5] = float(1 / 1100.0);
    view.matrix[3] = view.matrix[7] = float(-1024 / 1100.0);
    view.matrix[10] = view.matrix[15] = 1;
    view.prediction = view.matrix;
    double time = 0;
    std::size_t built = 0, holes = 0;
    // Every point of the dug page is under some square with a mesh, in every
    // plan on the way: the old ground is drawn until the new is ready.
    const auto covered = [](const TerrainPlan& plan) {
        std::size_t open = 0;
        for (int y = 8; y < 512; y += 32)
            for (int x = 8; x < 512; x += 32)
                open += std::none_of(plan.coverage.begin(), plan.coverage.end(), [&](const auto& block) {
                    return block.mesh && block.bounds.minX <= x && x < block.bounds.maxX &&
                           block.bounds.minY <= y && y < block.bounds.maxY;
                });
        return open;
    };
    const auto settle = [&] {
        built = 0;
        std::shared_ptr<const TerrainPlan> plan;
        for (int tick = 0; tick < 200 && (!plan || plan->needsUpdate); ++tick) {
            if (!planner.request(view, residency, time += 0.1, false) && plan) break;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::shared_ptr<const TerrainPlan> next;
            while (!next && std::chrono::steady_clock::now() < deadline) {
                next = planner.collect();
                if (!next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!next) break;
            plan = next;
            built += plan->meshesBuilt;
            holes += covered(*plan);
        }
        return plan;
    };
    const auto first = settle();
    const auto builtFirst = built;
    CHECK(first && !first->needsUpdate && first->coverage.size() > 8);
    if (!first) return;
    CHECK_EQ(covered(*first), std::size_t(0));
    holes = 0;
    const double flatAt200 = 100 + 0.05 * 200 + 15 * std::sin(200.0 / 37) * std::cos(200.0 / 53);

    // Page (0,0) at every level now stands on a thirty-metre mound at 200,200.
    auto moved = std::make_shared<TerrainResidency>(*residency);
    moved->revision = residency->revision + 1;
    for (std::uint8_t level : {1, 2, 4}) moved->surfaces[{0, 0, level}] = surfaceFor({0, 0, level}, 30, 7);
    residency = moved;
    const auto second = settle();
    CHECK(second && !second->needsUpdate);
    if (!second) return;
    // Built again: the meshes over the mound, and no more than a fraction of
    // the world's.
    std::cout << "  meshes built: world " << builtFirst << ", after the dig " << built << "\n";
    CHECK(built > 0);
    CHECK(built * 2 < builtFirst);
    bool mound = false;
    for (const auto& block : second->coverage) {
        CHECK(bool(block.mesh));
        if (!block.mesh) continue;
        const double ox = block.bounds.minX, oy = block.bounds.minY;
        if (ox <= 200 && oy <= 200 && block.bounds.maxX > 200 && block.bounds.maxY > 200) {
            mound = true;
            CHECK(block.mesh->sample(200 - ox, 200 - oy)[0] > flatAt200 + 15);
        }
    }
    CHECK(mound);
    // Nothing moved since: nothing to do.
    CHECK(!planner.request(view, residency, time + 1));
}

TEST(ground_edits_trees_are_placed_again_on_the_ground_dug_under_them) {
    auto delta = std::make_unique<world::delta::WorldDelta>(29);
    generation::WorldMapData map;
    map.width = map.height = 4;
    map.seed = 29;
    map.cells.resize(16);
    for (auto& cell : map.cells) { cell.sea = false; cell.elevation = 40; }
    streaming::PageStore::Config config;
    config.workerCount = 1;
    WorldSystem system(config);
    system.attach(delta->objects(), delta->heights());
    system.publish(std::move(map));
    const auto snapshot = system.read();
    CHECK(snapshot->edits() == delta->heights().get());
    auto channel = system.prepare(snapshot);
    auto& placement = channel->placement;
    const std::vector<decor::ScatterBounds> regions{{512, 512, 640, 640}, {1024, 1024, 1152, 1152}};
    const auto complete = [&] {
        placement.updateRegions(regions, true);
        return placement.read() && placement.read()->complete && !placement.busy();
    };
    CHECK(await(complete));
    const auto before = placement.read();
    if (!before) return;
    const auto inRegion = [](const decor::Object& o, const decor::ScatterBounds& r) {
        return o.x >= r.minX && o.x < r.maxX && o.y >= r.minY && o.y < r.maxY;
    };
    // Everything in the first region stands on ground raised by five metres.
    for (std::int64_t y = 512 / 4 - 20; y < 640 / 4 + 20; ++y)
        for (std::int64_t x = 512 / 4 - 20; x < 640 / 4 + 20; ++x) delta->heights()->add(x, y, Fixed::fromInt(5));
    CHECK(await([&] { return complete() && placement.read() != before; }));
    const auto after = placement.read();
    std::size_t raised = 0, untouched = 0;
    const auto beforeObjects = before->mergedObjects(), afterObjects = after->mergedObjects();
    for (const auto& was : beforeObjects) {
        const auto now = std::find_if(afterObjects.begin(), afterObjects.end(),
                                      [&](const auto& o) { return o.id == was.id; });
        CHECK(now != afterObjects.end());
        if (now == afterObjects.end()) continue;
        if (inRegion(was, regions[0])) { CHECK(std::abs(now->z - was.z - 5) < 0.5); ++raised; }
        if (inRegion(was, regions[1])) { CHECK_EQ(now->z, was.z); ++untouched; }
    }
    CHECK(raised > 0);
    CHECK(untouched > 0);
}

