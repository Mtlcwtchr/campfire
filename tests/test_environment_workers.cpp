#include "framework.hpp"
#include "game/client/explorer.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <set>
#include <thread>

namespace {
using namespace std::chrono_literals;
const generation::WorldMapData& inspectionCountry() {
    static const auto map = [] {
        generation::WorldMapParams params;
        params.seed = 42;
        params.width = params.height = 32;
        params.erosionPasses = 1;
        return generation::generateWorldMap(params);
    }();
    return map;
}
template<class Predicate> bool waitFor(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + 20s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}
std::shared_ptr<world::InspectionSnapshot> snapshot(std::uint64_t revision, world::MapView mode) {
    auto state = std::make_shared<world::InspectionSnapshot>();
    state->revision = revision;
    state->mode = mode;
    return state;
}
std::vector<world::InspectionOrder> inspectionOrders(int count = 12) {
    std::vector<world::InspectionOrder> orders;
    for (int i = 0; i < count; ++i) {
        auto positions = std::make_shared<std::vector<core::WorldPos>>();
        for (int j = 0; j < 128; ++j)
            positions->push_back({core::Fixed::fromInt(i*64+j%16*4),core::Fixed::fromInt(j/16*4)});
        orders.push_back({(std::uint64_t(1)<<32) + i + 1, positions, double(i)});
    }
    return orders;
}
}

TEST(inspection_workers_share_pool_parallelize_deduplicate_and_bound_delivery) {
    client::PatchWorkshop workshop(inspectionCountry(),42);
    workshop.debugFreeze(true);
    workshop.debugArtificialDelay(60ms);
    const auto state = snapshot(1,world::MapView::Wind);
    auto orders = inspectionOrders();
    auto repeated = orders;
    repeated.insert(repeated.end(),orders.begin(),orders.end());
    workshop.inspectionWants(state,repeated);
    CHECK_EQ(workshop.inspectionStats().queued,orders.size());
    CHECK_EQ(workshop.inspectionStats().workers,workshop.workerStats().workers);
    CHECK(workshop.collectInspection().empty());
    workshop.debugFreeze(false);
    CHECK(waitFor([&] { return workshop.inspectionStats().ready == 8; }));
    CHECK_EQ(workshop.inspectionStats().busy,0u);
    std::set<std::uint64_t> completed;
    world::HeightField reference(&inspectionCountry(),42);
    CHECK(waitFor([&] {
        const auto ready = workshop.collectInspection();
        CHECK(ready.size() <= 2);
        for (const auto& result : ready) {
            CHECK_EQ(result.revision,1u);
            CHECK(completed.insert(result.key).second);
            const auto order = std::find_if(orders.begin(),orders.end(),[&](const auto& o) { return o.key==result.key; });
            CHECK(order!=orders.end());
            CHECK_EQ(result.samples.size(),order->positions->size());
            const auto expected = world::sampleInspection(reference,*order->positions,*state);
            for (std::size_t i=0;i<expected.size();++i) {
                CHECK_EQ(result.samples[i].windX,expected[i].windX);
                CHECK_EQ(result.samples[i].windY,expected[i].windY);
            }
        }
        std::vector<world::InspectionOrder> remaining;
        for (const auto& order : orders) if (!completed.contains(order.key)) remaining.push_back(order);
        workshop.inspectionWants(state,std::move(remaining));
        CHECK(workshop.inspectionStats().ready+workshop.inspectionStats().busy<=8);
        return completed.size()==orders.size();
    }));
    CHECK(workshop.inspectionStats().peakBusy>=std::min<std::size_t>(2,workshop.inspectionStats().workers));
    std::cout << "  inspection pool: " << workshop.inspectionStats().workers
              << ", peak parallel: " << workshop.inspectionStats().peakBusy << '\n';
}

TEST(inspection_workers_snapshot_soil_and_drop_obsolete_revisions_and_rings) {
    client::PatchWorkshop workshop(inspectionCountry(),42);
    workshop.debugArtificialDelay(5s);
    auto old = snapshot(1,world::MapView::Travel);
    workshop.inspectionWants(old,inspectionOrders(4));
    CHECK(waitFor([&] { return workshop.inspectionStats().busy>0; }));
    world::SoilState soil;
    const core::WorldPos origin{};
    soil.treat(origin,64,world::SoilState::Treatment::Harvest,core::Fixed::ratio(1,5));
    auto current = snapshot(2,world::MapView::Fertility);
    current->soil = std::make_shared<const world::SoilState>(soil);
    const auto expected = static_cast<float>(soil.deltaAt(origin).toDouble());
    soil.clear(); // Workers must not see subsequent UI mutations.
    workshop.debugArtificialDelay(0ms);
    const auto orders = inspectionOrders(1);
    workshop.inspectionWants(current,orders);
    std::vector<world::InspectionResult> received;
    CHECK(waitFor([&] { received=workshop.collectInspection(); return !received.empty(); }));
    CHECK_EQ(received.size(),1u);
    CHECK_EQ(received[0].revision,2u);
    CHECK_EQ(received[0].samples[0].soil,expected);
    workshop.debugArtificialDelay(5s);
    workshop.inspectionWants(snapshot(3,world::MapView::Wind),inspectionOrders(4));
    CHECK(waitFor([&] { return workshop.inspectionStats().busy>0; }));
    // Abandon this view, without relying on the patch or its positions staying alive.
    workshop.inspectionWants(snapshot(3,world::MapView::Wind),{});
    CHECK(waitFor([&] { return workshop.inspectionStats().busy==0; }));
    CHECK(workshop.collectInspection().empty());
}

