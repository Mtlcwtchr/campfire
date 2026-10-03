#include "framework.hpp"
#include <set>
#include <unordered_map>
#include <cstdio>
#include <cmath>
#include <numbers>
#include <filesystem>
#include <cstdlib>

#include <atomic>
#include <chrono>
#include <thread>
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_plan.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace {
using namespace world::terrain;
using namespace world::streaming;

struct Country {
    generation::WorldMapData world;
    HydrologyGraph graph;
    std::unique_ptr<PageStore> pages;
    explicit Country(unsigned workers = 2, int width = 4, int height = 4, bool rugged = false) {
        world.width = width; world.height = height;
        world.cells.resize(width * height);
        for (int i = 0; i < width * height; ++i) {
            world.cells[i].elevation = rugged && (i + i / width) % 3 == 0 ? 176 : 40;
            world.cells[i].sea = false;
        }
        graph.macroWidth = width; graph.macroHeight = height;
        graph.macroCellMetres = generation::kMetresPerCell;
        PageStore::Config config; config.workerCount = workers;
        pages = std::make_unique<PageStore>(world, graph, hsimQuantisationFor(world), config);
    }
    std::vector<world::TileId> roots(int lod = 2) const {
        std::vector<world::TileId> result;
        const auto side = world::tileMetresAt(lod);
        for (int y = 0; y * side < world.height * generation::kMetresPerCell; ++y)
            for (int x = 0; x * side < world.width * generation::kMetresPerCell; ++x)
                result.push_back({x, y, lod});
        return result;
    }
    std::shared_ptr<TerrainResidency> resident() const {
        auto result = std::make_shared<TerrainResidency>();
        result->revision = 1;
        const int wide = (world.width * generation::kMetresPerCell + 511) / 512;
        const int high = (world.height * generation::kMetresPerCell + 511) / 512;
        for (std::uint8_t level : {0, 1, 2, 4})
            for (int y = -2; y < high + 2; ++y)
                for (int x = -2; x < wide + 2; ++x)
                    if (pages->containsLand({x, y, level})) result->pages.insert({x, y, level});
        return result;
    }
};

TerrainView viewAt(int target = 2, double x = 512, double y = 512, double half = 480) {
    TerrainView view;
    view.target = target; view.x = view.lookX = x; view.y = view.lookY = y;
    view.radius = half * 1.5; view.width = view.height = 800;
    view.matrix[0] = view.matrix[5] = float(1 / half);
    view.matrix[3] = float(-x / half); view.matrix[7] = float(-y / half);
    view.matrix[10] = view.matrix[15] = 1;
    view.prediction = view.matrix;
    return view;
}

TerrainView perspectiveView() {
    TerrainView view;
    view.width = 1600; view.height = 1000;
    view.x = view.lookX = -128; view.y = view.lookY = 512;
    view.radius = 50000;
    const float focal = 1.7320508f, depth = 50000.0f / 49999.5f;
    // Eye (-128,512,200), looking east with a horizontal horizon.
    view.matrix = {0, focal / 1.6f, 0, -512 * focal / 1.6f,
                   0, 0, focal, -200 * focal,
                   depth, 0, 0, 127.5f * depth,
                   1, 0, 0, 128};
    view.prediction = view.matrix;
    return view;
}

std::shared_ptr<const TerrainPlan> collect(TerrainPlanner& planner) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto plan = planner.collect()) return plan;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
    return {};
}

std::shared_ptr<const TerrainPlan> run(TerrainPlanner& planner, TerrainView view,
                                      std::shared_ptr<const TerrainResidency> residency, double time,
                                      bool restart = false) {
    CHECK(planner.request(view, std::move(residency), time, restart));
    return collect(planner);
}

bool waitCompleted(TerrainPlanner& planner, std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (planner.stats().completed >= count) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(false);
    return false;
}
}

TEST(terrain_plan_static_view_sleeps_and_unrelated_residency_reuses_roots) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto residency = country.resident();
    const auto view = viewAt();
    const auto first = run(planner, view, residency, 0.1);
    if (!first) return;
    CHECK(!first->drawing.empty());
    CHECK(!first->dirty(view, residency->revision));
    CHECK_EQ(first->viewMesh.percent(), 100);
    const auto submitted = planner.stats().submitted;
    for (int frame = 0; frame < 1000; ++frame)
        CHECK(!planner.request(view, residency, frame * 0.016));
    CHECK_EQ(planner.stats().submitted, submitted);
#if ASR_ENABLE_DIAGNOSTICS
    CHECK(first->buildMs > 0);
    CHECK(first->latencyMs > 0);
#else
    CHECK_EQ(first->buildMs, 0.0);
    CHECK_EQ(first->latencyMs, 0.0);
#endif
    CHECK_EQ(planner.stats().lastBuildMs, first->buildMs);
    CHECK_EQ(planner.stats().lastLatencyMs, first->latencyMs);
    CHECK(!planner.collect());
    auto changed = std::make_shared<TerrainResidency>(*residency);
    ++changed->revision; changed->pages.insert({999, 999, 0});
    const auto next = run(planner, view, changed, 0.2);
    if (!next) return;
    CHECK_EQ(next->rootsVisited, std::size_t(0));
    CHECK_EQ(next->rootsReused, country.roots().size());
    CHECK_EQ(first->viewMesh.percent(), 100); // published snapshots never mutate
}

TEST(terrain_plan_regional_root_does_not_wait_for_unrequested_ancestors) {
    Country country(1, 32, 32);
    TerrainPlanner planner(country.world, *country.pages, {{1, 1, 6}}, kRegionalDataLodPolicy);
    auto residency = std::make_shared<TerrainResidency>();
    auto view = viewAt(6, 12288, 12288, 1000);
    view.config.meshesPerPlan = 1;
    view.config.preloadPages = 0;
    view.config.morphSeconds = 0;
    auto plan = run(planner, view, residency, 0.1);
    CHECK(plan && !plan->missing.empty());
    if (!plan) return;
    residency = std::make_shared<TerrainResidency>(*residency);
    for (const auto key : plan->missing) {
        CHECK_EQ(key.level, 4);
        auto surface = std::make_shared<SurfacePage>();
        surface->step = 64; surface->padding = 2; surface->side = 13;
        surface->bed.assign(169, 360); surface->head.assign(169, 0);
        residency->pages.insert(key);
        residency->surfaces.emplace(key, std::move(surface));
    }
    ++residency->revision;
    plan = run(planner, view, residency, 0.2);
    for (int n = 0; plan && plan->needsUpdate && n < 20; ++n)
        plan = run(planner, view, residency, 0.3 + n * 0.1);
    CHECK(plan);
    if (!plan) return;
    CHECK(plan->missing.empty());
    CHECK_EQ(plan->blank, std::size_t(0));
    CHECK(!plan->drawing.empty());
    for (const auto& block : plan->drawing) CHECK(block.mesh != nullptr);
    CHECK_EQ(plan->viewMesh.percent(), 100);
}

TEST(terrain_generation_stage_switch_rebuilds_geometry_without_mutating_final_world) {
    Country country(2,2,2);
    auto f=std::make_shared<generation::TerrainFoundation>();
    f->columns=f->rows=18; f->pageColumns=f->pageRows=3;
    for (std::size_t s=0;s<5;++s) f->heightDm[s].assign(18*18,int(1000+s*1000));
    f->receiver.assign(18*18,-1); f->accumulation.assign(18*18,1); f->detailPages.assign(9,0);
    country.world.terrainFoundation=f;
    country.world.rockTypeField.assign(4,generation::RockType::HardRock);
    const auto identity=f->fingerprint();
    auto residency=country.resident();
    std::erase_if(residency->pages,[](auto key){return key.level!=4;});
    for (auto key:residency->pages) {
        auto s=std::make_shared<SurfacePage>(); s->step=64;s->padding=2;s->side=13;
        s->bed.assign(169,500);s->head.assign(169,0);residency->surfaces.emplace(key,s);
    }
    TerrainPlanner planner(country.world,*country.pages,country.roots(6),kRegionalDataLodPolicy);
    auto view=viewAt(4,512,512,400); double time=0;
    const auto settle=[&] {
        auto p=run(planner,view,residency,time+=0.1);
        for (int n=0;p && p->needsUpdate && n<100;++n) p=run(planner,view,residency,time+=0.1);
        CHECK(p && !p->needsUpdate);return p;
    };
    const auto final=settle(); if (!final || final->drawing.empty()) return;
    const auto original=final->drawing.front().mesh;
    CHECK(original); if (!original) return;
    const auto before=original->bed;
    view.stageFrom=generation::TerrainStage::Final;view.stageTo=generation::TerrainStage::Noise;
    const auto noise=settle(); if (!noise) return;
    CHECK(!noise->drawing.empty());
    for (const auto& block:noise->drawing) {
        CHECK(block.mesh); if (!block.mesh) continue;
        CHECK(block.mesh->step>=8);
        CHECK_EQ(block.mesh->sample(0,0)[0],100.0f); CHECK_EQ(block.mesh->samplePrior(0,0),500.0f);
    }
    CHECK_EQ(original->bed,before); CHECK_EQ(f->fingerprint(),identity);
    CHECK_EQ(country.world.terrainStage,generation::TerrainStage::Final);
    view.stageFrom=generation::TerrainStage::Noise;view.stageTo=generation::TerrainStage::Thermal;
    const auto thermal=settle(); if (!thermal) return;
    for (const auto& block:thermal->drawing) if (block.mesh) {
        CHECK_EQ(block.mesh->sample(0,0)[0],300.0f);CHECK_EQ(block.mesh->samplePrior(0,0),100.0f);
    }
}

TEST(terrain_regional_h16_admission_is_bounded_and_keeps_h64_fallback) {
    Country country;
    for (std::size_t limit:{0u,8u,256u}) {
        auto residency=country.resident();residency->capacity={0,64,limit};
        TerrainPlanner planner(country.world,*country.pages,country.roots(6),kRegionalDataLodPolicy);
        const auto view=viewAt(1);auto p=run(planner,view,residency,0.1);
        for (int n=2;p && p->needsUpdate && n<100;++n) p=run(planner,view,residency,n*0.1);
        CHECK(p);if (!p) continue;
        CHECK(!p->coverage.empty());CHECK(!p->needsUpdate);
        const auto h16=std::count_if(p->wanted.begin(),p->wanted.end(),[](auto key){return key.level==2;});
        CHECK(std::size_t(h16)<=limit);
        for (auto key:p->wanted) CHECK(key.level!=0);
    }
}

TEST(terrain_perspective_frustum_rejects_behind_near_far_and_side_planes) {
    const auto view = perspectiveView();
    CHECK(intersectsView({0, 500, 50, 520, 190, 210}, view.matrix));
    CHECK(!intersectsView({-300, 500, -200, 520, 190, 210}, view.matrix));
    CHECK(!intersectsView({-127.9, 511.9, -127.7, 512.1, 199.9, 200.1}, view.matrix));
    CHECK(!intersectsView({60000, 500, 61000, 520, 190, 210}, view.matrix));
    CHECK(!intersectsView({0, 5000, 50, 5100, 190, 210}, view.matrix));
    CHECK(!intersectsView({0, 500, 50, 520, 5000, 5100}, view.matrix));
    // Bounds straddling the eye must not be divided by a negative/zero w.
    CHECK(intersectsView({-200, 500, 200, 520, 190, 210}, view.matrix));
}

TEST(terrain_perspective_lod_tracks_depth_viewport_and_fov) {
    auto view = perspectiveView();
    const ViewBounds near{0, 0, 256, 256, 0, 100};
    const ViewBounds far{30000, 0, 30256, 256, 0, 100};
    CHECK(view.metresPerPixel(near) < view.metresPerPixel(far));
    CHECK(view.targetFor(near) < view.targetFor(far));
    const auto before = view.metresPerPixel(far);
    view.width *= 2; view.height *= 2;
    CHECK(std::abs(view.metresPerPixel(far) * 2 - before) < 1e-6);
    CHECK(view.targetFor(far) <= perspectiveView().targetFor(far));
    view = perspectiveView();
    for (int i = 0; i < 8; ++i) view.matrix[i] *= 2; // narrower FOV
    CHECK(std::abs(view.metresPerPixel(far) * 2 - before) < 1e-6);
}

