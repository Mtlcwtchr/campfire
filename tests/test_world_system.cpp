#include "framework.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <stdexcept>
#include <optional>
#include "engine/core/diagnostics.hpp"
#include "game/world/world_system.hpp"

namespace {
generation::WorldMapData ocean(std::uint64_t seed) {
    generation::WorldMapData map;
    map.width = map.height = 4; map.seed = seed;
    map.cells.resize(16);
    for (auto& cell : map.cells) { cell.sea = true; cell.elevation = -20; }
    return map;
}
world::streaming::PageStore::Config config() {
    world::streaming::PageStore::Config c;
    c.workerCount = 1; // no disk cache; tiny, deterministic test worlds
    return c;
}
template<class F> bool await(F predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
}

TEST(publication_latest_only_cancellation_and_foreign_tickets) {
    engine::Publication<int> p, other;
    const auto old = p.request();
    CHECK(p.publish(old, std::make_shared<const int>(10)));
    const auto lease = p.read();
    const auto stale = p.request(), latest = p.request();
    CHECK(!p.publish(stale, std::make_shared<const int>(20)));
    CHECK(!other.publish(latest, std::make_shared<const int>(20)));
    CHECK(p.publish(latest, std::make_shared<const int>(30)));
    CHECK(!p.current(latest));
    CHECK(!p.publish(latest, std::make_shared<const int>(40)));
    const auto cancelled = p.request(); p.cancel();
    CHECK(!p.publish(cancelled, std::make_shared<const int>(50)));
    CHECK_EQ(*lease.value, 10);
    CHECK_EQ(*p.read().value, 30);
    CHECK_EQ(p.read().version, latest.version());
}

TEST(publication_tickets_do_not_revive_when_owner_storage_is_reused) {
    std::optional<engine::Publication<int>> owner(std::in_place);
    const auto* address = &*owner;
    const auto stale = owner->request();
    owner.reset(); owner.emplace();
    CHECK(&*owner == address);
    const auto fresh = owner->request();
    CHECK_EQ(stale.version(), fresh.version());
    CHECK(!owner->current(stale));
    CHECK(!owner->publish(stale, std::make_shared<const int>(1)));
    CHECK(owner->publish(fresh, std::make_shared<const int>(2)));
}

TEST(publication_concurrent_readers_see_coherent_versions) {
    engine::Publication<std::uint64_t> p;
    std::atomic<bool> done = false, coherent = true;
    std::thread reader([&] {
        while (!done.load()) {
            const auto snapshot = p.read();
            if (snapshot.value && *snapshot.value != snapshot.version) coherent = false;
        }
    });
    for (int i = 0; i < 200; ++i) {
        const auto ticket = p.request();
        CHECK(p.publish(ticket, std::make_shared<const std::uint64_t>(ticket.version())));
    }
    done = true; reader.join();
    CHECK(coherent.load());
}

TEST(world_builder_rejects_stale_work_and_preserves_old_leases) {
    world::WorldBuilder builder(config());
    builder.publish(ocean(11));
    const auto old = builder.read();
    const auto stale = builder.request(), newest = builder.request();
    CHECK(!builder.complete(stale, ocean(22)));
    CHECK(builder.complete(newest, ocean(33)));
    CHECK(!builder.complete(newest, {})); // duplicate rejected before invalid-map construction
    const auto cancelled = builder.request(); builder.cancel();
    CHECK(!builder.complete(cancelled, ocean(44)));
    CHECK_EQ(old->worldMap().seed, std::uint64_t(11));
    CHECK_EQ(builder.read()->worldMap().seed, std::uint64_t(33));
    CHECK_EQ(builder.read()->version(), newest.version());
    CHECK(old->version() < builder.read()->version());
    const auto working = builder.read();
    auto broken = ocean(55); broken.cells.pop_back();
    bool rejected = false;
    try { builder.publish(std::move(broken)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(builder.read() == working);
}

TEST(world_system_channels_survive_republication_and_owner_destruction) {
    std::shared_ptr<world::WorldPreparation> retained;
    std::weak_ptr<const world::WorldSnapshot> old;
    {
        world::WorldSystem system(config()); system.publish(ocean(1));
        old = system.read(); retained = system.prepare(system.read());
        auto independent = system.prepare(system.read());
        CHECK(retained != independent);
        CHECK(!retained->surface.collect());
        CHECK(!retained->surface.busy());
        CHECK(!retained->surface.request({}, {}, 0));
        independent->surface.configure({});
        auto residency = std::make_shared<world::terrain::TerrainResidency>();
        world::terrain::TerrainView view;
        CHECK(retained->surface.request(view, residency, 0));
        view.x = 500;
        CHECK(retained->surface.request(view, residency, 1));
        CHECK(await([&] { return bool(retained->surface.collect()); }));
        const auto plan = retained->surface.read();
        CHECK_EQ(plan->view.x, 500.0);
        CHECK(!independent->surface.read());
        system.publish(ocean(2));
        CHECK(!system.prepare(retained->world));
        CHECK_EQ(retained->world->worldMap().seed, std::uint64_t(1));
        CHECK_EQ(plan->view.x, 500.0);
    }
    CHECK(!old.expired());
    retained.reset();
    CHECK(old.expired());
}

TEST(world_surface_uses_the_full_geometry_ladder_for_regional_roots) {
    auto map = ocean(11);
    for (auto& cell : map.cells) { cell.sea = false; cell.elevation = 40; }
    world::WorldSystem system(config());
    system.publish(std::move(map));
    auto channel = system.prepare(system.read());
    world::terrain::TerrainView view;
    view.target = int(world::terrain::kGeometryLevels) - 1;
    for (std::size_t lod = 0; lod < view.config.chunkMetres.size(); ++lod)
        view.config.chunkMetres[lod] = std::max(256, 4 << lod);
    view.config.morphSeconds = 0;
    view.config.preloadPages = 0;
    view.width = view.height = 800;
    view.x = view.y = view.lookX = view.lookY = 2048;
    view.radius = 4096;
    view.matrix = {1.0f/4096, 0, 0, -0.5f, 0, 1.0f/4096, 0, -0.5f,
                   0, 0, 1, 0, 0, 0, 0, 1};
    view.prediction = view.matrix;
    auto residency = std::make_shared<world::terrain::TerrainResidency>();
    residency->revision = 1;
    for (int y = -2; y < 7; ++y) for (int x = -2; x < 7; ++x) {
        const world::streaming::TileKey key{x, y, 4};
        if (!system.read()->pages().containsLand(key)) continue;
        auto surface = std::make_shared<world::terrain::SurfacePage>();
        surface->step = 64; surface->padding = 2; surface->side = 13;
        surface->bed.assign(169, 360); surface->head.assign(169, 0);
        residency->pages.insert(key); residency->surfaces.emplace(key, surface);
    }
    CHECK(channel->surface.request(view, residency, 0));
    CHECK(await([&] { return bool(channel->surface.collect()); }));
    const auto plan = channel->surface.read();
    CHECK(plan != nullptr);
    if (!plan) return;
    CHECK_EQ(plan->coverage.size(), std::size_t(1));
    CHECK_EQ(plan->drawing.size(), std::size_t(1));
    CHECK_EQ(plan->blank, std::size_t(0));
    for (const auto& block : plan->coverage) {
        CHECK_EQ(block.tile.lod, view.target);
        CHECK(block.mesh != nullptr);
    }
}

TEST(world_surface_builds_visible_coverage_before_nearby_detail) {
    auto map = ocean(11);
    map.width = 16; map.cells.resize(16 * 4);
    for (auto& cell : map.cells) { cell.sea = false; cell.elevation = 40; }
    world::WorldSystem system(config()); system.publish(std::move(map));
    auto channel = system.prepare(system.read());
    world::terrain::TerrainView view;
    for (std::size_t lod = 0; lod < view.config.chunkMetres.size(); ++lod)
        view.config.chunkMetres[lod] = std::max(256, 4 << lod);
    view.config.meshesPerPlan = 3;
    view.config.morphSeconds = 0; view.config.preloadPages = 0;
    view.target = 0; view.width = view.height = 800;
    view.x = view.lookX = 1024; view.y = view.lookY = 1024; view.radius = 16384;
    view.matrix = {1.0f/16384, 0, 0, 0, 0, 1.0f/4096, 0, 0,
                   0, 0, 1, 0, 0, 0, 0, 1};
    view.prediction = view.matrix;
    auto residency = std::make_shared<world::terrain::TerrainResidency>();
    residency->revision = 1;
    for (int y = -2; y < 7; ++y) for (int x = -2; x < 19; ++x) {
        const world::streaming::TileKey key{x, y, 4};
        if (!system.read()->pages().containsLand(key)) continue;
        auto surface = std::make_shared<world::terrain::SurfacePage>();
        surface->step = 64; surface->padding = 2; surface->side = 13;
        surface->bed.assign(169, 360); surface->head.assign(169, 0);
        residency->pages.insert(key); residency->surfaces.emplace(key, surface);
    }
    CHECK(channel->surface.request(view, residency, 0));
    CHECK(await([&] { return bool(channel->surface.collect()); }));
    const auto plan = channel->surface.read();
    CHECK(plan != nullptr);
    if (!plan) return;
    CHECK_EQ(plan->blank, std::size_t(0));
    CHECK_EQ(plan->coverage.size(), std::size_t(3));
    CHECK_EQ(plan->meshesBuilt, std::size_t(3));
    CHECK(plan->needsUpdate); // fine detail remains, but no visible holes
    for (const auto& block : plan->coverage) CHECK(block.mesh != nullptr);
}

TEST(scene_placement_publishes_latest_region_without_renderer_callbacks) {
    world::WorldSystem system(config()); system.publish(ocean(7));
    auto channel = system.prepare(system.read());
    auto& placement = channel->placement;
    const world::decor::ScatterBounds first{-128, -128, 128, 128};
    const world::decor::ScatterBounds latest{1024, -256, 1536, 128};
    const world::decor::ScatterBounds pending{1152, -256, 1664, 128};
    const world::decor::ScatterBounds cancelled{1280, -256, 1792, 128};
    placement.update(first, true);
    placement.update(latest, true);
    CHECK(await([&] {
        placement.update(latest, true);
        const auto result = placement.read();
        return result && result->bounds == latest;
    }));
    const auto old = placement.read();
    CHECK_EQ(old->worldVersion, system.read()->version());
    placement.update(pending, true);
    placement.update(cancelled, false); // cancels a running detail request
    CHECK(await([&] { placement.update(cancelled, false); return !placement.busy(); }));
    CHECK(placement.read() == old);
    CHECK(placement.error().empty());
}

TEST(scene_placement_resize_at_the_same_centre_publishes_once) {
    world::WorldSystem system(config()); system.publish(ocean(7));
    auto channel = system.prepare(system.read());
    auto& placement = channel->placement;
    const world::decor::ScatterBounds first{0, 0, 256, 256};
    const world::decor::ScatterBounds resized{-128, 64, 384, 192};
    const auto settle = [&](world::decor::ScatterBounds bounds) {
        return await([&] {
            placement.update(bounds, true);
            const auto result = placement.read();
            return result && result->bounds == bounds && !placement.busy();
        });
    };
    CHECK(settle(first));
    const auto old = placement.read();
    CHECK(settle(resized));
    const auto current = placement.read();
    CHECK(current != old);
    CHECK(current->version > old->version);
    CHECK(old->bounds == first);
    for (int i = 0; i < 3; ++i) placement.update(resized, true);
    CHECK(placement.read() == current);
    CHECK(!placement.busy());
    CHECK(placement.error().empty());
}

TEST(scene_placement_recovers_after_an_invalid_bounds_request) {
    world::WorldSystem system(config()); system.publish(ocean(7));
    auto channel = system.prepare(system.read());
    auto& placement = channel->placement;
    CHECK(await([&] {
        placement.update({}, true);
        return !placement.error().empty();
    }));
    CHECK(!placement.read());
    const world::decor::ScatterBounds valid{0, 0, 128, 256};
    CHECK(await([&] {
        placement.update(valid, true);
        return bool(placement.read());
    }));
    CHECK(placement.read()->bounds == valid);
    CHECK(placement.error().empty());
    CHECK(!placement.busy());
}

TEST(scene_placement_streams_all_visible_regions_beyond_one_scatter_budget) {
    world::WorldSystem system(config());system.publish(ocean(7));
    auto channel=system.prepare(system.read());
    auto& placement=channel->placement;
    std::vector<world::decor::ScatterBounds> regions;
    for (int y=0;y<18;++y) for (int x=0;x<18;++x)
        regions.push_back({x*128,y*128,(x+1)*128,(y+1)*128});
    CHECK(regions.size()*256>world::decor::kMaxScatterCells);
    bool partial=false;
    CHECK(await([&] {
        placement.updateRegions(regions,true);
        const auto snapshot=placement.read();
        if (snapshot && !snapshot->complete) {
            partial=true;
            CHECK(!snapshot->regions.empty());
            CHECK(snapshot->regions.size()<regions.size());
        }
        return snapshot && snapshot->complete && !placement.busy();
    }));
    CHECK(partial);
    const auto complete=placement.read();
    CHECK_EQ(complete->regions.size(),regions.size());
    CHECK(placement.error().empty());
    std::reverse(regions.begin(),regions.end()); // priority changes are not new ownership
    placement.updateRegions(regions,true);
    CHECK(placement.read()==complete);
    CHECK(!placement.busy());
}

TEST(scene_placement_camera_motion_retains_completed_regions_and_immutable_leases) {
    world::WorldSystem system(config());system.publish(ocean(7));
    auto channel=system.prepare(system.read());
    auto& placement=channel->placement;
    const world::decor::ScatterBounds anchor{0,0,128,128};
    std::vector<world::decor::ScatterBounds> regions{anchor};
    for (int i=1;i<40;++i) regions.push_back({i*128,0,(i+1)*128,128});
    placement.updateRegions(regions,true);
    // Move the far boundary while the anchor's batch is in flight. The old
    // ticket implementation discarded all results each time demand changed.
    int step=0;
    CHECK(await([&] {
        ++step;
        regions.back()={128*(40+step),0,128*(41+step),128};
        placement.updateRegions(regions,true);
        const auto snapshot=placement.read();
        return snapshot && std::find(snapshot->regions.begin(),snapshot->regions.end(),anchor)!=snapshot->regions.end();
    }));
    const auto retained=placement.read();
    const auto oldRegions=retained->regions;
    CHECK(await([&] {
        placement.updateRegions(regions,true);
        return placement.read()->complete && !placement.busy();
    }));
    CHECK_EQ(retained->regions,oldRegions);
    const auto complete=placement.read();
    placement.updateRegions(regions,false);
    CHECK(!placement.busy());
    CHECK(placement.read()==complete);
    placement.updateRegions(regions,true);
    CHECK(!placement.busy()); // cached, no resampling after re-enable
    CHECK(placement.read()->complete);
    placement.updateRegions({},true);
    CHECK(placement.read()->regions.empty());
    CHECK(placement.read()->scatter.objects.empty());
    CHECK(placement.read()->complete);
}

TEST(scene_placement_region_members_equal_a_monolithic_scatter_without_duplicates) {
    auto map=ocean(91);
    for (auto& cell:map.cells) { cell.sea=false;cell.elevation=40; }
    world::WorldSystem system(config());system.publish(std::move(map));
    auto channel=system.prepare(system.read());
    auto& placement=channel->placement;
    const std::vector<world::decor::ScatterBounds> regions{{512,512,640,640},{640,512,768,640},
        {512,640,640,768},{640,640,768,768},{512,512,640,640}}; // duplicate demand is harmless
    CHECK(await([&] {
        placement.updateRegions(regions,true);
        return placement.read() && placement.read()->complete && !placement.busy();
    }));
    auto expected=system.read()->scatter({512,512,768,768}).objects;
    std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) { return a.id<b.id; });
    CHECK(!expected.empty());
    CHECK_EQ(placement.read()->regions.size(),std::size_t(4));
    CHECK_EQ(placement.read()->scatter.objects,expected);
}

TEST(scene_placement_parallel_jobs_match_a_monolithic_scatter_while_priority_changes) {
    CHECK(world::ScenePlacement::workerLimit()>=1);
    CHECK(world::ScenePlacement::workerLimit()<=4);
    auto map=ocean(23);
    for (auto& cell:map.cells) { cell.sea=false;cell.elevation=40; }
    world::WorldSystem system(config());system.publish(std::move(map));
    auto channel=system.prepare(system.read());
    auto& placement=channel->placement;
    std::vector<world::decor::ScatterBounds> regions;
    for (int y=0;y<12;++y) for (int x=0;x<12;++x)
        regions.push_back({x*128,y*128,(x+1)*128,(y+1)*128});
    // More regions than one job carries, so several run at once.
    CHECK(regions.size()>8);
    int turn=0;
    CHECK(await([&] {
        // Re-prioritising every poll must not restart, duplicate or lose work.
        std::rotate(regions.begin(),regions.begin()+(++turn%regions.size()),regions.end());
        placement.updateRegions(regions,true);
        const auto snapshot=placement.read();
        return snapshot && snapshot->complete && !placement.busy();
    }));
    const auto snapshot=placement.read();
    CHECK_EQ(snapshot->regions.size(),regions.size());
    CHECK(placement.error().empty());
    auto expected=system.read()->scatter({0,0,12*128,12*128}).objects;
    std::sort(expected.begin(),expected.end(),[](const auto& a,const auto& b) { return a.id<b.id; });
    CHECK(!expected.empty());
    CHECK_EQ(snapshot->scatter.objects,expected);
    std::size_t population=0;
    for (const auto count:snapshot->scatter.populations) population+=count;
    CHECK_EQ(population,expected.size());
}

TEST(diagnostics_off_does_not_evaluate_arguments) {
    int calls = 0;
    ASR_DIAGNOSTIC(++calls);
#if ASR_ENABLE_DIAGNOSTICS
    CHECK_EQ(calls, 1);
#else
    ASR_DIAGNOSTIC(this_symbol_does_not_exist());
    CHECK_EQ(calls, 0);
#endif
}
