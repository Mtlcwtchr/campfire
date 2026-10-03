#include <vector>
#include <stdexcept>
#include <optional>
#include <algorithm>
#include "engine/render/geometry/range_pool.hpp"
#include "framework.hpp"

#include <cmath>

#include "game/generation/world_map_gen.hpp"
#include "game/generation/terrain_foundation.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/world/terrain_lod.hpp"
#include "game/world/terrain_residency.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_view.hpp"

using namespace world::terrain;

TEST(terrain_grid_is_one_compact_reusable_topology) {
    const GridTopology grid = makeGridTopology();
    CHECK_EQ(grid.cells, std::uint16_t(64));
    CHECK_EQ(grid.surfaceVertexCount(), std::size_t(65 * 65));
    CHECK_EQ(grid.vertices.size(), std::size_t(65 * 65 + 4 * 65));
    CHECK_EQ(grid.triangleCount(), std::size_t(64 * 64 * 2 + 4 * 64 * 2));
    CHECK(grid.bytes() < 90u * 1024u);
    for (const std::uint16_t index : grid.indices) CHECK(index < grid.vertices.size());
    for (std::size_t i = grid.surfaceVertexCount(); i < grid.vertices.size(); ++i)
        CHECK_EQ(grid.vertices[i].skirt, std::uint16_t(1));
}

TEST(terrain_grid_surface_diagonal_and_skirts_have_consistent_winding) {
    const auto grid = makeGridTopology(4);
    int twiceArea = 0;
    for (std::size_t i = 0; i < grid.indices.size(); i += 3) {
        const auto a = grid.vertices[grid.indices[i]];
        const auto b = grid.vertices[grid.indices[i + 1]];
        const auto c = grid.vertices[grid.indices[i + 2]];
        const int ux = int(b.column) - a.column, uy = int(b.row) - a.row;
        const int uz = int(a.skirt) - b.skirt;
        const int vx = int(c.column) - a.column, vy = int(c.row) - a.row;
        const int vz = int(a.skirt) - c.skirt;
        const int nx = uy * vz - uz * vy, ny = uz * vx - ux * vz;
        const int nz = ux * vy - uy * vx;
        if (i < 4 * 4 * 6) {
            CHECK_EQ(nz, 1); // positive area: no flipped or degenerate triangle
            twiceArea += nz;
            // Every cell shares the NE--SW diagonal used by parentTriangle().
            const auto first = (i / 6 / 4) * 5 + (i / 6 % 4);
            const auto offset = i % 6;
            if (offset == 0) {
                CHECK_EQ(grid.indices[i + 1], first + 1);
                CHECK_EQ(grid.indices[i + 2], first + 5);
            } else {
                CHECK_EQ(grid.indices[i], first + 1);
                CHECK_EQ(grid.indices[i + 2], first + 5);
            }
        } else {
            CHECK_EQ(nz, 0);
            CHECK((a.row == 0 && b.row == 0 && c.row == 0 && ny < 0) ||
                  (a.row == 4 && b.row == 4 && c.row == 4 && ny > 0) ||
                  (a.column == 0 && b.column == 0 && c.column == 0 && nx < 0) ||
                  (a.column == 4 && b.column == 4 && c.column == 4 && nx > 0));
        }
    }
    CHECK_EQ(twiceArea, 2 * 4 * 4);
}

TEST(terrain_focus_priority_measures_distance_to_footprint_not_tile_origin) {
    CHECK_EQ(focusDistanceSquared({0, 0, 1024, 1024}, 900, 900), 0.0);
    CHECK_EQ(focusDistanceSquared({1024, 0, 2048, 1024}, 900, 900), 124.0 * 124.0);
    CHECK_EQ(focusDistanceSquared({-1024, -1024, 0, 0}, -500, -500), 0.0);
    CHECK(focusDistanceSquared({0, 0, 512, 512}, 900, 900) >
          focusDistanceSquared({512, 512, 1024, 1024}, 900, 900));
}

