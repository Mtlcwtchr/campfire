#include "framework.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/macro.hpp"
#include "game/world/tile_mesh.hpp"
#include "game/world/terrain_lod.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/page_ground.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace {
using core::Fixed;

generation::WorldMapData riverCountry() {
    generation::WorldMapData map;
    map.width = map.height = 10;
    map.cells.resize(100);
    for (auto& cell : map.cells) {
        cell.elevation = 30;
        cell.moisture = 160;
        cell.temperature = 130;
    }
    int east = 0;
    for (int d = 0; d < core::kNeighbourCount; ++d)
        if (core::neighbour({4, 4}, d) == core::TilePos{5, 4}) east = d;
    for (int x = 1; x < 9; ++x) {
        auto& cell = map.at({x, 4});
        cell.river = true;
        cell.riverSize = cell.drainSize = 8;
        cell.riverOut = cell.drainOut = static_cast<std::int8_t>(east);
    }
    return map;
}
}

TEST(water_queries_keep_one_canonical_river_head) {
    // One head to a place, and it is the graph's.
    //
    // This used to walk MacroWorld's centreline and require the field to
    // reproduce MacroWorld's profile to the millimetre - "the channel profile,
    // not a locally ground-clamped copy of it". It was a test that two models
    // of one river agreed, and it was worth having while both existed. One has
    // gone, and the head is ground-clamped on purpose now: a river runs along
    // the lowest ground in its cell, not at the cell's own height.
    //
    // What is left is what was always the point. The field's two ways in give
    // one answer; where the water covers a place, that place is under water;
    // and the head never climbs going downstream, because water does not.
    generation::WorldMapParams params;
    params.seed = 11;
    params.width = params.height = 64;
    const auto map = generation::generateWorldMap(params);
    const auto graph = world::streaming::buildHydrologyGraph(map);
    world::HeightField field(&map, params.seed);
    int checked = 0, wrongLevel = 0, dry = 0, climbed = 0;
    double worstClimb = 0;
    for (const auto& segment : graph.segments) {
        if (segment.course.size() < 4) continue;
        Fixed previous = Fixed::fromInt(1 << 20);
        for (const auto& point : segment.course) {
            const auto water = field.waterOver(point.position, Fixed::fromInt(4), core::kZero);
            if (water.kind != world::HeightField::WaterKind::River ||
                water.cover < Fixed::ratio(9, 10))
                continue;
            ++checked;
            if (field.waterLevelAt(point.position) != water.level) ++wrongLevel;
            if (!field.underWater(point.position)) ++dry;
            // A course runs upstream to downstream, so each point stands no
            // higher than the one before it. A centimetre of slack: the heads
            // are resolved in metres over macro cells and interpolated between.
            if (water.level > previous + Fixed::ratio(1, 100)) {
                ++climbed;
                worstClimb = std::max(worstClimb, (water.level - previous).toDouble());
            }
            previous = water.level;
        }
    }
    if (climbed) std::printf("  head climbed %d times, worst %.3f m\n", climbed, worstClimb);
    CHECK(checked > 100);
    CHECK_EQ(wrongLevel, 0);
    CHECK_EQ(dry, 0);
    CHECK_EQ(climbed, 0);
}

TEST(water_queries_confluences_share_the_head_before_centrelines_meet) {
    generation::WorldMapParams params;
    params.seed = 11;
    params.width = params.height = 64;
    const auto map = generation::generateWorldMap(params);
    const auto graph = world::streaming::buildHydrologyGraph(map);
    world::HeightField field(&map, params.seed);
    int checked = 0;
    for (const auto& node : graph.nodes) {
        if (node.kind != world::streaming::RiverNodeKind::Confluence) continue;
        for (const auto& segment : graph.segments) {
            if (segment.to != node.id || segment.course.size() < 3) continue;
            const auto& a = segment.course[segment.course.size() - 2];
            const auto& b = segment.course.back();
            CHECK_EQ(a.surface, node.surface);
            CHECK_EQ(b.surface, node.surface);
            for (int n = 0; n <= 4; ++n) {
                const auto t = Fixed::ratio(n, 4);
                const core::WorldPos at{core::lerp(a.position.x, b.position.x, t),
                                        core::lerp(a.position.y, b.position.y, t)};
                const auto water = field.waterOver(at, Fixed::fromInt(4), core::kZero);
                if (water.kind != world::HeightField::WaterKind::River) continue;
                ++checked;
                CHECK(core::abs(water.level - node.surface) < Fixed::ratio(1, 100));
            }
        }
    }
    CHECK(checked > 20);
}

