#include "framework.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "engine/core/rng.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"
#include "game/world/macro.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/hydrology_cache.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"
#include "game/world/terrain_streaming/tile_cache.hpp"
#include "game/world/terrain_streaming/water_tile.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"
#include "game/world/terrain_streaming/worker_runtime.hpp"

namespace {

world::streaming::BaseTile baseTile(world::streaming::TileKey key) {
    using namespace world::streaming;
    BaseTile tile;
    tile.key = key;
    tile.width = tile.height = interiorSamples(4);
    tile.padding = kDefaultPaddingSamples;
    tile.sampleMetres = 4;
    tile.elevationMin = core::Fixed::fromInt(-20);
    tile.elevationMax = core::Fixed::fromInt(300);
    const auto count = tile.sampleCount();
    tile.heightQuantized.resize(count);
    tile.waterBodyId.resize(count);
    tile.watershedId.resize(count);
    tile.buildability.resize(count, 255);
    tile.walkability.resize(count, 255);
    for (std::size_t i = 0; i < count; ++i) {
        tile.heightQuantized[i] = static_cast<std::uint16_t>((i / tile.width + i % tile.width) & 0xffffu);
        tile.waterBodyId[i] = i % 97 == 0 ? 3 : 0;
        tile.watershedId[i] = static_cast<std::uint16_t>(i / 1024);
    }
    return tile;
}

} // namespace

TEST(terrain_storage_tiles_are_world_aligned_across_negative_coordinates) {
    using namespace world::streaming;
    const auto west = tileAt({core::Fixed::fromInt(-1), core::Fixed::fromInt(12)});
    const auto east = tileAt({core::Fixed::fromInt(511), core::Fixed::fromInt(12)});
    const auto next = tileAt({core::Fixed::fromInt(512), core::Fixed::fromInt(12)});
    CHECK_EQ(west.x, -1);
    CHECK_EQ(east.x, 0);
    CHECK_EQ(next.x, 1);
    CHECK_EQ(interiorSamples(4), 129);
    CHECK_EQ(storedSamples(4), 133);
}

TEST(terrain_base_tile_payload_round_trips_without_native_struct_layout) {
    using namespace world::streaming;
    const auto source = baseTile({-4, 7, 0});
    std::vector<std::uint8_t> payload;
    CHECK(encodeBaseTilePayload(source, payload));
    BaseTile decoded;
    CHECK(decodeBaseTilePayload(describeBaseTile(source), payload, decoded));
    CHECK(decoded.valid());
    CHECK_EQ(decoded.key, source.key);
    CHECK_EQ(decoded.heightQuantized, source.heightQuantized);
    CHECK_EQ(decoded.waterBodyId, source.waterBodyId);
    CHECK_EQ(decoded.watershedId, source.watershedId);
    CHECK_EQ(decoded.buildability, source.buildability);
    CHECK_EQ(decoded.walkability, source.walkability);
}

TEST(terrain_build_function_never_runs_on_the_submitting_thread) {
    using namespace world::streaming;
    const auto submittingThread = std::this_thread::get_id();
    std::atomic_bool ranElsewhere = false;
    TerrainWorkerRuntime runtime({2, 8, 8}, [&](const TerrainStreamRequest& request,
                                                const TerrainCancellation&) {
        ranElsewhere.store(std::this_thread::get_id() != submittingThread);
        TerrainBuildResult result;
        result.key = request.key;
        result.base = std::make_shared<const BaseTile>(baseTile(request.key.tile));
        return std::optional<TerrainBuildResult>{std::move(result)};
    });
    const TerrainStreamRequest request{{11, {2, 3, 0}, productMask(TileProduct::Base)}, 4, 100};
    CHECK(runtime.submit(request));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::vector<TerrainStreamCompletion> ready;
    while (ready.empty() && std::chrono::steady_clock::now() < deadline) {
        ready = runtime.takeReady(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(ranElsewhere.load());
    CHECK_EQ(ready.size(), std::size_t(1));
    if (!ready.empty()) CHECK_EQ(ready.front().result.key, request.key);
}

// ---------------------------------------------------------------------------
// HydrologyGraph extraction.
// ---------------------------------------------------------------------------

namespace {

using core::Fixed;
using core::TilePos;

constexpr std::int32_t kSynthWidth = 10;

std::int8_t directionTo(TilePos from, TilePos to) {
    for (int d = 0; d < core::kNeighbourCount; ++d)
        if (core::neighbour(from, d) == to) return static_cast<std::int8_t>(d);
    return -1;
}

generation::WorldMapData synthCountry() {
    generation::WorldMapData map;
    map.width = map.height = kSynthWidth;
    map.cells.resize(static_cast<std::size_t>(kSynthWidth) * kSynthWidth);
    for (auto& cell : map.cells) {
        cell.elevation = 30;
        cell.moisture = 160;
        cell.temperature = 130;
    }
    return map;
}

// A run of river cells, each shedding into the next.
void layCourse(generation::WorldMapData& map, const std::vector<TilePos>& course) {
    for (std::size_t i = 0; i + 1 < course.size(); ++i) {
        auto& cell = map.at(course[i]);
        cell.river = true;
        cell.drainSize = 8;
        cell.riverSize = 8;
        cell.riverOut = cell.drainOut = directionTo(course[i], course[i + 1]);
    }
}

void makeSea(generation::WorldMapData& map, TilePos at) {
    auto& cell = map.at(at);
    cell.sea = true;
    cell.elevation = 0;
    cell.river = false;
    cell.riverOut = cell.drainOut = -1;
}

// Which macro cell a centreline point names. The builder writes cell centres,
// so this is exact rather than a nearest-cell guess.
TilePos cellOfPoint(core::WorldPos point) {
    const std::int64_t metres = generation::kMetresPerCell;
    return {static_cast<std::int32_t>(world::floorDiv(point.x.toInt(), metres)),
            static_cast<std::int32_t>(world::floorDiv(point.y.toInt(), metres))};
}

std::uint64_t mixDigest(std::uint64_t hash, std::uint64_t value) {
    return core::splitmix64(hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2)));
}

// Everything an ID rule could get wrong, folded into one number: order of the
// lists, the IDs themselves, and every link between them.
std::uint64_t digestOf(const world::streaming::HydrologyGraph& graph) {
    std::uint64_t hash = mixDigest(0, graph.sourceFingerprint);
    hash = mixDigest(hash, graph.nodes.size());
    for (const auto& node : graph.nodes) {
        hash = mixDigest(hash, node.id);
        hash = mixDigest(hash, node.downstream);
        hash = mixDigest(hash, node.waterBody);
        hash = mixDigest(hash, static_cast<std::uint64_t>(node.macroCell));
        hash = mixDigest(hash, static_cast<std::uint64_t>(node.kind));
        hash = mixDigest(hash, node.order);
        hash = mixDigest(hash, static_cast<std::uint64_t>(node.surface.raw));
    }
    hash = mixDigest(hash, graph.segments.size());
    for (const auto& segment : graph.segments) {
        hash = mixDigest(hash, segment.id);
        hash = mixDigest(hash, segment.from);
        hash = mixDigest(hash, segment.to);
        hash = mixDigest(hash, segment.sourceWaterBody);
        hash = mixDigest(hash, segment.destinationWaterBody);
        hash = mixDigest(hash, segment.order);
        hash = mixDigest(hash, static_cast<std::uint64_t>(segment.discharge.raw));
        hash = mixDigest(hash, static_cast<std::uint64_t>(segment.width.raw));
        hash = mixDigest(hash, static_cast<std::uint64_t>(segment.depth.raw));
        hash = mixDigest(hash, static_cast<std::uint64_t>(segment.valleyReach.raw));
        hash = mixDigest(hash, segment.macroCells.size());
        for (const auto cell : segment.macroCells)
            hash = mixDigest(hash, static_cast<std::uint64_t>(cell));
        hash = mixDigest(hash, segment.course.size());
        for (const auto& point : segment.course) {
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.position.x.raw));
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.position.y.raw));
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.surface.raw));
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.halfWidth.raw));
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.depth.raw));
            hash = mixDigest(hash, static_cast<std::uint64_t>(point.valleyReach.raw));
        }
    }
    hash = mixDigest(hash, graph.waterBodies.size());
    for (const auto& body : graph.waterBodies) {
        hash = mixDigest(hash, body.id);
        hash = mixDigest(hash, static_cast<std::uint64_t>(body.kind));
        hash = mixDigest(hash, static_cast<std::uint64_t>(body.sourceRegion));
        hash = mixDigest(hash, static_cast<std::uint64_t>(body.level.raw));
        hash = mixDigest(hash, body.macroCells.size());
        for (const auto cell : body.macroCells)
            hash = mixDigest(mixDigest(hash, static_cast<std::uint32_t>(cell.x)),
                             static_cast<std::uint32_t>(cell.y));
        for (const auto id : body.inlets) hash = mixDigest(hash, id);
        for (const auto id : body.outlets) hash = mixDigest(hash, id);
    }
    hash = mixDigest(hash, graph.spatialPages.size());
    for (const auto& page : graph.spatialPages) {
        hash = mixDigest(mixDigest(hash, static_cast<std::uint32_t>(page.key.x)),
                         static_cast<std::uint32_t>(page.key.y));
        for (const auto id : page.segments) hash = mixDigest(hash, id);
        for (const auto id : page.waterBodies) hash = mixDigest(hash, id);
    }
    return hash;
}

