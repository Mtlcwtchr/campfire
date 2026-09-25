#include "framework.hpp"

#include <cmath>
#include <limits>
#include <chrono>
#include <thread>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_height_pages.hpp"
#include "game/world/terrain_progress.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"

using namespace world::terrain;
using world::streaming::BaseTile;

namespace {
BaseTile pageFor(const HeightPageLayout& layout) {
    BaseTile page;
    page.key = {-1, 2, layout.level()};
    page.sampleMetres = layout.sampleMetres();
    page.width = page.height = world::streaming::interiorSamples(page.sampleMetres);
    page.padding = world::streaming::kDefaultPaddingSamples;
    page.elevationMin = core::Fixed::fromInt(-200);
    page.elevationMax = core::Fixed::fromInt(1800);
    page.heightQuantized.resize(page.sampleCount());
    return page;
}
}

TEST(terrain_progress_distinguishes_unknown_empty_and_incomplete_work) {
    PreparationProgress p;
    CHECK_EQ(p.percent(), -1);
    p.known = true;
    CHECK_EQ(p.percent(), 100);
    p.total = 10;
    CHECK_EQ(p.percent(), 0);
    p.ready = 5;
    CHECK_EQ(p.percent(), 50);
    p.ready = 9.999;
    CHECK_EQ(p.percent(), 99);
    p.ready = 10;
    CHECK_EQ(p.percent(), 100);
    p.ready = 100;
    CHECK_EQ(p.percent(), 100);
    p.ready = -2;
    CHECK_EQ(p.percent(), 0);
}

TEST(terrain_progress_counts_target_tiles_not_changing_cut_nodes) {
    CHECK_EQ(detailContribution(4, 0, 0), 0.0); // coarse stand-in is not detail
    CHECK_EQ(detailContribution(0, 1, 0), 0.0);
    CHECK_EQ(detailContribution(0, 0.5f, 0), 0.5);
    CHECK_EQ(detailContribution(0, 0, 0), 1.0);
    CHECK_EQ(detailContribution(0, 1, 1) * 4, 1.0); // coarsening still meets LOD1
    CHECK_EQ(detailContribution(0, 0, 2) * 16, 1.0);
    PreparationProgress current{2, 4, true};
    CHECK_EQ(current.percent(), 50);
    current = {0, 8, true}; // a new camera view must not inherit old progress
    CHECK_EQ(current.percent(), 0);
}