TEST(water_queries_do_not_flood_dry_channel_shoulders) {
    const auto map = riverCountry();
    world::HeightField field(&map, 42);
    for (int x = 2; x <= 7; ++x) {
        const core::WorldPos p{Fixed::fromInt(x * generation::kMetresPerCell),
                              Fixed::fromInt(6 * generation::kMetresPerCell)};
        const auto water = field.waterOver(p, Fixed::fromInt(4), core::kZero);
        CHECK_EQ(water.cover, core::kZero);
        CHECK(!field.underWater(p));
    }
}

TEST(terrain_structural_samples_do_not_change_with_lod) {
    const auto map = riverCountry();
    world::HeightField field(&map, 42);
    for (const auto p : {core::TilePos{-12, -8}, core::TilePos{600, 607},
                         core::TilePos{760, 625}, core::TilePos{933, 614}}) {
        const auto expected = field.sampleHeight(p.x, p.y);
        for (int lod = 0; lod <= 12; ++lod)
            CHECK_EQ(field.sampleHeight(p.x, p.y, world::sampleMetresAt(lod)), expected);
    }
}

// A ring reads the ground the rest of the world reads.
//
// This used to assert that a vertex at every LOD carried the field's own
// full-resolution height - that the far view resampled the fine surface. That
// is what changed: a ring now reads the page its distance earns, and a page
// at one level is a strict subsample of the page below it. So the invariant
// is no longer "the ring agrees with the field" but "the ring agrees with the
// page", which is the whole point of routing it there - what is drawn and
// what is walked on are the same numbers.
TEST(terrain_ring_vertices_read_the_page_their_level_earns) {
    const auto map = riverCountry();
    world::HeightField field(&map, 42);
    const auto graph = world::streaming::buildHydrologyGraph(map);
    world::streaming::PageStore pages(map, graph, world::streaming::hsimQuantisationFor(map),
                                      {128u << 20, world::streaming::kDefaultPaddingSamples});
    const core::WorldPos centre{Fixed::ratio(14001, 5), Fixed::ratio(12003, 5)};
    world::ClimateField climate;
    climate.raise(map, field);
    for (int lod : {0, 1, 2, 3, 6}) {
        const auto side = static_cast<double>(world::tileMetresAt(lod));
        const world::TileId id{static_cast<std::int32_t>(std::floor(centre.x.toDouble() / side)),
                               static_cast<std::int32_t>(std::floor(centre.y.toDouble() / side)), lod};
        const auto mesh = world::buildTileMesh(field, pages, climate, id);
        CHECK(!mesh.vertices.empty());
        world::streaming::PageLattice lattice(pages, world::terrain::dataLevelForGeometryLevel(lod));
        std::size_t compared = 0, differed = 0;
        const auto surface = std::size_t(world::kTileCells + 1) * (world::kTileCells + 1);
        CHECK(mesh.vertices.size() >= surface);
        for (std::size_t i = 0; i < std::min(surface, mesh.vertices.size()); ++i) {
            const auto& v = mesh.vertices[i]; // skirts deliberately sit below the field
            // The same bilinear over the same level's lattice the sampler
            // walks, done here from the store rather than from the mesh.
            const std::int64_t stride = lattice.stride();
            const std::int64_t metres = 4 * stride;
            const std::int64_t sx = world::floorDiv(v.position.x.toInt(), metres) * stride;
            const std::int64_t sy = world::floorDiv(v.position.y.toInt(), metres) * stride;
            const Fixed step = Fixed::fromInt(metres);
            const Fixed fx = (v.position.x - Fixed::fromInt(sx * 4)) / step;
            const Fixed fy = (v.position.y - Fixed::fromInt(sy * 4)) / step;
            const Fixed a = lattice.visualHeight(sx, sy), b = lattice.visualHeight(sx + stride, sy);
            const Fixed c = lattice.visualHeight(sx, sy + stride);
            const Fixed d = lattice.visualHeight(sx + stride, sy + stride);
            const Fixed top = core::lerp(a, b, fx), bottom = core::lerp(c, d, fx);
            ++compared;
            if (core::abs(v.height - core::lerp(top, bottom, fy)) > Fixed::ratio(1, 100))
                ++differed;
        }
        CHECK(compared > 8);
        CHECK_EQ(differed, std::size_t(0));
    }
}

