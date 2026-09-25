#include "framework.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <thread>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_cut.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

using namespace world::streaming;
using core::Fixed;

namespace {
template<class Predicate> bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return predicate();
}

struct Country {
    generation::WorldMapData world;
    HydrologyGraph graph;
    HsimQuantisation quant{Fixed::fromInt(-256), Fixed::fromInt(2048)};
    Country() {
        world.width = world.height = 3;
        world.seed = 71;
        world.cells.resize(9);
        for (std::size_t i = 0; i < world.cells.size(); ++i)
            world.cells[i].elevation = 30 + static_cast<int>(i) * 7;
        world.basinIdField.assign(9, 0);
        graph.macroWidth = graph.macroHeight = 3;
        graph.macroCellMetres = generation::kMetresPerCell;
        RiverSegment river;
        river.id = 1;
        river.width = Fixed::fromInt(24);
        river.depth = Fixed::fromInt(2);
        river.valleyReach = Fixed::fromInt(128);
        river.bounds = {{Fixed::fromInt(244), Fixed::fromInt(-512)},
                        {Fixed::fromInt(268), Fixed::fromInt(1024)}};
        river.course = {
            {{Fixed::fromInt(256), Fixed::fromInt(-512)}, Fixed::fromInt(100),
                Fixed::fromInt(12), Fixed::fromInt(2), Fixed::fromInt(128)},
            {{Fixed::fromInt(256), Fixed::fromInt(1024)}, Fixed::fromInt(90),
                Fixed::fromInt(12), Fixed::fromInt(2), Fixed::fromInt(128)}};
        graph.segments.push_back(river);
        for (int y = -3; y <= 3; ++y)
            for (int x = -3; x <= 3; ++x) graph.spatialPages.push_back({{x, y, 0}, {1}, {}});
    }
};

void sameFields(const BakedPage& a, const BakedPage& b) {
    CHECK_EQ(a.base.heightQuantized, b.base.heightQuantized);
    CHECK_EQ(a.base.waterBodyId, b.base.waterBodyId);
    CHECK_EQ(a.base.watershedId, b.base.watershedId);
    CHECK_EQ(a.base.walkability, b.base.walkability);
    CHECK_EQ(a.base.buildability, b.base.buildability);
    CHECK_EQ(a.water.surfaceQuantized, b.water.surfaceQuantized);
    CHECK_EQ(a.water.waterBodyId, b.water.waterBodyId);
    CHECK_EQ(a.water.riverId, b.water.riverId);
    CHECK_EQ(a.water.shoreDecimetres, b.water.shoreDecimetres);
    CHECK_EQ(a.water.coverage, b.water.coverage);
    CHECK_EQ(a.water.flowX, b.water.flowX);
    CHECK_EQ(a.water.flowY, b.water.flowY);
    CHECK_EQ(a.large.deltaQuantized, b.large.deltaQuantized);
    CHECK_EQ(a.medium.deltaQuantized, b.medium.deltaQuantized);
    CHECK_EQ(a.materials, b.materials);
    CHECK_EQ(a.refinementDepth, b.refinementDepth);
    CHECK_EQ(a.refinementAshore, b.refinementAshore);
}
}

TEST(terrain_refinement_reuses_exact_parent_samples_and_recomputes_dependent_fields) {
    Country country;
    BaseTileBaker baker(country.world, country.graph, country.quant);
    for (const int x : {0, -1}) {
        auto parent = baker.bakePage({x, 0, 4}, 64);
        for (const std::uint8_t level : {2, 1, 0}) {
            const int step = 4 << level;
            const auto direct = baker.bakePage({x, 0, level}, step);
            auto refined = baker.bakePage({x, 0, level}, step, 2, {}, &parent);
            CHECK(refined.base.valid());
            CHECK(refined.reusedSamples > 0);
            CHECK_EQ(refined.evaluatedSamples + refined.reusedSamples,
                     direct.base.heightQuantized.size());
            CHECK(refined.evaluatedSamples < direct.evaluatedSamples);
            sameFields(refined, direct); // includes halo, water thresholds and finer slopes
            if (x == 0) {
                CHECK(std::any_of(refined.water.coverage.begin(), refined.water.coverage.end(),
                                  [](auto value) { return value > 0; }));
            }
            parent = std::move(refined);
        }
        CHECK(!parent.medium.deltaQuantized.empty());
    }
}

