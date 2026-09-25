#include "framework.hpp"
#include "game/world/weather.hpp"
#include "game/world/weather_temperature.hpp"
#include "game/simulation/world.hpp"
#include "game/client/explore_menu.hpp"
#include <filesystem>

namespace {
using namespace world::weather;
Sample at(double day, float thermal=0.5f, int preset=0, float x=14000) {
    return snapshot(42,day,15,{18,32,21,9},preset).at(thermal,0.65f,120,x,24000,0.5f);
}
}
TEST(weather_seed_time_and_query_order_are_reproducible) {
    const auto a=at(53.4);
    (void)at(102); (void)at(0,0.9f,3);
    const auto b=at(53.4);
    CHECK_EQ(a.air.temperature,b.air.temperature);
    CHECK_EQ(a.surface.snow,b.surface.snow);
    CHECK(std::abs(at(53.4,0.5f,0,14000).air.temperature-
        snapshot(17,53.4,15,{18,32,21,9}).at(0.5f,0.65f,120,14000,24000,0.5f).air.temperature)>0.001f);
}
TEST(weather_calendar_rollover_and_regions_are_continuous) {
    for (int d=1;d<90;++d) {
        const auto a=at(d-0.00001), b=at(d+0.00001);
        CHECK(std::abs(a.air.temperature-b.air.temperature)<0.001f);
        CHECK(std::abs(a.surface.snow-b.surface.snow)<0.0001f);
        CHECK(std::abs(a.surface.wet-b.surface.wet)<0.0001f);
        CHECK(std::abs(a.surface.ice-b.surface.ice)<0.0001f);
    }
    const auto a=at(24.5,0.5f,0,11999.99f), b=at(24.5,0.5f,0,12000.01f);
    CHECK(std::abs(a.air.precipitation-b.air.precipitation)<0.0001f);
}
TEST(weather_snow_and_ice_accumulate_then_thaw) {
    WeatherAir cold{-8,0.8f,1,1}, warm{15,0,0,1};
    auto s=wxEmpty();
    for (int i=0;i<12;++i) s=wxAdvance(s,cold,0.5f);
    CHECK(s.snow>0.3f); CHECK(s.ice>0.7f);
    for (int i=0;i<20;++i) s=wxAdvance(s,warm,0.5f);
    CHECK_EQ(s.snow,0.0f); CHECK_EQ(s.ice,0.0f); CHECK(s.wet<0.01f);
}
TEST(weather_climate_prevents_tropical_winter_snow) {
    CHECK(at(52.5,0.95f,2).air.temperature>10);
    CHECK_EQ(at(52.5,0.95f,2).surface.snow,0.0f);
    CHECK(at(52.5,0.35f,2).surface.snow>0.2f);
    CHECK(at(22.5,0.5f).air.temperature>at(52.5,0.5f).air.temperature);
}
TEST(weather_masks_respect_slopes_water_and_drainage) {
    CHECK_EQ(wxSnowMask(0.5f,0.1f,-1,0.2f),0.0f);
    CHECK_EQ(wxSnowMask(0.5f,1,1,0.2f),0.0f);
    CHECK(wxSnowMask(0.5f,1,-1,0.2f)>0.9f);
    WeatherAir rain{12,0.5f,1,1};
    auto slow=wxEmpty(),fast=wxEmpty();
    for (int i=0;i<10;++i) { slow=wxAdvance(slow,rain,0); fast=wxAdvance(fast,rain,1); }
    CHECK(slow.wet>fast.wet);
}
TEST(weather_fixed_temperature_matches_visual_model) {
    using core::Fixed;
    const core::TimeConfig time;
    const std::array<Fixed,4> seasons{Fixed::fromInt(18),Fixed::fromInt(32),Fixed::fromInt(21),Fixed::fromInt(9)};
    for (int d=0;d<65;d+=2) {
        const auto tick=d*time.ticksPerDay()+time.ticksPerDay()/2;
        const auto fixed=temperatureAt(42,tick,time,seasons,Fixed::ratio(1,2),Fixed::fromInt(120),
            {Fixed::fromInt(14000),Fixed::fromInt(24000)});
        CHECK(std::abs(fixed.toDouble()-at(d+0.5).air.temperature)<0.002);
    }
}
TEST(weather_controls_pause_advance_and_keep_camera_free) {
    client::ExploreMenu menu;
    menu.configureWeather({}, {18,32,21,9},0.5,0);
    menu.advanceWeather(1);
    CHECK(menu.weatherDay()>0.5);
    SDL_Event e{}; e.type=SDL_EVENT_KEY_DOWN; e.key.key=SDLK_F9;
    CHECK(menu.handle(e)); const auto day=menu.weatherDay();
    menu.advanceWeather(10); CHECK_EQ(menu.weatherDay(),day); CHECK(!menu.visible());
    e.key.key=SDLK_F10; CHECK(menu.handle(e)); CHECK(menu.weatherDay()>day);
}
TEST(weather_T_cycles_presets_independently_of_F11_ice_visibility) {
    client::ExploreMenu menu;
    menu.configureWeather({}, {18,32,21,9},52.5,0);
    SDL_Event e{}; e.type=SDL_EVENT_KEY_DOWN; e.key.key=SDLK_F11;
    CHECK(menu.handle(e));
    CHECK(!menu.iceVisible());
    CHECK_EQ(menu.weatherSnapshot().data[0][3],0.0f);
    e.key.key=SDLK_T;
    for (int i=1;i<=5;++i) {
        CHECK(menu.handle(e));
        CHECK_EQ(menu.weatherSnapshot().data[0][3],float(i%5));
        CHECK(!menu.iceVisible());
        CHECK(!menu.visible());
        CHECK_EQ(menu.mapView(),world::MapView::Natural);
    }
    e.key.repeat=true;
    CHECK(!menu.handle(e));
    CHECK_EQ(menu.weatherSnapshot().data[0][3],0.0f);
}
TEST(weather_ice_never_covers_the_ocean_and_fades_at_estuaries) {
    for (float ice : {0.0f,0.2f,0.8f,1.0f}) {
        CHECK_EQ(wxInlandIce(ice,0.0f,1.0f,0.0f),0.0f);
        CHECK_EQ(wxInlandIce(ice,-5.0f,1.0f,0.0f),0.0f);
    }
    CHECK(wxInlandIce(1,100,1,0.0f)>0.99f);
    CHECK_EQ(wxInlandIce(0,100,1,0.0f),0.0f);
    CHECK_EQ(wxInlandIce(1,100,0,0.0f),0.0f);
    float previous=0;
    for (int i=0;i<=100;++i) {
        const float current=wxInlandIce(1,float(i)/100,1,0.0f);
        CHECK(current>=previous);
        CHECK(current-previous<0.04f);
        previous=current;
    }
}
TEST(weather_flat_water_does_not_become_a_waterfall_over_sloping_beds) {
    // Water head is constant on lakes and oceans, regardless of bed normals.
    CHECK_EQ(wxWaterfall(0,1),0.0f);
    CHECK_EQ(wxWaterfall(0.1f,1),0.0f); // Gentle river: may carry ice.
    CHECK(wxWaterfall(0.8f,1)>0.99f);
    CHECK_EQ(wxWaterfall(2,0.5f),0.0f); // Partial dry-apron triangle.
    CHECK(wxInlandIce(1,120,1,0.0f)*(1-wxWaterfall(0,1))>0.99f);
    // Running water does not freeze like a pond. A full winter's cold closes
    // still water completely and leaves a river's channel open, which is what
    // rivers do and what the model used to get wrong: every brook on the map
    // iced over at the same instant the pond beside it did.
    CHECK(wxInlandIce(1,120,1,1.0f)<0.35f);
    CHECK(wxInlandIce(1,120,1,1.0f)<wxInlandIce(1,120,1,0.0f));
    // And the resistance is graded, not a flag: a slow reach still takes ice.
    CHECK(wxInlandIce(1,120,1,0.25f)>wxInlandIce(1,120,1,1.0f));
    CHECK_EQ(wxInlandIce(1,120,1,0.0f)*(1-wxWaterfall(0.8f,1)),0.0f);
}
TEST(weather_save_restores_history_and_temperature_without_extra_rng) {
    content::ContentDb db;
    CHECK(db.load(std::filesystem::path(__FILE__).parent_path().parent_path()/"content"));
    sim::WorldConfig cfg; cfg.seed=42; cfg.worldCells=32; cfg.mapWidth=32; cfg.mapHeight=32; cfg.startingPopulation=0;
    sim::World original(db,cfg);
    original.runTicks(12);
    const auto before=original.weather();
    core::BinaryWriter writer; original.write(writer);
    core::BinaryReader reader(writer.data());
    sim::World restored(db,cfg,sim::World::FromSave{});
    CHECK(restored.read(reader));
    const auto after=restored.weather();
    CHECK_EQ(before.air.temperature,after.air.temperature);
    CHECK_EQ(before.surface.snow,after.surface.snow);
    CHECK_EQ(before.surface.wet,after.surface.wet);
    CHECK_EQ(before.surface.ice,after.surface.ice);
    CHECK_EQ(original.outdoorTemperature(),restored.outdoorTemperature());
    original.runTicks(12); restored.runTicks(12);
    CHECK_EQ(original.checksum(),restored.checksum());
}
