// The country above the map (D84, D88).
//
// These are census tests, not pixel tests. A generator is only ever checked by
// counting what it produced: this project has twice shipped a map with no stone
// on it and a continent with no rivers, and both times the picture looked fine.
// What is asserted here is that the world is a world - big, mostly one land
// mass, watered, and settled - and that it is the same world every time from
// the same seed.

#include "framework.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <set>
#include <vector>

#include "game/generation/world_map_gen.hpp"

namespace {

generation::WorldMapParams params(std::uint64_t seed, std::int32_t cells = 256) {
    generation::WorldMapParams p;
    p.seed = seed;
    p.width = cells;
    p.height = cells;
    // Sites are left at zero: the generator works the count out from the area,
    // which is the property being relied on here - a test-sized world gets a
    // test-sized share of the settlements.
    return p;
}

struct Census {
    std::int64_t cells = 0, sea = 0, land = 0, river = 0;
    std::int32_t highest = 0;
};

Census censusOf(const generation::WorldMapData& w) {
    Census c;
    c.cells = static_cast<std::int64_t>(w.cells.size());
    for (const auto& cell : w.cells) {
        if (cell.sea) { ++c.sea; continue; }
        ++c.land;
        if (cell.river) ++c.river;
        c.highest = std::max(c.highest, static_cast<std::int32_t>(cell.elevation));
    }
    return c;
}

} // namespace

TEST(the_world_is_a_country_and_not_a_parish) {
    // A world is only a world if you cannot see the far side of it from the
    // settlement, and it is only playable if you can get to the far side of it.
    // The smallest offered is thirty-five kilometres - a day's walk across, and
    // still five hundred times the five and a half that D88 exists because of.
    const generation::WorldMapParams p;
    const std::int64_t metres = std::int64_t(p.width) * generation::kMetresPerCell;
    CHECK(metres >= 30000);
    CHECK(generation::kCellsPerLocalMap >= 1);

    // The default is one of the sizes on offer, and they go up.
    bool offered = false;
    std::int32_t last = 0;
    for (const generation::WorldSize& size : generation::kWorldSizes) {
        CHECK(size.cells > last);
        last = size.cells;
        if (size.cells == p.width) offered = true;
    }
    CHECK(offered);
    // And the largest of them is a country somebody could cross in a week, not
    // a continent. Ten thousand square kilometres against one and a quarter
    // million, which is what it was.
    const std::int64_t widest = std::int64_t(last) * generation::kMetresPerCell / 1000;
    CHECK(widest * widest <= 20000);
}