TEST(terrain_progress_counts_only_published_pages_as_permanent_residency) {
    generation::WorldMapData world;
    world.width = world.height = 1;
    world.cells.resize(1);
    world.cells[0].elevation = 100;
    const world::streaming::HydrologyGraph graph;
    world::streaming::PageStore store(world, graph,
        {core::Fixed::fromInt(-200), core::Fixed::fromInt(1800)},
        {128u << 20, world::streaming::kDefaultPaddingSamples, 1});
    store.prebakeInBackground({4});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (store.prebakeProgress().running && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto p = store.prebakeProgress();
    CHECK(!p.running);
    CHECK(p.total > 0);
    CHECK_EQ(p.done, p.total);
    CHECK_EQ(p.failed, std::size_t(0));
    CHECK_EQ(p.published, p.total);
    CHECK(store.pinnedBytes() > 0);
    store.clear(); // dropping runtime cache must not drop pinned progress
    CHECK_EQ(store.prebakeProgress().published, p.total);
    CHECK(bool(store.resident({0, 0, 4})));
}

TEST(terrain_height_pages_store_four_datasets_with_their_halo) {
    for (const int step : {4, 8, 16, 64}) {
        HeightPageLayout layout(step, 3, 2);
        CHECK_EQ(layout.storedSamples(), std::uint32_t(512 / step + 5));
        CHECK_EQ(layout.capacity(), std::size_t(6));
        CHECK_EQ(layout.bytes(), std::size_t(layout.storedSamples()) *
                                  layout.storedSamples() * 6 * sizeof(std::uint16_t));
        CHECK(layout.accepts(pageFor(layout)));
    }
    for (const int step : {0, 2, 32, 128, 256}) {
        bool rejected = false;
        try { HeightPageLayout layout(step, 1, 1); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
}

TEST(terrain_height_pages_address_texel_centres_and_keep_the_boundary_inside_the_slot) {
    for (const int step : {4, 8, 16, 64}) {
        HeightPageLayout layout(step, 3, 2);
        const BaseTile page = pageFor(layout);
        for (std::size_t slot = 0; slot < layout.capacity(); ++slot) {
            const auto address = layout.address(slot, page);
            CHECK_EQ(address.x, (slot % 3) * layout.storedSamples());
            CHECK_EQ(address.y, (slot / 3) * layout.storedSamples());
            // Include the two-sample halo on both sides, not just the interior.
            for (int sample = -2; sample <= 512 / step + 2; ++sample) {
                const float metres = float(sample * step);
                const float x = (address.uvOrigin[0] + metres * address.uvPerMetre[0]) * layout.width();
                const float y = (address.uvOrigin[1] + metres * address.uvPerMetre[1]) * layout.height();
                CHECK(std::abs(x - (float(address.x) + sample + 2.5f)) < 0.0001f);
                CHECK(std::abs(y - (float(address.y) + sample + 2.5f)) < 0.0001f);
                CHECK(x > address.x && x < address.x + layout.storedSamples());
                CHECK(y > address.y && y < address.y + layout.storedSamples());
            }
        }
    }
}

TEST(terrain_height_pages_reject_mismatched_or_incomplete_page_data) {
    HeightPageLayout layout(16, 2, 2);
    const BaseTile original = pageFor(layout);
    BaseTile page = original;
    page.key.level = 3;
    CHECK(!layout.accepts(page));
    page = original;
    page.sampleMetres = 4;
    CHECK(!layout.accepts(page));
    page = original;
    ++page.padding;
    CHECK(!layout.accepts(page));
    page = original;
    --page.width;
    CHECK(!layout.accepts(page));
    page = original;
    page.heightQuantized.pop_back();
    CHECK(!layout.accepts(page));
    page = original;
    page.elevationMax = page.elevationMin;
    CHECK(!layout.accepts(page));
}

TEST(terrain_height_pages_validate_dimensions_and_slot_bounds) {
    for (const auto dims : {std::array<std::uint32_t, 2>{0, 1}, {1, 0},
                            {std::numeric_limits<std::uint32_t>::max(), 1}}) {
        bool rejected = false;
        try { HeightPageLayout layout(4, dims[0], dims[1]); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
    }
    HeightPageLayout layout(64, 1, 1);
    bool rejected = false;
    try { (void)layout.address(1, pageFor(layout)); }
    catch (const std::out_of_range&) { rejected = true; }
    CHECK(rejected);
}

TEST(terrain_height_pages_decode_the_original_quantisation_without_another_round_trip) {
    HeightPageLayout layout(4, 1, 1);
    const BaseTile page = pageFor(layout);
    const auto address = layout.address(0, page);
    const world::streaming::HsimQuantisation quantisation{page.elevationMin, page.elevationMax};
    for (const std::uint16_t stored : {0, 1, 32767, 32768, 65534, 65535}) {
        const float decoded = address.heightLow + (float(stored) / 65535.0f) * address.heightRange;
        CHECK(std::abs(decoded - quantisation.dequantise(stored).toDouble()) < 0.0003);
    }
}

TEST(terrain_height_pages_preserve_every_coordinate_bit_in_a_dataset) {
    CHECK(heightPageKey({-1, 0, 0}) != heightPageKey({0, -1, 0}));
    CHECK(heightPageKey({0, 0, 0}) != heightPageKey({1 << 24, 0, 0}));
    CHECK(heightPageKey({std::numeric_limits<std::int32_t>::min(), 0, 0}) !=
          heightPageKey({std::numeric_limits<std::int32_t>::max(), 0, 0}));
    CHECK_EQ(heightPageKey({-1, -1, 0}), std::int64_t(-1));
    // Same coordinates at another data level live in another atlas, never in
    // an XOR-ed slot key: the layout rejects the wrong level before lookup.
    CHECK_EQ(heightPageKey({3, 7, 0}), heightPageKey({3, 7, 4}));
}