// The seeds the river linter is swept over, so a graph defect and a carving
// defect are found on the same country.
constexpr std::uint64_t kProbeSeeds[] = {1, 7, 11, 42};

const generation::WorldMapData& generatedWorld(std::uint64_t seed) {
    static std::map<std::uint64_t, generation::WorldMapData> built;
    const auto found = built.find(seed);
    if (found != built.end()) return found->second;
    generation::WorldMapParams params;
    params.seed = seed;
    params.width = params.height = 64;
    // kLakePage/kCoastPage below are coordinates in the original generator.
    // Keep that geographic fixture stable; hybrid pages have their own tests.
    params.hybridTerrain = false;
    return built.emplace(seed, generation::generateWorldMap(params)).first->second;
}

} // namespace

TEST(water_stands_in_its_channel_and_not_across_its_valley) {
    using namespace world::streaming;
    // A reach shapes the ground for hundreds of metres either side of itself -
    // that is its valley, and it is why the country around a river slopes. It
    // does not stand in water for hundreds of metres, and it used to: the
    // surface was offered anywhere the valley reached, so a channel eight
    // metres across came out wet across nine hundred and twenty-five, and the
    // world read as flooded from any height.
    //
    // Measured across the middle of every reach, at a metre a step, counting
    // only water that belongs to no body - a cross-section at a mouth crosses
    // the sea and one over a lake crosses the lake, and both are honestly wide.
    for (const std::uint64_t seed : {11ull, 42ull}) {
        const auto& world = generatedWorld(seed);
        const auto graph = buildHydrologyGraph(world);
        world::HeightField field(&world, seed);
        std::vector<double> ratios;
        double widest = 0;
        std::size_t sections = 0;
        for (const auto& segment : graph.segments) {
            if (segment.course.size() < 6) continue;
            const std::size_t middle = segment.course.size() / 2;
            const auto& before = segment.course[middle - 1];
            const auto& after = segment.course[middle + 1];
            const double dx = (after.position.x - before.position.x).toDouble();
            const double dy = (after.position.y - before.position.y).toDouble();
            const double along = std::hypot(dx, dy);
            const double declared = segment.course[middle].halfWidth.toDouble() * 2;
            if (along < 1 || declared <= 0) continue;
            const auto centre = segment.course[middle].position;
            const core::WorldRect around{
                    {centre.x - core::Fixed::fromInt(1200), centre.y - core::Fixed::fromInt(1200)},
                    {centre.x + core::Fixed::fromInt(1200), centre.y + core::Fixed::fromInt(1200)}};
            const GraphCarver carver(graph, around, 900);
            double wet = 0;
            for (int step = -600; step <= 600; ++step) {
                const core::WorldPos at{
                        centre.x + core::Fixed::fromDoubleForContent(-dy / along * step),
                        centre.y + core::Fixed::fromDoubleForContent(dx / along * step)};
                const auto pieces = field.piecesAt(at.x, at.y);
                const CarvedSample sample = carver.carve(at, pieces.country, pieces.moved);
                if (sample.wet && sample.body == kInvalidWaterBodyId) ++wet;
            }
            ratios.push_back(wet / declared);
            widest = std::max(widest, wet);
            ++sections;
        }
        CHECK(sections > 40);
        std::sort(ratios.begin(), ratios.end());
        const double median = ratios[ratios.size() / 2];
        // Three times its own width at the middle: a channel and the wet bank
        // either side of it. Measured at 3.1 and 4.6 on these two worlds; the
        // bound is what says a river is a river and not a flood.
        CHECK(median < 8.0);
        // And nowhere a river of a quarter of a mile. The widest reaches on
        // these worlds run to two hundred and ninety metres of open water,
        // which is a trunk river at its mouth.
        CHECK(widest < 400.0);
    }
}

TEST(hydrology_graph_splits_the_network_where_two_courses_meet) {
    using namespace world::streaming;
    auto map = synthCountry();
    layCourse(map, {{4, 2}, {4, 3}, {4, 4}});
    layCourse(map, {{1, 4}, {2, 4}, {3, 4}, {4, 4}});
    layCourse(map, {{4, 4}, {5, 4}, {6, 4}, {7, 4}, {8, 4}, {9, 4}});
    makeSea(map, {9, 4});

    const auto graph = buildHydrologyGraph(map);
    CHECK_EQ(graph.macroWidth, kSynthWidth);
    CHECK_EQ(graph.macroCellMetres, generation::kMetresPerCell);

    // Two springs, one junction, one mouth - and nothing in between, which is
    // the whole point of compressing the chain.
    CHECK_EQ(graph.nodes.size(), std::size_t(4));
    CHECK_EQ(graph.segments.size(), std::size_t(3));
    std::size_t sources = 0, confluences = 0, mouths = 0;
    for (const auto& node : graph.nodes) {
        sources += node.kind == RiverNodeKind::Source;
        confluences += node.kind == RiverNodeKind::Confluence;
        mouths += node.kind == RiverNodeKind::Mouth;
    }
    CHECK_EQ(sources, std::size_t(2));
    CHECK_EQ(confluences, std::size_t(1));
    CHECK_EQ(mouths, std::size_t(1));

    // Both tributaries end at the junction the trunk leaves from.
    const RiverSegment* trunk = nullptr;
    for (const auto& segment : graph.segments)
        if (segment.destinationWaterBody == kOceanWaterBodyId) trunk = &segment;
    CHECK(trunk != nullptr);
    if (trunk == nullptr) return;
    const auto* junction = riverNodeOf(graph, trunk->from);
    CHECK(junction != nullptr && junction->kind == RiverNodeKind::Confluence);
    for (const auto& segment : graph.segments) {
        if (segment.id == trunk->id) continue;
        CHECK_EQ(segment.to, trunk->from);
        CHECK_EQ(segment.order, std::uint8_t(1));
    }
    // Two equal orders meeting raise the trunk, which is what Strahler is for.
    CHECK_EQ(trunk->order, std::uint8_t(2));
    CHECK_EQ(waterBodyOf(graph, kOceanWaterBodyId)->inlets, std::vector<RiverId>{trunk->id});
}

TEST(hydrology_graph_carries_a_river_through_a_lake_as_one_body) {
    using namespace world::streaming;
    auto map = synthCountry();
    layCourse(map, {{1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 4}, {7, 4}, {8, 4}, {9, 4}});
    makeSea(map, {9, 4});
    const auto count = map.cells.size();
    map.lakeRegionField.assign(count, -1);
    map.lakeLevelField.assign(count, 0);
    map.lakeDepthField.assign(count, 0);
    for (const TilePos cell : {TilePos{5, 4}, TilePos{6, 4}}) {
        const auto i = static_cast<std::size_t>(cell.y) * kSynthWidth + cell.x;
        map.lakeRegionField[i] = 4 * kSynthWidth + 5;   // the smallest member index
        map.lakeLevelField[i] = 30;
        map.lakeDepthField[i] = 5;
    }

    const auto graph = buildHydrologyGraph(map);
    // Ocean plus one lake, and the lake stands at one head over both its cells.
    CHECK_EQ(graph.waterBodies.size(), std::size_t(2));
    const auto* lake = waterBodyOf(graph, 2);
    CHECK(lake != nullptr);
    if (lake == nullptr) return;
    CHECK(lake->kind == WaterBodyKind::Lake);
    CHECK_EQ(lake->macroCells.size(), std::size_t(2));
    CHECK_EQ(lake->level, Fixed::fromInt(30 * generation::kMetresPerElevationStep));

    // The river is cut at the shore: nothing runs a channel across standing
    // water, and the body is what joins the two halves.
    CHECK_EQ(graph.segments.size(), std::size_t(2));
    CHECK_EQ(lake->inlets.size(), std::size_t(1));
    CHECK_EQ(lake->outlets.size(), std::size_t(1));
    const auto* incoming = riverSegmentOf(graph, lake->inlets.front());
    const auto* outgoing = riverSegmentOf(graph, lake->outlets.front());
    CHECK(incoming != nullptr && outgoing != nullptr);
    if (incoming == nullptr || outgoing == nullptr) return;
    CHECK_EQ(incoming->destinationWaterBody, WaterBodyId(2));
    CHECK_EQ(outgoing->sourceWaterBody, WaterBodyId(2));
    CHECK_EQ(outgoing->destinationWaterBody, kOceanWaterBodyId);

    const auto* inlet = riverNodeOf(graph, incoming->to);
    const auto* outlet = riverNodeOf(graph, outgoing->from);
    CHECK(inlet != nullptr && outlet != nullptr);
    if (inlet == nullptr || outlet == nullptr) return;
    CHECK(inlet->kind == RiverNodeKind::LakeInlet);
    CHECK(outlet->kind == RiverNodeKind::LakeOutlet);
    CHECK_EQ(cellOfPoint(inlet->position), (TilePos{5, 4}));
    CHECK_EQ(cellOfPoint(outlet->position), (TilePos{6, 4}));
    // Downstream stays followable across the body rather than dead-ending in it.
    CHECK_EQ(inlet->downstream, outlet->id);
    // And the outflow continues the river it received instead of restarting it.
    CHECK_EQ(outgoing->order, incoming->order);
}