TEST(two_worlds_from_one_seed_are_the_same_world) {
    const auto a = generation::generateWorldMap(params(4242));
    const auto b = generation::generateWorldMap(params(4242));
    CHECK(a.constants.worldSeed == b.constants.worldSeed);
    CHECK(a.constants.seaLevel == b.constants.seaLevel);
    CHECK(a.constants.planetRotationSign == b.constants.planetRotationSign);
    CHECK(a.cells.size() == b.cells.size());
    if (a.cells.size() != b.cells.size()) return;
    for (std::size_t i = 0; i < a.cells.size(); ++i) {
        CHECK(a.cells[i].elevation == b.cells[i].elevation);
        CHECK(a.cells[i].sea == b.cells[i].sea);
        CHECK(a.cells[i].river == b.cells[i].river);
        CHECK(a.cells[i].riverOut == b.cells[i].riverOut);
        CHECK(a.continentalField[i] == b.continentalField[i]);
        CHECK(a.upliftField[i] == b.upliftField[i]);
        CHECK(a.riftField[i] == b.riftField[i]);
        CHECK(a.faultField[i] == b.faultField[i]);
        CHECK(a.macroHeightField[i] == b.macroHeightField[i]);
        CHECK(a.thermallyRelaxedHeightField[i] == b.thermallyRelaxedHeightField[i]);
        CHECK(a.hydrologicallyCorrectedHeightField[i] == b.hydrologicallyCorrectedHeightField[i]);
        CHECK(a.flowAccumulationField[i] == b.flowAccumulationField[i]);
        CHECK(a.basinIdField[i] == b.basinIdField[i]);
        CHECK(a.spillPointField[i].x == b.spillPointField[i].x);
        CHECK(a.spillPointField[i].y == b.spillPointField[i].y);
        CHECK(a.flowDirectionField[i] == b.flowDirectionField[i]);
        CHECK(a.riverDischargeField[i] == b.riverDischargeField[i]);
        CHECK(a.riverSourceField[i] == b.riverSourceField[i]);
        CHECK(a.lakeRegionField[i] == b.lakeRegionField[i]);
        CHECK(a.lakeLevelField[i] == b.lakeLevelField[i]);
        CHECK(a.waterfallField[i] == b.waterfallField[i]);
        CHECK(a.erosionField[i] == b.erosionField[i]);
        CHECK(a.floodplainPotentialField[i] == b.floodplainPotentialField[i]);
        CHECK(a.deltaPotentialField[i] == b.deltaPotentialField[i]);
        CHECK(a.rockTypeField[i] == b.rockTypeField[i]);
        CHECK(a.baseTemperatureField[i] == b.baseTemperatureField[i]);
        CHECK(a.prevailingWindField[i].x == b.prevailingWindField[i].x);
        CHECK(a.prevailingWindField[i].y == b.prevailingWindField[i].y);
        CHECK(a.windStrengthField[i] == b.windStrengthField[i]);
        CHECK(a.precipitationField[i] == b.precipitationField[i]);
        CHECK(a.airMoistureField[i] == b.airMoistureField[i]);
        CHECK(a.rainShadowField[i] == b.rainShadowField[i]);
        CHECK(a.temperatureField[i] == b.temperatureField[i]);
        CHECK(a.annualRainfallField[i] == b.annualRainfallField[i]);
        CHECK(a.humidityField[i] == b.humidityField[i]);
        CHECK(a.soilTypeField[i] == b.soilTypeField[i]);
        CHECK(a.soilFertilityField[i] == b.soilFertilityField[i]);
        CHECK(a.soilDrainageField[i] == b.soilDrainageField[i]);
        CHECK(a.soilOrganicPotentialField[i] == b.soilOrganicPotentialField[i]);
        CHECK(a.primaryBiomeSuitabilityField[i] == b.primaryBiomeSuitabilityField[i]);
        CHECK(a.secondaryBiomeSuitabilityField[i] == b.secondaryBiomeSuitabilityField[i]);
        CHECK(a.biomeTransitionField[i] == b.biomeTransitionField[i]);
        CHECK(a.materialSuitabilityField[i] == b.materialSuitabilityField[i]);
    }
    CHECK(a.sites.size() == b.sites.size());
    if (a.sites.size() != b.sites.size()) return;
    for (std::size_t i = 0; i < a.sites.size(); ++i) {
        CHECK(a.sites[i].cell == b.sites[i].cell);
        CHECK(a.sites[i].name == b.sites[i].name);
    }
    CHECK(a.playedCell == b.playedCell);
}

