#include "framework.hpp"
#include "game/world/environment.hpp"
#include "game/world/flood_preview.hpp"
#include "game/world/terrain_mesh.hpp"
#include "game/world/navmesh.hpp"
#include "game/generation/world_map_gen.hpp"
#include "../assets/shaders/environment_fields.hlsli"

#include <cmath>

namespace {
using core::Fixed;
core::WorldPos point(double x, double y) {
    return {Fixed::fromDoubleForContent(x),Fixed::fromDoubleForContent(y)};
}
generation::WorldMapData environmentWorld() {
    generation::WorldMapData map;
    map.width=map.height=4;
    map.cells.resize(16);
    for (auto& cell:map.cells) {
        cell.elevation=20; cell.fertility=153; cell.moisture=102; cell.temperature=204;
    }
    map.prevailingWindField.assign(16,{0,-100});
    map.windStrengthField.assign(16,150);
    map.soilDrainageField.assign(16,51);
    return map;
}
}

TEST(environment_static_fields_keep_units_and_wind_direction) {
    auto map=environmentWorld();
    world::HeightField field(&map,42);
    for (const auto p : {point(-0.01,-0.01),point(64,64),point(178.4,291.7),point(1000,1000)}) {
        const auto value=field.surfaceClimateAt(p).environment;
        CHECK(std::abs(value[0].toDouble()-0.8)<1e-6);
        CHECK(std::abs(value[1].toDouble()-0.6)<1e-6);
        CHECK(std::abs(value[2].toDouble()-0.4)<1e-6);
        CHECK(std::abs(value[3].toDouble())<1e-6);
        CHECK(std::abs(value[4].toDouble()+1)<1e-6);
        CHECK(std::abs(value[5].toDouble()-0.2)<1e-6);
    }
}

TEST(environment_fields_are_continuous_across_chunk_and_cell_boundaries) {
    auto map=environmentWorld();
    for (std::size_t i=0;i<map.cells.size();++i) {
        map.cells[i].fertility=static_cast<std::uint8_t>((i%4)*75);
        map.cells[i].temperature=static_cast<std::uint8_t>((i/4)*75);
        map.prevailingWindField[i]={static_cast<std::int16_t>(i%2 ? 100 : -100),25};
    }
    world::HeightField field(&map,42), worker(&map,42);
    for (double x : {-64.0,0.0,64.0,128.0,180.0,256.0,360.0,540.0}) {
        const auto a=field.surfaceClimateAt(point(x-0.001,123)).environment;
        const auto b=field.surfaceClimateAt(point(x+0.001,123)).environment;
        CHECK(a==worker.surfaceClimateAt(point(x-0.001,123)).environment);
        for (int i=0;i<6;++i) CHECK(std::abs(a[i].toDouble()-b[i].toDouble())<0.025);
    }
    // Old save/test worlds may lack auxiliary climate arrays.
    map.prevailingWindField.clear(); map.windStrengthField.clear(); map.soilDrainageField.clear();
    world::HeightField old(&map,42);
    for (const auto value:old.surfaceClimateAt(point(100,100)).environment)
        CHECK(std::isfinite(value.toDouble()));
}

TEST(environment_soil_depletes_recovers_and_ash_is_temporary) {
    using Treatment=world::SoilState::Treatment;
    world::SoilState soil;
    const auto p=point(64,64);
    const auto potential=Fixed::ratio(3,5);
    CHECK_EQ(soil.fertilityAt(p,potential),potential);
    soil.treat(p,64,Treatment::Harvest,Fixed::ratio(1,5));
    CHECK(soil.fertilityAt(p,potential)<potential);
    soil.treat(p,64,Treatment::Pasture,Fixed::ratio(1,10));
    const auto restored=soil.fertilityAt(p,potential);
    CHECK(restored>Fixed::ratio(2,5));
    soil.treat(p,64,Treatment::Ash,Fixed::ratio(1,10));
    CHECK(soil.fertilityAt(p,potential)>restored);
    soil.advanceDays(180);
    CHECK_EQ(soil.fertilityAt(p,potential),restored);
    CHECK_EQ(soil.fertilityAt(point(1000,1000),potential),potential);
    CHECK(std::abs((soil.deltaAt(point(63.999,64))-soil.deltaAt(point(64.001,64))).toDouble())<0.001);
    const auto revision=soil.revision();
    soil.clear(); CHECK(soil.revision()>revision);
    CHECK_EQ(soil.fertilityAt(p,potential),potential);
}

TEST(environment_soil_bounds_negative_coordinates_and_no_terrain_mutation) {
    using Treatment=world::SoilState::Treatment;
    auto map=environmentWorld();
    world::HeightField field(&map,42);
    world::SoilState soil;
    const auto p=point(-64,-64);
    const auto before=field.surfaceClimateAt(p).environment;
    for (int i=0;i<30;++i) soil.treat(p,64,Treatment::Harvest,Fixed::ratio(1,10));
    CHECK_EQ(soil.fertilityAt(p,before[1]),core::kZero);
    for (int i=0;i<30;++i) soil.treat(p,64,Treatment::Pasture,Fixed::ratio(1,10));
    CHECK(soil.fertilityAt(p,before[1])<=core::kOne);
    CHECK(before==field.surfaceClimateAt(p).environment);
    CHECK_EQ(map.cells[0].fertility,153);
}

