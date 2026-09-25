#include "game/generation/local_map_gen.hpp"

#include "game/generation/world_map_gen.hpp"

#include <algorithm>
#include <vector>

#include "engine/core/hash.hpp"
#include "game/simulation/world.hpp"

namespace generation {
namespace {

using core::Fixed;
using core::TilePos;

// Integer value noise. Deterministic on every platform - no trig, no floats.
// Returns 0..1023.
std::int32_t valueNoise(std::uint64_t seed, std::int32_t x, std::int32_t y) {
    std::uint64_t h = seed;
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(x)) * 0x9e3779b97f4a7c15ULL));
    h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(y)) * 0xc2b2ae3d27d4eb4fULL));
    return static_cast<std::int32_t>(h & 1023);
}

// Smoothed noise: average the lattice values over a cell so blobs form instead of
// per-tile static. `scale` is the cell size in tiles.
std::int32_t smoothNoise(std::uint64_t seed, std::int32_t x, std::int32_t y, std::int32_t scale) {
    const std::int32_t cx = x / scale;
    const std::int32_t cy = y / scale;
    const std::int32_t fx = x % scale;
    const std::int32_t fy = y % scale;

    const std::int32_t v00 = valueNoise(seed, cx, cy);
    const std::int32_t v10 = valueNoise(seed, cx + 1, cy);
    const std::int32_t v01 = valueNoise(seed, cx, cy + 1);
    const std::int32_t v11 = valueNoise(seed, cx + 1, cy + 1);

    // Bilinear in integers.
    const std::int32_t top = v00 + (v10 - v00) * fx / scale;
    const std::int32_t bottom = v01 + (v11 - v01) * fx / scale;
    return top + (bottom - top) * fy / scale;
}

// Two octaves is enough texture for a local map and stays cheap.
std::int32_t fractalNoise(std::uint64_t seed, std::int32_t x, std::int32_t y) {
    return (smoothNoise(seed, x, y, 24) * 2 + smoothNoise(seed + 1, x, y, 8)) / 3;
}

} // namespace

Biome parseBiome(std::string_view name) {
    return name == "river_valley" ? Biome::RiverValley : Biome::Temperate;
}

LocalMapParams presetFor(Biome biome, LocalMapParams p) {
    p.biome = biome;
    if (biome == Biome::RiverValley) {
        // A wide, braided river with marsh either side and dry country beyond.
        // Clay and reed everywhere, timber and stone almost nowhere: the reason
        // Mesopotamia built in brick.
        p.riverHalfWidth = 4;
        p.fordSpacing = 34;
        p.rockPercent = 1;
        p.orePercent = 1;
        p.clayPercent = 14;
        p.gamePercent = 2;
    }
    return p;
}

LocalMapParams shapedByCountry(std::int32_t climate, LocalMapParams p) {
    // Percentages of what the biome preset already decided. A country is not a
    // different generator - the ground is still built the same way - it is how
    // much of each thing that ground carries.
    struct Shape {
        std::int32_t wood, rock, food, grain, game, clay, reed;
        std::int32_t grass;    // how much of the ground is under grass, 0..100
    };
    // In the order of generation::Climate. Ice and alpine are nearly bare;
    // taiga is all timber and no grain; steppe is grass with almost no wood;
    // desert is rock and nothing else; a delta is reed and silt with no stone
    // in it at all.
    static const Shape kShapes[] = {
            {  5,  60,  10,   5,  20,  20,   0,   5},   // Ice
            { 15,  90,  35,  10,  60,  40,  10,  35},   // Tundra
            {190,  90,  70,  25,  90,  50,  20,  45},   // Taiga
            {140,  80, 120,  90, 110,  70,  40,  55},   // TemperateForest
            { 25,  70,  80, 130,  90,  60,  30,  85},   // Steppe: grass to the horizon
            { 80, 110, 130, 100,  70,  70,  40,  50},   // Mediterranean
            {  5, 120,  15,  15,  20, 110,   5,   8},   // Desert
            { 35,  70,  70, 110,  90,  80,  25,  70},   // Savanna
            {210,  40, 150,  50, 120,  60,  60,  40},   // TropicalForest
            { 20, 190,  20,  10,  40,  30,   5,  20},   // Alpine
            { 70,  60, 120, 140,  70, 120, 110,  35},   // RiverValley: green by the water
            { 45,  10, 130, 150,  60, 150, 160,  40},   // Delta
    };
    const std::size_t kCount = sizeof(kShapes) / sizeof(kShapes[0]);
    if (climate < 0 || static_cast<std::size_t>(climate) >= kCount) return p;
    p.climate = climate;
    const Shape& shape = kShapes[static_cast<std::size_t>(climate)];

    const auto scale = [](std::int32_t value, std::int32_t percent) {
        return std::clamp(value * percent / 100, 0, 95);
    };
    p.forestPercent = scale(p.forestPercent, shape.wood);
    p.fallenWoodPercent = scale(p.fallenWoodPercent, shape.wood);
    p.datePalmPercent = scale(p.datePalmPercent, shape.wood);
    p.tamariskPercent = scale(p.tamariskPercent, shape.wood);
    p.rockPercent = scale(p.rockPercent, shape.rock);
    p.orePercent = scale(p.orePercent, shape.rock);
    p.berryPercent = scale(p.berryPercent, shape.food);
    p.wildGrainPercent = scale(p.wildGrainPercent, shape.grain);
    p.emmerPercent = scale(p.emmerPercent, shape.grain);
    p.gamePercent = scale(p.gamePercent, shape.game);
    p.clayPercent = scale(p.clayPercent, shape.clay);
    p.reedPercent = scale(p.reedPercent, shape.reed);
    p.grassiness = shape.grass;

    // And the cell's own rain on top of that: the same country is greener on
    // its wet side than on its dry one.
    const std::int32_t wet = 70 + p.moisture * 60 / 255;               // 70..130 percent
    p.forestPercent = scale(p.forestPercent, wet);
    p.berryPercent = scale(p.berryPercent, wet);
    p.wildGrainPercent = scale(p.wildGrainPercent, wet);
    p.emmerPercent = scale(p.emmerPercent, wet);

    // A community has to be able to make its first tool, whatever country it
    // drew. This is the oldest trap in this project: a map with no stone on it
    // cannot start, and it fails silently three days in.
    if (p.rockPercent < 1) p.rockPercent = 1;
    return p;
}