TEST(global_generation_passes_store_explicit_fields) {
    const generation::WorldMapParams p = params(17, 128);
    const auto w = generation::generateWorldMap(p);
    const std::size_t count = static_cast<std::size_t>(p.width) * p.height;
    CHECK(w.constants.worldSeed == p.seed);
    CHECK(w.constants.worldWidth == p.width);
    CHECK(w.constants.worldHeight == p.height);
    CHECK(w.constants.globalMoistureScale == p.rainfallPercent);
    CHECK(w.constants.seaLevel != 0);
    CHECK(w.constants.planetRotationSign == 1 || w.constants.planetRotationSign == -1);
    CHECK(w.continentalField.size() == count);
    CHECK(w.distanceToCoast.size() == count);
    CHECK(w.initialLandMask.size() == count);
    CHECK(w.upliftField.size() == count);
    CHECK(w.riftField.size() == count);
    CHECK(w.faultField.size() == count);
    CHECK(w.geologyRegion.size() == count);
    CHECK(w.macroHeightField.size() == count);
    CHECK(w.rockTypeField.size() == count);
    CHECK(w.erosionResistanceField.size() == count);
    CHECK(w.soilParentMaterialField.size() == count);
    CHECK(w.permeabilityField.size() == count);
    CHECK(w.thermallyRelaxedHeightField.size() == count);
    CHECK(w.hydrologicallyCorrectedHeightField.size() == count);
    CHECK(w.basinIdField.size() == count);
    CHECK(w.spillPointField.size() == count);
    CHECK(w.flowDirectionField.size() == count);
    CHECK(w.flowAccumulationField.size() == count);
    CHECK(w.riverDischargeField.size() == count);
    CHECK(w.riverSourceField.size() == count);
    CHECK(w.lakeRegionField.size() == count);
    CHECK(w.lakeLevelField.size() == count);
    CHECK(w.waterfallField.size() == count);
    CHECK(w.sedimentPotentialField.size() == count);
    CHECK(w.erosionField.size() == count);
    CHECK(w.floodplainPotentialField.size() == count);
    CHECK(w.deltaPotentialField.size() == count);
    CHECK(w.erodedHeightField.size() == count);
    CHECK(w.baseTemperatureField.size() == count);
    CHECK(w.prevailingWindField.size() == count);
    CHECK(w.windStrengthField.size() == count);
    CHECK(w.windVariabilityField.size() == count);
    CHECK(w.precipitationField.size() == count);
    CHECK(w.airMoistureField.size() == count);
    CHECK(w.rainShadowField.size() == count);
    CHECK(w.oceanCurrentField.size() == count);
    CHECK(w.seaSurfaceTemperatureBiasField.size() == count);
    CHECK(w.coastalClimateBiasField.size() == count);
    CHECK(w.temperatureField.size() == count);
    CHECK(w.annualRainfallField.size() == count);
    CHECK(w.humidityField.size() == count);
    CHECK(w.seasonalityField.size() == count);
    CHECK(w.winterRainField.size() == count);
    CHECK(w.summerRainField.size() == count);
    CHECK(w.drySeasonStrengthField.size() == count);
    CHECK(w.soilTypeField.size() == count);
    CHECK(w.soilFertilityField.size() == count);
    CHECK(w.soilDrainageField.size() == count);
    CHECK(w.soilOrganicPotentialField.size() == count);
    CHECK(w.primaryBiomeSuitabilityField.size() == count);
    CHECK(w.secondaryBiomeSuitabilityField.size() == count);
    CHECK(w.biomeTransitionField.size() == count);
    CHECK(w.materialSuitabilityField.size() == count);
}

TEST(global_generation_fields_show_geology_and_erosion_structure) {
    const auto w = generation::generateWorldMap(params(29, 256));
    std::set<generation::RockType> rocks;
    std::int64_t uplifted = 0, rifted = 0, faulted = 0, thermalChanged = 0, eroded = 0;
    std::int64_t inland = 0, coast = 0;
    for (std::size_t i = 0; i < w.cells.size(); ++i) {
        rocks.insert(w.rockTypeField[i]);
        if (w.upliftField[i] > 0) ++uplifted;
        if (w.riftField[i] > 0) ++rifted;
        if (w.faultField[i] > 0) ++faulted;
        if (w.thermallyRelaxedHeightField[i] != w.macroHeightField[i]) ++thermalChanged;
        if (w.erosionField[i] > 0) ++eroded;
        if (w.distanceToCoast[i] == 0) ++coast;
        if (w.distanceToCoast[i] > 6) ++inland;
    }
    CHECK(rocks.size() >= 3);
    CHECK(uplifted > 0);
    CHECK(rifted > 0);
    CHECK(faulted > 0);
    CHECK(thermalChanged > 0);
    CHECK(eroded > 0);
    CHECK(coast > 0);
    CHECK(inland > 0);
}