TEST(terrain_skirt_covers_independent_coastal_morphs_larger_than_two_grid_steps) {
    const double low = -120, high = 80;
    CHECK(high - 2 * 4 > low); // the old 8 m skirt leaves a gap
    for (int a = 0; a <= 10; ++a)
        for (int b = 0; b <= 10; ++b) {
            const double left = std::lerp(low, high, a / 10.0);
            const double right = std::lerp(high, low, b / 10.0);
            CHECK(skirtFloor(low) < std::min(left, right));
        }
    CHECK(skirtFloor(100) < -60); // also seals against the missing ocean page
}

TEST(terrain_lod_uses_real_h8_and_geometry_only_coarser_transitions) {
    CHECK(validGeometryStep(4));
    CHECK(validGeometryStep(8));
    CHECK(validGeometryStep(16));
    CHECK(validGeometryStep(32));
    CHECK(validGeometryStep(64));
    CHECK(validGeometryStep(128));
    CHECK(validGeometryStep(256));
    CHECK(!validGeometryStep(2));
    CHECK(!validGeometryStep(48));

    CHECK_EQ(dataStepForGeometry(4), 4);
    CHECK_EQ(dataStepForGeometry(8), 8);
    CHECK_EQ(dataStepForGeometry(16), 16);
    CHECK_EQ(dataStepForGeometry(32), 16);
    CHECK_EQ(dataStepForGeometry(64), 64);
    CHECK_EQ(dataStepForGeometry(128), 64);
    CHECK_EQ(dataStepForGeometry(256), 64);
    CHECK_EQ(dataLevelForGeometryLevel(0), std::uint8_t(0));
    CHECK_EQ(dataLevelForGeometryLevel(1), std::uint8_t(1));
    CHECK_EQ(dataLevelForGeometryLevel(2), std::uint8_t(2));
    CHECK_EQ(dataLevelForGeometryLevel(3), std::uint8_t(2));
    CHECK_EQ(dataLevelForGeometryLevel(4), std::uint8_t(4));
    CHECK_EQ(dataLevelForGeometryLevel(6), std::uint8_t(4));
}

TEST(terrain_lod_targets_pixels_and_obeys_geometric_error) {
    const LodPolicy policy{1.75, 2.5, 1.0}; // a custom high-density profile
    // Ground with shape in it, which is what the edge-length ladder is for: an
    // error of nine tenths of a pixel is over the bar that says "nobody can see
    // this" and under the one that forces a split, so the step is decided by
    // the target edge length alone. Passing a dead-flat nought here instead -
    // which these used to - measures the OTHER rule.
    const auto seen = [&](double metresPerPixel) { return metresPerPixel * 0.9; };
    CHECK_EQ(geometryStepFor(64.0, seen(64.0), policy), 64);
    CHECK_EQ(geometryStepFor(128.0, seen(128.0), policy), 128);
    CHECK_EQ(geometryStepFor(256.0, seen(256.0), policy), 256);
    CHECK_EQ(geometryStepFor(1.0, seen(1.0), policy), 4);
    CHECK_EQ(geometryStepFor(5.0, seen(5.0), policy), 8);
    CHECK_EQ(dataStepForGeometry(geometryStepFor(5.0, seen(5.0), policy)), 8);
    CHECK(!shouldRefine(8, 5.0, seen(5.0), policy));
    CHECK(shouldRefine(8, 2.0, seen(2.0), policy));

    CHECK(shouldRefine(64, 16.0, seen(16.0), policy));
    CHECK(!shouldCoarsen(64, 16.0, seen(16.0), policy));
    CHECK(shouldCoarsen(64, 128.0, 0.0, policy));
    CHECK(!shouldCoarsen(64, 128.0, 100.0, policy));
    CHECK_EQ(geometryStepFor(128.0, 200.0, policy), 64);
}