TEST(terrain_camera_lod_reserves_finest_mesh_for_closeups) {
    const auto perspective = perspectiveView();
    const ViewBounds close{-108, 512, -107, 513, 200, 201.7}; // 20 m from the eye
    const ViewBounds middle{172, 512, 173, 513, 200, 201.7}; // 300 m
    const ViewBounds far{2872, 512, 2873, 513, 200, 201.7}; // 3000 m
    CHECK_EQ(perspective.targetFor(close), 0);
    CHECK(perspective.targetFor(middle) >= 2);
    CHECK(perspective.targetFor(far) >= 4);
    const ViewBounds focus{512, 512, 512, 512, 0, 0};
    for (double mpp : {0.05, 0.125, 0.5, 2.0}) {
        auto map = viewAt(0, 512, 512, mpp * 400);
        CHECK(std::abs(map.metresPerPixel(focus) - mpp) < 1e-6);
        CHECK_EQ(geometryLevelFor(map.metresPerPixel(focus)), geometryLevelFor(mpp));
        // Ortho framebuffer resize with the same world window really changes
        // projected object size, just as it does for a perspective view.
        map.width *= 2; map.height *= 2;
        CHECK(std::abs(map.metresPerPixel(focus) - mpp * 0.5) < 1e-6);
    }
}

TEST(terrain_plan_coarser_mesh_keeps_fine_data_and_parent_morph_dependencies) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots(3), kRenderDataLodPolicy);
    const auto residency = country.resident();
    auto view = viewAt(1);
    double time = 0;
    auto settle = [&](std::shared_ptr<const TerrainResidency> pages) {
        auto result = run(planner, view, pages, time += 0.1);
        for (int tick = 0; result && result->needsUpdate && tick < 100; ++tick)
            result = run(planner, view, pages, time += 0.1);
        return result;
    };
    const auto fine = settle(residency);
    if (!fine) return;
    CHECK(!fine->needsUpdate);
    CHECK(!fine->drawing.empty());
    CHECK_EQ(fine->viewMesh.percent(), 100);
    CHECK_EQ(fine->viewPages.percent(), 100);
    const std::unordered_set<TileKey> pinned(fine->pins.begin(), fine->pins.end());
    for (const auto& block : fine->drawing) {
        CHECK_EQ(block.tile.lod, 1); // H4 must NOT force a 4 m mesh
        CHECK_EQ(block.dataLevel, std::uint8_t(0));
        CHECK_EQ(block.parentDataLevel, std::uint8_t(1));
        const auto side = world::tileMetresAt(block.tile.lod);
        const auto halo = 2 * world::sampleMetresAt(block.tile.lod + 1);
        for (auto level : {block.dataLevel, block.parentDataLevel})
            for (auto y = world::floorDiv(block.tile.y * side - halo, 512);
                 y <= world::floorDiv((block.tile.y + 1) * side + halo, 512); ++y)
                for (auto x = world::floorDiv(block.tile.x * side - halo, 512);
                     x <= world::floorDiv((block.tile.x + 1) * side + halo, 512); ++x) {
                    const TileKey key{int(x), int(y), level};
                    if (country.pages->containsLand(key)) CHECK(pinned.contains(key));
                }
    }
    view = viewAt(2);
    const auto reversing = run(planner, view, residency, time += 0.1);
    if (!reversing) return;
    CHECK(reversing->morphing > 0);
    CHECK(std::any_of(reversing->pins.begin(), reversing->pins.end(), [](auto key) { return key.level == 0; }));
    const auto coarse = settle(residency);
    if (!coarse) return;
    for (const auto& block : coarse->drawing) {
        CHECK_EQ(block.tile.lod, 2);
        CHECK_EQ(block.dataLevel, std::uint8_t(1));
    }
    CHECK(std::none_of(coarse->pins.begin(), coarse->pins.end(), [](auto key) { return key.level == 0; }));
    view = viewAt(1);
    const auto restored = settle(residency);
    if (!restored) return;
    auto lost = std::make_shared<TerrainResidency>(*residency);
    ++lost->revision;
    lost->pages.erase({0, 0, 0});
    CHECK(!restored->compatible(*lost));
    const auto fallback = settle(lost);
    if (!fallback) return;
    CHECK(fallback->compatible(*lost));
    CHECK(!fallback->drawing.empty());
    for (const auto& block : fallback->drawing) CHECK(block.tile.lod >= 2);
    // A lost fine page leaves exactly one covering surface, not a hole or an
    // overlapping parent/child pair, across the entire retained root.
    for (int y = 128; y < 2048; y += 256)
        for (int x = 128; x < 2048; x += 256) {
            int count = 0;
            for (const auto& block : fallback->coverage) {
                const auto side = world::tileMetresAt(block.tile.lod);
                count += x >= block.tile.x * side && x < (block.tile.x + 1) * side &&
                         y >= block.tile.y * side && y < (block.tile.y + 1) * side;
            }
            CHECK_EQ(count, 1);
        }
}

TEST(terrain_plan_finer_data_admission_applies_before_lod2) {
    Country country;
    for (const bool allowH8 : {false, true}) {
        TerrainPlanner planner(country.world, *country.pages, country.roots(3), kRenderDataLodPolicy);
        auto residency = country.resident();
        residency->capacity = {0, allowH8 ? std::size_t(256) : std::size_t(0),256};
        const auto view = viewAt(1);
        auto plan = run(planner, view, residency, 0.1);
        for (int tick = 2; plan && plan->needsUpdate && tick < 100; ++tick)
            plan = run(planner, view, residency, tick * 0.1);
        if (!plan) return;
        CHECK(!plan->needsUpdate);
        CHECK(plan->capacityLimited);
        CHECK(!plan->drawing.empty());
        for (const auto& block : plan->drawing) CHECK_EQ(block.tile.lod, allowH8 ? 2 : 3);
        for (int level = 0; level < 2; ++level) {
            const auto count = std::count_if(plan->wanted.begin(), plan->wanted.end(),
                [&](auto key) { return key.level == level; });
            CHECK(std::size_t(count) <= residency->capacity[level]);
        }
        CHECK(!planner.request(view, residency, 100)); // no refill/morph busy loop
    }
}

TEST(terrain_plan_data_quality_changes_without_changing_mesh_density) {
    Country country;
    const auto residency = country.resident();
    std::unordered_set<std::int64_t> mesh;
    for (int finer = 0; finer <= 2; ++finer) {
        TerrainPlanner planner(country.world, *country.pages, country.roots(3), DataLodPolicy{finer});
        const auto view = viewAt(2);
        auto plan = run(planner, view, residency, 0.1);
        for (int tick = 2; plan && plan->needsUpdate && tick < 50; ++tick)
            plan = run(planner, view, residency, tick * 0.1);
        if (!plan) return;
        CHECK(!plan->drawing.empty());
        CHECK_EQ(plan->viewMesh.percent(), 100);
        CHECK_EQ(plan->viewPages.percent(), 100);
        CHECK(!plan->capacityLimited);
        for (const auto& block : plan->drawing) {
            CHECK_EQ(block.tile.lod, 2); // always 16 m geometry
            CHECK_EQ(int(block.dataLevel), 2 - finer); // H16, H8, then H4
            if (finer == 0) mesh.insert(world::tileKeyOf(block.tile));
            else CHECK(mesh.contains(world::tileKeyOf(block.tile)));
        }
        CHECK_EQ(plan->drawing.size(), mesh.size());
        CHECK(!planner.request(view, residency, 100));
    }
}

TEST(terrain_water_proof_requires_both_datasets_halo_and_real_residency) {
    TerrainResidency residency;
    const world::TileId tile{2, 2, 1}; // [1024,1536], mesh 8 m, data H4/H8
    CHECK(residency.mayHaveWater(tile, 0, 1));
    for (std::uint8_t level : {0, 1})
        for (int y = 1; y <= 3; ++y)
            for (int x = 1; x <= 3; ++x) {
                residency.pages.insert({x, y, level});
                residency.dryPages.insert({x, y, level});
            }
    CHECK(!residency.mayHaveWater(tile, 0, 1));
    // Missing/ocean or potentially wet samples at either endpoint invalidate
    // the proof, including the parent corner beyond the tile's visible edge.
    for (std::uint8_t level : {0, 1})
        for (const auto key : {TileKey{2, 2, level}, TileKey{3, 3, level}, TileKey{1, 1, level}}) {
            residency.dryPages.erase(key);
            CHECK(residency.mayHaveWater(tile, 0, 1));
            residency.dryPages.insert(key);
            residency.pages.erase(key); // stale proof cannot mask unknown fields
            CHECK(residency.mayHaveWater(tile, 0, 1));
            residency.pages.insert(key);
        }
    CHECK(!residency.mayHaveWater(tile, 0, 1));
    CHECK(!residency.mayHaveWater(tile, 0, 0)); // same-dataset morph
    CHECK(residency.mayHaveWater({-2, -2, 1}, 0, 1)); // absent ocean outside world
}

TEST(terrain_plan_dry_water_flags_survive_retained_coverage_and_reverse_morph) {
    Country country(2, 12, 12);
    const auto residency = country.resident();
    residency->dryPages = residency->pages;
    TerrainPlanner planner(country.world, *country.pages, {{1, 1, 2}}, kRenderDataLodPolicy);
    auto view = viewAt(1, 1536, 1536);
    double time = 0;
    auto settle = [&](const auto& pages) {
        auto result = run(planner, view, pages, time += 0.1);
        for (int tick = 0; result && result->needsUpdate && tick < 50; ++tick)
            result = run(planner, view, pages, time += 0.1);
        return result;
    };
    const auto dry = settle(residency);
    if (!dry) return;
    CHECK(!dry->drawing.empty());
    for (const auto& block : dry->coverage) CHECK(!block.mayHaveWater);
    auto wet = std::make_shared<TerrainResidency>(*residency);
    ++wet->revision;
    wet->dryPages.erase({3, 3, 1}); // wet H8 parent despite dry H4 children
    const auto water = settle(wet);
    if (!water) return;
    CHECK_EQ(water->viewMesh.percent(), 100);
    CHECK_EQ(water->coverage.size(), dry->coverage.size());
    for (const auto& block : water->coverage) CHECK(block.mayHaveWater);
    for (const auto& block : dry->coverage) CHECK(!block.mayHaveWater); // immutable
    std::vector<TerrainPlan::Block> recull;
    water->selectDrawing(viewAt(1, 1800, 1800), recull);
    CHECK(!recull.empty());
    for (const auto& block : recull) CHECK(block.mayHaveWater);
    view = viewAt(2, 1536, 1536);
    const auto reversing = run(planner, view, wet, time += 0.1);
    if (!reversing) return;
    CHECK(reversing->morphing > 0);
    for (const auto& block : reversing->coverage) CHECK(block.mayHaveWater);
    const auto coarse = settle(wet);
    if (!coarse) return;
    CHECK_EQ(coarse->coverage.size(), std::size_t(1));
    CHECK(coarse->coverage.front().mayHaveWater);
    auto restored = std::make_shared<TerrainResidency>(*residency);
    restored->revision = wet->revision + 1;
    const auto dryAgain = settle(restored);
    if (!dryAgain) return;
    for (const auto& block : dryAgain->coverage) CHECK(!block.mayHaveWater);
    CHECK(!planner.request(view, restored, time + 1)); // no new idle traversal
}

TEST(terrain_plan_perspective_drains_one_snapshot_without_camera_starvation) {
    Country country(1);
    TerrainPlanner planner(country.world, *country.pages, country.roots(6));
    const auto residency = country.resident();
    auto& pool = country.pages->workerPool();
    std::atomic<bool> gate{true};
    auto blocker = pool.add([](std::size_t) { return false; }, TerrainWorkerPool::Task::Preparation,
                            [&] { return gate.load(); });
    auto view = perspectiveView();
    CHECK(!planner.busy());
    CHECK(planner.request(view, residency, 0.1));
    const auto submittedView = view;
    for (int frame = 0; frame < 100; ++frame) {
        view.matrix[7] -= 0.001f;
        CHECK(planner.busy());
        if (!planner.busy()) planner.request(view, residency, frame * 0.1);
    }
    CHECK_EQ(planner.stats().submitted, std::size_t(1));
    gate = false; pool.remove(blocker);
    if (!waitCompleted(planner, 1)) return;
    CHECK(planner.busy()); // completed but not collected: don't invalidate it
    const auto completed = planner.collect();
    if (!completed) { CHECK(false); return; }
    CHECK(!planner.busy());
    CHECK(completed->view == submittedView);
    CHECK(completed->view != view);
    CHECK(completed->compatible(*residency));
    CHECK_EQ(planner.stats().discarded, std::size_t(0));
    auto lost = *residency;
    CHECK(!completed->pins.empty());
    if (!completed->pins.empty()) lost.pages.erase(completed->pins.front());
    CHECK(!completed->compatible(lost));
    const auto newest = run(planner, view, residency, 11);
    if (!newest) return;
    CHECK(newest->view == view);
    CHECK(newest->sequence > completed->sequence);
}