TEST(climate_generation_fields_show_winds_and_seasons) {
    const auto w = generation::generateWorldMap(params(31, 192));
    std::int64_t windy = 0, rainy = 0, shadowed = 0, coastalBias = 0, seasonality = 0;
    for (std::size_t i = 0; i < w.cells.size(); ++i) {
        if (w.prevailingWindField[i].x != 0 || w.prevailingWindField[i].y != 0) ++windy;
        if (w.precipitationField[i] > 0) ++rainy;
        if (w.rainShadowField[i] > 0) ++shadowed;
        if (w.coastalClimateBiasField[i] != 0) ++coastalBias;
        if (w.seasonalityField[i] > 0) ++seasonality;
        if (!w.cells[i].sea) CHECK(w.temperatureField[i] == w.cells[i].temperature);
    }
    CHECK(windy > 0);
    CHECK(rainy > 0);
    CHECK(shadowed > 0);
    CHECK(coastalBias > 0);
    CHECK(seasonality > 0);
}

TEST(hydrology_fields_show_basins_lakes_and_waterfalls) {
    const auto w = generation::generateWorldMap(params(73, 256));
    std::int64_t withDirection = 0, withBasins = 0, lakes = 0, falls = 0, floodplains = 0, deltas = 0;
    for (std::size_t i = 0; i < w.cells.size(); ++i) {
        if (w.flowDirectionField[i] >= 0) ++withDirection;
        if (w.basinIdField[i] >= -1) ++withBasins;
        if (w.lakeRegionField[i] >= 0) {
            ++lakes;
            CHECK(w.lakeLevelField[i] > 0);
        }
        if (w.waterfallField[i] != 0) {
            ++falls;
            CHECK(w.cells[i].river);
        }
        if (w.floodplainPotentialField[i] > 0) ++floodplains;
        if (w.deltaPotentialField[i] > 0) {
            ++deltas;
            CHECK(w.cells[i].river);
        }
        if (!w.cells[i].sea && w.cells[i].river)
            CHECK(w.riverDischargeField[i] >= w.flowAccumulationField[i]);
    }
    CHECK(withDirection > 0);
    CHECK(withBasins == static_cast<std::int64_t>(w.cells.size()));
    CHECK(lakes > 0);
    CHECK(falls > 0);
    CHECK(floodplains > 0);
    CHECK(deltas > 0);
}

TEST(soil_biome_and_material_suitability_fields_are_populated) {
    const auto w = generation::generateWorldMap(params(91, 192));
    std::int64_t nonDefaultSoil = 0, nonZeroTransition = 0, nonUniformMaterial = 0;
    for (std::size_t i = 0; i < w.cells.size(); ++i) {
        if (w.cells[i].sea) continue;
        if (w.soilTypeField[i] != generation::SoilType::Mineral) ++nonDefaultSoil;
        if (w.biomeTransitionField[i] > 0) ++nonZeroTransition;
        const auto& m = w.materialSuitabilityField[i];
        int sum = 0;
        for (std::uint8_t v : m) sum += v;
        CHECK(sum >= 240);
        CHECK(sum <= 260);
        bool varied = false;
        for (std::size_t c = 1; c < m.size(); ++c)
            if (m[c] != m[0]) { varied = true; break; }
        if (varied) ++nonUniformMaterial;
    }
    CHECK(nonDefaultSoil > 0);
    CHECK(nonZeroTransition > 0);
    CHECK(nonUniformMaterial > 0);
}