TEST(terrain_lod_stops_paying_for_triangles_on_ground_with_no_shape_in_it) {
    // The other rule, and the reason a featureless basin came out denser than
    // the range beside it.
    //
    // The target edge length is a guess at where detail will be wanted, and on
    // flat ground it is simply wrong: a plain whose deviation is centimetres
    // gains nothing from a triangle every ninety-six pixels. It used to get
    // them anyway - refinement fired on edge length ALONE - so the flattest
    // ground in the world was some of the most expensive.
    const LodPolicy policy;   // the shipped profile: 96 / 128 / 48 pixels
    for (const double metresPerPixel : {0.25, 1.0, 4.0, 16.0}) {
        const auto flat = geometryStepFor(metresPerPixel, 0.0, policy);
        const auto shaped = geometryStepFor(metresPerPixel, metresPerPixel * 0.9, policy);
        CHECK(flat > shaped);                       // coarser where there is nothing to see
        CHECK(flat <= shaped * 4);                  // and bounded: not the whole screen
        // Nothing splits ground it cannot see the shape of, at any size.
        CHECK(!shouldRefine(flat, metresPerPixel, 0.0, policy));
    }
    // And the two rules cannot both fire on one node, whatever its edge length:
    // refinement needs the error above the same bar coarsening needs it below.
    for (const std::int32_t step : {8, 32, 128, 1024})
        for (const double error : {0.0, 0.4, 0.6, 2.0}) {
            const double metresPerPixel = 2.0;
            const double deviation = error * metresPerPixel;
            CHECK(!(shouldRefine(step, metresPerPixel, deviation, policy) &&
                    shouldCoarsen(step, metresPerPixel, deviation, policy)));
        }
}

TEST(terrain_mesh_lod0_is_for_human_scale_closeups_not_map_scale) {
    const double mpp[]{0.05, 0.125, 0.25, 0.5, 1, 2, 4};
    const int mesh[]{4, 8, 16, 32, 64, 128, 256};
    const int data[]{4, 4, 8, 16, 16, 64, 64};
    for (int lod = 0; lod <= 6; ++lod) {
        // Country with shape in it. Flat ground is allowed past this ladder now
        // and has a test of its own.
        CHECK_EQ(geometryStepFor(mpp[lod], mpp[lod] * 0.9), mesh[lod]);
        CHECK_EQ(geometryLevelFor(mpp[lod]), lod);
        CHECK_EQ(dataStepForGeometry(mesh[lod], kRenderDataLodPolicy), data[lod]);
        CHECK_EQ(4 << dataLevelForGeometryLevel(lod, kRenderDataLodPolicy), data[lod]);
    }
    // A 34 px person merits LOD0. A 3.4 px person does not, even if H4
    // happens to be resident. Geometry policy knows nothing about residency.
    //
    // Measured on ground that has shape in it - four centimetres of deviation,
    // which is most of a pixel at this zoom. On a surface that is a PLANE the
    // answer is no at either size and rightly so: there is nothing there for
    // the triangles to describe.
    CHECK(shouldRefine(8, 1.7 / 34.0, 0.04));
    CHECK(!shouldRefine(8, 1.7 / 3.4, 0.04));
    CHECK(!shouldRefine(8, 1.7 / 34.0, 0));
    CHECK_EQ(dataStepForGeometry(16, DataLodPolicy{0}), 16);
    CHECK_EQ(dataStepForGeometry(16, DataLodPolicy{1}), 8);
    CHECK_EQ(dataStepForGeometry(16, DataLodPolicy{2}), 4);
    CHECK_EQ(dataLevelForGeometryLevel(6, DataLodPolicy{2}), std::uint8_t(4)); // retained foundation
    CHECK(shouldRefine(64, 1, 2)); // real geometric error can still force a split
}