TEST(terrain_plan_perspective_has_mixed_lod_and_retains_coverage_looking_up) {
    Country country(2, 64, 4);
    TerrainPlanner planner(country.world, *country.pages, country.roots(6));
    const auto residency = country.resident();
    const auto view = perspectiveView();
    auto plan = run(planner, view, residency, 0.1);
    if (!plan) return;
    for (int tick = 2; tick <= 100 && plan->needsUpdate; ++tick) {
        plan = run(planner, view, residency, tick * 0.1);
        if (!plan) return;
    }
    int finest = 6, coarsest = 0;
    for (const auto& block : plan->drawing) {
        finest = std::min(finest, block.tile.lod);
        coarsest = std::max(coarsest, block.tile.lod);
    }
    CHECK(finest < coarsest);
    CHECK(coarsest >= 3);
    for (int level = 0; level < 2; ++level) {
        const auto count = std::count_if(plan->pins.begin(), plan->pins.end(),
            [&](TileKey key) { return key.level == level; });
        CHECK(std::size_t(count) <= residency->capacity[level]);
    }
    auto sky = view;
    const float depth = 50000.0f / 49999.5f;
    sky.matrix = {0, 1, 0, -512, 1, 0, 0, 128,
                  0, 0, depth, -10000.5f * depth, 0, 0, 1, -10000};
    sky.prediction = sky.matrix;
    std::vector<TerrainPlan::Block> selected;
    plan->selectDrawing(sky, selected);
    CHECK(selected.empty());
    plan = run(planner, sky, residency, 11);
    if (!plan) return;
    for (int tick = 1; tick <= 100 && plan->needsUpdate; ++tick) {
        plan = run(planner, sky, residency, 11 + tick * 0.1);
        if (!plan) return;
    }
    CHECK(plan->drawing.empty());
    CHECK(!plan->coverage.empty());
    CHECK_EQ(plan->viewMesh.percent(), 100);
    plan->selectDrawing(view, selected);
    CHECK(!selected.empty());
}

TEST(terrain_plan_h64_covers_new_camera_while_detail_worker_is_blocked) {
    // Wide enough that the third camera below (two root tiles in) is still
    // over the world, whatever a macro cell measures.
    Country country(1, int((3 * world::tileMetresAt(6)) / generation::kMetresPerCell) - 24, 2);
    const auto roots = country.roots(6);
    TerrainPlanner planner(country.world, *country.pages, roots);
    auto residency = country.resident();
    std::erase_if(residency->pages, [](TileKey key) { return key.level != 4; });
    const TileKey outsideOldView{40, 0, 4};
    residency->pages.erase(outsideOldView);
    const auto original = viewAt(6);
    const auto waiting = run(planner, original, residency, 0.1);
    if (!waiting) return;
    CHECK(std::find(waiting->missing.begin(), waiting->missing.end(), outsideOldView) != waiting->missing.end());
    // Foundation is requested outside the old viewport, before fine geometry.
    auto ready = std::make_shared<TerrainResidency>(*residency);
    ++ready->revision; ready->pages.insert(outsideOldView);
    const auto accepted = run(planner, original, ready, 0.2);
    if (!accepted) return;
    CHECK_EQ(accepted->coverage.size(), roots.size());
    CHECK(accepted->drawing.size() < accepted->coverage.size());
    CHECK(accepted->compatible(*ready));
    CHECK(std::find(accepted->pins.begin(), accepted->pins.end(), outsideOldView) != accepted->pins.end());

    auto& pool = country.pages->workerPool();
    std::atomic<bool> gate{true};
    auto blocker = pool.add([](std::size_t) { return false; }, TerrainWorkerPool::Task::Preparation,
                            [&] { return gate.load(); });
    TerrainView latest;
    std::vector<TerrainPlan::Block> visible;
    for (int frame = 0; frame < 100; ++frame) {
        latest = viewAt(0, 512 + (frame % 3) * world::tileMetresAt(6));
        CHECK(planner.request(latest, ready, 0.3 + frame * 0.016));
        CHECK(!planner.collect());
        accepted->selectDrawing(latest, visible);
        int covering = 0;
        for (const auto& block : visible) {
            const auto side = world::tileMetresAt(block.tile.lod);
            covering += latest.x >= block.tile.x * side && latest.x < (block.tile.x + 1) * side &&
                        latest.y >= block.tile.y * side && latest.y < (block.tile.y + 1) * side;
            CHECK_EQ(dataLevelForGeometryLevel(block.tile.lod), std::uint8_t(4));
        }
        CHECK_EQ(covering, 1); // neither a hole nor a parent/child double surface
    }
    CHECK_EQ(planner.stats().completed, std::size_t(2));
    gate = false; pool.remove(blocker);
    const auto completed = collect(planner);
    if (!completed) return;
    CHECK(completed->view == latest);
    CHECK(completed->compatible(*ready));
    CHECK_EQ(accepted->coverage.size(), roots.size()); // old pinned snapshot never mutated
}

TEST(terrain_plan_page_arrival_invalidates_only_dependent_roots) {
    Country country(2, 8, 2);
    const std::vector<world::TileId> roots{{0, 0, 2}, {3, 0, 2}};
    TerrainPlanner planner(country.world, *country.pages, roots);
    auto residency = country.resident();
    const TileKey missing{0, 0, 2};
    residency->pages.erase(missing);
    const auto view = viewAt(2, 2048, 512, 2300);
    const auto first = run(planner, view, residency, 0.1);
    if (!first) return;
    CHECK_EQ(first->blank, std::size_t(1));
    CHECK(!first->needsUpdate); // waiting is not a reason to spin
    auto arrived = std::make_shared<TerrainResidency>(*residency);
    ++arrived->revision; arrived->pages.insert(missing);
    const auto next = run(planner, view, arrived, 0.2);
    if (!next) return;
    CHECK_EQ(next->rootsVisited, std::size_t(1));
    CHECK_EQ(next->rootsReused, std::size_t(1));
    CHECK_EQ(next->blank, std::size_t(0));
}

TEST(terrain_plan_morphs_finish_then_stop_and_camera_changes_invalidate) {
    Country country(1);
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto residency = country.resident();
    auto view = viewAt(0);
    std::shared_ptr<const TerrainPlan> plan;
    bool morphSeen = false;
    double time = 0;
    for (int i = 0; i < 30; ++i) {
        plan = run(planner, view, residency, time += 0.1);
        if (!plan) return;
        morphSeen = morphSeen || plan->morphing > 0;
        CHECK(plan->compatible(*residency));
        if (!plan->needsUpdate) break;
    }
    CHECK(morphSeen);
    CHECK(!plan->needsUpdate);
    CHECK_EQ(plan->coarse, std::size_t(0));
    CHECK_EQ(plan->viewMesh.percent(), 100);
    CHECK(!plan->dirty(view, residency->revision));
    auto lost = *residency;
    CHECK(!plan->pins.empty());
    if (!plan->pins.empty()) lost.pages.erase(plan->pins.front());
    CHECK(!plan->compatible(lost));
    view = viewAt(2); // reverse morph must continue even with no new pages
    CHECK(plan->dirty(view, residency->revision));
    for (int i = 0; i < 30; ++i) {
        plan = run(planner, view, residency, time += 0.1);
        if (!plan || !plan->needsUpdate) break;
    }
    if (!plan) return;
    CHECK(!plan->needsUpdate);
    for (const auto& block : plan->drawing) CHECK_EQ(block.tile.lod, 2);
    auto resized = view; ++resized.width;
    CHECK(plan->dirty(resized, residency->revision));
    auto tilted = view; tilted.matrix[2] = 0.25f;
    CHECK(plan->dirty(tilted, residency->revision));
}

TEST(terrain_plan_capacity_limited_cut_does_not_busy_loop) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto residency = country.resident(); residency->capacity = {0, 0,256};
    auto plan = run(planner, viewAt(0), residency, 0.1);
    if (!plan) return;
    CHECK(plan->capacityLimited);
    CHECK(!plan->needsUpdate);
    CHECK(plan->coarse > 0);
    CHECK(plan->preload.empty());
    CHECK(!plan->dirty(viewAt(0), residency->revision));
}

TEST(terrain_plan_cached_waiting_family_keeps_its_slot_reservation) {
    Country country(2, 8, 2);
    const auto view = viewAt(0, 1500, 256, 2200);
    auto residency = country.resident();
    std::erase_if(residency->pages, [](TileKey key) { return key.level == 0; });
    TerrainPlanner reference(country.world, *country.pages, {{0, 0, 1}});
    const auto family = run(reference, view, residency, 0.1);
    if (!family) return;
    std::unordered_set<TileKey> reserved;
    for (auto key : family->required) if (key.level == 0) reserved.insert(key);
    CHECK(!reserved.empty());
    residency->capacity[0] = reserved.size();
    TerrainPlanner planner(country.world, *country.pages, {{0, 0, 1}, {6, 0, 1}});
    const auto first = run(planner, view, residency, 0.1);
    if (!first) return;
    CHECK(first->capacityLimited);
    CHECK(!first->needsUpdate);
    auto changed = std::make_shared<TerrainResidency>(*residency);
    ++changed->revision; changed->pages.insert({999, 999, 2});
    const auto next = run(planner, view, changed, 0.2);
    if (!next) return;
    CHECK_EQ(next->rootsReused, std::size_t(1));
    CHECK_EQ(next->rootsVisited, std::size_t(1));
    CHECK(next->capacityLimited);
    for (auto key : next->required) if (key.level == 0) CHECK(reserved.contains(key));
}

TEST(terrain_plan_lost_page_drops_stale_child_transition) {
    Country country;
    auto residency = country.resident();
    TerrainPlanner planner(country.world, *country.pages, {{0, 0, 2}});
    const auto view = viewAt(1);
    const auto first = run(planner, view, residency, 0.1);
    if (!first) return;
    CHECK(first->morphing > 0);
    auto changed = std::make_shared<TerrainResidency>(*residency);
    ++changed->revision; changed->pages.erase({0, 0, 1});
    CHECK(!first->compatible(*changed));
    const auto next = run(planner, view, changed, 0.2);
    if (!next) return;
    CHECK(next->compatible(*changed));
    CHECK(!next->drawing.empty());
    for (const auto& block : next->drawing) CHECK_EQ(block.tile.lod, 2);
    CHECK(!next->needsUpdate);
    CHECK_EQ(next->viewMesh.percent(), 0);
}

TEST(terrain_plan_single_worker_gate_bounds_queue_and_teardown) {
    Country country(1);
    auto& pool = country.pages->workerPool();
    std::atomic<bool> gate{true};
    auto blocker = pool.add([](std::size_t) { return false; }, TerrainWorkerPool::Task::Preparation,
                            [&] { return gate.load(); });
    {
        TerrainPlanner planner(country.world, *country.pages, country.roots());
        CHECK(planner.request(viewAt(), country.resident(), 0.1));
        for (int i = 0; i < 100; ++i)
            CHECK(planner.request(viewAt(0, i * 100), country.resident(), 0.2));
        CHECK(!planner.collect());
        CHECK_EQ(planner.stats().submitted, std::size_t(1));
        // Removing a queued planner must not wait for an exclusive phase.
    }
    gate = false; pool.remove(blocker);
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    const auto plan = run(planner, viewAt(), country.resident(), 0.1);
    CHECK(bool(plan)); // two stages also make progress on ONE shared worker
    CHECK_EQ(planner.stats().completed, std::size_t(1));
    for (int i = 0; i < 20; ++i) {
        TerrainPlanner transient(country.world, *country.pages, country.roots());
        CHECK(transient.request(viewAt(0), country.resident(), 0.1));
    } // drain active callbacks before destroying their caches/world references
}