TEST(hydrology_graph_rejects_a_circular_drainage_instead_of_walking_it) {
    using namespace world::streaming;
    auto map = synthCountry();
    // Two cells shedding into each other. Resolving where their water ends up
    // has no answer, and the build has to say so rather than follow the loop.
    layCourse(map, {{3, 3}, {4, 3}, {3, 3}});
    map.flowDirectionField.assign(map.cells.size(), -1);
    for (std::size_t i = 0; i < map.cells.size(); ++i)
        map.flowDirectionField[i] = map.cells[i].drainOut;
    bool rejected = false;
    try {
        buildHydrologyGraph(map);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    CHECK(rejected);
}

TEST(hydrology_graph_ids_do_not_depend_on_the_build) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto first = buildHydrologyGraph(world);
    const auto second = buildHydrologyGraph(world);
    CHECK(!first.segments.empty());
    CHECK_EQ(first.sourceFingerprint, second.sourceFingerprint);
    CHECK_EQ(digestOf(first), digestOf(second));
    // A different macro map has to be a cache miss, not a silent reuse.
    CHECK(first.sourceFingerprint != buildHydrologyGraph(generatedWorld(7)).sourceFingerprint);
}

TEST(hydrology_graph_every_course_reaches_a_node_a_body_or_the_sea) {
    using namespace world::streaming;
    for (const std::uint64_t seed : kProbeSeeds) {
    const auto graph = buildHydrologyGraph(generatedWorld(seed));
    CHECK(!graph.segments.empty());

    for (std::size_t n = 0; n < graph.nodes.size(); ++n)
        CHECK_EQ(graph.nodes[n].id, RiverNodeId(n + 1));
    for (std::size_t n = 0; n < graph.waterBodies.size(); ++n)
        CHECK_EQ(graph.waterBodies[n].id, WaterBodyId(n + 1));

    std::size_t unterminated = 0, unordered = 0, backwards = 0, unsized = 0;
    for (std::size_t s = 0; s < graph.segments.size(); ++s) {
        const auto& segment = graph.segments[s];
        CHECK_EQ(segment.id, RiverId(s + 1));
        if (segment.to == kInvalidRiverNodeId) ++unterminated;
        if (segment.order == 0) ++unordered;
        if (segment.course.size() < 2) ++backwards;
        const auto* target = riverNodeOf(graph, segment.to);
        if (target != nullptr && target->kind == RiverNodeKind::Source) ++backwards;
        // A course that reaches the sea has an outgoing reach the whole way, so
        // it is sized: no wet channel this world draws is under the 4 m the
        // height lattice can carry either side of its middle.
        if (segment.destinationWaterBody == kOceanWaterBodyId &&
            segment.width < Fixed::fromInt(8))
            ++unsized;
    }
    CHECK_EQ(unterminated, std::size_t(0));
    CHECK_EQ(unordered, std::size_t(0));
    CHECK_EQ(backwards, std::size_t(0));
    CHECK_EQ(unsized, std::size_t(0));

    // No node can be walked back to itself, however long its chain.
    std::vector<std::uint8_t> settled(graph.nodes.size() + 1, 0);
    std::size_t cyclic = 0;
    for (const auto& start : graph.nodes) {
        std::vector<RiverNodeId> walked;
        RiverNodeId at = start.id;
        while (at != kInvalidRiverNodeId && !settled[at]) {
            if (walked.size() > graph.nodes.size()) break;
            walked.push_back(at);
            at = graph.nodes[at - 1].downstream;
        }
        if (walked.size() > graph.nodes.size()) ++cyclic;
        for (const auto id : walked) settled[id] = 1;
    }
    CHECK_EQ(cyclic, std::size_t(0));

    // A lake belongs to one body and one level, everywhere it is asked about.
    std::map<std::int64_t, WaterBodyId> owner;
    std::size_t shared = 0;
    for (const auto& body : graph.waterBodies)
        for (const auto cell : body.macroCells) {
            const std::int64_t key = std::int64_t(cell.y) * graph.macroWidth + cell.x;
            if (!owner.emplace(key, body.id).second) ++shared;
        }
    CHECK_EQ(shared, std::size_t(0));

    // A junction is where courses actually meet, and a spring is where none do.
    std::vector<std::size_t> arrivals(graph.nodes.size() + 1, 0);
    for (const auto& segment : graph.segments)
        if (segment.to != kInvalidRiverNodeId) ++arrivals[segment.to];
    std::size_t misplaced = 0;
    for (const auto& node : graph.nodes) {
        const std::size_t in = arrivals[node.id];
        switch (node.kind) {
            case RiverNodeKind::Source: misplaced += in != 0; break;
            case RiverNodeKind::Confluence: misplaced += in < 2; break;
            case RiverNodeKind::Mouth:
            case RiverNodeKind::LakeInlet:
            case RiverNodeKind::Terminal: misplaced += in < 1; break;
            case RiverNodeKind::LakeOutlet: break;   // fed by its body, not by a segment
        }
    }
    CHECK_EQ(misplaced, std::size_t(0));
    }
}

TEST(hydrology_graph_compresses_the_river_network_without_losing_a_cell) {
    using namespace world::streaming;
    for (const std::uint64_t seed : kProbeSeeds) {
    const auto& world = generatedWorld(seed);
    const auto graph = buildHydrologyGraph(world);

    // From the segments' own provenance, not from where their courses run:
    // a shaped course wanders off the cells it was compressed from, which is
    // the point of shaping it.
    std::map<std::int64_t, std::size_t> covered;
    for (const auto& segment : graph.segments)
        for (const auto cell : segment.macroCells) ++covered[cell];

    // Standing water is the graph's own footprint, not the macro map's: the
    // build absorbs the holes the flood left in a lake it covers.
    std::map<std::int64_t, bool> standing;
    for (const auto& body : graph.waterBodies)
        for (const auto cell : body.macroCells)
            standing[std::int64_t(cell.y) * world.width + cell.x] = true;

    // Every wet course the macro map resolved is either on a segment or under
    // standing water. A cell that is neither is water the graph has dropped.
    world::MacroWorld macro;
    macro.attach(&world);
    std::size_t missing = 0, interiorSeenTwice = 0;
    for (std::size_t i = 0; i < world.cells.size(); ++i) {
        if (world.cells[i].sea) continue;
        const core::TilePos at{static_cast<std::int32_t>(i % world.width),
                               static_cast<std::int32_t>(i / world.width)};
        if (!macro.wetAt(at)) continue;
        const bool inLake = standing.count(static_cast<std::int64_t>(i)) != 0;
        const auto found = covered.find(static_cast<std::int64_t>(i));
        if (found == covered.end()) {
            // One gap is allowed, and only where the geometry the graph is
            // replacing has the same one: a wet cell that sheds nowhere is a
            // sink, MacroWorld builds no reach from it and draws no water in
            // it. Anything the old path would have drawn and the new one drops
            // is a river gone missing.
            if (!inLake && macro.channelOf(at).has_value()) ++missing;
            continue;
        }
        // Only a junction may be shared, and a junction is a node.
        const bool isNode = [&] {
            for (const auto& node : graph.nodes)
                if (node.macroCell == static_cast<std::int32_t>(i)) return true;
            return false;
        }();
        if (found->second > 1 && !isNode) ++interiorSeenTwice;
    }
    CHECK_EQ(missing, std::size_t(0));
    CHECK_EQ(interiorSeenTwice, std::size_t(0));
    }
}

TEST(hydrology_graph_pages_answer_for_every_course_that_reaches_them) {
    using namespace world::streaming;
    for (const std::uint64_t seed : kProbeSeeds) {
    const auto graph = buildHydrologyGraph(generatedWorld(seed));
    CHECK(!graph.spatialPages.empty());

    // Sorted by (y, x), which is what the lookup assumes.
    std::size_t disordered = 0, unfindable = 0;
    for (std::size_t p = 1; p < graph.spatialPages.size(); ++p) {
        const auto previous = graph.spatialPages[p - 1].key;
        const auto current = graph.spatialPages[p].key;
        if (previous.y > current.y || (previous.y == current.y && previous.x >= current.x))
            ++disordered;
    }
    for (const auto& page : graph.spatialPages)
        if (findHydrologySpatialPage(graph, page.key) != &page) ++unfindable;
    CHECK_EQ(disordered, std::size_t(0));
    CHECK_EQ(unfindable, std::size_t(0));

    // Every page a segment's water actually touches lists it.
    std::size_t unindexed = 0;
    for (const auto& segment : graph.segments) {
        const auto first = tileAt(segment.bounds.min);
        const auto last = tileAt(segment.bounds.max);
        for (std::int32_t y = first.y; y <= last.y; ++y)
            for (std::int32_t x = first.x; x <= last.x; ++x) {
                const auto* page = findHydrologySpatialPage(graph, {x, y, 0});
                if (page == nullptr || !std::binary_search(page->segments.begin(),
                                                           page->segments.end(), segment.id))
                    ++unindexed;
            }
    }
    CHECK_EQ(unindexed, std::size_t(0));

    // A page off the map holds nothing rather than the nearest thing to it.
    CHECK(findHydrologySpatialPage(graph, {-9999, -9999, 0}) == nullptr);
    }
}