TEST(world_map_data_roundtrip_preserves_global_fields) {
    const auto src = generation::generateWorldMap(params(123, 128));
    const std::filesystem::path file = "/tmp/asr_worldmap_roundtrip.json";
    CHECK(generation::saveWorldMapData(src, file));
    generation::WorldMapData loaded;
    CHECK(generation::loadWorldMapData(file, loaded));
    CHECK(src.width == loaded.width);
    CHECK(src.height == loaded.height);
    CHECK(src.seed == loaded.seed);
    CHECK(src.constants.seaLevel == loaded.constants.seaLevel);
    CHECK(src.cells.size() == loaded.cells.size());
    CHECK(src.sites.size() == loaded.sites.size());
    CHECK(src.flowAccumulationField == loaded.flowAccumulationField);
    CHECK(src.temperatureField == loaded.temperatureField);
    CHECK(src.rockTypeField == loaded.rockTypeField);
    CHECK(src.soilTypeField == loaded.soilTypeField);
    CHECK(src.materialSuitabilityField == loaded.materialSuitabilityField);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(every_seed_makes_land_sea_and_rivers) {
    // Across seeds, because a threshold tuned on one seed is a coin toss on the
    // next: the sea level is a percentile so it cannot drift, but rivers are a
    // flow threshold and those are exactly what went to zero before.
    for (std::uint64_t seed : {1ull, 5ull, 11ull, 23ull, 42ull, 101ull}) {
        const auto w = generation::generateWorldMap(params(seed));
        const Census c = censusOf(w);
        // The sea is asked for as a share of cells and must be delivered as one.
        // Against what was asked for rather than against two fixed numbers: the
        // default moved to seventy-one - the share this world has - and a test
        // written round the old default then fails as "the sea is wrong" when
        // what changed was the request.
        const std::int64_t asked = params(seed).seaPercent;
        const std::int64_t got = c.sea * 100 / c.cells;
        CHECK(got >= asked - 4);
        CHECK(got <= asked + 4);
        // Rivers: enough that the land is watered, few enough that the map is
        // not a marsh. Measured in thousandths of the land.
        const std::int64_t perMille = c.river * 1000 / std::max<std::int64_t>(1, c.land);
        CHECK(perMille >= 3);
        CHECK(perMille <= 80);
        // Ground high enough for a river to run off. Everything flat was the
        // symptom when the hollow-filling ran out of passes.
        CHECK(c.highest >= 60);
    }
}

TEST(rivers_run_downhill_to_somewhere) {
    // A river cell either leaves in a direction, or it is a mouth. Where it
    // leaves, it must leave into water - another river cell or the sea - or the
    // course drawn on the local map starts in the middle of dry ground.
    const auto w = generation::generateWorldMap(params(11));
    std::int64_t river = 0, joins = 0;
    for (std::int32_t y = 0; y < w.height; ++y)
        for (std::int32_t x = 0; x < w.width; ++x) {
            const auto& c = w.at({x, y});
            if (!c.river || c.sea) continue;
            ++river;
            if (c.riverOut < 0) continue;
            const core::TilePos n = core::neighbour({x, y}, c.riverOut);
            if (!w.inBounds(n)) { ++joins; continue; }
            const auto& next = w.at(n);
            if (next.sea || next.river) ++joins;
            // Downhill, or level where a filled basin made it level.
            CHECK(next.sea || next.elevation <= c.elevation);
        }
    CHECK(river > 0);
    if (river == 0) return;
    // Not every cell: the last cell above a coast drains into land that is
    // below the flow threshold. Nearly all of them, though.
    CHECK(joins * 100 / river >= 90);
}

TEST(peoples_are_spread_out_and_named) {
    const auto w = generation::generateWorldMap(params(7));
    CHECK(!w.sites.empty());
    if (w.sites.empty()) return;
    // Spacing scales with the map (D96): on a test-sized world eighty cells
    // apart would leave room for exactly one community, so what is asserted is
    // the rule, not the number.
    const generation::WorldMapParams p = params(7);
    const std::int32_t spacing = std::clamp(p.siteSpacing * p.width / 2048, 6, p.siteSpacing);
    for (std::size_t i = 0; i < w.sites.size(); ++i) {
        const auto& s = w.sites[i];
        CHECK(!s.name.empty());
        CHECK(!s.ethnos.empty());
        CHECK(!w.at(s.cell).sea);
        for (std::size_t j = i + 1; j < w.sites.size(); ++j)
            CHECK(core::tileDistance(s.cell, w.sites[j].cell) >= spacing);
    }
}

TEST(the_high_ground_is_in_ranges_and_not_in_spots) {
    // What tectonics is for (D90). Noise puts its peaks wherever it likes, so
    // high ground comes out as isolated bumps; plate margins are lines, so it
    // comes out as ranges - a high cell nearly always has another high cell
    // beside it. This counts exactly that: of the cells in the top of the
    // height range, what share have a high neighbour.
    const auto w = generation::generateWorldMap(params(11, 384));
    std::int64_t high = 0, withNeighbour = 0;
    const auto tall = [&](core::TilePos p) {
        return w.inBounds(p) && !w.at(p).sea && w.at(p).elevation >= 150;
    };
    for (std::int32_t y = 0; y < w.height; ++y)
        for (std::int32_t x = 0; x < w.width; ++x) {
            if (!tall({x, y})) continue;
            ++high;
            for (int dir : core::kCardinalDirections)
                if (tall(core::neighbour({x, y}, dir))) { ++withNeighbour; break; }
        }
    CHECK(high > 0);
    if (high == 0) return;
    CHECK(withNeighbour * 100 / high >= 90);
}

TEST(a_settlement_is_a_people_and_not_a_dot) {
    // On a bigger sample than the other tests use: the map deliberately carries
    // few settlements, so a small world can hold two of them.
    const auto w = generation::generateWorldMap(params(11, 512));
    CHECK(!w.sites.empty());
    if (w.sites.empty()) return;

    for (const auto& s : w.sites) {
        // Everybody is founded the same way, on the same morning, with the same
        // band: what a place becomes has to be what it makes of its ground, not
        // what the generator handed it (D94).
        CHECK(s.population == generation::kFoundingCommunity);
        CHECK(!s.name.empty());
        // What the land offers has to be something the ground could actually
        // give: no fishermen inland, no miners on a flood plain.
        const auto& cell = w.at(s.cell);
        if (s.livelihood == generation::Livelihood::Fishers) {
            bool coastal = false;
            for (int dir : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour(s.cell, dir);
                if (w.inBounds(n) && w.at(n).sea) coastal = true;
            }
            CHECK(coastal);
        }
        if (s.livelihood == generation::Livelihood::Miners) CHECK(cell.elevation > 140);
    }

    // Neighbours, not a countryside: a world whose settlements are a short walk
    // apart is one settlement drawn many times.
    for (std::size_t i = 0; i < w.sites.size(); ++i)
        for (std::size_t j = i + 1; j < w.sites.size(); ++j)
            CHECK(core::tileDistance(w.sites[i].cell, w.sites[j].cell) >= 40);
}

TEST(the_world_has_countries_in_it) {
    // A world whose land is all one kind has nowhere to put a second people
    // (D96). What is checked is variety and plausibility: several climates, no
    // single one swallowing the map, and each of them where it belongs - ice
    // cold, desert dry, alpine high.
    for (std::uint64_t seed : {5ull, 11ull, 42ull}) {
        const auto w = generation::generateWorldMap(params(seed, 384));
        std::array<std::int64_t, static_cast<std::size_t>(generation::Climate::Count)> tally{};
        std::int64_t land = 0;
        for (const auto& c : w.cells) {
            if (c.sea) continue;
            ++land;
            ++tally[static_cast<std::size_t>(c.climate)];
            if (c.climate == generation::Climate::Ice) CHECK(c.temperature < 90);
            if (c.climate == generation::Climate::Desert) CHECK(c.moisture < 90);
            if (c.climate == generation::Climate::Alpine) CHECK(c.elevation > 150);
            if (c.climate == generation::Climate::Delta) CHECK(c.elevation < 40);
        }
        CHECK(land > 0);
        if (land == 0) continue;

        std::int32_t kinds = 0;
        std::int64_t biggest = 0;
        for (auto n : tally) {
            if (n * 100 / land >= 2) ++kinds;
            biggest = std::max(biggest, n);
        }
        // Half a dozen countries at least, and no single one is the whole world.
        CHECK(kinds >= 5);
        CHECK(biggest * 100 / land <= 60);
    }
}

TEST(the_coast_is_not_drawn_with_a_compass) {
    // A coastline is the most fractal line on a planet, and the one the eye
    // recognises a world by (D96). Measured the way a coastline is measured: how
    // long it is for the land it encloses. Smooth bays give a short coast.
    const auto w = generation::generateWorldMap(params(11, 512));
    std::int64_t land = 0, coast = 0;
    for (std::int32_t y = 0; y < w.height; ++y)
        for (std::int32_t x = 0; x < w.width; ++x) {
            const auto& c = w.at({x, y});
            if (c.sea) continue;
            ++land;
            for (int dir : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour({x, y}, dir);
                if (w.inBounds(n) && w.at(n).sea) { ++coast; break; }
            }
        }
    CHECK(land > 0);
    if (land == 0) return;
    // A smooth blob of this area has a coast of a couple of per cent of it; a
    // torn one runs to a twentieth and beyond. Anything under this is a lake
    // shore drawn with a compass.
    CHECK(coast * 100 / land >= 4);
}

TEST(a_people_belongs_to_the_country_it_settled) {
    const auto w = generation::generateWorldMap(params(11, 512));
    CHECK(!w.sites.empty());
    if (w.sites.empty()) return;

    std::set<generation::Climate> countries;
    std::set<std::string> peoples;
    for (const auto& s : w.sites) {
        countries.insert(s.climate);
        peoples.insert(s.ethnos);
        CHECK(s.climate == w.at(s.cell).climate);
        // A people belongs to the country its way of living works in (D100):
        // reed and mud brick in the wet valleys, felt and flocks on the dry
        // grass, timber and smokehouses in the cold woods, and the Achaeans on
        // the warm broken coasts. Getting this backwards puts a culture that
        // builds in reed on a mountainside with no reeds on it.
        const std::string expected =
                s.climate == generation::Climate::Delta ||
                                s.climate == generation::Climate::RiverValley
                        ? "sumerian"
                : s.climate == generation::Climate::Steppe ||
                                  s.climate == generation::Climate::Savanna ||
                                  s.climate == generation::Climate::Desert
                        ? "yamna"
                : s.climate == generation::Climate::Taiga ||
                                  s.climate == generation::Climate::Tundra ||
                                  s.climate == generation::Climate::Ice
                        ? "northfolk"
                        : "achaean";
        CHECK(s.ethnos == expected);
    }
    // And they are not all from the same place: dealing the sites out by climate
    // is the whole reason the map has climates (D96).
    CHECK(countries.size() >= 3);
    // With four peoples on a world this size, more than one of them should have
    // drawn a home.
    CHECK(peoples.size() >= 2);
}

TEST(the_played_community_is_put_somewhere_worth_living) {
    for (std::uint64_t seed : {1ull, 5ull, 11ull, 42ull}) {
        const auto w = generation::generateWorldMap(params(seed));
        CHECK(!w.sites.empty());
        if (w.sites.empty()) continue;
        const auto played = std::find_if(w.sites.begin(), w.sites.end(),
                                         [](const generation::WorldSite& s) { return s.played; });
        CHECK(played != w.sites.end());
        if (played == w.sites.end()) continue;
        CHECK(played->cell == w.playedCell);
        const auto& cell = w.at(w.playedCell);
        CHECK(!cell.sea);
        // The content this game ships is written for a river valley, and the
        // local generator is handed that biome: if the cell underneath is dry
        // upland the map it grows has no reeds and no clay in it.
        CHECK(cell.biome == generation::Biome::RiverValley);
        // The block the local map refines has to be inside the world.
        CHECK(w.inBounds(w.playedBlock));
    }
}

