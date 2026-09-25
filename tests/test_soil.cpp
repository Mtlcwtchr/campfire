#include "framework.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_mesh.hpp"
#include "game/generation/world_map_gen.hpp"

#include <algorithm>

namespace {
using core::Fixed;
using world::Soil;
const core::WorldPos point{Fixed::fromInt(2100), Fixed::fromInt(2200)};

generation::WorldMapData country() {
    generation::WorldMapData map;
    map.width = map.height = 8;
    map.cells.resize(64);
    for (auto& cell : map.cells) {
        cell.elevation = 30;
        cell.moisture = 128;
        cell.temperature = 120;
        cell.climate = generation::Climate::Steppe;
    }
    map.rockTypeField.assign(64,generation::RockType::SoftSediment);
    map.humidityField.assign(64,128);
    map.soilDrainageField.assign(64,128);
    map.soilOrganicPotentialField.assign(64,0);
    map.permeabilityField.assign(64,128);
    map.soilFertilityField.assign(64,128);
    map.floodplainPotentialField.assign(64,0);
    map.deltaPotentialField.assign(64,0);
    map.distanceToCoast.assign(64,8);
    return map;
}

world::SoilSample sample(const generation::WorldMapData& map, int height=240, Fixed slope=core::kZero) {
    return world::HeightField(&map,42).soilGiven(point,Fixed::fromInt(height),slope);
}

void valid(const world::SoilSample& sample) {
    Fixed total;
    for (auto weight : sample.weights.weight) {
        CHECK(weight >= core::kZero && weight <= core::kOne);
        total += weight;
    }
    CHECK_EQ(total,core::kOne);
    const auto& p = sample.properties;
    for (auto value : {p.fertility,p.permeability,p.diggingResistance,p.bearingStrength,p.erodibility,p.mudPotential})
        CHECK(value >= core::kZero && value <= core::kOne);
}
}

TEST(soil_weights_normalise_exactly_and_have_a_safe_fallback) {
    world::SoilWeights weights;
    weights.normalise();
    CHECK_EQ(weights.of(Soil::Loam),core::kOne);
    weights.add(Soil::Sand,Fixed::fromInt(2));
    weights.add(Soil::Clay,Fixed::fromInt(-1));
    weights.add(Soil::Silt,Fixed::fromInt(3));
    weights.normalise();
    CHECK_EQ(weights.of(Soil::Clay),core::kZero);
    CHECK_EQ(weights.strongest(),Soil::Silt);
    valid({weights,weights.properties()});
    for (int i=0;i<static_cast<int>(Soil::Count);++i) {
        world::SoilWeights pure;
        pure.add(static_cast<Soil>(i),core::kOne);
        valid({pure,pure.properties()});
        CHECK(std::string(world::soilName(static_cast<Soil>(i))) != "unknown");
    }
}

TEST(soil_properties_expose_distinct_relative_responses) {
    const auto properties=[](Soil soil) {
        world::SoilWeights weights; weights.add(soil,core::kOne); return weights.properties();
    };
    const auto sand=properties(Soil::Sand), clay=properties(Soil::Clay), rock=properties(Soil::Rocky);
    CHECK(sand.permeability > clay.permeability);
    CHECK(clay.mudPotential > sand.mudPotential);
    CHECK(rock.diggingResistance > sand.diggingResistance);
    CHECK(rock.bearingStrength > properties(Soil::Peat).bearingStrength);
    CHECK(properties(Soil::Alluvium).fertility > properties(Soil::Saline).fertility);
    CHECK(properties(Soil::Silt).erodibility > rock.erodibility);
}

TEST(soil_parent_geology_and_coarse_soil_are_not_biome_cover) {
    auto map=country();
    map.rockTypeField.assign(64,generation::RockType::Sandstone);
    const auto sandy=sample(map);
    CHECK(sandy.weights.of(Soil::Sand) > Fixed::ratio(1,2));
    for (auto& cell : map.cells) cell.climate=generation::Climate::Ice;
    CHECK_EQ(sample(map).weights.weight,sandy.weights.weight);
    map.rockTypeField.assign(64,generation::RockType::ClayRich);
    CHECK(sample(map).weights.of(Soil::Clay) > Fixed::ratio(1,2));
    map.rockTypeField.clear();
    map.soilParentMaterialField.assign(64,5); // Sandstone fallback from G4.
    CHECK_EQ(sample(map).weights.weight,sandy.weights.weight);
    map.soilTypeField.assign(64,generation::SoilType::Peat);
    CHECK(sample(map).weights.of(Soil::Peat) >= Fixed::ratio(1,2));
}