TEST(terrain_plan_prediction_never_becomes_draw_geometry_and_ocean_is_empty) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto view = viewAt(0, -10000, -10000);
    view.prediction = viewAt(0).matrix;
    view.lookX = view.lookY = 512;
    auto residency = std::make_shared<TerrainResidency>();
    const auto plan = run(planner, view, residency, 0.1);
    if (!plan) return;
    CHECK(plan->drawing.empty());
    CHECK(!plan->preload.empty());
    CHECK(plan->preload.size() <= 128);
    CHECK_EQ(plan->viewPages.percent(), 100);
    CHECK_EQ(plan->viewMesh.percent(), 100);
    CHECK(!plan->needsUpdate);
}

TEST(terrain_plan_only_latest_camera_and_residency_can_publish) {
    Country country(1);
    auto& pool = country.pages->workerPool();
    std::atomic<bool> gate{true};
    auto blocker = pool.add([](std::size_t) { return false; }, TerrainWorkerPool::Task::Preparation,
                            [&] { return gate.load(); });
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto residency = country.resident();
    CHECK(planner.request(viewAt(), residency, 0.1));
    TerrainView latest;
    for (int i = 0; i < 100; ++i) {
        latest = viewAt(0, i * 10);
        CHECK(planner.request(latest, residency, 0.2));
        CHECK(!planner.request(latest, residency, 0.3));
    }
    auto changed = std::make_shared<TerrainResidency>(*residency);
    ++changed->revision;
    changed->pages.erase({0, 0, 0});
    CHECK(planner.request(latest, changed, 0.4));
    CHECK_EQ(planner.stats().submitted, std::size_t(1));
    CHECK_EQ(planner.stats().completed, std::size_t(0)); // H64/H16 gate still owns the pool
    CHECK(!planner.collect());
    gate = false; pool.remove(blocker);
    const auto plan = collect(planner);
    if (!plan) return;
    CHECK(plan->view == latest);
    CHECK_EQ(plan->residencyRevision, changed->revision);
    CHECK(plan->compatible(*changed));
    CHECK_EQ(plan->sequence, std::uint64_t(102));
    CHECK_EQ(planner.stats().submitted, std::size_t(2)); // no camera-history backlog
    CHECK_EQ(planner.stats().discarded, std::size_t(1));
    CHECK(!planner.collect());
}

TEST(terrain_plan_ready_result_is_discarded_before_camera_returns_to_old_view) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    const auto residency = country.resident();
    const auto original = run(planner, viewAt(), residency, 0.1);
    if (!original) return;
    const auto moved = viewAt(2, 1536);
    CHECK(planner.request(moved, residency, 0.2));
    if (!waitCompleted(planner, 2)) return;
    // Even though an accepted snapshot of this camera exists, returning to it
    // must invalidate the newer request that has already completed on a worker.
    CHECK(planner.request(viewAt(), residency, 0.3));
    const auto next = collect(planner);
    if (!next) return;
    CHECK(next->view == original->view);
    CHECK(next->sequence > original->sequence);
    CHECK_EQ(next->sequence, std::uint64_t(3));
    CHECK_EQ(planner.stats().discarded, std::size_t(1));
    CHECK(original->view == viewAt());
    CHECK_EQ(original->viewMesh.percent(), 100);
}

TEST(terrain_plan_superseded_residency_does_not_restart_a_safe_morph) {
    for (const bool alreadyCompleted : {false, true}) {
        Country country(1);
        TerrainPlanner planner(country.world, *country.pages, {{0, 0, 2}});
        auto residency = country.resident();
        const auto view = viewAt(1);
        const auto first = run(planner, view, residency, 0.1);
        if (!first) return;
        CHECK(first->morphing > 0);
        auto& pool = country.pages->workerPool();
        std::atomic<bool> gate{!alreadyCompleted};
        auto blocker = pool.add([](std::size_t) { return false; }, TerrainWorkerPool::Task::Preparation,
                                [&] { return gate.load(); });
        CHECK(planner.request(view, residency, 0.2));
        if (alreadyCompleted) CHECK(waitCompleted(planner, 2));
        auto changed = std::make_shared<TerrainResidency>(*residency);
        ++changed->revision;
        changed->pages.insert({999, 999, 2}); // no dependency has been lost
        CHECK(planner.request(view, changed, 0.3));
        gate = false; pool.remove(blocker);
        const auto next = collect(planner);
        if (!next) return;
        CHECK(next->compatible(*changed));
        CHECK_EQ(next->sequence, std::uint64_t(3));
        CHECK_EQ(planner.stats().discarded, std::size_t(1));
        CHECK_EQ(next->morphing, std::size_t(0));
        CHECK_EQ(next->viewMesh.percent(), 100);
        CHECK_EQ(next->drawing.size(), first->drawing.size());
        CHECK(first->morphing > 0); // the accepted immutable snapshot is untouched
    }
}

TEST(terrain_plan_prediction_reserves_h4_h8_capacity_including_resident_pages) {
    Country country;
    TerrainPlanner planner(country.world, *country.pages, country.roots());
    auto view = viewAt(0, -10000, -10000);
    view.prediction = viewAt(0).matrix;
    view.lookX = view.lookY = 512;
    auto residency = std::make_shared<TerrainResidency>();
    residency->capacity = {3, 2,256};
    const auto first = run(planner, view, residency, 0.1);
    if (!first) return;
    CHECK(first->drawing.empty());
    std::array<std::size_t, 2> requested{};
    for (auto key : first->preload) if (key.level < 2) ++requested[key.level];
    CHECK_EQ(requested[0], residency->capacity[0]);
    CHECK_EQ(requested[1], residency->capacity[1]);
    auto arrived = std::make_shared<TerrainResidency>(*residency);
    ++arrived->revision;
    arrived->pages.insert(first->preload.begin(), first->preload.end());
    const auto next = run(planner, view, arrived, 0.2);
    if (!next) return;
    CHECK(next->preload.empty()); // no endless refill/eviction on a static view
    std::array<std::size_t, 2> wanted{};
    for (auto key : next->wanted) if (key.level < 2) ++wanted[key.level];
    CHECK_EQ(wanted[0], requested[0]);
    CHECK_EQ(wanted[1], requested[1]);
    CHECK(!planner.request(view, arrived, 0.3));
}

TEST(terrain_plan_world_teardown_drops_uncollected_results) {
    {
        Country previous;
        TerrainPlanner planner(previous.world, *previous.pages, previous.roots());
        CHECK(planner.request(viewAt(0), previous.resident(), 0.1));
        CHECK(waitCompleted(planner, 1));
    } // planner drains before PageStore and world; no result survives this scope
    Country next(1);
    TerrainPlanner planner(next.world, *next.pages, next.roots());
    CHECK(!planner.collect());
    const auto view = viewAt(2, -10000, -10000);
    const auto plan = run(planner, view, next.resident(), 0.1);
    if (!plan) return;
    CHECK(plan->drawing.empty());
    CHECK(plan->view == view);
    CHECK_EQ(plan->sequence, std::uint64_t(1));
}

TEST(terrain_plan_source_quality_floor_does_not_force_dense_planar_geometry) {
    for (const double slope : {0.0,2.0}) {
        const auto mesh = makeAdaptiveMesh(64,16,0.1,[=](double x,double y) {
            return std::array{float(100+slope*x+0.25*y),0.0f};
        });
        CHECK_EQ(mesh->detailError,0.0);
        const ViewBounds box{0,0,1024,1024,0,4096};
        auto view = viewAt(3);
        CHECK(view.refineSurface(box,6,mesh.get(),kRenderDataLodPolicy)); // flat H64 is not proof
        CHECK(view.refineSurface(box,5,mesh.get(),kRenderDataLodPolicy));
        CHECK(!view.refineSurface(box,4,mesh.get(),kRenderDataLodPolicy)); // H16 already satisfies data goal
        view.target = 1;
        CHECK(view.refineSurface(box,4,mesh.get(),kRenderDataLodPolicy));
        CHECK(view.refineSurface(box,2,mesh.get(),kRenderDataLodPolicy));
        CHECK(!view.refineSurface(box,1,mesh.get(),kRenderDataLodPolicy)); // H4 without forced LOD0
        CHECK(mesh->surfaceIndices < 64u*64*6/3);
    }
}

TEST(terrain_regional_policy_never_requests_full_h4_pages) {
    for (int lod=0;lod<=6;++lod) {
        CHECK(dataLevelForGeometryLevel(lod,kRegionalDataLodPolicy)>=1);
        CHECK(dataStepForGeometry(4<<lod,kRegionalDataLodPolicy)>=8);
    }
    Country country;
    TerrainPlanner planner(country.world,*country.pages,country.roots(3),kRegionalDataLodPolicy);
    const auto residency=country.resident();
    const auto view=viewAt(0);
    auto plan=run(planner,view,residency,0.1);
    for (int tick=2;plan && plan->needsUpdate && tick<100;++tick) plan=run(planner,view,residency,tick*0.1);
    if (!plan) return;
    for (const auto key:plan->wanted) CHECK(key.level!=0);
    for (const auto& block:plan->coverage) CHECK(block.tile.lod>=1);
    CHECK_EQ(plan->detail.localSamples,std::size_t(0));
}

TEST(terrain_local_h4_window_has_fixed_budget_and_reuses_overlap_without_pages) {
    Country country;
    TerrainDetail detail(country.world,*country.pages);
    const auto before=country.pages->stats().baked;
    CHECK(detail.updateLocal(true,256,256,0.25));
    CHECK_EQ(detail.stats().localSamples,std::size_t(2601));
    CHECK_EQ(detail.stats().localEvaluated,std::size_t(2601));
    CHECK(!detail.transitioning());
    CHECK(!detail.updateLocal(true,257,257,0.1));
    CHECK_EQ(detail.stats().localEvaluated,std::size_t(2601));
    CHECK(detail.updateLocal(true,272,256,0.1));
    CHECK_EQ(detail.stats().localEvaluated,std::size_t(2601+4*51));
    CHECK_EQ(detail.stats().localSamples,std::size_t(2601));
    CHECK_EQ(country.pages->stats().baked,before); // not a disguised 512 m page
    CHECK(detail.updateLocal(false,272,256,0.1));
    CHECK(detail.transitioning());
    CHECK(detail.stats().localSamples>0);
    CHECK(detail.updateLocal(false,272,256,0.25));
    CHECK_EQ(detail.stats().localSamples,std::size_t(0));
    CHECK(!detail.transitioning());
}

TEST(terrain_regional_geometry_stops_at_h8_even_for_a_nearby_perspective_demand) {
    const ViewBounds near{0,0,256,256,0,100},far{2048,2048,2304,2304,0,100};
    auto view=viewAt(0,128,128); view.localDetail=true;
    CHECK(!view.localTouches(near)); // map view cannot acquire an H4 window
    view.matrix[12]=1;
    CHECK(view.localTouches(near));
    CHECK(!view.localTouches(far));
    CHECK(!view.refineSurface(near,1,nullptr,kRegionalDataLodPolicy));
    CHECK(!view.refineSurface(far,1,nullptr,kRegionalDataLodPolicy));
    view.localDetail=false;
    CHECK(!view.refineSurface(near,1,nullptr,kRegionalDataLodPolicy));
}

TEST(terrain_h16_feature_metadata_is_bounded_and_does_not_bake_h4) {
    Country country;
    const auto page=country.pages->page({0,0,2});
    CHECK(bool(page));
    if (!page) return;
    CHECK_EQ(page->featureCells.size(),std::size_t(1024));
    for (const auto flags:page->featureCells) CHECK(flags<=7);
    CHECK(!country.pages->resident({0,0,0}));
    CHECK(!country.pages->resident({0,0,1}));
}