// ---------------------------------------------------------------------------
// HydrologyGraph on disk.
// ---------------------------------------------------------------------------

namespace {

// A directory of its own per test, removed on the way in and on the way out,
// so one run cannot read what another left behind.
struct ScratchCache {
    std::filesystem::path root;
    explicit ScratchCache(const char* name)
        : root(std::filesystem::temp_directory_path() / (std::string("asr_hydrology_") + name)) {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    ~ScratchCache() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    std::filesystem::path file() const {
        return root / world::streaming::hydrologyCacheRelativePath();
    }
};

} // namespace

TEST(hydrology_cache_round_trips_a_whole_world_graph) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto built = buildHydrologyGraph(world);
    CHECK(!built.segments.empty());

    // The payload alone, so a failure points at the encoding rather than at
    // the file it happened to be in.
    std::vector<std::uint8_t> payload;
    CHECK(encodeHydrologyGraphPayload(built, payload).status == CacheStatus::Ok);
    HydrologyCacheHeader header;
    header.worldSeed = built.worldSeed;
    header.sourceFingerprint = built.sourceFingerprint;
    header.graphVersion = kHydrologyGraphVersion;
    header.macroWidth = built.macroWidth;
    header.macroHeight = built.macroHeight;
    header.macroCellMetres = built.macroCellMetres;
    header.nodeCount = static_cast<std::uint32_t>(built.nodes.size());
    header.segmentCount = static_cast<std::uint32_t>(built.segments.size());
    header.waterBodyCount = static_cast<std::uint32_t>(built.waterBodies.size());
    header.spatialPageCount = static_cast<std::uint32_t>(built.spatialPages.size());
    HydrologyGraph decoded;
    CHECK(decodeHydrologyGraphPayload(header, payload, decoded).status == CacheStatus::Ok);
    CHECK_EQ(digestOf(decoded), digestOf(built));

    // Canonical: encoding what was decoded gives back the same bytes.
    std::vector<std::uint8_t> again;
    CHECK(encodeHydrologyGraphPayload(decoded, again).status == CacheStatus::Ok);
    CHECK_EQ(again, payload);

    // And through a file, which is what a worker actually does.
    const ScratchCache scratch("round_trip");
    CHECK(writeHydrologyGraphCache(scratch.root, built).status == CacheStatus::Ok);
    HydrologyGraph loaded;
    CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint, loaded)
                  .status == CacheStatus::Ok);
    CHECK_EQ(digestOf(loaded), digestOf(built));
    // The page index survives as an index, not only as bytes.
    for (const auto& page : built.spatialPages) {
        const auto* found = findHydrologySpatialPage(loaded, page.key);
        CHECK(found != nullptr);
        if (found == nullptr) break;
        CHECK_EQ(found->segments, page.segments);
        CHECK_EQ(found->waterBodies, page.waterBodies);
    }

    // The whole point of the encoding: a graph costs far less on disk than in
    // memory, where most of the page index is one water body per page.
    const auto onDisk = std::filesystem::file_size(scratch.file());
    std::size_t inMemory = built.nodes.size() * sizeof(RiverNode) +
                           built.segments.size() * sizeof(RiverSegment) +
                           built.waterBodies.size() * sizeof(WaterBody) +
                           built.spatialPages.size() * sizeof(HydrologySpatialPage);
    for (const auto& s : built.segments) inMemory += s.course.size() * sizeof(ReachPoint);
    CHECK(onDisk < inMemory);
}

TEST(hydrology_cache_refuses_a_file_from_another_world) {
    using namespace world::streaming;
    const auto built = buildHydrologyGraph(generatedWorld(11));
    const ScratchCache scratch("other_world");
    HydrologyGraph loaded;
    // Nothing written yet.
    CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint, loaded)
                  .status == CacheStatus::NotFound);

    CHECK(writeHydrologyGraphCache(scratch.root, built).status == CacheStatus::Ok);
    // The same seed regenerated with different macro data must not be reused:
    // that is exactly the silently-different river network the fingerprint is
    // there to prevent.
    CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint ^ 1ull,
                                  loaded)
                  .status == CacheStatus::Incompatible);
    CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed + 1, built.sourceFingerprint,
                                  loaded)
                  .status == CacheStatus::Incompatible);
    // A refused read leaves the caller's graph alone rather than half filled.
    CHECK(loaded.segments.empty());
}

TEST(hydrology_cache_rejects_a_damaged_file_rather_than_streaming_from_it) {
    using namespace world::streaming;
    const auto built = buildHydrologyGraph(generatedWorld(11));

    const auto bytesOf = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<char>((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    };
    const auto putBytes = [](const std::filesystem::path& path, const std::vector<char>& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };

    {   // One flipped payload bit.
        const ScratchCache scratch("flipped");
        CHECK(writeHydrologyGraphCache(scratch.root, built).status == CacheStatus::Ok);
        auto bytes = bytesOf(scratch.file());
        CHECK(bytes.size() > kHydrologyCacheHeaderBytes + 16);
        bytes[kHydrologyCacheHeaderBytes + 7] = static_cast<char>(bytes[kHydrologyCacheHeaderBytes + 7] ^ 0x40);
        putBytes(scratch.file(), bytes);
        HydrologyGraph loaded;
        CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint,
                                      loaded)
                      .status == CacheStatus::Corrupt);
        CHECK(loaded.segments.empty());
    }
    {   // A write that stopped half way.
        const ScratchCache scratch("truncated");
        CHECK(writeHydrologyGraphCache(scratch.root, built).status == CacheStatus::Ok);
        auto bytes = bytesOf(scratch.file());
        bytes.resize(bytes.size() / 2);
        putBytes(scratch.file(), bytes);
        HydrologyGraph loaded;
        CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint,
                                      loaded)
                      .status == CacheStatus::Corrupt);
    }
    {   // A header from something else entirely.
        const ScratchCache scratch("foreign");
        CHECK(writeHydrologyGraphCache(scratch.root, built).status == CacheStatus::Ok);
        auto bytes = bytesOf(scratch.file());
        bytes[3] = 'X';
        putBytes(scratch.file(), bytes);
        HydrologyGraph loaded;
        CHECK(readHydrologyGraphCache(scratch.root, built.worldSeed, built.sourceFingerprint,
                                      loaded)
                      .status == CacheStatus::InvalidHeader);
    }
}

TEST(hydrology_cache_will_not_publish_a_graph_that_does_not_hold_together) {
    using namespace world::streaming;
    auto built = buildHydrologyGraph(generatedWorld(11));
    std::string why;
    CHECK(validateHydrologyGraph(built, &why));

    // A page index that is not sorted cannot be binary-searched, so it must
    // never reach disk: a reader has no way to notice at query time.
    auto unsorted = built;
    CHECK(unsorted.spatialPages.size() > 4);
    std::swap(unsorted.spatialPages[1], unsorted.spatialPages[3]);
    CHECK(!validateHydrologyGraph(unsorted, &why));
    const ScratchCache scratch("inconsistent");
    CHECK(writeHydrologyGraphCache(scratch.root, unsorted).status == CacheStatus::InvalidArgument);
    CHECK(!std::filesystem::exists(scratch.file()));

    // And a dangling link is refused for the same reason: an ID indexes
    // straight into a vector at query time.
    auto dangling = built;
    dangling.segments.front().to = static_cast<RiverNodeId>(dangling.nodes.size() + 99);
    CHECK(!validateHydrologyGraph(dangling, &why));
    CHECK(writeHydrologyGraphCache(scratch.root, dangling).status == CacheStatus::InvalidArgument);
}

// ---------------------------------------------------------------------------
// Baking H_sim into pages.
// ---------------------------------------------------------------------------