TEST(terrain_mesh_hysteresis_never_oscillates_after_zooming_either_way) {
    for (int previous = 0; previous < int(kGeometryLevels); ++previous)
        for (double mpp : {0.04, 0.0625, 0.08, 0.09, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 32.0}) {
            const int settled = geometryLevelFor(mpp, previous);
            for (int frame = 0; frame < 100; ++frame)
                CHECK_EQ(geometryLevelFor(mpp, settled), settled);
        }
    CHECK_EQ(geometryLevelFor(0.08, 0), 0);
    CHECK_EQ(geometryLevelFor(0.08, 1), 1); // overlap: keep either side of the transition
    CHECK_EQ(geometryLevelFor(0.05, 6), 0);
    // Thirty-two metres to the pixel is a map view, and a map view now has
    // somewhere to go: a two-hundred-and-fifty-six-metre step there is an edge
    // of eight pixels, which is six times finer than the policy asks for. It
    // used to stop at the sixth level because there was no seventh.
    CHECK_EQ(geometryLevelFor(32, 0), 9);
    CHECK_EQ(4 << geometryLevelFor(32, 0), 2048);
}

TEST(terrain_land_mask_marks_every_coarse_cell_touched_by_land) {
    LandMask64 mask(192, 128);
    CHECK_EQ(mask.width(), 3);
    CHECK_EQ(mask.height(), 2);
    CHECK_EQ(mask.bytes(), sizeof(std::int32_t));   // one all-sea tile, no bits

    // A one-metre coastal sliver crossing x=64 keeps both 64-metre cells.
    mask.markWorldRect(63, 12, 65, 13);
    CHECK(mask.land(0, 0));
    CHECK(mask.land(1, 0));
    CHECK(!mask.land(2, 0));
    CHECK(!mask.land(0, 1));
    CHECK(!mask.land(-1, 0));
}

TEST(terrain_land_mask_holds_bits_only_where_a_tile_is_part_land) {
    // 800 x 2000 km of sea: a table word per 4 km tile, and no bits at all.
    LandMask64 sea(786432, 1966080);
    CHECK(sea.bytes() < 400u * 1024);
    CHECK(!sea.anyLandInWorldRect(0, 0, 786432, 1966080));
    // A rectangle covering whole tiles holds no bits either; its edges do.
    LandMask64 mask(65536, 65536);
    mask.markWorldRect(4096, 4096, 3 * 4096, 2 * 4096);
    CHECK_EQ(mask.bytes(), std::size_t(16 * 16) * sizeof(std::int32_t));
    CHECK(mask.land(64, 64));
    CHECK(mask.land(191, 127));
    CHECK(!mask.land(192, 64));
    CHECK(!mask.land(63, 64));
    mask.markWorldRect(100, 100, 200, 130);
    CHECK(mask.bytes() > std::size_t(16 * 16) * sizeof(std::int32_t));
    CHECK(mask.land(1, 1));
    CHECK(mask.land(3, 2));
    CHECK(!mask.land(4, 2));
    CHECK(!mask.land(1, 3));
    // Queries across tile borders see land in any tile of the rectangle.
    CHECK(mask.anyLandInWorldRect(3000, 3000, 5000, 5000));
    CHECK(!mask.anyLandInWorldRect(300, 300, 4000, 4000));
    CHECK(mask.anyLandInWorldRect(150, 120, 151, 121));
    CHECK(!mask.anyLandInWorldRect(256, 100, 4096, 4096));
}