TEST(terrain_refinement_rejects_incompatible_parent_and_handles_cancellation) {
    Country country;
    BaseTileBaker baker(country.world, country.graph, country.quant);
    const auto direct = baker.bakePage({0, 0, 1}, 8);
    const auto parent = baker.bakePage({0, 0, 2}, 16);
    for (int mismatch = 0; mismatch < 8; ++mismatch) {
        auto bad = parent;
        if (mismatch == 0) bad.world = nullptr;
        if (mismatch == 1) bad.graph = nullptr;
        if (mismatch == 2) ++bad.base.key.x;
        if (mismatch == 3) bad.base.elevationMin = Fixed::fromInt(-255);
        if (mismatch == 4) bad.refinementDepth.clear();
        if (mismatch == 5) bad.base.waterBodyId.clear();
        if (mismatch == 6) bad.base.watershedId.clear();
        if (mismatch == 7) bad.water.sampleMetres *= 2;
        const auto result = baker.bakePage({0, 0, 1}, 8, 2, {}, &bad);
        CHECK_EQ(result.reusedSamples, std::size_t(0));
        sameFields(result, direct);
    }
    CHECK(!baker.bakePage({0, 0, 1}, 8, 2, [] { return true; }, &parent).base.valid());
    // A narrow parent halo only supplies the points it actually owns.
    const auto unpadded = baker.bakePage({0, 0, 2}, 16, 0);
    const auto refined = baker.bakePage({0, 0, 1}, 8, 2, {}, &unpadded);
    CHECK(refined.reusedSamples > 0);
    sameFields(refined, direct);
}

TEST(terrain_refinement_h8_request_never_bakes_h4_and_h4_reuses_h8) {
    Country country;
    PageStore pages(country.world, country.graph, country.quant);
    const auto h8 = pages.page({0, 0, 1});
    CHECK(bool(h8));
    CHECK(bool(pages.resident({0, 0, 2})));
    CHECK(!pages.resident({0, 0, 0}));
    CHECK_EQ(pages.stats().baked, std::size_t(2));
    const auto before = pages.stats();
    const auto h4 = pages.page({0, 0, 0});
    CHECK(bool(h4));
    CHECK_EQ(pages.resident({0, 0, 1}), h8);
    CHECK_EQ(pages.stats().baked, before.baked + 1);
    CHECK(pages.stats().reusedSamples > before.reusedSamples);
}