static void checkRegionalSurfaces(bool rugged) {
    Country country(2,2,2,rugged);
    auto resident=country.resident();
    resident->capacity[0]=0;
    std::erase_if(resident->pages,[](auto key){return key.level==0;});
    for (const auto key:resident->pages) {
        const auto baked=country.pages->page(key);
        CHECK(bool(baked));
        if (!baked) return;
        auto page=std::make_shared<SurfacePage>();
        page->step=baked->base.sampleMetres;page->padding=baked->base.padding;
        page->side=baked->base.width+2*page->padding;
        const double low=baked->base.elevationMin.toDouble(),range=(baked->base.elevationMax-baked->base.elevationMin).toDouble();
        for (std::size_t i=0;i<baked->base.heightQuantized.size();++i) {
            page->bed.push_back(float(low+baked->base.heightQuantized[i]*range/65535.0+
                (baked->large.deltaQuantized.empty()?0:baked->large.deltaQuantized[i]*0.01)));
            page->head.push_back(baked->water.surfaceQuantized[i]?float(low+baked->water.surfaceQuantized[i]*range/65535.0):0.0f);
        }
        resident->surfaces.emplace(key,std::move(page));
    }
    TerrainPlanner planner(country.world,*country.pages,country.roots(6),kRegionalDataLodPolicy);
    auto view=perspectiveView();
    view.localDetail=true;
    view.x=view.lookX=128;
    view.matrix[15]=-128;
    view.matrix[11]=-128.5f*(50000.0f/49999.5f);
    view.prediction=view.matrix;
    double time=0;
    auto settle=[&] {
        auto plan=run(planner,view,resident,time+=0.1);
        for (int tick=0;plan && plan->needsUpdate && tick<80;++tick) plan=run(planner,view,resident,time+=0.1);
        return plan;
    };
    const auto close=settle();
    if (!close) return;
    CHECK(!close->needsUpdate);
    CHECK(!close->coverage.empty());
    CHECK_EQ(close->detail.localSamples,std::size_t(0));
    CHECK_EQ(close->detail.localAmount,0.0);
    CHECK_EQ(close->viewMesh.percent(),100);
    for (const auto key:close->wanted) CHECK(key.level!=0);
    for (const auto& block:close->coverage) {
        CHECK(bool(block.mesh));
        if (block.mesh) CHECK(block.mesh->step>=8);
        if (block.mesh) for (const auto& v:block.mesh->vertices) CHECK(v.skirt&4);
    }
    CHECK(close->compatible(*resident));
    CHECK(!planner.request(view,resident,time+1));
    view.localDetail=false;
    const auto away=settle();
    if (!away) return;
    CHECK_EQ(away->detail.localSamples,std::size_t(0));
    CHECK(!away->needsUpdate);
    CHECK_EQ(away->viewMesh.percent(),100);
    CHECK_EQ(close->detail.localSamples,std::size_t(0)); // accepted product is immutable
    CHECK_EQ(planner.stats().failed,std::size_t(0));
}

TEST(terrain_regional_real_surfaces_settle_local_geometry_without_h4_residency) {
    checkRegionalSurfaces(false);
}

TEST(terrain_regional_feature_geometry_progress_uses_the_actual_source) {
    checkRegionalSurfaces(true);
}

static void checkStagedSurfaces(DataLodPolicy policy) {
    Country country(2,2,2);
    const auto available = country.resident();
    TerrainPlanner planner(country.world,*country.pages,country.roots(6),policy);
    auto residency = std::make_shared<TerrainResidency>();
    if (policy.targeted) residency->capacity[0] = 0;
    double time = 0;
    const auto view = viewAt(1,512,512,500);
    const auto arrive = [&](std::uint8_t level) {
        auto next = std::make_shared<TerrainResidency>(*residency);
        ++next->revision;
        for (auto key : available->pages) if (key.level == level) {
            auto surface = std::make_shared<SurfacePage>();
            surface->step = 4 << level; surface->padding = 2;
            surface->side = 512/surface->step+1+2*surface->padding;
            const auto count = std::size_t(surface->side)*surface->side;
            surface->bed.resize(count); surface->head.assign(count,0);
            for (int y = 0; y < surface->side; ++y) for (int x = 0; x < surface->side; ++x) {
                const double wx = key.x*512+(x-surface->padding)*surface->step;
                const double wy = key.y*512+(y-surface->padding)*surface->step;
                // H64 misses a narrow incision entirely; H16 and finer see it.
                const double incision = std::max(0.0,1.0-std::abs(wx-272)/16)*20;
                surface->bed[std::size_t(y)*surface->side+x] = float(100+0.125*wy-incision);
            }
            next->pages.insert(key); next->surfaces.emplace(key,std::move(surface));
        }
        residency = std::move(next);
    };
    const auto settle = [&] {
        auto plan = run(planner,view,residency,time += 0.1);
        for (int tick = 0; plan && plan->needsUpdate && tick < 100; ++tick) {
            for (const auto& block : plan->coverage) CHECK(bool(block.mesh));
            CHECK(plan->compatible(*residency));
            plan = run(planner,view,residency,time += 0.1);
        }
        return plan;
    };
    std::shared_ptr<const TerrainPlan> first;
    for (const std::uint8_t level : {4,2,1,0}) {
        arrive(level);
        const auto plan = settle();
        if (!plan) return;
        CHECK(!plan->coverage.empty());
        CHECK(!plan->needsUpdate);
        CHECK(plan->compatible(*residency));
        for (const auto& block : plan->coverage) CHECK(bool(block.mesh));
        if (policy.targeted) for (const auto key : plan->wanted) CHECK(key.level != 0);
        const bool complete = level == (policy.targeted ? 1 : 0);
        if (!complete) {
            CHECK(!plan->missing.empty());
            CHECK(plan->coarse > 0);
            CHECK(plan->viewMesh.percent() < 100);
        } else {
            CHECK_EQ(plan->coarse,std::size_t(0));
            CHECK_EQ(plan->viewPages.percent(),100);
            CHECK_EQ(plan->viewMesh.percent(),100);
            CHECK(!planner.request(view,residency,time+1));
        }
        if (!first) first = plan;
        if (complete) break;
    }
    // Later surface publication cannot alter an accepted H64 snapshot.
    CHECK(first->coarse > 0);
    CHECK(!first->missing.empty());
    CHECK_EQ(planner.stats().failed,std::size_t(0));
}

TEST(terrain_plan_real_surfaces_load_in_stages_without_null_meshes_or_false_completion) {
    checkStagedSurfaces(kRenderDataLodPolicy);
}

TEST(terrain_regional_staged_surfaces_never_report_false_completion_or_wait_for_h4) {
    checkStagedSurfaces(kRegionalDataLodPolicy);
}

TEST(terrain_stable_floodplain_survives_zoom_motion_and_h8_eviction) {
    Country country(2,2,2);
    RiverSegment river;
    river.id=1;
    river.width=core::Fixed::fromInt(24);river.depth=core::Fixed::fromInt(3);
    river.valleyReach=core::Fixed::fromInt(160);
    river.bounds={{core::Fixed::fromInt(244),core::Fixed::fromInt(-512)},
                  {core::Fixed::fromInt(268),core::Fixed::fromInt(1536)}};
    for (int y:{-512,1536}) river.course.push_back({
        {core::Fixed::fromInt(256),core::Fixed::fromInt(y)},core::Fixed::fromInt(30),
        core::Fixed::fromInt(12),core::Fixed::fromInt(3),core::Fixed::fromInt(160)});
    country.graph.segments.push_back(river);
    for (int y=-3;y<=4;++y) for (int x=-3;x<=4;++x) country.graph.spatialPages.push_back({{x,y,0},{1},{}});
    auto resident=country.resident();
    std::erase_if(resident->pages,[](auto k){return k.level<2;});
    resident->capacity={0,0,256};resident->dryPages=resident->pages; // legacy CPU H2 fixture, not runtime policy
    for (const auto key:resident->pages) {
        const auto p=country.pages->page(key);
        auto s=std::make_shared<SurfacePage>();
        s->step=p->base.sampleMetres;s->padding=p->base.padding;s->side=p->base.width+2*s->padding;
        const double low=p->base.elevationMin.toDouble(),range=(p->base.elevationMax-p->base.elevationMin).toDouble();
        for (std::size_t i=0;i<p->base.heightQuantized.size();++i) {
            s->bed.push_back(float(low+p->base.heightQuantized[i]*range/65535.0+
                (p->large.deltaQuantized.empty()?0:p->large.deltaQuantized[i]*0.01)));
            s->head.push_back(p->water.wet(i)?float(low+p->water.surfaceQuantized[i]*range/65535.0):0.0f);
        }
        resident->surfaces.emplace(key,std::move(s));
    }
    TerrainDetail detail(country.world,*country.pages);
    CHECK(detail.hasFeatures({0,0,6}));
    CHECK_EQ(detail.tolerance(256,256),0.125);
    CHECK(detail.tolerance(368,256)<=0.5); // a floodplain may itself contain a sharp bend
    TerrainPlanner planner(country.world,*country.pages,{{0,0,2}},kStableFeatureDataLodPolicy);
    double time=0;
    std::vector<float> expected;
    for (int pass=0;pass<4;++pass) {
        auto view=viewAt(pass%2?6:0,256+pass*8,256,pass%2?4096:240);
        auto plan=run(planner,view,resident,time+=0.1);
        for (int tick=0;plan && plan->needsUpdate && tick<80;++tick) plan=run(planner,view,resident,time+=0.1);
        if (!plan) return;
        CHECK(!plan->needsUpdate);
        CHECK_EQ(plan->viewMesh.percent(),100);
        CHECK(!plan->capacityLimited);
        CHECK(plan->missing.empty());
        const auto it=std::find_if(plan->coverage.begin(),plan->coverage.end(),[](const auto& b){return b.tile==world::TileId{0,0,1};});
        CHECK(it!=plan->coverage.end());
        if (it==plan->coverage.end() || !it->mesh) return;
        CHECK_EQ(it->mesh->step,2.0);CHECK_EQ(it->dataLevel,2);CHECK(it->mayHaveWater);
        std::vector<float> heights;
        for (int x=112;x<=400;x+=2) heights.push_back(it->mesh->sample(x,256)[0]);
        if (pass==0) expected=heights; else CHECK_EQ(heights,expected);
        for (const auto k:plan->pins) CHECK(k.level>=2);
        resident=std::make_shared<TerrainResidency>(*resident);
        ++resident->revision; // emulate unrelated streaming churn
    }
}

static std::shared_ptr<TerrainResidency> flatSurfaces(const Country& country) {
    auto resident = country.resident();
    for (const std::uint8_t level : {0,1,2,4}) {
        auto page = std::make_shared<SurfacePage>();
        page->step = 4 << level; page->padding = 2;
        page->side = 512/page->step + 1 + 2*page->padding;
        page->bed.assign(std::size_t(page->side)*page->side,100);
        page->head.assign(page->bed.size(),0);
        for (const auto key : resident->pages)
            if (key.level == level) resident->surfaces.emplace(key,page);
    }
    return resident;
}

TEST(terrain_camera_far_planes_keep_coarse_geometry_after_finer_source_probes) {
    // A plane sixteen kilometres away stays a plane, and now it stays one for
    // the right reason.
    //
    // This used to assert it with a mesh claiming a HUNDRED KILOMETRES of
    // error, which passed only because the runtime path threw the mesh's error
    // away and decided on screen size alone. Screen size alone is what drew an
    // ocean floor at the same density as a cordillera and filled the far view
    // with triangles smaller than a pixel. The error decides now, so the
    // fixture has to state an error a far plane would really have.
    AdaptiveMesh flat;
    flat.detailError = 0.01;
    const ViewBounds bounds{0,0,4096,4096,0,4000};
    const auto far = viewAt(6,4096,4096,16000);
    CHECK(!far.refineSurface(bounds,6,&flat,kRegionalDataLodPolicy));
    CHECK(!far.refineSurface(bounds,5,&flat,kRegionalDataLodPolicy));
    CHECK(!far.refineSurface(bounds,4,&flat,kRegionalDataLodPolicy));
    CHECK(!far.refineSurface(bounds,3,&flat,kRegionalDataLodPolicy));
    // And the converse, which is the point of the change: ground with real
    // relief in it at that same distance IS worth splitting, because a
    // kilometre of crest sixteen kilometres away is not a sub-pixel detail.
    AdaptiveMesh sharp;
    sharp.detailError = 1000;
    CHECK(far.refineSurface(bounds,6,&sharp,kRegionalDataLodPolicy));
    // All ancestors fit inside this fixture. Looking for hidden crests must
    // not leave a dense H64 mesh or cache on a plane after simplification.
    Country country(2,32,32);
    const auto resident = flatSurfaces(country);
    for (int lod : {5,6}) {
        TerrainPlanner planner(country.world,*country.pages,{{0,0,lod}},kRegionalDataLodPolicy);
        const auto view = viewAt(lod,4096,1024,8000);
        auto plan = run(planner,view,resident,0.1);
        for (int tick=2;plan && plan->needsUpdate && tick<100;++tick)
            plan=run(planner,view,resident,tick*0.1);
        if (!plan) return;
        CHECK(!plan->needsUpdate);
        CHECK_EQ(plan->viewMesh.percent(),100);
        CHECK(!plan->drawing.empty());
        for (const auto& block : plan->drawing) {
            CHECK(bool(block.mesh));
            if (!block.mesh) continue;
            // The chunk's extent is the contract; the lattice it is STORED on
            // is not. A plane keeps no probe rows it did not use, so the stored
            // step may be coarser than the nominal one - never finer, and never
            // covering anything other than exactly this chunk.
            CHECK(block.mesh->step>=double(4 << lod));
            CHECK(block.mesh->cells<=32);
            CHECK_EQ(block.mesh->step*block.mesh->cells,double(32*(4 << lod)));
            CHECK_EQ(block.metres(),std::int64_t(32*(4 << lod)));
            CHECK_EQ(block.dataLevel,std::uint8_t(4)); // H64 shading, coarse geometry
            CHECK(block.mesh->surfaceIndices<32u*32*6/3);
        }
    }
}