std::vector<std::string> resourceNamesFor(Biome biome) {
    if (biome == Biome::RiverValley)
        return {"reed_bed",   "date_palm",   "tamarisk", "wild_emmer", "wild_flax",
                "clay_bank",  "stone_outcrop", "copper_vein", "berry_bush",
                "fallen_branch", "yarrow", "willow_stand"};
    return {"fallen_branch", "oak_tree",  "stone_outcrop", "berry_bush", "wild_einkorn",
            "wild_flax",     "clay_bank", "copper_vein",   "tin_vein",  "yarrow",
            "willow_stand"};
}

// The course of the river through this map, as a distance field: for every tile,
// how far it is from the middle of the water. Built either from the world map's
// own river - the cells this block covers know where the water runs and which way
// it leaves - or, when there is no world above this map, from a meander of the
// generator's own.
//
// A field rather than one river-x per row, because a world river runs whichever
// way the land falls: north to south on one block, east to west on the next, and
// through a corner on the one after.
struct RiverCourse {
    std::vector<std::int32_t> distance;    // per tile, in tiles
    std::vector<core::TilePos> centreLine; // the water's middle, for the fords
};

RiverCourse riverFromWorld(const LocalMapParams& params, core::Rng& rng) {
    RiverCourse course;
    course.distance.assign(static_cast<std::size_t>(params.width) * params.height, 1 << 20);

    std::vector<std::pair<core::TilePos, core::TilePos>> segments;
    if (params.world != nullptr) {
        // A cell the world says has no river through it gets no river. Drawing
        // one anyway is what put a watercourse down the middle of every map in
        // the country, and it is the single loudest way a generated place says
        // "I am a template" (D97).
        if (!params.hasRiver) return course;

        // Where the water crosses this ground: in at the edge it comes from, out
        // at the edge it leaves by, and through a point that is not the middle
        // of the map. Joining centre to centre was what made every river cross
        // the middle; the crossing point is drawn from the cell itself, so it is
        // the same every time this cell is generated and different from its
        // neighbour's.
        const std::int32_t w = params.width, h = params.height;
        const std::uint64_t hash = core::splitmix64(params.seed ^ 0xA24BAED4963EE407ull);
        const core::TilePos through{
                static_cast<std::int32_t>(w / 4 + hash % static_cast<std::uint64_t>(std::max(1, w / 2))),
                static_cast<std::int32_t>(h / 4 + (hash >> 21) % static_cast<std::uint64_t>(std::max(1, h / 2)))};

        // Which edge a direction leaves by. Along that edge the crossing point
        // wanders too, so the water does not always leave a side at its middle.
        const auto edgePoint = [&](std::int8_t dir, std::uint64_t salt) -> core::TilePos {
            const std::uint64_t r = core::splitmix64(params.seed ^ salt);
            const std::int32_t alongX = static_cast<std::int32_t>(r % static_cast<std::uint64_t>(std::max(1, w)));
            const std::int32_t alongY = static_cast<std::int32_t>((r >> 17) % static_cast<std::uint64_t>(std::max(1, h)));
            const core::TilePos step = core::kNeighbourOffsets[static_cast<std::size_t>(dir)];
            if (step.x > 0) return {w + 6, alongY};
            if (step.x < 0) return {-6, alongY};
            if (step.y > 0) return {alongX, h + 6};
            return {alongX, -6};
        };

        const core::TilePos out = params.riverOut >= 0
                                          ? edgePoint(params.riverOut, 0x51ED270BD7373BFDull)
                                          : core::TilePos{through.x, h + 6};   // a mouth: carry on out
        // Where it comes from. A headwater has no upstream neighbour, so it
        // rises on this map instead of crossing it - which is a spring, and a
        // map with a spring on it looks nothing like a map with a river through
        // it.
        const bool headwater = params.riverIn < 0;
        const core::TilePos in = headwater
                                         ? through
                                         : edgePoint(params.riverIn, 0x9E3779B97F4A7C15ull);

        // Two lengths with a bend in the middle, then broken into shorter pieces
        // so the wobble below has something to bend.
        const auto run = [&](core::TilePos from, core::TilePos to) {
            const std::int32_t pieces = 6;
            core::TilePos previous = from;
            for (std::int32_t i = 1; i <= pieces; ++i) {
                const core::TilePos next{from.x + (to.x - from.x) * i / pieces,
                                         from.y + (to.y - from.y) * i / pieces};
                segments.emplace_back(previous, next);
                previous = next;
            }
        };
        if (!headwater) run(in, through);
        run(through, out);
    }

    if (segments.empty() && params.world == nullptr) {
        // No world above this map: the generator's own meander, north to south.
        // This is what the tests use, and what the game did before there was a
        // country above the ground.
        std::int32_t rx = params.width / 2 + rng.range(-params.width / 8, params.width / 8);
        core::TilePos previous{rx, -1};
        for (std::int32_t y = 0; y < params.height; ++y) {
            rx = std::clamp(rx + rng.range(-1, 1), 4, params.width - 5);
            segments.emplace_back(previous, core::TilePos{rx, y});
            previous = {rx, y};
        }
    }
    if (segments.empty()) return course;

    // A wobble, so a river taken from a world map does not read as a ruled
    // diagram. Both ends of a length move together with their neighbours' - the
    // shift is a function of where the point is, not a fresh roll per end - or
    // the course comes apart at every join.
    {
        const std::int32_t sway = std::max(3, params.width / 14);
        const auto shift = [&](core::TilePos at) {
            const std::uint64_t h = core::splitmix64(params.seed ^ (std::uint64_t(std::uint32_t(at.x)) << 20) ^
                                                     std::uint32_t(at.y));
            return core::TilePos{at.x + static_cast<std::int32_t>(h % (2 * sway + 1)) - sway,
                                 at.y + static_cast<std::int32_t>((h >> 20) % (2 * sway + 1)) - sway};
        };
        for (auto& [from, to] : segments) {
            from = shift(from);
            to = shift(to);
        }
        (void)rng;
    }

    // The distance field, walked segment by segment.
    for (const auto& [from, to] : segments) {
        const std::int32_t steps = std::max(std::abs(to.x - from.x), std::abs(to.y - from.y)) + 1;
        for (std::int32_t i = 0; i <= steps; ++i) {
            const std::int32_t px = from.x + (to.x - from.x) * i / steps;
            const std::int32_t py = from.y + (to.y - from.y) * i / steps;
            if (px >= 0 && py >= 0 && px < params.width && py < params.height)
                course.centreLine.push_back({px, py});
            // Everything within reach of this point of the course.
            const std::int32_t reach = 40;
            for (std::int32_t dy = -reach; dy <= reach; ++dy)
                for (std::int32_t dx = -reach; dx <= reach; ++dx) {
                    const std::int32_t qx = px + dx, qy = py + dy;
                    if (qx < 0 || qy < 0 || qx >= params.width || qy >= params.height) continue;
                    const std::int32_t d = std::max(std::abs(dx), std::abs(dy));
                    auto& slot = course.distance[static_cast<std::size_t>(qy) * params.width + qx];
                    if (d < slot) slot = d;
                }
        }
    }
    return course;
}