TEST(water_drainage_nodes_share_head_width_and_bed_after_erosion) {
    auto map = riverCountry();
    map.at({3, 4}).elevation = 15;
    map.at({4, 4}).elevation = 35; // different erosion resistance creates a rise
    world::MacroWorld macro;
    macro.attach(&map);
    for (int x = 2; x < 8; ++x) {
        const auto a = macro.channelOf({x, 4});
        const auto b = macro.channelOf({x + 1, 4});
        CHECK(a && b);
        if (!a || !b) continue;
        CHECK(a->surfaceFrom >= a->surfaceTo);
        CHECK_EQ(a->surfaceTo, b->surfaceFrom);
        CHECK_EQ(world::MacroWorld::halfWidthAt(*a, core::kOne),
                 world::MacroWorld::halfWidthAt(*b, core::kZero));
        CHECK_EQ(world::MacroWorld::depthAt(*a, core::kOne),
                 world::MacroWorld::depthAt(*b, core::kZero));
    }
}

TEST(water_upstream_runoff_survives_a_dry_climate_cell) {
    auto map = riverCountry();
    for (int x = 4; x < 9; ++x) {
        auto& cell = map.at({x, 4});
        cell.river = false;
        cell.riverOut = -1;
        cell.moisture = 0;
        cell.drainSize = 4;
    }
    world::MacroWorld macro;
    macro.attach(&map);
    for (int x = 3; x < 9; ++x) {
        const auto c = macro.channelOf({x, 4});
        CHECK(c && c->wet);
    }
}

TEST(water_generated_network_is_monotone_and_continuous) {
    for (const auto seed : {5, 11, 42}) {
        generation::WorldMapParams params;
        params.seed = seed;
        params.width = params.height = 32;
        const auto map = generation::generateWorldMap(params);
        world::MacroWorld macro;
        macro.attach(&map);
        int checked = 0;
        for (int y = 0; y < map.height; ++y) for (int x = 0; x < map.width; ++x) {
            const auto c = macro.channelOf({x, y});
            if (!c || !c->wet) continue;
            ++checked;
            CHECK(c->surfaceFrom >= c->surfaceTo);
            const auto& cell = map.at({x, y});
            const auto down = core::neighbour({x, y}, cell.river && cell.riverOut >= 0 ? cell.riverOut : cell.drainOut);
            const auto next = macro.channelOf(down);
            if (next) {
                CHECK(next->wet);
                CHECK_EQ(c->surfaceTo, next->surfaceFrom);
            } else if (map.at(down).sea) CHECK_EQ(c->surfaceTo, core::kZero);
        }
        CHECK(checked > 0);
    }
}