TEST(terrain_camera_subdivides_a_hidden_crest_but_not_its_flat_neighbour) {
    Country country;
    auto resident=country.resident();
    std::erase_if(resident->pages,[](auto key){return key.level!=4;});
    for (const auto key:resident->pages) {
        auto page=std::make_shared<SurfacePage>();
        page->step=64;page->padding=2;page->side=13;
        for (int y=0;y<page->side;++y) for (int x=0;x<page->side;++x) {
            const double wx=key.x*512+(x-page->padding)*64;
            page->bed.push_back(float(100+80*std::max(0.0,1-std::abs(wx-192)/64)));
            page->head.push_back(0);
        }
        resident->surfaces.emplace(key,std::move(page));
    }
    auto policy=kRegionalDataLodPolicy;policy.chunkMetres.fill(256);
    TerrainPlanner planner(country.world,*country.pages,{{0,0,6},{1,0,6}},policy);
    const auto view=viewAt(6,256,128,400);
    auto plan=run(planner,view,resident,0.1);
    for (int tick=2;plan && plan->needsUpdate && tick<40;++tick)
        plan=run(planner,view,resident,tick*0.1);
    CHECK(plan && !plan->needsUpdate);if (!plan) return;
    CHECK_EQ(plan->drawing.size(),std::size_t(2));
    const AdaptiveMesh* crest=nullptr;const AdaptiveMesh* flat=nullptr;
    for (const auto& block:plan->drawing) {
        CHECK_EQ(block.tile.lod,6);CHECK_EQ(block.dataLevel,4);
        CHECK(block.mesh);if (!block.mesh) continue;
        if (block.tile.x==0) crest=block.mesh.get();else flat=block.mesh.get();
    }
    CHECK(crest && flat);if (!crest || !flat) return;
    CHECK(crest->step>=8 && crest->step<256);
    CHECK(crest->cells<=256);
    CHECK(std::abs(crest->sample(192,128)[0]-180)<0.01);
    CHECK_EQ(flat->step,256.0);CHECK_EQ(flat->cells,1);CHECK_EQ(flat->surfaceIndices,6u);
    CHECK(crest->surfaceIndices>flat->surfaceIndices);
    for (const auto key:plan->wanted) CHECK_EQ(key.level,4); // no global fine-page preparation
    CHECK_EQ(plan->viewMesh.percent(),100);
    CHECK(!planner.request(view,resident,1)); // geometry is stable, not resampled each frame
}

TEST(terrain_camera_builds_bounded_batches_nearest_first_without_null_coverage) {
    Country country(2,16,4);
    const auto resident = flatSurfaces(country);
    // Reverse input order: camera priority, not insertion order, must win.
    const std::vector<world::TileId> roots{{3,0,3},{2,0,3},{1,0,3},{0,0,3}};
    TerrainPlanner planner(country.world,*country.pages,roots,kRegionalDataLodPolicy);
    const auto view = viewAt(2,512,512,8000);
    bool deferred = false, partial = false;
    std::shared_ptr<const TerrainPlan> first;
    std::shared_ptr<const TerrainPlan> plan;
    for (int tick=1;tick<=100;++tick) {
        plan=run(planner,view,resident,tick*0.1);
        if (!plan) return;
        CHECK(plan->meshesBuilt<=kMeshesPerPlan);
        CHECK(plan->preload.size()<=kCameraPreloadPages);
        CHECK(plan->compatible(*resident));
        deferred = deferred || plan->deferredRegions>0;
        partial = partial || (!plan->drawing.empty() && plan->needsUpdate);
        for (const auto& block : plan->coverage) CHECK(bool(block.mesh));
        if (!first && !plan->drawing.empty()) {
            first=plan;
            CHECK_EQ(plan->drawing.front().tile.x,0);
        }
        if (!plan->needsUpdate) break;
    }
    CHECK(deferred);
    CHECK(partial);
    CHECK(bool(first));
    CHECK(!plan->needsUpdate);
    CHECK_EQ(plan->viewMesh.percent(),100);
    CHECK(!planner.request(view,resident,100));
    if (first) CHECK(first->needsUpdate); // old partial snapshot stays immutable
}

TEST(terrain_camera_partial_batches_preserve_coverage_while_camera_moves) {
    Country country(2,16,4);
    const auto resident = flatSurfaces(country);
    TerrainPlanner planner(country.world,*country.pages,
        {{0,0,3},{1,0,3},{2,0,3},{3,0,3}},kRegionalDataLodPolicy);
    std::shared_ptr<const TerrainPlan> previous;
    bool sawCoverage = false;
    for (int tick=1;tick<=16;++tick) {
        const auto view = viewAt(2,256+tick*384,512,1200);
        const auto plan = run(planner,view,resident,tick*0.1);
        if (!plan) return;
        CHECK(plan->view == view);
        CHECK(plan->meshesBuilt<=kMeshesPerPlan);
        CHECK(plan->compatible(*resident));
        for (const auto& block : plan->coverage) CHECK(bool(block.mesh));
        if (previous) for (const auto& old : previous->coverage) {
            const double side = double(old.metres());
            for (const double u : {0.25,0.75}) for (const double v : {0.25,0.75}) {
                const double x = (old.tile.x+u)*side, y = (old.tile.y+v)*side;
                int covering = 0;
                for (const auto& block : plan->coverage) {
                    const double size = double(block.metres());
                    covering += x>=block.tile.x*size && x<(block.tile.x+1)*size &&
                                y>=block.tile.y*size && y<(block.tile.y+1)*size;
                }
                CHECK_EQ(covering,1); // no hole or overlapping parent/child
            }
        }
        sawCoverage = sawCoverage || !plan->coverage.empty();
        previous = plan;
    }
    CHECK(sawCoverage);
    CHECK_EQ(planner.stats().failed,std::size_t(0));
}

TEST(terrain_runtime_boundaries_are_earned_by_shape_rather_than_forced_everywhere) {
    // Forcing the whole source lattice onto every chunk boundary was most of
    // the landscape's geometry: a 32-cell chunk paid 4*32 mandatory edge
    // segments, 256 skirt triangles to close them, and the interior splits the
    // restricted quadtree needs to reach them - on a plane as much as on a
    // ridge. The runtime policy now asks for eight mandatory points a side.
    Country country(2,32,32);
    const auto resident = flatSurfaces(country);
    TerrainPlanner planner(country.world,*country.pages,{{1,1,2}},kRegionalDataLodPolicy);
    const double side = double(world::tileMetresAt(2,32));
    const auto view = viewAt(2,side*1.5,side*1.5,side*0.4);
    auto plan = run(planner,view,resident,0.1);
    for (int tick=2;plan && plan->needsUpdate && tick<40;++tick)
        plan = run(planner,view,resident,tick*0.1);
    if (!plan || plan->drawing.empty() || !plan->drawing.front().mesh) return;
    const auto& mesh = *plan->drawing.front().mesh;
    // Distinct positions, not vertex records: a corner is emitted once per
    // skirt/corner combination it is used in.
    std::set<std::pair<int,int>> edge;
    for (const auto& v : mesh.vertices)
        if (v.x==0 || v.y==0 || v.x==mesh.cells || v.y==mesh.cells) edge.emplace(v.x,v.y);
    // A closed perimeter has as many segments as it has points, and two skirt
    // triangles hold each one, so the walls fall with the mandatory lattice.
    CHECK(edge.size()<=std::size_t(4*8));
    CHECK_EQ(mesh.indices.size()-mesh.surfaceIndices,edge.size()*6);
    CHECK(mesh.surfaceIndices<std::size_t(4*32*6));
}
TEST(terrain_runtime_chunks_have_32_cells_at_every_geometry_step) {
    Country country(2,32,32);
    const auto resident = flatSurfaces(country);
    for (const auto policy : {kRegionalDataLodPolicy,kCameraFeatureDataLodPolicy}) {
        CHECK_EQ(policy.chunkCells,32);
        for (int lod=1;lod<=6;++lod) { // runtime geometry starts at H8, never H4
            const world::TileId tile{1,1,lod};
            const double side = double(world::tileMetresAt(lod,policy.chunkCells));
            TerrainPlanner planner(country.world,*country.pages,{tile},policy);
            const auto view=viewAt(lod,side*1.5,side*1.5,side*0.4);
            auto plan=run(planner,view,resident,0.1);
            for (int tick=2;plan && plan->needsUpdate && tick<40;++tick)
                plan=run(planner,view,resident,tick*0.1);
            if (!plan) return;
            CHECK(!plan->needsUpdate);
            CHECK_EQ(plan->drawing.size(),std::size_t(1));
            if (plan->drawing.empty()) continue;
            const auto& block=plan->drawing.front();
            CHECK(block.tile==tile);
            CHECK(bool(block.mesh));
            if (!block.mesh) continue;
            // 32 cells is the chunk policy, not a floor on how coarsely a
            // flat chunk may be stored once its unused probe rows are dropped.
            CHECK(block.mesh->cells<=32);
            CHECK(block.mesh->step>=double(4 << lod));
            CHECK_EQ(block.metres(),std::int64_t(32*(4 << lod)));
            CHECK_EQ(block.chunkCells,32);
            CHECK_EQ(block.bounds.minX,side);
            CHECK_EQ(block.bounds.maxX,side*2);
            CHECK_EQ(block.mesh->step*block.mesh->cells,side);
        }
    }
}

TEST(terrain_chunk_family_morphs_when_its_four_meshes_are_ready) {
    Country country(2,8,8);
    const auto resident=flatSurfaces(country);
    TerrainPlanner planner(country.world,*country.pages,{{0,0,2},{4,0,2}},kRegionalDataLodPolicy);
    // First publish only 512 m parents, then ask for 256 m / 8 m children.
    const auto coarseView=viewAt(2,128,128,4000);
    double time=0;
    auto plan=run(planner,coarseView,resident,time+=0.1);
    for (int tick=0;plan && plan->needsUpdate && tick<40;++tick)
        plan=run(planner,coarseView,resident,time+=0.1);
    if (!plan) return;
    const auto fineView=viewAt(1,128,128,4000);
    plan=run(planner,fineView,resident,time+=0.1);
    if (!plan) return;
    int nearChildren=0,farParents=0;
    for (const auto& block:plan->coverage) {
        CHECK(bool(block.mesh));
        if (block.tile.lod==1 && block.tile.x<2) {
            ++nearChildren;
            CHECK(block.parentMorph>0 && block.parentMorph<1);
            CHECK_EQ(block.metres(),std::int64_t(256));
            CHECK(block.mesh->step>=8.0); // flat: stored coarser, still 256 m wide
            CHECK_EQ(block.mesh->step*block.mesh->cells,256.0);
        }
        farParents+=block.tile==world::TileId{4,0,2};
    }
    CHECK_EQ(nearChildren,4);
    CHECK_EQ(farParents,1); // the distant family's build cannot delay this morph
    CHECK(plan->needsUpdate);
    CHECK_EQ(plan->meshesBuilt,std::size_t(4));
}