TEST(terrain_land_mask_places_a_coarse_foundation_at_its_own_step) {
    // A foundation at 512 m (the reference world's): land at point 6 is 3 km
    // out, not 384 m.
    generation::WorldMapData world;
    world.width = 8;
    world.height = 1;
    world.cells.resize(8);
    for (auto& cell : world.cells) cell.sea = true;
    auto foundation = std::make_shared<generation::TerrainFoundation>();
    foundation->step = 512;
    foundation->stepShift = 9;
    foundation->columns = 9;
    foundation->rows = 2;
    for (auto& plane : foundation->heightDm) plane.assign(18, -600);
    for (auto& plane : foundation->heightDm) plane[6] = 400;
    world.terrainFoundation = foundation;
    const auto mask = makeLandMask64(world);
    CHECK(mask.land(6 * 512 / 64, 0));
    CHECK(mask.land((5 * 512 + 1) / 64, 0));
    CHECK(mask.land(7 * 512 / 64, 0));
    CHECK(!mask.land(6, 0));
    CHECK(!mask.land(4 * 512 / 64, 0));
}

TEST(terrain_land_mask_is_conservative_at_generated_coasts) {
    generation::WorldMapData world;
    world.width = 8;
    world.height = 1;
    world.cells.resize(8);
    for (auto& cell : world.cells) cell.sea = true;
    world.cells[0].sea = false;
    world.cells[1].sea = true;
    const LandMask64 mask = makeLandMask64(world);
    CHECK_EQ(mask.width(), (world.width * generation::kMetresPerCell + 63) / 64);
    for (int x = 0; x < generation::kMetresPerCell; x += 64) CHECK(mask.land(x / 64, 0));
    CHECK(!mask.land(mask.width() - 1, 0));
}

TEST(terrain_land_mask_keeps_the_warped_interpolated_coast) {
    generation::WorldMapData world;
    world.width = world.height = 8;
    world.cells.resize(64);
    for (auto& cell : world.cells) cell.sea = true;
    world.cells[3 * 8 + 3].sea = false;
    world.cells[3 * 8 + 3].elevation = 100;
    world::HeightField field(&world, 1);
    const auto mask = makeLandMask64(world);
    int coastOutsideMacroLand = 0;
    for (int y = 2 * generation::kMetresPerCell; y < 5 * generation::kMetresPerCell; y += 32)
        for (int x = 2 * generation::kMetresPerCell; x < 5 * generation::kMetresPerCell; x += 32) {
            const auto height = field.sampleHeight(x / 4, y / 4);
            if (height <= core::kZero) continue;
            if (x < 3 * generation::kMetresPerCell || y < 3 * generation::kMetresPerCell ||
                x >= 4 * generation::kMetresPerCell || y >= 4 * generation::kMetresPerCell) ++coastOutsideMacroLand;
            CHECK(mask.land(x / 64, y / 64));
        }
    CHECK(coastOutsideMacroLand > 0);
}