TEST(soil_processes_distinguish_floodplain_peat_salt_and_rock) {
    auto map=country();
    const auto plain=sample(map);
    map.floodplainPotentialField.assign(64,255);
    const auto floodplain=sample(map);
    CHECK(floodplain.weights.of(Soil::Alluvium) > plain.weights.of(Soil::Alluvium));
    map.floodplainPotentialField.assign(64,0);
    map.deltaPotentialField.assign(64,255);
    CHECK(sample(map).weights.of(Soil::Silt) > plain.weights.of(Soil::Silt));
    map.deltaPotentialField.assign(64,0);
    map.humidityField.assign(64,255);
    map.soilDrainageField.assign(64,0);
    map.soilOrganicPotentialField.assign(64,255);
    CHECK(sample(map).weights.of(Soil::Peat) > Fixed::ratio(3,4));
    map.humidityField.assign(64,0);
    CHECK_EQ(sample(map).weights.of(Soil::Peat),core::kZero);
    map.distanceToCoast.assign(64,0);
    CHECK(sample(map,2).weights.of(Soil::Saline) > Fixed::ratio(3,4));
    CHECK_EQ(sample(map,100).weights.of(Soil::Saline),core::kZero);
    CHECK_EQ(sample(map,-8).weights.of(Soil::Saline),core::kZero);
    const auto cliff=sample(map,2,Fixed::fromInt(2));
    CHECK_EQ(cliff.weights.of(Soil::Rocky),core::kOne);
    valid(cliff);
}

TEST(soil_local_alluvium_follows_wet_channels_not_dry_gullies) {
    auto map=country();
    int east=0;
    for (int d=0;d<core::kNeighbourCount;++d)
        if (core::neighbour({3,4},d)==core::TilePos{4,4}) east=d;
    for (int x=1;x<7;++x) {
        auto& cell=map.at({x,4});
        cell.river=true; cell.drainSize=cell.riverSize=8;
        cell.riverOut=cell.drainOut=static_cast<std::int8_t>(east);
    }
    world::HeightField river(&map,42);
    const auto channel=river.macro().channelOf({3,4});
    CHECK(channel.has_value());
    if (!channel) return;
    const auto p=world::MacroWorld::pointOn(*channel,Fixed::ratio(1,2));
    const auto wet=river.soilGiven(p,Fixed::fromInt(240),core::kZero);
    for (auto& cell : map.cells) { cell.river=false; cell.drainSize=cell.riverSize=1; }
    world::HeightField dry(&map,42);
    const auto gully=dry.soilGiven(p,Fixed::fromInt(240),core::kZero);
    CHECK(wet.weights.of(Soil::Alluvium) > Fixed::ratio(1,2));
    CHECK_EQ(gully.weights.of(Soil::Alluvium),core::kZero);
}

TEST(soil_mixes_continuously_between_geologies_in_one_biome) {
    auto map=country();
    for (int y=0;y<8;++y) for (int x=0;x<8;++x)
        map.rockTypeField[y*8+x]=x<4 ? generation::RockType::Sandstone : generation::RockType::ClayRich;
    world::HeightField field(&map,42);
    auto previous=field.soilGiven({Fixed::fromInt(1000),point.y},Fixed::fromInt(240),core::kZero);
    CHECK(previous.weights.of(Soil::Sand) > previous.weights.of(Soil::Clay));
    for (int x=1001;x<=3100;++x) {
        const auto current=field.soilGiven({Fixed::fromInt(x),point.y},Fixed::fromInt(240),core::kZero);
        valid(current);
        for (std::size_t i=0;i<world::kSoilCount;++i)
            CHECK(core::abs(current.weights.weight[i]-previous.weights.weight[i]) < Fixed::ratio(1,50));
        previous=current;
    }
    CHECK(previous.weights.of(Soil::Clay) > previous.weights.of(Soil::Sand));
}

TEST(soil_ground_sample_is_query_order_and_lod_independent) {
    generation::WorldMapParams params; params.width=params.height=32; params.seed=42;
    const auto map=generation::generateWorldMap(params);
    world::HeightField field(&map,42), independent(&map,42);
    for (int x : {-200,0,4000,9000,20000}) {
        const core::WorldPos p{Fixed::fromInt(x),Fixed::fromInt(5000)};
        const auto before=field.soilAt(p);
        valid(before);
        for (int lod : {0,3,6,10}) {
            (void)field.sampleHeight(x/4,1250,world::sampleMetresAt(lod));
            (void)field.soilAt({p.y,p.x});
        }
        const auto ground=field.groundAt(p);
        CHECK_EQ(ground.soil.weights.weight,before.weights.weight);
        CHECK_EQ(independent.soilAt(p).weights.weight,before.weights.weight);
        CHECK_EQ(ground.soil.properties.fertility,before.properties.fertility);
    }
    world::HeightField fallback(nullptr,42);
    valid(fallback.soilAt(point));
    auto partial=country();
    partial.soilTypeField.assign(1,generation::SoilType::Sandy);
    partial.rockTypeField.assign(1,generation::RockType::Count);
    partial.humidityField.assign(1,-100);
    partial.soilDrainageField.assign(1,999);
    valid(world::HeightField(&partial,42).soilAt({Fixed::fromInt(-100),Fixed::fromInt(-100)}));
}