TEST(terrain_configured_mesh_batches_progress_and_change_without_mutating_old_plans) {
    Country country(2,16,4);
    const auto resident = flatSurfaces(country);
    for (const std::size_t budget : {1u,2u,8u}) {
        TerrainPlanner planner(country.world,*country.pages,
            {{3,0,3},{2,0,3},{1,0,3},{0,0,3}},kRegionalDataLodPolicy);
        auto view = viewAt(2,512,512,8000);
        view.config.meshesPerPlan = budget;
        std::shared_ptr<const TerrainPlan> first, plan;
        for (int tick=1;tick<=100;++tick) {
            // Exercise a live budget change while a family is only half built.
            if (budget == 2 && tick == 3) view.config.meshesPerPlan = 8;
            plan = run(planner,view,resident,tick*0.1);
            if (!plan) return;
            if (!first) first = plan;
            CHECK(plan->meshesBuilt <= view.config.meshesPerPlan);
            CHECK(plan->compatible(*resident));
            for (const auto& block : plan->coverage) CHECK(bool(block.mesh));
            if (!plan->needsUpdate) break;
        }
        CHECK(!plan->needsUpdate);
        CHECK_EQ(plan->viewMesh.percent(),100);
        CHECK_EQ(first->view.config.meshesPerPlan,budget);
        CHECK_EQ(planner.stats().failed,std::size_t(0));
    }
}

TEST(terrain_configured_preload_limit_including_zero) {
    Country country(2,32,4);
    auto residency = std::make_shared<TerrainResidency>();
    std::vector<world::TileId> roots;
    for (int x=0;x<8;++x) roots.push_back({x,0,3});
    TerrainPlanner planner(country.world,*country.pages,roots,kRegionalDataLodPolicy);
    // Root H16/H64 pages are demanded even offscreen. Predict finer H8 pages,
    // not those same mandatory parent pages, to exercise speculative admission.
    auto view = viewAt(1,512,512,300);
    view.lookX = 5000;
    view.prediction = viewAt(1,5000,512,300).matrix;
    double time = 0;
    for (const std::size_t budget : {0u,1u,7u,0u}) {
        view.config.preloadPages = budget;
        const auto plan = run(planner,view,residency,time+=0.1);
        if (!plan) return;
        CHECK_EQ(plan->preload.size(),budget);
    }
}

TEST(terrain_configured_chunk_cells_reach_mesh_and_world_bounds) {
    Country country(2,32,32);
    const auto resident = flatSurfaces(country);
    for (const int cells : {16,64}) {
        auto policy = kRegionalDataLodPolicy;
        policy.chunkCells = cells;
        const double side = double(world::tileMetresAt(3,cells));
        TerrainPlanner planner(country.world,*country.pages,{{0,0,3}},policy);
        auto view = viewAt(3,side/2,side/2,side*0.4);
        view.config.chunkCells = cells;
        auto plan = run(planner,view,resident,0.1);
        for (int tick=2;plan && plan->needsUpdate && tick<40;++tick)
            plan = run(planner,view,resident,tick*0.1);
        if (!plan) return;
        CHECK(!plan->needsUpdate);
        CHECK_EQ(plan->drawing.size(),std::size_t(1));
        if (plan->drawing.empty() || !plan->drawing.front().mesh) return;
        const auto& block = plan->drawing.front();
        CHECK_EQ(block.chunkCells,cells);
        CHECK(block.mesh->cells<=cells);
        CHECK_EQ(block.mesh->step*block.mesh->cells,side);
        CHECK_EQ(block.metres(),std::int64_t(side));
        CHECK_EQ(block.bounds.maxX,side);
        CHECK_EQ(block.bounds.maxY,side);
    }
}


TEST(terrain_camera_spends_a_bounded_number_of_triangles_on_every_square) {
    // The ladder, stated as a number so it cannot quietly stop being true.
    //
    // A square may hold at most eight cells a side - a hundred and twenty-eight
    // triangles - at EVERY level. Detail is bought by drawing more and smaller
    // squares, which is the decision the level was chosen to make; it is never
    // bought by letting one square subdivide without limit.
    //
    // It could not hold before. The lattice inside a square had a floor of
    // eight metres whatever the square's own step was, so a square drawn at two
    // hundred and fifty-six metres to a triangle was still built on the same
    // eight-metre grid as one underfoot and could spend two thousand triangles
    // describing ground that had been chosen to be drawn coarsely. Raising the
    // level ceiling above it changed which number was printed and not one
    // triangle of what was drawn.
    Country country(2, 8, 8, true);
    auto resident = country.resident();
    int levelsSeen = 0;
    for (const int lod : {2, 4, 6}) {
        TerrainPlanner planner(country.world, *country.pages, country.roots(lod),
                               kRegionalDataLodPolicy);
        const auto view = viewAt(lod, 1024, 1024, 2000);
        auto plan = run(planner, view, resident, 0.1);
        for (int tick = 2; plan && plan->needsUpdate && tick < 60; ++tick)
            plan = run(planner, view, resident, tick * 0.1);
        if (!plan || plan->drawing.empty()) continue;
        ++levelsSeen;
        std::size_t worst = 0;
        for (const auto& block : plan->drawing) {
            if (!block.mesh) continue;
            worst = std::max(worst, std::size_t(block.mesh->surfaceIndices / 3));
            // And the lattice itself: eight cells a side, unless the square is
            // already at the finest step the runtime draws, where the cap is
            // the step and not the count.
            CHECK(block.mesh->cells <= 8 || block.mesh->step <= 8);
        }
        CHECK(worst <= 128);
    }
    CHECK(levelsSeen > 0);
}

// Walking evicts H8 pages from the small atlas all the time - the ones the eye
// left behind. That used to clear the whole region's cut (a world root is
// 65 km), so the square under the pawn fell from 8 m triangles to a 1 km slab
// at once and came back through every level over the next few seconds: "the
// chunk under my feet is rebuilt as I walk". Losing pages now folds back only
// the subtree that stood on them, to its nearest drawable ancestor.
TEST(terrain_plan_lost_pages_fold_back_only_their_own_subtree) {
    TerrainConfig config;
    std::string error;
    const auto file = std::filesystem::path(__FILE__).parent_path().parent_path() / "content/config/terrain.json";
    CHECK(readTerrainConfig(file, config, error));
    config.preloadPages = 0;
    auto policy = kRegionalDataLodPolicy;
    policy.chunkCells = config.chunkCells;
    policy.chunkMetres = config.chunkMetres;
    Country country(2, 16, 16);
    const auto height = [](double x, double y) {
        return 100 + 18 * std::sin(x / 41.0) * std::cos(y / 57.0) + 6 * std::sin(x / 13.0 + y / 9.0);
    };
    auto resident = std::make_shared<TerrainResidency>();
    resident->revision = 1;
    const auto addPage = [&](TileKey key) {
        if (resident->surfaces.contains(key)) return false;
        auto page = std::make_shared<SurfacePage>();
        page->step = 4 << key.level; page->padding = 2;
        const int metres = pageMetresAtLevel(key.level);
        page->side = metres / page->step + 1 + 2 * page->padding;
        for (int y = 0; y < page->side; ++y) for (int x = 0; x < page->side; ++x) {
            page->bed.push_back(float(height(double(key.x) * metres + (x - page->padding) * page->step,
                                             double(key.y) * metres + (y - page->padding) * page->step)));
            page->head.push_back(0);
        }
        resident->pages.insert(key);
        resident->surfaces.emplace(key, std::move(page));
        return true;
    };
    TerrainPlanner planner(country.world, *country.pages, {{0, 0, int(kGeometryLevels) - 1}}, policy);
    const double px = 2000, py = 2000;
    TerrainView view;
    view.config = config;
    view.width = 1920; view.height = 1080;
    {
        const double pitch = 14 * std::numbers::pi / 180, ex = px - 4, ey = py, ez = height(px, py) + 2.2;
        const double f[3]{std::cos(pitch), 0, -std::sin(pitch)}, r[3]{0, -1, 0}, u[3]{std::sin(pitch), 0, std::cos(pitch)};
        const double focal = 1.7320508, sx = focal * 1080.0 / 1920.0;
        const auto at = [&](const double* a) { return a[0] * ex + a[1] * ey + a[2] * ez; };
        view.matrix = {float(sx * r[0]), float(sx * r[1]), float(sx * r[2]), float(-sx * at(r)),
                       float(focal * u[0]), float(focal * u[1]), float(focal * u[2]), float(-focal * at(u)),
                       0, 0, 0, 0.5f,
                       float(f[0]), float(f[1]), float(f[2]), float(-at(f))};
        view.prediction = view.matrix;
        view.x = view.lookX = view.windowX = ex;
        view.y = view.lookY = view.windowY = ey;
        view.radius = 50000;
    }
    double time = 0;
    std::shared_ptr<const TerrainPlan> plan;
    const auto replan = [&] {
        for (int attempt = 0; attempt < 50; ++attempt) {
            time += 0.1;
            if (!planner.request(view, resident, time)) return;
            const auto next = collect(planner);
            if (!next) return;
            plan = next;
            // A new residency object per change, as GpuTerrain publishes one:
            // the planner diffs against the previous object it was given.
            resident = std::make_shared<TerrainResidency>(*resident);
            bool added = false;
            for (const auto key : plan->missing) added = addPage(key) || added;
            if (!added) return;
            ++resident->revision;
        }
    };
    for (int n = 0; n < 200 && (!plan || plan->needsUpdate || !plan->missing.empty()); ++n) replan();
    if (!plan) return;
    const auto under = [&]() -> const TerrainPlan::Block* {
        for (const auto& b : plan->coverage)
            if (px >= b.bounds.minX && px < b.bounds.maxX && py >= b.bounds.minY && py < b.bounds.maxY) return &b;
        return nullptr;
    };
    CHECK(under() != nullptr);
    if (!under()) return;
    const int settled = under()->tile.lod;
    CHECK_EQ(settled, 1);                     // H8 underfoot, the finest runtime geometry
    // (a) The atlas lets go of H8 far from the eye, as it does on every walk.
    resident = std::make_shared<TerrainResidency>(*resident);
    std::size_t evicted = 0;
    for (auto it = resident->pages.begin(); it != resident->pages.end();) {
        const auto key = *it;
        const double cx = (key.x + 0.5) * pageMetresAtLevel(key.level), cy = (key.y + 0.5) * pageMetresAtLevel(key.level);
        if (key.level == 1 && std::hypot(cx - px, cy - py) > 1200) {
            resident->surfaces.erase(key);
            it = resident->pages.erase(it);
            ++evicted;
        } else ++it;
    }
    CHECK(evicted > 0);
    ++resident->revision;
    // Do not re-add what was evicted: the stream has not brought it back yet.
    for (int n = 0; n < 8; ++n) {
        time += 0.1;
        if (!planner.request(view, resident, time)) break;
        plan = collect(planner);
        if (!plan) return;
        const auto* b = under();
        CHECK(b != nullptr);
        if (!b) return;
        CHECK_EQ(b->tile.lod, settled);       // never back up to the root
        CHECK(plan->compatible(*resident));
    }
    // (b) The page under the eye itself goes: only this square folds back, to
    // its parent - which here has the same footprint and reads H16.
    const TileKey own{int(std::floor(px / pageMetresAtLevel(1))), int(std::floor(py / pageMetresAtLevel(1))), 1};
    CHECK(resident->pages.contains(own));
    resident = std::make_shared<TerrainResidency>(*resident);
    resident->surfaces.erase(own);
    resident->pages.erase(own);
    ++resident->revision;
    time += 0.1;
    if (!planner.request(view, resident, time)) { CHECK(false); return; }
    plan = collect(planner);
    if (!plan) return;
    const auto* b = under();
    CHECK(b != nullptr);
    if (!b) return;
    CHECK(b->tile.lod > settled);
    CHECK(b->tile.lod <= settled + 2);
    CHECK(plan->compatible(*resident));
    // Every point in front of the eye is still covered exactly once.
    for (double x = px - 300; x <= px + 1500; x += 97)
        for (double y = py - 600; y <= py + 600; y += 113) {
            int count = 0;
            for (const auto& block : plan->coverage)
                count += x >= block.bounds.minX && x < block.bounds.maxX && y >= block.bounds.minY && y < block.bounds.maxY;
            CHECK_EQ(count, 1);
        }
}