TEST(water_cyclic_input_is_rejected_instead_of_hanging_a_worker) {
    auto map = riverCountry();
    for (int d = 0; d < core::kNeighbourCount; ++d)
        if (core::neighbour({5, 4}, d) == core::TilePos{4, 4}) {
            map.at({5, 4}).drainOut = map.at({5, 4}).riverOut = static_cast<std::int8_t>(d);
        }
    bool rejected = false;
    try { world::MacroWorld macro; macro.attach(&map); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}

TEST(water_generated_streams_keep_wet_monotone_profiles_through_terminals) {
    int terminals = 0;
    for (int seed : {1, 3, 5, 7, 11, 17, 23, 42}) {
        generation::WorldMapParams params;
        params.seed = seed;
        params.width = params.height = 64;
        // This test compares the legacy MacroWorld centreline/profile, not the
        // canonical GraphCarver path used by the hybrid generator.
        params.hybridTerrain = false;
        const auto map = generation::generateWorldMap(params);
        world::HeightField field(&map, seed);
        int checked = 0, dry = 0, uphill = 0, mismatched = 0;
        for (int y = 0; y < map.height; ++y) for (int x = 0; x < map.width; ++x) {
            const auto index = std::size_t(y) * map.width + x;
            const auto& cell = map.cells[index];
            if (!cell.sea && cell.drainOut < 0 && map.riverDischargeField[index] >= 2) {
                ++terminals;
                int size = 0;
                for (int q = map.riverDischargeField[index]; q > 1 && size < 15; q /= 2) ++size;
                CHECK_EQ(int(cell.drainSize), size);
            }
            const auto channel = field.macro().channelOf({x, y});
            if (!channel || !channel->wet) continue;
            CHECK(channel->halfWidth >= Fixed::fromInt(4));
            Fixed previous;
            for (int i = 1; i <= 80; ++i) {
                const auto t = Fixed::ratio(i, 80);
                const auto p = world::MacroWorld::pointOn(*channel, t);
                const auto level = field.waterLevelAt(p);
                const auto water = field.waterOver(p, Fixed::fromInt(4), core::kZero);
                ++checked;
                if (field.heightAt(p) >= level) ++dry;
                if (i > 1 && level - previous > Fixed::ratio(1, 100)) ++uphill;
                if (water.level != level) ++mismatched;
                if (water.kind == world::HeightField::WaterKind::River &&
                    core::abs(level - core::lerp(channel->surfaceFrom, channel->surfaceTo, t)) > Fixed::ratio(1, 1000))
                    ++mismatched;
                previous = level;
            }
        }
        CHECK(checked > 1000);
        CHECK_EQ(dry, 0);
        CHECK_EQ(uphill, 0);
        CHECK_EQ(mismatched, 0);
    }
    CHECK(terminals > 0);
}

TEST(water_lake_queries_never_interpolate_different_body_heads) {
    auto map = riverCountry();
    for (auto& c : map.cells) { c.river = false; c.drainOut = c.riverOut = -1; c.drainSize = 0; }
    map.lakeDepthField.assign(100, 0);
    map.lakeLevelField.assign(100, 0);
    map.lakeRegionField.assign(100, -1);
    map.lakeDepthField[44] = 10; map.lakeLevelField[44] = 30; map.lakeRegionField[44] = 44;
    map.lakeDepthField[45] = 20; map.lakeLevelField[45] = 50; map.lakeRegionField[45] = 45;
    world::HeightField field(&map, 42);
    int sampled = 0;
    for (int x = 2430; x <= 2970; x += 7) {
        const auto p = core::WorldPos{Fixed::fromInt(x), Fixed::fromInt(2430)};
        const auto lake = field.piecesAt(p.x, p.y);
        CHECK(lake.lakeLevel == Fixed::fromInt(270) || lake.lakeLevel == Fixed::fromInt(450));
        for (int lod : {0, 3, 6, 12})
            CHECK_EQ(field.piecesAt(p.x, p.y, world::sampleMetresAt(lod)).lakeLevel, lake.lakeLevel);
        ++sampled;
    }
    CHECK(sampled > 50);
}

TEST(water_generated_lakes_have_one_head_per_connected_body) {
    int sampled = 0;
    for (int seed : {1, 5, 7, 11, 17, 42}) {
        generation::WorldMapParams params;
        params.seed = seed;
        params.width = params.height = 64;
        const auto map = generation::generateWorldMap(params);
        for (std::size_t i = 0; i < map.cells.size(); ++i) {
            if (map.lakeDepthField[i] <= 0) { CHECK_EQ(map.lakeRegionField[i], -1); continue; }
            ++sampled;
            const auto id = map.lakeRegionField[i];
            CHECK(id >= 0 && std::size_t(id) <= i);
            if (id < 0 || std::size_t(id) > i) continue;
            CHECK_EQ(map.lakeLevelField[i], map.lakeLevelField[id]);
            CHECK(map.lakeDepthField[id] > 0);
        }
    }
    CHECK(sampled > 0);
}

TEST(water_valley_projection_is_continuous_at_bends) {
    generation::WorldMapParams params;
    params.seed = 11;
    params.width = params.height = 64;
    params.hybridTerrain = false; // the measured bend below belongs to the legacy map
    const auto map = generation::generateWorldMap(params);
    world::HeightField field(&map, params.seed);
    Fixed previous;
    for (int i = 0; i <= 80; ++i) {
        const core::WorldPos p{Fixed::ratio(2310089, 100) + Fixed::ratio(i, 4),
                              Fixed::ratio(1733361, 100)};
        const auto g = field.piecesAt(p.x, p.y);
        const auto profile = field.macro().carve(p, g.country, g.moved);
        if (i) CHECK(core::abs(profile.floor - previous) < Fixed::ratio(3, 4));
        previous = profile.floor;
    }
}

TEST(water_lake_shores_meet_the_ground_instead_of_ending_in_midair) {
    struct Regression { int seed, x, y; int worldSize = 64; };
    for (const auto example : {Regression{3, 11854, 13913}, Regression{7, 11493, 5470},
                               Regression{11, 7011, 29189}, Regression{7, 6830, 29982},
                               Regression{42, 25060, 12776}, Regression{11, 23559, 82321, 192}}) {
        generation::WorldMapParams params;
        params.seed = example.seed;
        params.width = params.height = example.worldSize;
        params.hybridTerrain = false; // fixed regression coordinates, not new-world placement
        const auto map = generation::generateWorldMap(params);
        world::HeightField field(&map, params.seed);
        int transitions = 0;
        for (int axis = 0; axis < 2; ++axis) {
            bool wasWet = false;
            Fixed previous;
            for (int d = -1200; d <= 1200; ++d) {
                const core::WorldPos p{Fixed::fromInt(example.x + (axis ? 0 : d)),
                                       Fixed::fromInt(example.y + (axis ? d : 0))};
                const auto level = field.waterLevelAt(p), ground = field.heightAt(p);
                const bool wet = ground < level;
                if (wasWet && !wet) {
                    ++transitions;
                    if (ground < previous - Fixed::ratio(1, 2))
                        std::cerr << "  shoreline seed=" << example.seed << " at " << p.x.toDouble()
                                  << "," << p.y.toDouble() << " old head=" << previous.toDouble()
                                  << " ground=" << ground.toDouble() << " new head=" << level.toDouble() << '\n';
                    CHECK(ground >= previous - Fixed::ratio(1, 2));
                }
                previous = level;
                wasWet = wet;
            }
        }
        CHECK(transitions > 0);
    }
}

TEST(water_lake_shores_use_the_interpolated_ground_between_samples) {
    generation::WorldMapParams params;
    params.seed = 11;
    params.width = params.height = 192;
    params.hybridTerrain = false; // preserve the named 234-metre lake and its measured rim
    const auto map = generation::generateWorldMap(params);
    world::HeightField field(&map, params.seed);
    const auto head = Fixed::fromInt(234);
    int submerged = 0;
    // The analytic rim has already crossed the lake head here, while the
    // canonical four-metre ground is still submerged. Integer probes miss it.
    for (int x = 0; x <= 2; ++x) for (int y = 0; y <= 16; ++y) {
        const core::WorldPos p{Fixed::ratio(94221 + x, 4),
                               Fixed::ratio(329429, 4) + Fixed::ratio(y, 16)};
        const auto ground = field.heightAt(p);
        if (ground >= head) continue;
        ++submerged;
        CHECK_EQ(field.waterLevelAt(p), head);
        CHECK(field.underWater(p));
        const auto water = field.waterOver(p, Fixed::fromInt(4), core::kZero);
        CHECK_EQ(water.kind, world::HeightField::WaterKind::Lake);
        CHECK_EQ(water.level, head);
        CHECK(water.cover > core::kZero);
    }
    CHECK(submerged > 20);

    // A four-metre stride can skip the top of the rim entirely. Measure the
    // actual exit, not the dry slope on the far side of that ridge.
    core::WorldPos wet{Fixed::ratio(47111, 2), Fixed::ratio(82357183, 1000)};
    core::WorldPos dry{wet.x, wet.y + Fixed::fromInt(4)};
    CHECK(field.underWater(wet));
    CHECK(!field.underWater(dry));
    CHECK(field.heightAt(dry) < head - Fixed::fromInt(3));
    for (int i = 0; i < 12; ++i) {
        const core::WorldPos mid{(wet.x + dry.x) / 2, (wet.y + dry.y) / 2};
        if (field.underWater(mid)) wet = mid;
        else dry = mid;
    }
    CHECK(field.heightAt(dry) >= field.waterLevelAt(wet) - Fixed::ratio(1, 2));
    CHECK_EQ(field.waterOver(wet, Fixed::fromInt(4), core::kZero).level, field.waterLevelAt(wet));
}