// The height the world gives a tile of this block: bilinear between the cell
// heights around it, so the local map joins up with its neighbours instead of
// stepping at every cell boundary.
std::int32_t worldHeightAt(const LocalMapParams& params, std::int32_t x, std::int32_t y) {
    if (params.world == nullptr) return 90;
    const auto& world = *params.world;
    const std::int32_t cellTiles = std::max(1, params.width / std::max(1, params.cellsPerSide));
    const std::int32_t gx = x * 1000 / cellTiles;          // thousandths of a cell
    const std::int32_t gy = y * 1000 / cellTiles;
    const std::int32_t cx = gx / 1000, cy = gy / 1000;
    const std::int32_t fx = gx % 1000, fy = gy % 1000;
    const auto at = [&](std::int32_t ox, std::int32_t oy) {
        const core::TilePos cell{
                std::clamp(params.block.x + cx + ox, 0, world.width - 1),
                std::clamp(params.block.y + cy + oy, 0, world.height - 1)};
        const WorldCell& c = world.at(cell);
        // The sea is the bottom of the world; a river cell is a valley floor.
        if (c.sea) return 0;
        return static_cast<std::int32_t>(c.elevation) - (c.river ? 6 : 0);
    };
    const std::int32_t top = at(0, 0) + (at(1, 0) - at(0, 0)) * fx / 1000;
    const std::int32_t bottom = at(0, 1) + (at(1, 1) - at(0, 1)) * fx / 1000;
    return top + (bottom - top) * fy / 1000;
}