// A diagnostic, not a regression test - it returns at once unless asked:
//
//     ASR_UNDERFOOT_PROBE=1 [ASR_UNDERFOOT_SPEED=1.4] asr_terrain_tests terrain_probe_walk_underfoot
//
// A third-person camera walks east over rolling ground with the runtime policy
// and terrain.json, planning the way GpuTerrain does: the next plan is asked
// for only once the last one's display transition (morph_seconds) is over.
// Every plan reports what happened to the square under the pawn - its tile,
// its built mesh, its stitched copy, its morph - and how far the ground near
// the pawn visibly moves when the plan's transition plays.
TEST(terrain_probe_walk_underfoot) {
    if (!std::getenv("ASR_UNDERFOOT_PROBE")) return;
    const double speed = std::getenv("ASR_UNDERFOOT_SPEED") ? std::atof(std::getenv("ASR_UNDERFOOT_SPEED")) : 1.4;
    const double walk = std::getenv("ASR_UNDERFOOT_METRES") ? std::atof(std::getenv("ASR_UNDERFOOT_METRES")) : 300;
    TerrainConfig config;
    std::string error;
    const auto file = std::filesystem::path(__FILE__).parent_path().parent_path() / "content/config/terrain.json";
    CHECK(readTerrainConfig(file, config, error));
    auto policy = kRegionalDataLodPolicy;
    policy.chunkCells = config.chunkCells;
    policy.chunkMetres = config.chunkMetres;
    Country country(2, 16, 16);
    const auto height = [](double x, double y) {
        return 100 + 18 * std::sin(x / 41.0) * std::cos(y / 57.0) + 6 * std::sin(x / 13.0 + y / 9.0) + 0.002 * x;
    };
    auto resident = std::make_shared<TerrainResidency>();
    resident->revision = 1;
    const auto addPage = [&](TileKey key) {
        if (resident->surfaces.contains(key)) return false;
        auto page = std::make_shared<SurfacePage>();
        page->step = 4 << key.level; page->padding = 2;
        const int metres = pageMetresAtLevel(key.level);
        page->side = metres / page->step + 1 + 2 * page->padding;
        for (int y = 0; y < page->side; ++y) for (int x = 0; x < page->side; ++x) {
            const double wx = double(key.x) * metres + (x - page->padding) * page->step;
            const double wy = double(key.y) * metres + (y - page->padding) * page->step;
            page->bed.push_back(float(height(wx, wy)));
            page->head.push_back(0);
        }
        resident->pages.insert(key);
        resident->surfaces.emplace(key, std::move(page));
        return true;
    };
    TerrainPlanner planner(country.world, *country.pages, {{0, 0, int(kGeometryLevels) - 1}}, policy);
    const auto cameraAt = [&](double px, double py) {
        // Third person: four metres behind the pawn, 2.2 m over its ground, 14 degrees down.
        TerrainView view;
        view.config = config;
        view.width = 1920; view.height = 1080;
        const double pitch = 14 * std::numbers::pi / 180, ex = px - 4, ey = py, ez = height(px, py) + 2.2;
        const double f[3]{std::cos(pitch), 0, -std::sin(pitch)}, r[3]{0, -1, 0}, u[3]{std::sin(pitch), 0, std::cos(pitch)};
        const double focal = 1.7320508, aspect = 1920.0 / 1080.0, nearPlane = 0.5;
        const auto at = [&](const double* a) { return a[0] * ex + a[1] * ey + a[2] * ez; };
        const double sx = focal / aspect;
        view.matrix = {float(sx * r[0]), float(sx * r[1]), float(sx * r[2]), float(-sx * at(r)),
                       float(focal * u[0]), float(focal * u[1]), float(focal * u[2]), float(-focal * at(u)),
                       0, 0, 0, float(nearPlane),
                       float(f[0]), float(f[1]), float(f[2]), float(-at(f))};
        view.prediction = view.matrix;
        view.x = view.lookX = view.windowX = ex;
        view.y = view.lookY = view.windowY = ey;
        view.radius = 50000;
        return view;
    };
    double time = 0;
    std::shared_ptr<const TerrainPlan> plan;
    const auto step = [&](const TerrainView& view) {
        for (int attempt = 0; attempt < 50; ++attempt) {
            if (!planner.request(view, resident, time)) return plan;
            const auto next = collect(planner);
            if (!next) return plan;
            plan = next;
            // A new residency object per change, as GpuTerrain publishes one:
            // the planner diffs against the previous object it was given.
            resident = std::make_shared<TerrainResidency>(*resident);
            bool added = false;
            for (const auto key : plan->missing) added = addPage(key) || added;
            if (!added) return plan;
            ++resident->revision;
            time += 0.05;
        }
        return plan;
    };
    // Settle at the start: every page there, every mesh built, no morph left.
    double px = 2000, py = 2000;
    for (int n = 0; n < 200; ++n) {
        step(cameraAt(px, py));
        time += 0.1;
        if (plan && !plan->needsUpdate && !plan->interpolated) break;
    }
    if (!plan) return;
    const auto under = [&](const TerrainPlan& p) -> const TerrainPlan::Block* {
        for (const auto& b : p.coverage)
            if (px >= b.bounds.minX && px < b.bounds.maxX && py >= b.bounds.minY && py < b.bounds.maxY) return &b;
        return nullptr;
    };
    const auto base = [](const TerrainPlan::Block& b) {
        return b.mesh && b.mesh->unstitched ? b.mesh->unstitched.get() : b.mesh.get();
    };
    // How far the ground within `reach` of the pawn moves while this plan's
    // display transition plays: |displayFrom - what the new mesh shows|.
    const auto jump = [&](const TerrainPlan& p, double reach) {
        double worst = 0;
        for (const auto& b : p.coverage) {
            if (!b.mesh || b.bounds.maxX < px - reach || b.bounds.minX > px + reach ||
                b.bounds.maxY < py - reach || b.bounds.minY > py + reach) continue;
            const auto& m = *b.mesh;
            for (const auto& v : m.vertices) {
                if (!(v.skirt & 8)) continue;
                const double x = b.tile.x * double(b.metres()) + v.x * m.step;
                const double y = b.tile.y * double(b.metres()) + v.y * m.step;
                if (std::hypot(x - px, y - py) > reach) continue;
                const auto i = std::size_t(v.y) * (m.cells + 1) + v.x;
                const double shown = (v.skirt & 2) ? v.edgeBed : std::lerp(m.bed[i], v.parentBed, b.parentMorph);
                worst = std::max(worst, std::abs(v.displayFrom[0] - shown));
            }
        }
        return worst;
    };
    std::printf("walk %.1f m/s over %.0f m; start under foot: lod %d tile %d,%d step %.0f m\n", speed, walk,
                under(*plan) ? under(*plan)->tile.lod : -1, under(*plan) ? under(*plan)->tile.x : 0,
                under(*plan) ? under(*plan)->tile.y : 0, under(*plan) && under(*plan)->mesh ? under(*plan)->mesh->step : 0.0);
    struct Count { int plans = 0, tile = 0, built = 0, stitched = 0, morph = 0, visible = 0; } count;
    // Every block whose mesh object differs from the last plan's for the same
    // tile is a new vertex + index buffer on the card. Why it is new:
    struct Copies {
        std::size_t blocks = 0, uploads = 0, newBase = 0, restitched = 0, interpolated = 0, noop = 0, bytes = 0;
    } copies;
    TerrainPlan::CopyCounts why;
    const auto shown = [](const TerrainPlan::Block& b, const AdaptiveVertex& v) {
        const auto& m = *b.mesh;
        const auto i = std::size_t(v.y) * (m.cells + 1) + v.x;
        return (v.skirt & 2) ? double(v.edgeBed) : std::lerp(double(m.bed[i]), double(v.parentBed), double(b.parentMorph));
    };
    const double startX = px;
    auto previous = plan;
    while (px - startX < walk) {
        // GpuTerrain asks again only after the last transition has played.
        const double dt = previous->interpolated ? std::max(0.1, config.morphSeconds) : 0.1;
        px += speed * dt;
        time += dt;
        step(cameraAt(px, py));
        if (!plan || plan == previous) continue;
        ++count.plans;
        why.stitchCopies += plan->copies.stitchCopies; why.stitchReused += plan->copies.stitchReused;
        why.displayMorphOnly += plan->copies.displayMorphOnly; why.displayRestitched += plan->copies.displayRestitched;
        why.displayNewBase += plan->copies.displayNewBase; why.displayNoChange += plan->copies.displayNoChange;
        {
            std::unordered_map<std::int64_t, const TerrainPlan::Block*> before;
            for (const auto& b : previous->coverage) before.emplace(world::tileKeyOf(b.tile), &b);
            for (const auto& b : plan->coverage) {
                if (!b.mesh) continue;
                ++copies.blocks;
                const auto it = before.find(world::tileKeyOf(b.tile));
                if (it != before.end() && it->second->mesh == b.mesh) continue;
                ++copies.uploads;
                copies.bytes += b.mesh->vertices.size() * sizeof(AdaptiveVertex) + b.mesh->indices.size() * 4;
                const bool interpolating = std::any_of(b.mesh->vertices.begin(), b.mesh->vertices.end(),
                                                       [](const auto& v) { return (v.skirt & 8) != 0; });
                if (it == before.end() || !it->second->mesh || base(*it->second) != base(b)) { ++copies.newBase; continue; }
                (interpolating ? copies.interpolated : copies.restitched) += 1;
                // Same base mesh: does any vertex show a different height at all?
                const auto& o = *it->second;
                bool same = o.mesh->vertices.size() == b.mesh->vertices.size() && o.parentMorph == b.parentMorph;
                for (std::size_t i = 0; same && i < b.mesh->vertices.size(); ++i)
                    same = std::abs(shown(o, o.mesh->vertices[i]) - shown(b, b.mesh->vertices[i])) < 1e-4;
                copies.noop += same;
            }
        }
        const auto* a = under(*previous);
        const auto* b = under(*plan);
        if (!a || !b) { std::printf("  x=%.1f no block under foot\n", px - startX); previous = plan; continue; }
        const bool tile = !(a->tile == b->tile);
        const bool built = !tile && base(*a) != base(*b);
        const bool stitched = !tile && !built && a->mesh != b->mesh;
        const bool morph = !tile && a->parentMorph != b->parentMorph;
        const double near = jump(*plan, 30), far = jump(*plan, 128);
        count.tile += tile; count.built += built; count.stitched += stitched; count.morph += morph;
        count.visible += near > 0.02;
        if (tile || built || stitched || morph || near > 0.02)
            std::printf("  x=%6.1f %s%s%s%s lod %d->%d morph %.2f->%.2f step %.0f->%.0f  ground moves %.3f m within 30 m, %.3f within 128 m  meshes-built %zu\n",
                        px - startX, tile ? "TILE " : "", built ? "REBUILT " : "", stitched ? "RESTITCHED " : "",
                        morph ? "MORPH " : "", a->tile.lod, b->tile.lod, a->parentMorph, b->parentMorph,
                        a->mesh ? a->mesh->step : 0.0, b->mesh ? b->mesh->step : 0.0, near, far, plan->meshesBuilt);
        previous = plan;
    }
    std::printf("plans %d: under foot tile changed %d, base mesh rebuilt %d, restitched only %d, morph changed %d; "
                "ground near the pawn visibly moved in %d\n",
                count.plans, count.tile, count.built, count.stitched, count.morph, count.visible);
    std::printf("mesh objects: %zu blocks drawn over %d plans, %zu new on the card (%.1f per plan, %.1f KB per plan): "
                "new base %zu, restitched copy %zu, display-transition copy %zu; copies showing no height change %zu; "
                "AdaptiveVertex %zu bytes\n",
                copies.blocks, count.plans, copies.uploads, double(copies.uploads) / std::max(1, count.plans),
                double(copies.bytes) / 1024.0 / std::max(1, count.plans), copies.newBase, copies.restitched,
                copies.interpolated, copies.noop, sizeof(AdaptiveVertex));
    std::printf("planner side: seam copies %zu (identical seams reused %zu); display copies: morph only %zu, "
                "re-seamed %zu, new base %zu; display copies with no visible change %zu\n",
                why.stitchCopies, why.stitchReused, why.displayMorphOnly, why.displayRestitched, why.displayNewBase,
                why.displayNoChange);
}