TEST(inspection_workers_flood_is_one_connected_job_then_shared_ring_samples) {
    client::PatchWorkshop workshop(inspectionCountry(),42);
    auto state = snapshot(1,world::MapView::Flood);
    state->floodRequest = world::FloodRequest{{},512};
    workshop.inspectionWants(state,{{0,{},0},{0,{},0}});
    std::vector<world::InspectionResult> done;
    CHECK(waitFor([&] { done=workshop.collectInspection(); return !done.empty(); }));
    CHECK_EQ(done.size(),1u);
    CHECK_EQ(done[0].key,0u);
    CHECK(done[0].flood!=nullptr);
    world::HeightField reference(&inspectionCountry(),42);
    world::FloodPreview expected;
    CHECK(expected.build(reference,{},512));
    CHECK_EQ(done[0].flood->bounds(),expected.bounds());
    auto samples = snapshot(2,world::MapView::Flood);
    samples->flood = done[0].flood;
    const auto orders = inspectionOrders(2);
    workshop.inspectionWants(samples,orders);
    std::size_t received=0;
    CHECK(waitFor([&] {
        for (const auto& result : workshop.collectInspection()) {
            const auto order=std::find_if(orders.begin(),orders.end(),[&](const auto& o) { return o.key==result.key; });
            CHECK(order!=orders.end());
            for (std::size_t i=0;i<result.samples.size();++i)
                CHECK_EQ(result.samples[i].flood,expected.at((*order->positions)[i]));
            ++received;
        }
        return received==orders.size();
    }));
}

TEST(inspection_workers_shutdown_cancels_delay_and_field_loops) {
    const auto start=std::chrono::steady_clock::now();
    {
        client::PatchWorkshop workshop(inspectionCountry(),42);
        workshop.debugArtificialDelay(5s);
        workshop.inspectionWants(snapshot(1,world::MapView::Wind),inspectionOrders());
        CHECK(waitFor([&] { return workshop.inspectionStats().busy>0; }));
    }
    CHECK(std::chrono::steady_clock::now()-start<2s);
    world::HeightField field(&inspectionCountry(),42);
    world::FloodPreview flood;
    int probes=0;
    CHECK(!flood.build(field,{},512,[&] { return ++probes>=2; }));
    CHECK_EQ(flood.at({}),-1.0f);
    auto order=inspectionOrders(1).front();
    CHECK(world::sampleInspection(field,*order.positions,*snapshot(1,world::MapView::Travel),[] { return true; }).empty());
}

TEST(inspection_workers_restart_rejects_old_world_and_owns_position_lifetimes) {
    client::Explorer explorer(inspectionCountry(),42);
    const auto version=explorer.worldRevision();
    explorer.setStreamingDebugDelay(5s);
    explorer.inspectionWants(snapshot(1,world::MapView::Wind),inspectionOrders(4));
    CHECK(waitFor([&] { return explorer.inspectionStats().busy>0; }));
    explorer.restart(inspectionCountry(),43);
    CHECK(explorer.worldRevision()>version);
    CHECK(explorer.collectInspection().empty());
    const auto state=snapshot(1,world::MapView::Wind); // Reuse keys/revisions in a NEW pool.
    explorer.inspectionWants(state,inspectionOrders(1)); // No caller retains the input positions.
    std::vector<world::InspectionResult> done;
    CHECK(waitFor([&] { done=explorer.collectInspection(); return !done.empty(); }));
    CHECK_EQ(done.size(),1u);
    CHECK_EQ(done[0].samples.size(),128u);
    world::HeightField reference(&inspectionCountry(),43);
    const auto expected=reference.windAt({});
    CHECK_EQ(done[0].samples[0].windX,static_cast<float>(expected[0].toDouble()));
    CHECK_EQ(done[0].samples[0].windY,static_cast<float>(expected[1].toDouble()));
}