TEST(terrain_view_uses_projected_bounds_instead_of_a_camera_centred_square) {
    std::array<float, 16> matrix{1, -1, 0, 0, -0.5f, -0.5f, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    CHECK(intersectsView({0, 0, 1, 1, 0, 0}, matrix));
    // Both fit a generous XY radius, but the first is actually offscreen.
    CHECK(!intersectsView({4, 0, 5, 1, 0, 0}, matrix));
    CHECK(!intersectsView({3, 3, 4, 4, 0, 0}, matrix));
    CHECK(intersectsView({3, 3, 4, 4, 2, 4}, matrix)); // tall mountain enters the view
    const ViewBounds preloadOnly{2, 0, 2.2, 0.1, 0, 0};
    CHECK(!intersectsView(preloadOnly, matrix));
    CHECK(intersectsView(preloadOnly, matrix, 1, 1));
}

TEST(terrain_refinement_waits_for_data_and_only_evicts_after_reverse_morph) {
    RefinementTransition transition(0.2);
    CHECK_EQ(transition.phase(), RefinementPhase::Parent);
    CHECK(transition.fineDataMayEvict());

    transition.requestChildren(false);
    CHECK_EQ(transition.phase(), RefinementPhase::Waiting);
    CHECK(!transition.drawChildren());
    CHECK(!transition.fineDataMayEvict());
    transition.tick(1.0);
    CHECK_EQ(transition.phase(), RefinementPhase::Waiting);

    transition.dataBecameResident();
    transition.tick(0.1);
    CHECK_EQ(transition.phase(), RefinementPhase::Refining);
    CHECK(std::abs(transition.morph() - 0.5f) < 0.001f);
    CHECK(transition.drawChildren());

    transition.tick(0.1);
    CHECK_EQ(transition.phase(), RefinementPhase::Children);
    CHECK_EQ(transition.morph(), 1.0f);
    transition.requestParent();
    CHECK_EQ(transition.phase(), RefinementPhase::Coarsening);
    CHECK(!transition.fineDataMayEvict());
    transition.tick(0.1);
    CHECK(!transition.fineDataMayEvict());
    transition.tick(0.1);
    CHECK_EQ(transition.phase(), RefinementPhase::Parent);
    CHECK(transition.fineDataMayEvict());
}

TEST(terrain_refinement_can_reverse_without_a_topology_pop) {
    RefinementTransition transition(1.0);
    transition.requestChildren(true);
    transition.tick(0.4);
    CHECK(std::abs(transition.morph() - 0.4f) < 0.001f);
    transition.requestParent();
    transition.tick(0.1);
    CHECK(std::abs(transition.morph() - 0.3f) < 0.001f);
    transition.requestChildren(true);
    transition.tick(0.2);
    CHECK(std::abs(transition.morph() - 0.5f) < 0.001f);
}

TEST(terrain_detail_residency_never_evicts_a_page_used_by_a_morph) {
    PageResidency pages(2);
    const auto a = pages.acquire(10, 1);
    const auto b = pages.acquire(20, 2);
    CHECK(a.has_value());
    CHECK(b.has_value());
    CHECK(pages.pin(10, 3));

    const auto c = pages.acquire(30, 4);
    CHECK(c.has_value());
    CHECK(pages.find(10).has_value());
    CHECK(!pages.find(20).has_value());
    CHECK(pages.find(30).has_value());
    CHECK_EQ(c->replacedKey.value_or(0), std::int64_t(20));

    CHECK(pages.pin(30, 5));
    CHECK(!pages.acquire(40, 6).has_value());
    CHECK(pages.unpin(10));
    CHECK(pages.acquire(40, 7).has_value());
    CHECK(!pages.find(10).has_value());
}

TEST(terrain_detail_residency_hold_protects_without_counting_as_a_use) {
    // The renderer holds every resident page while a plan is built against
    // them. Held pages must survive; and holding must not make them all
    // "recent", or the oldest page would no longer be the one let go.
    PageResidency pages(3);
    CHECK(pages.acquire(10, 1).has_value());   // oldest
    CHECK(pages.acquire(20, 2).has_value());
    CHECK(pages.acquire(30, 3).has_value());   // newest
    CHECK(pages.hold(10));
    CHECK(pages.hold(20));
    CHECK(pages.hold(30));
    CHECK(!pages.hold(99));                     // not resident: nothing to hold
    CHECK(!pages.acquire(40, 4).has_value());   // everything held: nothing goes
    CHECK(pages.unpin(10));
    CHECK(pages.unpin(20));
    CHECK(pages.unpin(30));
    // Released, the order is as it was before the hold: 10 is still the oldest.
    const auto next = pages.acquire(40, 5);
    CHECK(next.has_value());
    CHECK_EQ(next->replacedKey.value_or(0), std::int64_t(10));
    // A hold stacks with a pin; each is undone on its own.
    CHECK(pages.pin(20, 6));
    CHECK(pages.hold(20));
    CHECK(pages.unpin(20));                     // the hold
    const auto after = pages.acquire(50, 7);
    CHECK(after.has_value());
    CHECK_EQ(after->replacedKey.value_or(0), std::int64_t(30));
    CHECK(pages.find(20).has_value());          // the pin still holds it
}

TEST(range_pool_hands_out_aligned_ranges_and_coalesces_on_release) {
    engine::RangePool pool(4096, 256);
    std::optional<std::uint32_t> page;
    const auto a = pool.allocate(100, page);
    CHECK(page.has_value());                       // first range: a page was added
    CHECK_EQ(a.offset, std::uint64_t(0));
    CHECK_EQ(a.bytes, std::uint64_t(256));         // rounded to the alignment
    const auto b = pool.allocate(1000, page);
    CHECK(!page.has_value());                      // same page
    CHECK_EQ(b.offset, std::uint64_t(256));
    const auto c = pool.allocate(256, page);
    CHECK_EQ(c.offset, std::uint64_t(256 + 1024));
    CHECK_EQ(pool.used(), std::uint64_t(256 + 1024 + 256));
    pool.release(b);
    pool.release(a);                               // merges with b's hole
    const auto d = pool.allocate(1280, page);      // exactly a + b
    CHECK(!page.has_value());
    CHECK_EQ(d.offset, std::uint64_t(0));
    pool.release(d);
    pool.release(c);
    CHECK_EQ(pool.used(), std::uint64_t(0));
    CHECK_EQ(pool.freeRanges(), std::size_t(1));   // one hole: the whole page
}

TEST(range_pool_rejects_double_release_and_gives_big_requests_their_own_page) {
    engine::RangePool pool(4096, 256);
    std::optional<std::uint32_t> page;
    const auto a = pool.allocate(512, page);
    pool.release(a);
    bool threw = false;
    try { pool.release(a); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    const auto big = pool.allocate(10000, page);
    CHECK(page.has_value());
    CHECK_EQ(pool.pageSize(*page), std::uint64_t(10240));
    CHECK_EQ(big.offset, std::uint64_t(0));
    pool.release(big);
    // Both pages empty: keep one, drop the other.
    const auto dropped = pool.trim(1);
    CHECK_EQ(dropped.size(), std::size_t(1));
    CHECK_EQ(pool.livePages(), std::size_t(1));
    // A dropped page's slot is reused rather than growing the page list.
    const auto again = pool.allocate(9000, page);
    CHECK(page.has_value());
    CHECK_EQ(*page, dropped.front());
    pool.release(again);
}

TEST(range_pool_churn_never_overlaps_and_settles) {
    // Meshes of every size coming and going, as the terrain cache does: no
    // two live ranges may overlap, and released space must be found again
    // instead of the pool growing without end.
    engine::RangePool pool(1 << 20, 256);
    std::vector<engine::RangePool::Range> live;
    std::uint32_t seed = 12345;
    const auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    std::optional<std::uint32_t> page;
    std::size_t pagesAfterWarmup = 0;
    for (int step = 0; step < 20000; ++step) {
        if (live.size() < 200 || next() % 2) {
            live.push_back(pool.allocate(256 + next() % 40000, page));
        } else {
            const auto at = next() % live.size();
            pool.release(live[at]);
            live.erase(live.begin() + long(at));
        }
        if (step == 5000) pagesAfterWarmup = pool.livePages();
    }
    std::vector<engine::RangePool::Range> sorted = live;
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a.page != b.page ? a.page < b.page : a.offset < b.offset;
    });
    for (std::size_t i = 1; i < sorted.size(); ++i)
        if (sorted[i].page == sorted[i - 1].page)
            CHECK(sorted[i - 1].offset + sorted[i - 1].bytes <= sorted[i].offset);
    std::uint64_t sum = 0;
    for (const auto& r : live) sum += r.bytes;
    CHECK_EQ(pool.used(), sum);
    CHECK(pool.livePages() <= pagesAfterWarmup + 2);   // settled, not growing
    for (const auto& r : live) pool.release(r);
    CHECK_EQ(pool.used(), std::uint64_t(0));
    CHECK_EQ(pool.freeRanges(), pool.livePages());     // every page one hole again
}