namespace {

// The global lattice index a stored sample of a tile stands on. Everything
// about a page's agreement with its neighbours is this function being the
// same on both sides.
std::pair<std::int64_t, std::int64_t> latticeOf(const world::streaming::BaseTile& tile,
                                                std::int64_t column, std::int64_t row) {
    using namespace world::streaming;
    const std::int64_t stride = tile.sampleMetres / world::kSampleMetres;
    const std::int64_t perPage = kPageMetres / tile.sampleMetres;
    return {((static_cast<std::int64_t>(tile.key.x) * perPage - tile.padding) + column) * stride,
            ((static_cast<std::int64_t>(tile.key.y) * perPage - tile.padding) + row) * stride};
}

std::int64_t storedSide(const world::streaming::BaseTile& tile) {
    return static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
}

// Explicit geography for page contracts, independent of continent generator
// revisions. Keep the non-vacuity assertions: these pages contain wet AND dry
// samples, and the lake spans three separate storage pages.
constexpr world::streaming::TileKey kLakePage{21, 32, 0};
constexpr world::streaming::TileKey kCoastPage{28, 22, 0};

const generation::WorldMapData& waterPageFixture() {
    static const auto map = [] {
        generation::WorldMapData world;
        world.width = world.height = 64;
        world.seed = 11;
        world.cells.resize(64 * 64);
        for (auto& cell : world.cells) {
            cell.elevation = 10; cell.moisture = 160; cell.temperature = 130;
        }
        world.lakeRegionField.assign(world.cells.size(), -1);
        world.lakeLevelField.assign(world.cells.size(), 0);
        world.lakeDepthField.assign(world.cells.size(), 0);
        const int lake = 31 * 64 + 21;
        world.lakeRegionField[lake] = lake;
        world.lakeLevelField[lake] = world.cells[lake].elevation = 5;
        world.lakeDepthField[lake] = 3;
        auto foundation = std::make_shared<generation::TerrainFoundation>();
        foundation->columns = foundation->rows =
            (64 * generation::kMetresPerCell + foundation->step - 1) / foundation->step + 1;
        for (auto& plane : foundation->heightDm)
            plane.assign(std::size_t(foundation->columns) * foundation->rows, 900);
        auto& slopes = foundation->heightDm[static_cast<std::size_t>(generation::TerrainStage::Slopes)];
        for (int y = 260; y <= 274; ++y) for (int x = 172; x <= 186; ++x)
            slopes[std::size_t(y) * foundation->columns + x] = 200;
        // A separate sea inlet crossing the coastal page near its midpoint.
        for (int y = 156; y <= 204; ++y) for (int x = 188; x <= 228; ++x)
            slopes[std::size_t(y) * foundation->columns + x] = -200;
        world.terrainFoundation = std::move(foundation);
        return world;
    }();
    return map;
}

// What a page is made of, so a test can say out loud that its subject is not
// a flat field of one value.
struct PageMix {
    std::size_t dry = 0, ocean = 0, lake = 0;
};
PageMix mixOf(const world::streaming::BaseTile& tile) {
    using namespace world::streaming;
    PageMix mix;
    for (const auto id : tile.waterBodyId) {
        if (id == kInvalidWaterBodyId) ++mix.dry;
        else if (id == kOceanWaterBodyId) ++mix.ocean;
        else ++mix.lake;
    }
    return mix;
}

} // namespace

TEST(base_tile_pages_agree_sample_for_sample_where_they_overlap) {
    using namespace world::streaming;
    const auto& world = waterPageFixture();
    const auto graph = buildHydrologyGraph(world);
    const BaseTileBaker baker(world, graph, hsimQuantisationFor(world));

    // A page, the page east of it, and the page south-east: an edge overlap
    // both ways and a corner where four of them meet.
    for (const TileKey origin : {kLakePage, kCoastPage}) {
    const BaseTile here = baker.bake(origin);
    const BaseTile east = baker.bake({origin.x + 1, origin.y, 0});
    const BaseTile south = baker.bake({origin.x, origin.y + 1, 0});
    const BaseTile corner = baker.bake({origin.x + 1, origin.y + 1, 0});
    CHECK(here.valid() && east.valid() && south.valid() && corner.valid());
    // Pages share their edge sample and two of padding either side, so there
    // is something to disagree about in the first place.
    CHECK_EQ(here.width, std::uint16_t(129));
    CHECK_EQ(here.padding, kDefaultPaddingSamples);

    std::map<std::pair<std::int64_t, std::int64_t>, std::array<std::uint16_t, 4>> seen;
    std::size_t overlapping = 0, disagreed = 0;
    for (const BaseTile* tile : {&here, &east, &south, &corner}) {
        const auto side = storedSide(*tile);
        for (std::int64_t row = 0; row < side; ++row)
            for (std::int64_t column = 0; column < side; ++column) {
                const auto at = static_cast<std::size_t>(row * side + column);
                const std::array<std::uint16_t, 4> value{tile->heightQuantized[at],
                                                         tile->waterBodyId[at],
                                                         tile->watershedId[at],
                                                         tile->walkability[at]};
                const auto lattice = latticeOf(*tile, column, row);
                const auto found = seen.find(lattice);
                if (found == seen.end()) {
                    seen.emplace(lattice, value);
                    continue;
                }
                ++overlapping;
                // Where a page has not decided how a body crosses a sample -
                // its outermost ring has no neighbours to take a slope from -
                // there is nothing to disagree about, so the comparison is
                // over what both of them answered.
                auto mine = value, theirs = found->second;
                if (mine[3] == 0 || theirs[3] == 0) mine[3] = theirs[3] = 0;
                if (theirs != mine) ++disagreed;
                if (found->second[3] == 0 && value[3] != 0) found->second = value;
            }
    }
    // Bit for bit, not near enough: a tile owns bytes, not edge truth.
    CHECK(overlapping > 500);
    CHECK_EQ(disagreed, std::size_t(0));

    // The pages also quantise against one range, which is what makes the
    // integers above comparable at all.
    CHECK_EQ(here.elevationMin, east.elevationMin);
    CHECK_EQ(here.elevationMax, corner.elevationMax);
    // And there was a mixture to agree about, not a flat page of one value.
    const PageMix mix = mixOf(here);
    CHECK(mix.dry > 500);
    CHECK(mix.ocean + mix.lake > 500);
    }
}

TEST(base_tile_height_is_the_surface_the_graph_carved) {
    using namespace world::streaming;
    const auto& world = waterPageFixture();
    const auto graph = buildHydrologyGraph(world);
    const auto quantisation = hsimQuantisationFor(world);
    const BaseTileBaker baker(world, graph, quantisation);

    // A coastal page: dry ground on one side, sea on the other, so both
    // branches of the water test are actually walked.
    const BaseTile tile = baker.bake(kCoastPage);
    CHECK(tile.valid());
    const PageMix mix = mixOf(tile);
    CHECK(mix.dry > 3000);
    CHECK(mix.ocean > 3000);

    // The page stores what the carver said, to within what sixteen bits over
    // a world can hold. Nothing is compared against HeightField here: the
    // legacy surface is what this replaces, not what it has to match.
    const GraphCarver carver(graph, tileBounds(kCoastPage), 800 + 4 * (tile.padding + 1));
    world::HeightField field(&world, world.seed);
    const auto side = storedSide(tile);
    const core::Fixed tolerance = quantisation.resolution();
    std::size_t offBy = 0, clamped = 0, floating = 0;
    core::Fixed worst{};
    for (std::int64_t row = 0; row < side; ++row)
        for (std::int64_t column = 0; column < side; ++column) {
            const auto at = static_cast<std::size_t>(row * side + column);
            const auto [sx, sy] = latticeOf(tile, column, row);
            const core::WorldPos p{core::Fixed::fromInt(sx * world::kSampleMetres),
                                   core::Fixed::fromInt(sy * world::kSampleMetres)};
            const auto pieces = field.piecesAt(p.x, p.y);
            const CarvedSample carved = carver.carve(p, pieces.country, pieces.moved);
            if (carved.floor < quantisation.low || carved.floor > quantisation.high) ++clamped;
            const core::Fixed stored = quantisation.dequantise(tile.heightQuantized[at]);
            const core::Fixed error = core::abs(stored - carved.floor);
            worst = core::max(worst, error);
            if (error > tolerance) ++offBy;

            // The property one call buys that two could not: a sample named
            // as water stands under that water. Ground and surface come out
            // of the same carve, so a bed cannot end up above its own
            // surface - which is what a floating lake is.
            if (tile.waterBodyId[at] != kInvalidWaterBodyId &&
                !(carved.wet && carved.floor < carved.surface))
                ++floating;
        }
    CHECK_EQ(clamped, std::size_t(0));
    CHECK_EQ(offBy, std::size_t(0));
    CHECK_EQ(floating, std::size_t(0));
    CHECK(worst <= tolerance);
    CHECK(tolerance < core::Fixed::ratio(1, 10));   // better than ten centimetres
}

TEST(base_tile_envelope_holds_the_whole_world) {
    using namespace world::streaming;
    for (const std::uint64_t seed : {std::uint64_t(11), std::uint64_t(7)}) {
        const auto& world = generatedWorld(seed);
        const auto quantisation = hsimQuantisationFor(world);
        CHECK(quantisation.valid());
        world::HeightField field(&world, world.seed);
        // Every ninth lattice sample across the whole map, which is a
        // different set of points from any one page's.
        const std::int64_t last =
                (static_cast<std::int64_t>(world.width) * generation::kMetresPerCell) /
                world::kSampleMetres;
        std::size_t outside = 0;
        for (std::int64_t sy = -4; sy <= last + 4; sy += 9)
            for (std::int64_t sx = -4; sx <= last + 4; sx += 9) {
                const core::Fixed height = field.sampleHeight(sx, sy);
                if (height < quantisation.low || height > quantisation.high) ++outside;
            }
        CHECK_EQ(outside, std::size_t(0));
    }
}

