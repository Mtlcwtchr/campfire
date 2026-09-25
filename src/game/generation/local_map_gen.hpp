#pragma once
// Local map generation.
//
// This is stage 8 of the pipeline in GDD 4.2 - refining macro data down to play
// scale. The macro stages (tectonics, climate, biomes, ethnos placement) are not
// implemented yet; this module generates one temperate local map directly from a
// seed and exposes the same interface a real refinement stage would, so it can be
// replaced without touching the simulation.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game/content/content_db.hpp"
#include "engine/core/geometry.hpp"
#include "engine/core/rng.hpp"

namespace sim { class World; }

namespace generation {

// Which land is being refined down to play scale. GDD 4.2 has a whole macro
// pipeline deciding this; until that exists, a culture names the country it
// belongs in and the generator has a preset for it.
// A byte, because a world map now holds a million cells and a four-byte enum in
// WorldCell is four megabytes of nothing.
enum class Biome : std::uint8_t { Temperate, RiverValley };
Biome parseBiome(std::string_view name);

struct WorldMapData;   // the country above this map (world_map_gen.hpp)

struct LocalMapParams {
    std::uint64_t seed = 1;
    std::int32_t width = 192;
    std::int32_t height = 192;
    Biome biome = Biome::Temperate;

    // The block of world cells this map refines, and the world it came out of.
    // With them, the local map is that block drawn at play scale: the heights
    // come down from the cells, and the river runs where the world says it runs.
    // Without them the generator makes its own country, which is what it did
    // before the world map existed and is what the tests still use.
    const WorldMapData* world = nullptr;
    core::TilePos block{0, 0};
    std::int32_t cellsPerSide = 1;

    // What the world says about this particular cell, copied out so the local
    // generator does not have to know how to read a world map. Every one of
    // these is a reason for two maps to look nothing alike (D97):
    //
    //   hasRiver  a map with no river on it has no river on it. Drawing one
    //             anyway put a watercourse down the middle of every map in the
    //             world, whatever the country above it said.
    //   toSea     which way the sea lies, if it is next door: a coastal cell is
    //             a beach and a bay, not another stretch of inland.
    //   moisture, fertility, elevation, temperature
    //             the cell's own numbers, which set how much grows and how high
    //             and dry the ground is.
    bool hasRiver = false;
    // Direction indices into core::kNeighbourOffsets, or -1: where the water
    // comes in from and where it leaves. A river crosses the map the way it
    // crosses the country - a corner to a corner, an edge to an edge - rather
    // than always through the middle.
    std::int8_t riverIn = -1;
    std::int8_t riverOut = -1;
    std::int8_t toSea = -1;
    std::int32_t moisture = 128;
    std::int32_t groundFertility = 128;
    std::int32_t elevation = 60;
    std::int32_t temperature = 128;
    // Which of generation::Climate this cell is, or -1 when the map is being
    // made without a country above it. It decides what lives here: the beasts
    // belong to the climate rather than to the tile they stand on.
    std::int32_t climate = -1;

    // Shared shape of the land.
    // How much of the dry country is grass rather than bare ground, 0..100.
    // A flood plain is green along the water and dust beyond it; a steppe is
    // grass to the horizon and always was. Set by the country the cell is in
    // (shapedByCountry), because it is exactly the difference between them.
    std::int32_t grassiness = 30;

    std::int32_t riverHalfWidth = 2;
    std::int32_t fordSpacing = 28;   // a river with no crossing halves the usable map

    // Temperate scatter.
    std::int32_t fallenWoodPercent = 6;
    std::int32_t forestPercent = 22;
    std::int32_t rockPercent = 6;
    std::int32_t berryPercent = 10;
    std::int32_t wildGrainPercent = 8;
    std::int32_t gamePercent = 2;
    std::int32_t clayPercent = 5;
    std::int32_t orePercent = 3;

    // River-valley scatter.
    std::int32_t reedPercent = 26;
    std::int32_t datePalmPercent = 13;
    std::int32_t tamariskPercent = 3;
    std::int32_t emmerPercent = 13;
};

// The preset for a biome, on top of whatever the caller has already set.
LocalMapParams presetFor(Biome biome, LocalMapParams base);

// The same, but for the country the cell is actually in: how much wood, stone,
// grain and game a desert, a taiga or a delta puts on the ground. Scales what
// the biome preset set rather than replacing it, so a map is its biome shaped
// by its country (D97). Takes the climate as an integer because the local
// generator must not depend on the world map's headers.
LocalMapParams shapedByCountry(std::int32_t climate, LocalMapParams base);

// Which natural features this biome actually puts on the ground. The content
// validator needs it: a culture of the marsh can only bootstrap from what the
// marsh grows, and asking it to reach an oak is asking the wrong question.
std::vector<std::string> resourceNamesFor(Biome biome);

// Fills the world's tile map, scatters resource nodes, and returns the hearth
// tile: a spot with water in reach, workable ground and no blocking feature.
core::TilePos generateLocalMap(sim::World& w, const LocalMapParams& params);

} // namespace generation