TEST(environment_flood_cannot_cross_a_bank_or_diagonal_corner) {
    constexpr int side=7;
    std::vector<float> height(side*side,0),source(side*side,-1e20f);
    source[3*side]=0;
    for (int i=2;i<=4;++i) {
        height[2*side+i]=height[4*side+i]=10;
        height[i*side+2]=height[i*side+4]=10;
    }
    height[3*side+3]=-4;
    const auto low=world::FloodPreview::connectedHeads(height,source,side,3);
    CHECK(low[3*side+1]>-1e19f);
    CHECK(low[3*side+3]<-1e19f);
    const auto high=world::FloodPreview::connectedHeads(height,source,side,11);
    CHECK(high[3*side+3]>-1e19f);
    height[2*side+2]=0; // A diagonal hole still doesn't connect the basin.
    CHECK(world::FloodPreview::connectedHeads(height,source,side,3)[3*side+3]<-1e19f);
    std::fill(source.begin(),source.end(),-1e20f);
    for (float head:world::FloodPreview::connectedHeads(height,source,side,20)) CHECK(head<-1e19f);
    world::FloodPreview preview;
    CHECK_EQ(preview.at(point(0,0)),-1.0f);
}

TEST(environment_shader_readouts_respond_to_state_not_frame_time) {
    using namespace world::environment_logic;
    CHECK_EQ(soilReadout(0.6f,-0.8f),0.0f);
    CHECK_EQ(soilReadout(0.9f,0.3f),1.0f);
    CHECK(moistureReadout(0.6f,0.25f,0.5f)<moistureReadout(0.6f,1,0.5f));
    CHECK(moistureReadout(0.6f,1.8f,0.5f)>moistureReadout(0.6f,1,0.5f));
    CHECK_EQ(travelReadout(1),0.0f);
    CHECK_EQ(travelReadout(65536),1.0f);
    CHECK(travelReadout(5)<travelReadout(30)); // A ford is not deep impassable water.
    CHECK_EQ(foundationReadout(0,1,0,1),0.0f);
    CHECK(foundationReadout(0.1f,0,0.4f,0.8f)>foundationReadout(0.4f,0,0.9f,0.2f));
    float previous=0;
    for (auto travel : {world::Travel::Walk, world::Travel::Scramble, world::Travel::Ford,
                       world::Travel::Climb, world::Travel::Swim, world::Travel::None}) {
        const float value=travelReadout(static_cast<float>(world::travelCost(travel).toDouble()));
        CHECK(value>=previous && value<=1);
        previous=value;
    }
}

TEST(environment_local_wind_is_static_sheltered_continuous_and_worker_safe) {
    auto map=environmentWorld();
    world::HeightField field(&map,42), worker(&map,42);
    bool sheltered=false;
    for (int y=-4;y<=12;++y) for (int x=-4;x<=12;++x) {
        const auto p=point(x*64,y*64);
        const auto wind=field.windAt(p);
        CHECK(wind==worker.windAt(p));
        CHECK_EQ(wind[0],core::kZero);
        CHECK(wind[1]<=Fixed::ratio(-1,5) && wind[1]>=-core::kOne);
        if (wind[1]>Fixed::ratio(-99,100)) sheltered=true;
        const auto before=field.windAt(point(x*64-0.001,y*64));
        const auto after=field.windAt(point(x*64+0.001,y*64));
        CHECK(std::abs((before[1]-after[1]).toDouble())<0.001);
    }
    CHECK(sheltered);
    // A calm generation field never invents arrows through normalization.
    map.windStrengthField.assign(16,0);
    world::HeightField calm(&map,42);
    CHECK_EQ(calm.windAt(point(64,64))[0],core::kZero);
    CHECK_EQ(calm.windAt(point(64,64))[1],core::kZero);
}

TEST(environment_flood_bounds_and_invalid_requests_are_explicit) {
    auto map=environmentWorld();
    world::HeightField field(&map,42);
    world::FloodPreview preview;
    preview.build(field,point(512,512),512);
    const auto bounds=preview.bounds();
    CHECK(bounds[0]<512 && bounds[1]<512 && bounds[2]>512 && bounds[3]>512);
    CHECK(preview.at(point(512,512))>=0);
    CHECK_EQ(preview.at(point(bounds[0]-1,512)),-1.0f);
    CHECK_EQ(preview.at(point(bounds[2],512)),-1.0f);
    CHECK(world::FloodPreview::connectedHeads({0},{0},1,-1).empty());
    CHECK(world::FloodPreview::connectedHeads({0},{0},2,1).empty());
    const auto revision=preview.revision();
    preview.build(field,point(512,512),0);
    CHECK(preview.revision()>revision);
    CHECK_EQ(preview.at(point(512,512)),-1.0f);
}