TEST(base_tile_coarser_pages_subsample_the_same_surface) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    const BaseTileBaker baker(world, graph, hsimQuantisationFor(world));

    // Geometry LOD approximates one simulation surface; it does not sample a
    // different one. A page at eight metres has to be every other sample of
    // the page at four, exactly.
    const BaseTile fine = baker.bake(kLakePage, 4);
    const BaseTile coarse = baker.bake(kLakePage, 8);
    CHECK(fine.valid() && coarse.valid());
    CHECK_EQ(coarse.width, std::uint16_t(65));

    std::map<std::pair<std::int64_t, std::int64_t>, std::uint16_t> fineByLattice;
    const auto fineSide = storedSide(fine);
    for (std::int64_t row = 0; row < fineSide; ++row)
        for (std::int64_t column = 0; column < fineSide; ++column)
            fineByLattice[latticeOf(fine, column, row)] =
                    fine.heightQuantized[static_cast<std::size_t>(row * fineSide + column)];

    const auto coarseSide = storedSide(coarse);
    std::size_t compared = 0, disagreed = 0;
    for (std::int64_t row = 0; row < coarseSide; ++row)
        for (std::int64_t column = 0; column < coarseSide; ++column) {
            const auto found = fineByLattice.find(latticeOf(coarse, column, row));
            if (found == fineByLattice.end()) continue;   // outside the fine page's padding
            ++compared;
            if (found->second !=
                coarse.heightQuantized[static_cast<std::size_t>(row * coarseSide + column)])
                ++disagreed;
        }
    CHECK(compared > 4000);
    CHECK_EQ(disagreed, std::size_t(0));
}

TEST(base_tile_bakes_the_same_bytes_on_any_worker) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    const auto quantisation = hsimQuantisationFor(world);
    const TileKey key = kCoastPage;

    const BaseTileBaker first(world, graph, quantisation);
    const BaseTile once = first.bake(key);
    CHECK(once.valid());

    // A baker is a worker's private object, so four of them on four threads
    // have to produce one answer or the cache is not shareable.
    std::array<BaseTile, 4> elsewhere;
    std::vector<std::thread> workers;
    for (std::size_t n = 0; n < elsewhere.size(); ++n)
        workers.emplace_back([&, n] {
            const BaseTileBaker mine(world, graph, quantisation);
            elsewhere[n] = mine.bake(key);
        });
    for (auto& worker : workers) worker.join();
    for (const BaseTile& other : elsewhere) {
        CHECK_EQ(other.heightQuantized, once.heightQuantized);
        CHECK_EQ(other.waterBodyId, once.waterBodyId);
        CHECK_EQ(other.watershedId, once.watershedId);
    }
    // And twice from one baker, which is the cheaper half of the same claim.
    CHECK_EQ(first.bake(key).heightQuantized, once.heightQuantized);
}

TEST(base_tile_survives_the_cache_it_will_be_streamed_from) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    const BaseTileBaker baker(world, graph, hsimQuantisationFor(world));
    const BaseTile tile = baker.bake(kCoastPage);
    CHECK(tile.valid());
    CHECK(validateBaseTile(tile).status == CacheStatus::Ok);

    // Walkability and buildability are absent, and the record says so rather
    // than inventing a column nobody decided.
    const auto descriptor = describeBaseTile(tile);
    CHECK(containsChannel(descriptor.channels, BaseTileChannel::Height));
    CHECK(containsChannel(descriptor.channels, BaseTileChannel::WaterBody));
    CHECK(containsChannel(descriptor.channels, BaseTileChannel::Watershed));
    CHECK(containsChannel(descriptor.channels, BaseTileChannel::Walkability));
    // Buildability has no owner in the game yet, so the record says so
    // rather than inventing a column nobody decided.
    CHECK(!containsChannel(descriptor.channels, BaseTileChannel::Buildability));

    const ScratchCache scratch("base_tile");
    const CacheIdentity identity{world.seed, graph.sourceFingerprint};
    CHECK(writeBaseTileCache(scratch.root, identity, tile).status == CacheStatus::Ok);
    BaseTile loaded;
    CHECK(readBaseTileCache(scratch.root, identity, tile.key, loaded).status == CacheStatus::Ok);
    CHECK_EQ(loaded.heightQuantized, tile.heightQuantized);
    CHECK_EQ(loaded.waterBodyId, tile.waterBodyId);
    CHECK_EQ(loaded.watershedId, tile.watershedId);
    CHECK_EQ(loaded.walkability, tile.walkability);
    CHECK(loaded.buildability.empty());
    CHECK_EQ(loaded.elevationMin, tile.elevationMin);
    CHECK_EQ(loaded.elevationMax, tile.elevationMax);
    // Another world's cache is a miss, not a page of someone else's terrain.
    CHECK(readBaseTileCache(scratch.root, {world.seed, graph.sourceFingerprint ^ 1ull}, tile.key,
                            loaded)
                  .status == CacheStatus::Incompatible);
}

TEST(base_tile_baking_happens_on_a_worker_and_never_on_the_frame) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    const auto submitting = std::this_thread::get_id();
    std::atomic_bool onTheFrame = false;

    auto build = makeBaseTileBuildFunction(world, graph, hsimQuantisationFor(world));
    TerrainWorkerRuntime runtime({2, 16, 16},
                                 [&](const TerrainStreamRequest& request,
                                     const TerrainCancellation& cancellation) {
                                     if (std::this_thread::get_id() == submitting)
                                         onTheFrame.store(true);
                                     return build(request, cancellation);
                                 });

    std::vector<TerrainStreamCompletion> ready;
    for (std::int32_t n = 0; n < 4; ++n)
        CHECK(runtime.submit(
                {{1, {kCoastPage.x + n, kCoastPage.y, 0}, productMask(TileProduct::Base)}, 1, 0}));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (ready.size() < 4 && std::chrono::steady_clock::now() < deadline) {
        for (auto& done : runtime.takeReady(4)) ready.push_back(std::move(done));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!onTheFrame.load());
    CHECK_EQ(ready.size(), std::size_t(4));
    for (const auto& done : ready) {
        CHECK(done.result.base != nullptr);
        if (done.result.base == nullptr) continue;
        CHECK(done.result.base->valid());
        CHECK_EQ(done.result.base->key, done.result.key.tile);
    }

    // A request for something this function does not make is declined rather
    // than answered with an empty tile.
    std::atomic_bool declined = false;
    TerrainWorkerRuntime residuals({1, 4, 4},
                                   [&](const TerrainStreamRequest& request,
                                       const TerrainCancellation& cancellation) {
                                       const auto made = build(request, cancellation);
                                       if (!made) declined.store(true);
                                       return made;
                                   });
    CHECK(residuals.submit({{1, kCoastPage, productMask(TileProduct::FineResidual)}, 1, 0}));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!declined.load() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(declined.load());
}

TEST(base_tile_a_lake_keeps_one_name_across_every_page_it_reaches) {
    using namespace world::streaming;
    const auto& world = waterPageFixture();
    const auto graph = buildHydrologyGraph(world);
    const BaseTileBaker baker(world, graph, hsimQuantisationFor(world));

    // The fixture's lake spans three pages. Sampled from three
    // different bakes, it has to come back as one body: a lake that is body 4
    // in one page and body 7 in the next is the fragmented water this
    // storage exists to remove.
    std::map<std::uint16_t, std::size_t> named;
    for (const TileKey key : {TileKey{21, 32, 0}, TileKey{22, 32, 0}, TileKey{22, 33, 0}}) {
        const BaseTile tile = baker.bake(key);
        CHECK(tile.valid());
        for (const auto id : tile.waterBodyId)
            if (id != kInvalidWaterBodyId && id != kOceanWaterBodyId) ++named[id];
    }
    CHECK_EQ(named.size(), std::size_t(1));
    if (named.empty()) return;
    const WaterBodyId lake = named.begin()->first;
    CHECK(named.begin()->second > 10000);

    // And the name means something: it is the body the graph knows, standing
    // at one head, whatever page asked.
    const auto* body = waterBodyOf(graph, lake);
    CHECK(body != nullptr);
    if (body == nullptr) return;
    CHECK(body->kind == WaterBodyKind::Lake);
    CHECK(body->level > core::kZero);
}