core::TilePos generateLocalMap(sim::World& w, const LocalMapParams& params) {
    using namespace sim;
    auto& map = w.map();
    map.resize(params.width, params.height);
    auto& rng = w.rng(core::stream::kWorldGen);

    // --- terrain ---------------------------------------------------------
    // Water is the constraint the early economy is organised around (GDD 7), and
    // where it runs is the world's business, not this generator's (D89).
    const RiverCourse river = riverFromWorld(params, rng);
    const auto distanceToRiver = [&](std::int32_t x, std::int32_t y) {
        return river.distance[static_cast<std::size_t>(y) * params.width + x];
    };
    const auto worldHeight = [&](std::int32_t x, std::int32_t y) {
        return worldHeightAt(params, x, y);
    };

    // How far each tile is from the sea, when the sea is next door. The world
    // says which way it lies (params.toSea); here that becomes an actual shore,
    // with a wandering line so it is a coast and not a ruled edge. Inland maps
    // get an empty field and nothing changes for them (D97).
    std::vector<std::int32_t> toSea(static_cast<std::size_t>(params.width) * params.height, 1 << 20);
    if (params.toSea >= 0) {
        const core::TilePos step = core::kNeighbourOffsets[static_cast<std::size_t>(params.toSea)];
        const std::int32_t w = params.width, h = params.height;
        for (std::int32_t y = 0; y < h; ++y)
            for (std::int32_t x = 0; x < w; ++x) {
                // Distance to the edge the sea is on, pushed about by noise so
                // the shoreline has bays and headlands in it.
                std::int32_t plain = 0;
                if (step.x > 0)      plain = w - 1 - x;
                else if (step.x < 0) plain = x;
                else if (step.y > 0) plain = h - 1 - y;
                else                 plain = y;
                const std::int32_t wander = (smoothNoise(params.seed + 606161, x, y, 26) - 512) / 14 +
                                            (smoothNoise(params.seed + 909091, x, y, 9) - 512) / 30;
                toSea[static_cast<std::size_t>(y) * w + x] = plain + wander;
            }
    }
    // How far the water reaches in: a third of the map at most, so there is
    // always a settlement's worth of dry ground.
    const std::int32_t shoreDepth = params.toSea >= 0 ? std::max(8, params.width / 5) : -1;

    // Fords. Without them the river cuts the map in two: half the land becomes
    // unreachable and every path search burns its whole budget proving it.
    // Spaced by the distance along the water, not by how many points the course
    // happens to be made of: the course carries several points per tile where
    // its lengths overlap, so counting points put a crossing every eight tiles
    // and the river read as a causeway.
    std::vector<std::uint8_t> ford(static_cast<std::size_t>(params.width) * params.height, 0);
    std::vector<core::TilePos> crossings;
    for (const core::TilePos at : river.centreLine) {
        bool tooClose = false;
        for (const core::TilePos other : crossings)
            if (core::tileDistance(at, other) < params.fordSpacing) { tooClose = true; break; }
        if (tooClose) continue;
        crossings.push_back(at);
    }
    for (const core::TilePos at : crossings) {
        for (std::int32_t dy = -params.riverHalfWidth - 2; dy <= params.riverHalfWidth + 2; ++dy)
            for (std::int32_t dx = -params.riverHalfWidth - 2; dx <= params.riverHalfWidth + 2; ++dx) {
                const std::int32_t fx = at.x + dx, fy = at.y + dy;
                if (fx < 0 || fy < 0 || fx >= params.width || fy >= params.height) continue;
                if (std::max(std::abs(dx), std::abs(dy)) > params.riverHalfWidth + 1) continue;
                ford[static_cast<std::size_t>(fy) * params.width + fx] = 1;
            }
    }

    for (std::int32_t y = 0; y < params.height; ++y) {
        for (std::int32_t x = 0; x < params.width; ++x) {
            const TilePos p{x, y};
            Tile& t = map.at(p);

            const std::int32_t distToRiver = distanceToRiver(x, y);
            // Three independent fields rather than bands cut out of one. Averaged
            // value noise clusters hard around its mean, so slicing a single
            // channel into four bands gave a map with no stone on it at all - and
            // a community that could never make its first tool.
            const std::int32_t cover = fractalNoise(params.seed, x, y);
            const std::int32_t rockiness = fractalNoise(params.seed + 7919, x, y);
            const std::int32_t wetness = fractalNoise(params.seed + 104729, x, y);
            // Its own field, at its own scale. Borrowing the rockiness for the
            // height put the hills exactly where the stone was and drew the
            // whole map in horizontal streaks.
            const std::int32_t relief = (smoothNoise(params.seed + 15485863, x, y, 40) * 3 +
                                         smoothNoise(params.seed + 32452843, x, y, 13)) / 4;
            const std::int32_t half = params.riverHalfWidth;

            // Soil first, ground second. What makes land fertile is water and
            // weather, not what it is called: the silt of a floodplain, the rain
            // on a hillside. So the number is worked out here and the terrain
            // follows from it - green where things grow, thin soil beyond that,
            // desert where nothing does. Deriving it the other way round had a
            // farming people unable to see the best soil on the map, because in
            // a river valley that soil reads as dirt (D90).
            const std::int32_t fromWater = std::max(0, distToRiver - half - 4);
            // The cell's own weather, not the same figure everywhere: a map in
            // a wet country grows more than a map in a dry one, and that is the
            // whole point of the world above knowing where the rain falls.
            const std::int32_t cellRain = (params.moisture - 128) / 4;      // about -32..+32
            const std::int32_t rain = (wetness - 460) / 12 + cellRain;
            const std::int32_t seaDistance = toSea[static_cast<std::size_t>(y) * params.width + x];
            std::int32_t fertility = 0;
            if (shoreDepth > 0 && seaDistance <= shoreDepth) {
                // Open water, then the beach it breaks on. Nothing grows in the
                // salt, and the strand is sand whatever country this is.
                if (seaDistance <= shoreDepth - 6) {
                    t.terrain = Terrain::Water;
                } else {
                    t.terrain = Terrain::Sand;
                    fertility = std::clamp(20 + rain / 2, 0, 60);
                }
            } else if (distToRiver <= half) {
                const bool crossing = ford[static_cast<std::size_t>(y) * params.width + x] != 0;
                t.terrain = crossing ? Terrain::Sand : Terrain::Water;
                t.ford = crossing;
            } else if (params.biome == Biome::RiverValley) {
                // Marsh at the water's edge, silt floodplain behind it, then dry
                // country. The fertile band is narrow and it is everything.
                // The silt runs wide and gives out slowly. It used to fall five
                // points a tile from a river that ran dead straight down the
                // map, which made a fertile strip twenty tiles wide and a
                // hundred and eighty long; once the river came down from the
                // world map and meandered, that strip became a curling ribbon, a
                // disc-shaped field caught almost none of it, and the community
                // starved on ground with a fifth of the soil it needed (D89).
                const std::int32_t ceiling = 50 + params.groundFertility * 50 / 255;
                fertility = params.hasRiver ? std::clamp(ceiling - fromWater * 3 + rain / 2, 0, 100)
                                            : std::clamp(ceiling / 2 + rain, 0, 100);
                // Where the grass line falls is the country's business, not the
                // soil's alone: a steppe is grass on ground a flood plain would
                // leave as dust (D100).
                const std::int32_t grassAt = std::clamp(80 - params.grassiness * 3 / 4, 8, 80);
                if (params.hasRiver && distToRiver <= half + 4) t.terrain = Terrain::Marsh;
                else if (fertility >= grassAt) t.terrain = Terrain::Grass;
                else if (fertility >= 22) t.terrain = Terrain::Dirt;
                else if (rockiness > 780) t.terrain = Terrain::Rock;
                else t.terrain = Terrain::Sand;
            } else {
                // Temperate country: the rain does most of the work, the river
                // the rest, and the wood grows where the ground is good enough
                // to carry it.
                // The cell's own soil sets the ceiling, so a map on thin
                // upland is thin and a map on good ground is good - and the
                // higher the country, the more of it is bare rock.
                const std::int32_t ceiling = 40 + params.groundFertility * 60 / 255;
                const std::int32_t bareness = std::clamp(params.elevation * 2, 0, 260);
                fertility = std::clamp(ceiling - fromWater * 2 + rain, 0, 100);
                if (params.hasRiver && distToRiver <= half + 2) t.terrain = Terrain::Sand;
                else if (rockiness > 760 - bareness && fertility < 55) t.terrain = Terrain::Rock;
                else if (wetness > 620 && distToRiver < 18 && params.hasRiver) t.terrain = Terrain::Marsh;
                else if (cover > 600 && fertility >= 35) t.terrain = Terrain::Forest;
                else if (fertility >= 25) t.terrain = Terrain::Grass;
                else t.terrain = Terrain::Sand;
            }
            t.fertility = Fixed::ratio(std::min(100, fertility), 100);
            // The land rises away from the water. The river bed is the bottom of
            // the map by definition, the floodplain is barely above it, and the
            // dry country behind climbs - with the same noise that decides where
            // the rock is, so the high ground and the stony ground agree.
            {
                // The valley floor climbs away from the water, and the country
                // behind it rolls. The bed of the river is the bottom of the map
                // by definition; the terraces are what the hill shading picks up.
                const std::int32_t fromBank = std::max(0, distToRiver - half);
                const std::int32_t climb = std::min(90, fromBank * 7 / 4);
                const std::int32_t roll = (relief - 500) * 70 / 500;   // about -70..+70
                const std::int32_t bed = distToRiver <= half ? -10 : 0;
                // And the whole thing sits at the height the world gives this
                // ground: read off the block's cells, bilinearly, so a map on the
                // flank of a range climbs the way the range does (D89).
                t.elevation = static_cast<std::uint8_t>(
                        std::clamp(worldHeight(x, y) + climb / 2 + roll / 2 + bed, 0, 255));
            }
            t.grass = grassFor(t.terrain);
            map.setBlocked(p, !terrainPassable(t.terrain));
        }
    }

    // The height field is smoothed before anything reads it. Bilinear value
    // noise has a constant slope inside each of its cells, so hill shading drew
    // it as flat bands with a crease at every cell edge - a corduroy desert. Two
    // passes of a box blur leave the same landforms with slopes that actually
    // curve.
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<std::uint8_t> smoothed(std::size_t(params.width) * params.height, 0);
        for (std::int32_t y = 0; y < params.height; ++y) {
            for (std::int32_t x = 0; x < params.width; ++x) {
                std::int32_t sum = 0, n = 0;
                for (std::int32_t dy = -1; dy <= 1; ++dy) {
                    for (std::int32_t dx = -1; dx <= 1; ++dx) {
                        const TilePos q{std::clamp(x + dx, 0, params.width - 1),
                                        std::clamp(y + dy, 0, params.height - 1)};
                        sum += map.at(q).elevation;
                        ++n;
                    }
                }
                smoothed[std::size_t(y) * params.width + x] =
                        static_cast<std::uint8_t>(sum / std::max(1, n));
            }
        }
        for (std::int32_t y = 0; y < params.height; ++y)
            for (std::int32_t x = 0; x < params.width; ++x)
                map.at({x, y}).elevation = smoothed[std::size_t(y) * params.width + x];
    }

    // --- natural features -------------------------------------------------
    const DefId branches = w.db().resourceNodeByName("fallen_branch");
    const DefId oak = w.db().resourceNodeByName("oak_tree");
    const DefId outcrop = w.db().resourceNodeByName("stone_outcrop");
    const DefId berries = w.db().resourceNodeByName("berry_bush");
    // Physic plants. Thin on the ground and worth walking for, which is what
    // makes a herbalist a trade rather than a chore (D98).
    const DefId yarrow = w.db().resourceNodeByName("yarrow");
    const DefId willow = w.db().resourceNodeByName("willow_stand");
    const DefId wildGrain = w.db().resourceNodeByName("wild_einkorn");
    const DefId flax = w.db().resourceNodeByName("wild_flax");
    const DefId clay = w.db().resourceNodeByName("clay_bank");
    const DefId copper = w.db().resourceNodeByName("copper_vein");
    const DefId tin = w.db().resourceNodeByName("tin_vein");

    auto scatter = [&](DefId def, std::int32_t percent, Terrain wanted, bool anyLand) {
        if (!def.valid() || percent <= 0) return;
        for (std::int32_t y = 0; y < params.height; ++y) {
            for (std::int32_t x = 0; x < params.width; ++x) {
                const TilePos p{x, y};
                Tile& t = map.at(p);
                if (t.node.valid() || t.terrain == Terrain::Water) continue;
                if (!anyLand && t.terrain != wanted) continue;
                if (!rng.chance(percent, 100)) continue;
                w.spawnNode(def, p);
            }
        }
    };

    // Fallen wood is the first thing a community with no tools can pick up, so it
    // has to exist on the ground before anything else in the chain works.
    const DefId reeds = w.db().resourceNodeByName("reed_bed");
    const DefId datePalm = w.db().resourceNodeByName("date_palm");
    const DefId tamarisk = w.db().resourceNodeByName("tamarisk");
    const DefId wildEmmer = w.db().resourceNodeByName("wild_emmer");
    const DefId shallows = w.db().resourceNodeByName("fishing_shallows");

    if (params.biome == Biome::RiverValley) {
        // The marsh is this country's forest and its quarry both: reed for
        // building, clay for brick. Timber is a scatter of tamarisk and nothing
        // else, which is the whole reason the architecture is what it is.
        scatter(reeds, params.reedPercent, Terrain::Marsh, false);
        scatter(datePalm, params.datePalmPercent, Terrain::Dirt, false);
        scatter(tamarisk, params.tamariskPercent, Terrain::Grass, false);
        scatter(tamarisk, params.tamariskPercent / 2 + 1, Terrain::Dirt, false);
        scatter(branches, params.fallenWoodPercent / 2, Terrain::Dirt, false);
        scatter(wildEmmer, params.emmerPercent, Terrain::Dirt, false);
        scatter(wildEmmer, params.emmerPercent / 2, Terrain::Grass, false);
        scatter(flax, params.wildGrainPercent / 2, Terrain::Marsh, false);
        scatter(clay, params.clayPercent, Terrain::Marsh, false);
        scatter(clay, params.clayPercent, Terrain::Sand, false);
        scatter(clay, params.clayPercent / 2, Terrain::Dirt, false);
        scatter(outcrop, params.rockPercent * 3, Terrain::Rock, false);
        scatter(copper, params.orePercent, Terrain::Rock, false);
        scatter(berries, params.berryPercent / 3, Terrain::Grass, false);

        // Fishing is worked from the bank, so the shoals go on the dry side of
        // the water's edge rather than in the river.
        if (shallows.valid()) {
            for (std::int32_t y = 0; y < params.height; ++y)
                for (std::int32_t x = 0; x < params.width; ++x) {
                    const TilePos p{x, y};
                    Tile& t = map.at(p);
                    if (t.node.valid() || t.building.valid() || t.ford) continue;
                    if (t.terrain != Terrain::Marsh && t.terrain != Terrain::Sand) continue;
                    bool onWater = false;
                    for (int dir : core::kCardinalDirections) {
                        const TilePos n = core::neighbour(p, dir);
                        if (map.inBounds(n) && map.at(n).terrain == Terrain::Water) onWater = true;
                    }
                    if (!onWater) continue;
                    if (!rng.chance(1, 3)) continue;
                    w.spawnNode(shallows, p);
                }
        }
    } else {

    scatter(branches, params.fallenWoodPercent, Terrain::Forest, false);
    scatter(branches, params.fallenWoodPercent / 2, Terrain::Grass, false);
    scatter(oak, params.forestPercent, Terrain::Forest, false);
    // Stone is a feature, not a biome: outcrops crowd the rocky ground but loose
    // flint also turns up in open country.
    scatter(outcrop, params.rockPercent * 2, Terrain::Rock, false);
    scatter(outcrop, params.rockPercent / 3 + 1, Terrain::Grass, false);
    scatter(berries, params.berryPercent, Terrain::Grass, false);
    scatter(wildGrain, params.wildGrainPercent, Terrain::Grass, false);
    // Flax on the marsh alone was too rare to feed the fibre chain, and without
    // fibre there is no thatch, no store, and everything gathered rots outdoors.
    scatter(flax, params.wildGrainPercent + 2, Terrain::Marsh, false);
    scatter(flax, params.wildGrainPercent / 2, Terrain::Grass, false);
    // Clay along the wet ground, ore in the rock. Tin is deliberately scarce:
    // that scarcity is what makes bronze worth the chain behind it.
    scatter(clay, params.clayPercent, Terrain::Marsh, false);
    scatter(clay, params.clayPercent / 2 + 1, Terrain::Sand, false);
    scatter(copper, params.orePercent, Terrain::Rock, false);
    scatter(tin, 1, Terrain::Rock, false);
    }

    // --- the wild animals --------------------------------------------------
    // Herds and packs, not shrubs that grow deer back. They are put down in
    // small groups on ground that suits them and left to breed, move and be
    // hunted; a hunted one is gone for good.
    // What lives here is a fact about the country, not about the game: elk and
    // bear belong to a cold wood, aurochs and wild horses to open grass, boar
    // to a marsh in any weather, and yak to the cold high ground. A community
    // that finds them can take their young and end up herding a kind its
    // neighbours have never kept (D111).
    struct WildStart { const char* name; std::int32_t groups; std::int32_t perGroup; Terrain on; };
    // Which beasts each of generation::Climate carries, and how thick on the
    // ground: the divisor is the map's width, so a bigger map is not a barer
    // one. Ice carries nothing anybody can live off.
    struct Fauna { const char* name; std::int32_t per; std::int32_t perGroup; Terrain on; };
    static const std::vector<Fauna> kFauna[] = {
            {},                                                              // Ice
            {{"moose", 64, 3, Terrain::Grass}, {"wolf", 48, 3, Terrain::Grass},
             {"bear", 128, 2, Terrain::Grass}},                              // Tundra
            {{"deer", 32, 5, Terrain::Grass}, {"moose", 48, 3, Terrain::Grass},
             {"boar", 48, 4, Terrain::Marsh}, {"wolf", 40, 3, Terrain::Grass},
             {"bear", 96, 2, Terrain::Grass}},                               // Taiga
            {{"deer", 24, 5, Terrain::Grass}, {"boar", 32, 4, Terrain::Marsh},
             {"wolf", 48, 3, Terrain::Grass}, {"aurochs", 64, 4, Terrain::Grass},
             {"bear", 128, 2, Terrain::Grass}},                              // TemperateForest
            {{"deer", 40, 5, Terrain::Grass}, {"wolf", 48, 3, Terrain::Grass},
             {"aurochs", 56, 4, Terrain::Grass},
             {"wild_horse", 48, 5, Terrain::Grass}},                         // Steppe
            {{"deer", 32, 4, Terrain::Grass}, {"boar", 40, 4, Terrain::Marsh},
             {"wolf", 56, 3, Terrain::Grass}, {"aurochs", 80, 3, Terrain::Grass}},
                                                                             // Mediterranean
            {{"wolf", 96, 2, Terrain::Grass},
             {"wild_horse", 96, 4, Terrain::Grass}},                         // Desert
            {{"deer", 40, 5, Terrain::Grass}, {"aurochs", 56, 4, Terrain::Grass},
             {"wild_horse", 64, 4, Terrain::Grass},
             {"wolf", 56, 3, Terrain::Grass}},                               // Savanna
            {{"boar", 24, 4, Terrain::Marsh}, {"deer", 40, 4, Terrain::Grass}},
                                                                             // TropicalForest
            {{"wild_yak", 64, 4, Terrain::Grass}, {"bear", 96, 2, Terrain::Grass},
             {"wolf", 64, 3, Terrain::Grass}},                               // Alpine
            {{"deer", 24, 5, Terrain::Grass}, {"boar", 32, 4, Terrain::Marsh},
             {"wolf", 48, 3, Terrain::Grass}, {"aurochs", 72, 4, Terrain::Grass}},
                                                                             // RiverValley
            {{"boar", 28, 4, Terrain::Marsh}, {"deer", 32, 5, Terrain::Grass},
             {"wolf", 48, 3, Terrain::Grass}},                               // Delta
    };
    const std::size_t kClimates = sizeof(kFauna) / sizeof(kFauna[0]);
    std::vector<WildStart> wild;
    if (params.climate >= 0 && static_cast<std::size_t>(params.climate) < kClimates) {
        for (const Fauna& f : kFauna[params.climate])
            wild.push_back({f.name, std::max(1, params.width / f.per), f.perGroup, f.on});
    } else {
        // No country above this map - the tests make maps that way. The old
        // three kinds, so those maps still have something to hunt.
        wild = {{"deer", params.width / 24, 5, Terrain::Grass},
                {"boar", params.width / 32, 4, Terrain::Marsh},
                {"wolf", params.width / 48, 3, Terrain::Grass}};
    }

    for (const auto& kind : wild) {
        const DefId def = w.db().animalByName(kind.name);
        if (!def.valid()) continue;
        for (std::int32_t g = 0; g < kind.groups; ++g) {
            TilePos centre{};
            bool found = false;
            for (int tries = 0; tries < 200 && !found; ++tries) {
                const TilePos p{static_cast<std::int32_t>(rng.below(std::uint32_t(params.width))),
                                static_cast<std::int32_t>(rng.below(std::uint32_t(params.height)))};
                if (!map.inBounds(p) || map.blocked(p)) continue;
                if (map.at(p).terrain != kind.on) continue;
                centre = p;
                found = true;
            }
            if (!found) continue;
            for (std::int32_t i = 0; i < kind.perGroup; ++i) {
                const auto nearby = core::tilesWithin(centre, 3);
                const TilePos at = nearby[rng.below(static_cast<std::uint32_t>(nearby.size()))];
                if (!map.inBounds(at) || map.blocked(at)) continue;
                const auto& animalDef = w.db().animal(def);
                const Sex sex = (i % 2 == 0) ? Sex::Female : Sex::Male;
                const std::int32_t age =
                        animalDef.adultAgeDays + static_cast<std::int32_t>(rng.below(600));
                w.spawnAnimal(def, at, SettlementId{}, sex, age);
            }
        }
    }

    // Physic plants, on every map (D98). Yarrow on open ground, willow at the
    // water: where they actually grow, and thin enough that gathering them is a
    // walk somebody chooses to make. Scattered here, after both countries have
    // had their turn, because a delta community falls ill exactly as often as a
    // forest one and has to be able to answer it - and before this the herbs
    // were inside the woodland branch, so the delta the game starts in had none.
    scatter(yarrow, std::max(2, params.berryPercent / 3), Terrain::Grass, false);
    scatter(yarrow, 2, Terrain::Dirt, false);
    scatter(willow, std::max(2, params.reedPercent / 4), Terrain::Marsh, false);
    scatter(willow, 2, Terrain::Sand, false);

    // --- hearth ------------------------------------------------------------
    // Pick the buildable spot with the best mix of water access, open ground and
    // nearby wood and stone: the "economic suitability" idea of GDD 4.2 stage 5,
    // reduced to what one local map can express.
    TilePos best{params.width / 2, params.height / 2};
    std::int32_t bestScore = -1;
    // The settled ground is a disc of radius twelve and the community works out
    // to twice that, so a hearth eight tiles from the edge spends its life with
    // half its territory off the map: its harvest lies thirty tiles out, its
    // stores never come home, and it never builds past a granary and one house.
    constexpr std::int32_t kEdgeMargin = 16;
    for (std::int32_t y = kEdgeMargin; y < params.height - kEdgeMargin; y += 2) {
        for (std::int32_t x = kEdgeMargin; x < params.width - kEdgeMargin; x += 2) {
            const TilePos p{x, y};
            if (map.blocked(p)) continue;
            const Terrain terr = map.at(p).terrain;
            if (terr != Terrain::Grass && terr != Terrain::Dirt && terr != Terrain::Sand) continue;

            std::int32_t water = 0, wood = 0, stone = 0, open = 0, fertile = 0;
            for (TilePos q : core::tilesWithin(p, 8)) {
                if (!map.inBounds(q)) continue;
                const Tile& t = map.at(q);
                if (t.terrain == Terrain::Water) ++water;
                else if (!map.blocked(q) && !t.node.valid()) ++open;
                // Any ground a plough can break, not only the green kind: in a
                // river valley the good soil is silt, which reads as dirt, so
                // counting grass alone meant a farming people picked its site
                // without looking at the soil at all - and on a meandering river
                // it settled on the dry outside of a bend and starved.
                if (t.terrain == Terrain::Dirt || t.terrain == Terrain::Grass ||
                    t.terrain == Terrain::Sand)
                    fertile += (t.fertility * 4).toInt();
                if (t.node.valid()) {
                    const auto kind = w.db().resourceNode(w.node(t.node).def).kind;
                    if (kind == content::ResourceKind::Tree) ++wood;
                    if (kind == content::ResourceKind::Rock) ++stone;
                }
            }
            if (water == 0 || open < 50) continue;
            // A farming and herding people wants open fertile ground as much as it
            // wants timber and stone, so the hearth is scored on all of them.
            const std::int32_t score = std::min(water, 30) * 3 + std::min(wood, 40) * 2 +
                                       std::min(stone, 20) * 3 + std::min(fertile, 400) * 2 + open / 4;
            if (score > bestScore) { bestScore = score; best = p; }
        }
    }
    return best;
}

} // namespace generation