TEST(terrain_refinement_cut_waits_at_h16_then_h8_until_child_data_is_ready) {
    using namespace world::terrain;
    TerrainCut cut;
    int finestReady = 2;
    bool requestedH4 = false;
    const auto visible = [](world::TileId) { return true; };
    const auto ready = [&](world::TileId tile) {
        if (tile.lod == 0) requestedH4 = true;
        return tile.lod >= finestReady;
    };
    const auto tick = [&](int target, double dt) { cut.update({{0, 0, 2}}, target, visible, ready, dt); };
    tick(1, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    CHECK_EQ(cut.drawing().front().tile.lod, 2);
    finestReady = 1;
    tick(1, 0);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    for (const auto block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    tick(1, 0.125);
    for (const auto block : cut.drawing()) CHECK_EQ(block.parentMorph, 0.5f);
    tick(1, 1);
    CHECK(!requestedH4);
    tick(0, 1); // zoomed in, but H4 unavailable: keep H8
    CHECK(requestedH4);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    for (const auto block : cut.drawing()) CHECK_EQ(block.tile.lod, 1);
    finestReady = 0;
    tick(0, 0);
    CHECK_EQ(cut.drawing().size(), std::size_t(16));
    for (const auto block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    tick(0, 1);
    tick(2, 0.125); // reverse morph retains children
    CHECK_EQ(cut.drawing().size(), std::size_t(16));
    tick(2, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(16));
    for (const auto block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    tick(2, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    for (const auto block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    tick(2, 0);
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
}

TEST(terrain_worker_budget_is_half_the_logical_processors) {
    CHECK_EQ(terrainWorkerBudget(12), 6u);
    CHECK_EQ(terrainWorkerBudget(13), 6u);
    CHECK_EQ(terrainWorkerBudget(2), 1u);
    CHECK_EQ(terrainWorkerBudget(1), 1u);
    CHECK_EQ(terrainWorkerBudget(0), 1u); // hardware query unavailable
    Country country;
    PageStore pages(country.world, country.graph, country.quant, {1u << 20, 2, 1000});
    CHECK_EQ(pages.workerPool().size(), std::size_t(terrainWorkerBudget(std::thread::hardware_concurrency())));
    CHECK_EQ(&pages.workerPool(), &pages.workerPool());
}

TEST(terrain_worker_prioritizes_visible_then_preload_then_preparation) {
    using Task = TerrainWorkerPool::Task;
    TerrainWorkerPool pool(1);
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::atomic<bool> entered = false;
    // Test-only gate: register every source before allowing the next dispatch.
    auto gate = pool.add([&](std::size_t) {
        if (entered.exchange(true)) return false;
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return release; });
        return true;
    });
    CHECK(waitFor([&] { return entered.load(); }));
    std::vector<Task> order;
    const auto work = [&](Task task) {
        return [&, task](std::size_t) {
            std::lock_guard lock(mutex);
            if (std::find(order.begin(), order.end(), task) != order.end()) return false;
            order.push_back(task);
            return true;
        };
    };
    auto background = pool.add(work(Task::Preparation), Task::Preparation);
    auto preload = pool.add(work(Task::Preload), Task::Preload);
    auto visible = pool.add(work(Task::Visible), Task::Visible);
    { std::lock_guard lock(mutex); release = true; }
    wake.notify_all();
    CHECK(waitFor([&] { std::lock_guard lock(mutex); return order.size() == 3; }));
    pool.remove(gate); pool.remove(visible); pool.remove(preload); pool.remove(background);
    CHECK_EQ(order, (std::vector<Task>{Task::Visible, Task::Preload, Task::Preparation}));
}

TEST(terrain_worker_sources_share_six_workers_not_six_each) {
    using Task = TerrainWorkerPool::Task;
    TerrainWorkerPool pool(terrainWorkerBudget(12));
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    std::array<int, 3> started{};
    const auto work = [&](std::size_t type) {
        return [&, type](std::size_t) {
            std::unique_lock lock(mutex);
            if (started[type] == 2) return false;
            ++started[type];
            wake.wait(lock, [&] { return release; });
            return true;
        };
    };
    auto background = pool.add(work(2), Task::Preparation);
    auto preload = pool.add(work(1), Task::Preload);
    auto visible = pool.add(work(0), Task::Visible);
    CHECK(waitFor([&] { std::lock_guard lock(mutex); return started == std::array<int, 3>{2, 2, 2}; }));
    const auto stats = pool.stats();
    CHECK_EQ(stats.workers, std::size_t(6));
    CHECK_EQ(stats.busy, std::size_t(6));
    CHECK_EQ(std::accumulate(stats.tasks.begin(), stats.tasks.end(), std::size_t(0)), stats.busy);
    for (const auto task : {Task::Visible, Task::Preload, Task::Preparation})
        CHECK_EQ(stats.tasks[static_cast<std::size_t>(task)], std::size_t(2));
    { std::lock_guard lock(mutex); release = true; }
    wake.notify_all();
    pool.remove(background); pool.remove(preload); pool.remove(visible);
    CHECK_EQ(pool.stats().busy, std::size_t(0));
}

TEST(terrain_worker_permanent_phase_keeps_all_six_until_the_last_page_finishes) {
    using Task = TerrainWorkerPool::Task;
    TerrainWorkerPool pool(6);
    std::atomic<bool> preparing = true;
    std::atomic<int> claimed = 0, entered = 0, completed = 0, emptyPolls = 0;
    std::atomic<int> runtimeCalls = 0, earlyCalls = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool releaseFive = false, releaseLast = false;
    auto permanent = pool.add([&](std::size_t) {
        const int page = claimed++;
        if (page >= 6) { ++emptyPolls; return false; }
        ++entered;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [&] { return page == 0 ? releaseLast : releaseFive; });
        }
        if (++completed == 6) preparing = false;
        return true;
    }, Task::Preparation, [&] { return preparing.load(); });
    std::array<std::atomic<bool>, 3> ran{};
    const auto runtime = [&](std::size_t kind) {
        return [&, kind](std::size_t) {
            if (ran[kind].exchange(true)) return false;
            if (preparing) ++earlyCalls;
            ++runtimeCalls;
            return true;
        };
    };
    auto visible = pool.add(runtime(0), Task::Visible);
    auto preload = pool.add(runtime(1), Task::Preload);
    auto inspection = pool.add(runtime(2), Task::Inspection);
    CHECK(waitFor([&] { return entered == 6; }));
    const auto stats = pool.stats();
    CHECK_EQ(stats.busy, std::size_t(6));
    CHECK_EQ(stats.tasks[static_cast<std::size_t>(Task::Preparation)], std::size_t(6));
    CHECK_EQ(runtimeCalls.load(), 0);
    { std::lock_guard lock(mutex); releaseFive = true; }
    wake.notify_all();
    CHECK(waitFor([&] { return completed == 5 && emptyPolls > 0; }));
    // Five workers have no permanent tasks to claim. This must not let them
    // spend the budget on runtime work while the final page is still running.
    CHECK_EQ(runtimeCalls.load(), 0);
    { std::lock_guard lock(mutex); releaseLast = true; }
    wake.notify_all();
    CHECK(waitFor([&] { return runtimeCalls == 3; }));
    CHECK_EQ(earlyCalls.load(), 0);
    CHECK_EQ(completed.load(), 6);
    pool.remove(permanent); pool.remove(visible); pool.remove(preload); pool.remove(inspection);
}

TEST(terrain_worker_removing_an_exclusive_phase_wakes_waiting_detail) {
    TerrainWorkerPool pool(1);
    std::atomic<int> polls = 0;
    std::atomic<bool> ran = false;
    auto permanent = pool.add([&](std::size_t) { ++polls; return false; },
        TerrainWorkerPool::Task::Preparation, [] { return true; });
    auto detail = pool.add([&](std::size_t) { return !ran.exchange(true); },
        TerrainWorkerPool::Task::Visible);
    CHECK(waitFor([&] { return polls > 0; }));
    CHECK(!ran);
    pool.remove(permanent);
    CHECK(waitFor([&] { return ran.load(); }));
    pool.remove(detail);
    // Shutdown must also wake workers sleeping behind an unreleased phase.
    permanent = pool.add([](std::size_t) { return false; },
        TerrainWorkerPool::Task::Preparation, [] { return true; });
}

TEST(terrain_permanent_pages_are_all_published_before_runtime_detail_starts) {
    Country country;
    PageStore pages(country.world, country.graph, country.quant, {16u << 20, 2, 2});
    pages.prebakeInBackground({2, 4});
    std::atomic<bool> claimed = false, done = false, complete = false;
    auto detail = pages.workerPool().add([&](std::size_t) {
        if (claimed.exchange(true)) return false;
        const auto progress = pages.prebakeProgress();
        complete = !progress.running && progress.total > 0 && progress.done == progress.total &&
                   progress.published == progress.total && progress.failed == 0 && progress.finestPinned == 2;
        const auto h8 = pages.page({0, 0, 1});
        done = h8 && h8->base.valid();
        return true;
    }, TerrainWorkerPool::Task::Visible);
    CHECK(waitFor([&] { return done.load(); }));
    CHECK(complete);
    CHECK(!pages.resident({0, 0, 0}));
    pages.workerPool().remove(detail);
}

TEST(terrain_empty_permanent_preparation_does_not_block_runtime) {
    Country country;
    for (auto& cell : country.world.cells) cell.sea = true;
    PageStore pages(country.world, country.graph, country.quant, {1u << 20, 2, 1});
    for (const auto levels : {std::initializer_list<std::uint8_t>{}, {99}, {2, 4}}) {
        pages.prebakeInBackground(levels);
        CHECK(!pages.prebakeProgress().running);
        CHECK_EQ(pages.prebakeProgress().total, std::size_t(0));
        std::atomic<bool> ran = false;
        auto detail = pages.workerPool().add([&](std::size_t) { return !ran.exchange(true); },
            TerrainWorkerPool::Task::Visible);
        CHECK(waitFor([&] { return ran.load(); }));
        pages.workerPool().remove(detail);
    }
}