TEST(base_tile_a_lake_is_a_lake_and_not_a_lawn) {
    using namespace world::streaming;
    const auto& world = waterPageFixture();
    const auto graph = buildHydrologyGraph(world);
    const auto quantisation = hsimQuantisationFor(world);
    const BaseTileBaker baker(world, graph, quantisation);

    // The generator raises a basin to its outlet and records how far, exactly
    // so the floor can be had back. Nothing did: the ground was brought up to
    // the waterline instead and every lake came out with a millimetre of
    // water on flat ground. Giving the floor back is the point of this.
    const TileKey key{22, 33, 0};
    const BaseTile tile = baker.bake(key);
    CHECK(tile.valid());
    const GraphCarver carver(graph, tileBounds(key), 812);
    world::HeightField field(&world, world.seed);

    // Natural basin depths are bounded by the uncarved foundation, not by
    // macro fill metadata. Compare against that independent source below.
    core::Fixed deepest{};

    const auto side = storedSide(tile);
    std::size_t samples = 0, floating = 0, tooDeep = 0;
    core::Fixed total{}, worst{};
    for (std::int64_t row = 0; row < side; ++row)
        for (std::int64_t column = 0; column < side; ++column) {
            const auto at = static_cast<std::size_t>(row * side + column);
            const auto id = tile.waterBodyId[at];
            if (id == kInvalidWaterBodyId || id == kOceanWaterBodyId) continue;
            const auto [sx, sy] = latticeOf(tile, column, row);
            const core::WorldPos p{core::Fixed::fromInt(sx * world::kSampleMetres),
                                   core::Fixed::fromInt(sy * world::kSampleMetres)};
            const auto pieces = field.piecesAt(p.x, p.y);
            const CarvedSample carved = carver.carve(p, pieces.country, pieces.moved);
            const auto* body = waterBodyOf(graph, id);
            CHECK(body != nullptr);
            if (!body) continue;
            const auto naturalDepth = body->level - (pieces.country + pieces.moved);
            deepest = core::max(deepest, naturalDepth);
            CHECK_EQ(carved.floor, pieces.country + pieces.moved);
            ++samples;
            const core::Fixed depth = carved.surface - carved.floor;
            total += depth;
            worst = core::max(worst, depth);
            if (depth <= core::kZero) ++floating;    // a bed above its own surface
            if (depth > naturalDepth) ++tooDeep;     // the basin dug twice
        }
    CHECK(samples > 10000);
    CHECK_EQ(floating, std::size_t(0));
    CHECK_EQ(tooDeep, std::size_t(0));
    if (samples == 0) return;
    CHECK(deepest > core::Fixed::fromInt(10));
    // Deep enough to be water rather than a wet lawn, without artificial digging.
    const core::Fixed mean = total / static_cast<std::int64_t>(samples);
    CHECK(mean > core::Fixed::fromInt(10));
    CHECK(worst <= deepest);
}

TEST(hydrology_course_is_a_river_and_not_the_lattice) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    CHECK(!graph.segments.empty());

    std::size_t unjoined = 0, tooCoarse = 0, wet = 0, fordable = 0, deep = 0;
    core::Fixed furthest{};
    for (const auto& segment : graph.segments) {
        // A course begins and ends exactly on its nodes. Anything less and
        // two reaches meeting at a junction pull apart, which is a river
        // drawn in dashes.
        const auto* from = riverNodeOf(graph, segment.from);
        const auto* to = riverNodeOf(graph, segment.to);
        if (from == nullptr || segment.course.front().position != from->position) ++unjoined;
        if (to != nullptr && segment.course.back().position != to->position) ++unjoined;

        for (std::size_t n = 0; n < segment.course.size(); ++n) {
            const ReachPoint& point = segment.course[n];
            // How far the water actually runs from the cell centre it was
            // compressed from: a course that stays on them draws the macro
            // map's own lattice across the country.
            const auto cell = cellOfPoint(point.position);
            const auto centre = core::WorldPos{
                    (core::Fixed::fromInt(cell.x) + core::Fixed::ratio(1, 2)) *
                            core::Fixed::fromInt(generation::kMetresPerCell),
                    (core::Fixed::fromInt(cell.y) + core::Fixed::ratio(1, 2)) *
                            core::Fixed::fromInt(generation::kMetresPerCell)};
            furthest = core::max(furthest, core::distance(point.position, centre));

            // Smooth and stepped finely enough to be a curve rather than a
            // chain of bearings: no chord longer than a macro cell, and no
            // corner between two of them. A polyline that turns sharply is
            // the lattice showing through under another name.
            if (n > 0) {
                const core::Fixed step =
                        core::distance(point.position, segment.course[n - 1].position);
                if (step > core::Fixed::fromInt(generation::kMetresPerCell)) ++tooCoarse;
            }
            if (n > 1) {
                const auto leg = [](core::WorldPos a, core::WorldPos b) {
                    const core::Fixed dx = b.x - a.x, dy = b.y - a.y;
                    const core::Fixed length = core::max(core::hypot(dx, dy),
                                                         core::Fixed::ratio(1, 1000));
                    return core::WorldPos{dx / length, dy / length};
                };
                const auto before = leg(segment.course[n - 2].position,
                                        segment.course[n - 1].position);
                const auto after = leg(segment.course[n - 1].position, point.position);
                if (before.x * after.x + before.y * after.y < core::Fixed::ratio(7, 10))
                    ++tooCoarse;   // turning by more than forty-five degrees
            }
            if (point.halfWidth <= core::kZero) continue;
            ++wet;
            if (point.depth <= world::fordableDepth()) ++fordable;
            if (point.depth > world::fordableDepth() * 2) ++deep;
        }
    }
    CHECK_EQ(unjoined, std::size_t(0));
    CHECK_EQ(tooCoarse, std::size_t(0));
    // The reach wanders: measured on seed 11 it leaves a cell centre by
    // hundreds of metres, which is what takes the eight-direction lattice out
    // of the rivers.
    CHECK(furthest > core::Fixed::fromInt(100));

    // And it is not one depth along its length. A channel of one depth is a
    // fence: at a ford a cart gets over and out of its depth it does not, so
    // a network of even channels cuts the country into pieces.
    CHECK(wet > 200);
    CHECK(fordable > wet / 20);
    CHECK(deep > wet / 20);

    // A spring grows out of its hillside rather than a hole appearing in it.
    std::size_t springs = 0, pits = 0;
    for (const auto& segment : graph.segments) {
        const auto* from = riverNodeOf(graph, segment.from);
        if (from == nullptr || from->kind != RiverNodeKind::Source) continue;
        ++springs;
        if (segment.course.front().valleyReach >= segment.course.back().valleyReach) ++pits;
    }
    CHECK(springs > 10);
    CHECK_EQ(pits, std::size_t(0));
}

TEST(base_tile_says_how_a_body_crosses_it_from_its_own_lattice) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    const auto quantisation = hsimQuantisationFor(world);
    const BaseTileBaker baker(world, graph, quantisation);

    for (const TileKey key : {kCoastPage, kLakePage}) {
        const BaseTile tile = baker.bake(key);
        CHECK(tile.valid());
        CHECK_EQ(tile.walkability.size(), tile.sampleCount());
        const auto side = storedSide(tile);

        std::size_t undecided = 0, wrongDepth = 0, walkable = 0, crossable = 0;
        for (std::int64_t row = 1; row + 1 < side; ++row)
            for (std::int64_t column = 1; column + 1 < side; ++column) {
                const auto at = static_cast<std::size_t>(row * side + column);
                if (!travelDecided(tile.walkability[at])) {
                    ++undecided;
                    continue;
                }
                const world::Travel travel = decodeTravel(tile.walkability[at]);
                // Water is classified by how deep it is, not by whether there
                // is any: a stream a body wades is not a wall.
                //
                // Only one direction can be asserted here, and the reason is
                // worth writing down. `waterBodyId` names standing water -
                // the ocean and lakes - and a river belongs to no body,
                // because a river is a reach. So a sample under a body must
                // be crossed as water, while a sample crossed as water need
                // not carry a body: most of those are rivers. Anything
                // reading a page for "is this wet" wants this channel, not
                // that one.
                if (tile.waterBodyId[at] != kInvalidWaterBodyId &&
                    travel != world::Travel::Ford && travel != world::Travel::Swim &&
                    travel != world::Travel::None)
                    ++wrongDepth;
                if (travel == world::Travel::Walk) ++walkable;
                if (travel != world::Travel::None) ++crossable;
            }
        // Every interior sample is answered; only the outermost ring, which
        // has no neighbours of its own, is left undecided.
        CHECK_EQ(undecided, std::size_t(0));
        CHECK_EQ(wrongDepth, std::size_t(0));
        (void)walkable;   // how much is level is the country's business, not this test's

        // And the country is not in pieces. Treating every watercourse as a
        // wall cut this world into three to six pieces a square mile; a ford
        // is what joins them, so most of what can be crossed at all has to be
        // reachable from one place.
        std::vector<std::int32_t> component(static_cast<std::size_t>(side * side), -1);
        std::size_t largest = 0, components = 0;
        std::vector<std::int64_t> queue;
        for (std::int64_t row = 1; row + 1 < side; ++row)
            for (std::int64_t column = 1; column + 1 < side; ++column) {
                const auto start = row * side + column;
                if (component[static_cast<std::size_t>(start)] >= 0) continue;
                const auto passable = [&](std::int64_t index) {
                    const auto stored = tile.walkability[static_cast<std::size_t>(index)];
                    return travelDecided(stored) && decodeTravel(stored) != world::Travel::None;
                };
                if (!passable(start)) continue;
                ++components;
                queue.assign(1, start);
                component[static_cast<std::size_t>(start)] = static_cast<std::int32_t>(components);
                for (std::size_t head = 0; head < queue.size(); ++head) {
                    const auto index = queue[head];
                    const std::int64_t r = index / side, c = index % side;
                    for (const auto& step : {std::pair<std::int64_t, std::int64_t>{0, 1},
                                             {0, -1}, {1, 0}, {-1, 0}}) {
                        const std::int64_t nr = r + step.first, nc = c + step.second;
                        if (nr < 1 || nc < 1 || nr + 1 >= side || nc + 1 >= side) continue;
                        const auto next = nr * side + nc;
                        if (component[static_cast<std::size_t>(next)] >= 0) continue;
                        if (!passable(next)) continue;
                        component[static_cast<std::size_t>(next)] =
                                static_cast<std::int32_t>(components);
                        queue.push_back(next);
                    }
                }
                largest = std::max(largest, queue.size());
            }
        CHECK(crossable > 1000);
        CHECK(components > 0);
        // Four fifths of everything crossable in one piece. Measured on seed
        // 11: the coastal page comes out whole and the lake page at nine
        // tenths, the remainder being ground the lake cuts off from the rest.
        CHECK(largest * 5 >= crossable * 4);
    }
}

TEST(page_store_bakes_a_page_once_and_keeps_it) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    // A mixed working set now includes each H4 page's H8/H16 parents. Bound
    // bytes, not a fixed page count: the three levels have different footprints.
    constexpr std::size_t budget = 4 * 460u * 1024u;
    PageStore store(world, graph, hsimQuantisationFor(world),
                    {budget, kDefaultPaddingSamples});

    const auto first = store.page(kCoastPage);
    CHECK(first != nullptr);
    if (first == nullptr) return;
    CHECK(first->base.valid());
    CHECK(first->water.valid());
    CHECK_EQ(store.stats().baked, std::size_t(3)); // H16 -> H8 -> H4
    CHECK(store.resident({kCoastPage.x, kCoastPage.y, 1}) != nullptr);
    CHECK(store.resident({kCoastPage.x, kCoastPage.y, 2}) != nullptr);

    // Asked again it is the same object, not the same bytes derived twice: a
    // bake is tens of milliseconds and a ring asks for one page from several
    // jobs at once.
    const auto again = store.page(kCoastPage);
    CHECK(again == first);
    CHECK_EQ(store.stats().baked, std::size_t(3));
    CHECK(store.stats().hits > 0);

    // The frame thread may look but never bake.
    CHECK(store.resident(kCoastPage) == first);
    CHECK(store.resident({kCoastPage.x + 40, kCoastPage.y, 0}) == nullptr);
    CHECK_EQ(store.stats().baked, std::size_t(3));

    // Residency is bounded, and the page asked for longest ago is the one
    // that goes: holding every page a session touched would grow without
    // limit for no gain.
    for (std::int32_t n = 1; n <= 4; ++n) CHECK(store.page({kCoastPage.x + n, kCoastPage.y, 0}) != nullptr);
    const auto after = store.stats();
    CHECK_EQ(after.baked, std::size_t(15));
    CHECK_EQ(after.resident + after.evicted, after.baked);
    CHECK(after.residentBytes <= budget);
    CHECK(store.resident({kCoastPage.x + 4, kCoastPage.y, 0}) != nullptr);
    CHECK(after.evicted > 0);
    CHECK(store.resident(kCoastPage) == nullptr);   // the oldest went first

    // A rectangle of world becomes the pages under it, which is what a ring
    // or a frustum turns into before anything can be read.
    const auto keys = store.keysOverlapping(tileBounds(kCoastPage, 600));
    CHECK(keys.size() >= std::size_t(9));
    CHECK(std::find(keys.begin(), keys.end(), kCoastPage) != keys.end());
}

TEST(page_store_gives_every_worker_the_same_page) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    PageStore store(world, graph, hsimQuantisationFor(world),
                    {16 * 460u * 1024u, kDefaultPaddingSamples});

    // Four workers asking for the same four pages at once. Two of them may
    // bake one page - that waste is bounded and counted - but no two answers
    // may differ, or a page is not a shareable thing.
    std::array<std::vector<std::shared_ptr<const BakedPage>>, 4> got;
    std::vector<std::thread> workers;
    const std::array<TileKey, 4> wanted{kCoastPage,
                                        TileKey{kCoastPage.x + 1, kCoastPage.y, 0},
                                        kLakePage,
                                        TileKey{kLakePage.x, kLakePage.y + 1, 0}};
    for (std::size_t n = 0; n < got.size(); ++n)
        workers.emplace_back([&, n] {
            for (const TileKey key : wanted) got[n].push_back(store.page(key));
        });
    for (auto& worker : workers) worker.join();

    std::size_t missing = 0, differed = 0;
    for (std::size_t which = 0; which < wanted.size(); ++which) {
        const auto& reference = got[0][which];
        if (reference == nullptr) {
            ++missing;
            continue;
        }
        for (std::size_t n = 1; n < got.size(); ++n) {
            const auto& theirs = got[n][which];
            if (theirs == nullptr) {
                ++missing;
                continue;
            }
            if (theirs->base.heightQuantized != reference->base.heightQuantized ||
                theirs->base.walkability != reference->base.walkability ||
                theirs->water.surfaceQuantized != reference->water.surfaceQuantized)
                ++differed;
        }
    }
    CHECK_EQ(missing, std::size_t(0));
    CHECK_EQ(differed, std::size_t(0));
    CHECK_EQ(store.stats().resident, wanted.size() * 3); // includes H8/H16 parents
    for (const auto key : wanted)
        for (const std::uint8_t level : {0, 1, 2}) CHECK(store.resident({key.x, key.y, level}) != nullptr);
}

TEST(page_store_answers_every_level_from_one_surface) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);
    const auto graph = buildHydrologyGraph(world);
    // Enough for the handful of pages this test asks for and no more.
    PageStore store(world, graph, hsimQuantisationFor(world),
                    {8u << 20, kDefaultPaddingSamples});

    // A ring reads the level its distance earns. Every level has to be a
    // strict subsample of the one under it, or the morph between two rings is
    // a dissolve between two different grounds rather than a morph.
    CHECK_EQ(PageStore::sampleMetresAtLevel(0), world::kSampleMetres);
    CHECK_EQ(PageStore::sampleMetresAtLevel(3), world::kSampleMetres << 3);
    CHECK_EQ(PageStore::sampleMetresAtLevel(200), 0);   // no such level

    const auto fine = store.page({kCoastPage.x, kCoastPage.y, 0});
    CHECK(fine != nullptr);
    if (fine == nullptr) return;

    std::map<std::pair<std::int64_t, std::int64_t>, std::uint16_t> byLattice;
    const auto sideOf = [](const BaseTile& tile) {
        return static_cast<std::int64_t>(tile.width) + 2 * tile.padding;
    };
    const auto fineSide = sideOf(fine->base);
    for (std::int64_t row = 0; row < fineSide; ++row)
        for (std::int64_t column = 0; column < fineSide; ++column)
            byLattice[latticeOf(fine->base, column, row)] =
                    fine->base.heightQuantized[static_cast<std::size_t>(row * fineSide + column)];

    std::size_t compared = 0, disagreed = 0;
    for (std::uint8_t level = 1; level <= 3; ++level) {
        const auto coarse = store.page({kCoastPage.x, kCoastPage.y, level});
        CHECK(coarse != nullptr);
        if (coarse == nullptr) continue;
        CHECK_EQ(coarse->base.sampleMetres, PageStore::sampleMetresAtLevel(level));
        CHECK(coarse->base.valid() && coarse->water.valid());
        const auto side = sideOf(coarse->base);
        for (std::int64_t row = 0; row < side; ++row)
            for (std::int64_t column = 0; column < side; ++column) {
                const auto found = byLattice.find(latticeOf(coarse->base, column, row));
                if (found == byLattice.end()) continue;   // outside the fine page's padding
                ++compared;
                if (found->second !=
                    coarse->base.heightQuantized[static_cast<std::size_t>(row * side + column)])
                    ++disagreed;
            }
    }
    CHECK(compared > 1000);
    CHECK_EQ(disagreed, std::size_t(0));

    // And the levels are separate pages, not one page reinterpreted.
    CHECK_EQ(store.stats().resident, std::size_t(4));
}

TEST(page_store_bakers_do_not_outlive_the_graph_they_were_made_for) {
    using namespace world::streaming;
    const auto& world = generatedWorld(11);

    // Two stores over one world, each with its own graph, one after the
    // other. The bakers used to be kept in a thread_local keyed on the world
    // alone, so the second store was handed a baker still pointing at the
    // first store's graph - which by then had gone. It crashed.
    std::vector<std::uint16_t> first;
    {
        const auto graph = buildHydrologyGraph(world);
        // Room for four pages of four metres and no fifth: one is 444 KB, so the
    // budget is in bytes rather than in a count - a page is not a fixed size.
    PageStore store(world, graph, hsimQuantisationFor(world),
                    {4 * 460u * 1024u, kDefaultPaddingSamples});
        const auto page = store.page(kCoastPage);
        CHECK(page != nullptr);
        if (page == nullptr) return;
        first = page->base.heightQuantized;
        CHECK(!first.empty());
    }
    {
        const auto graph = buildHydrologyGraph(world);
        // Room for four pages of four metres and no fifth: one is 444 KB, so the
    // budget is in bytes rather than in a count - a page is not a fixed size.
    PageStore store(world, graph, hsimQuantisationFor(world),
                    {4 * 460u * 1024u, kDefaultPaddingSamples});
        const auto page = store.page(kCoastPage);
        CHECK(page != nullptr);
        if (page == nullptr) return;
        // And the same world gives the same ground, whichever store asked.
        CHECK_EQ(page->base.heightQuantized, first);
    }
}
