#include "framework.hpp"
#include "game/client/explore_menu.hpp"

namespace {
SDL_Event key(SDL_Keycode code, bool repeat=false) {
    SDL_Event event{};
    event.type=SDL_EVENT_KEY_DOWN;
    event.key.key=code;
    event.key.repeat=repeat;
    return event;
}
SDL_Event stageKey(int index, SDL_Keymod modifiers=SDL_KMOD_LSHIFT) {
    auto event=key(SDLK_UNKNOWN);
    // Shifted keycodes depend on the layout; physical 1..8 select stages.
    event.key.scancode=static_cast<SDL_Scancode>(SDL_SCANCODE_1+index);
    event.key.mod=modifiers;
    return event;
}
}

TEST(environment_menu_selects_all_eight_modes_without_blocking_camera) {
    client::ExploreMenu menu;
    CHECK(!menu.showingAnything());
    for (int i=0;i<8;++i) {
        CHECK(menu.handle(key(SDLK_F1+i)));
        CHECK_EQ(static_cast<int>(menu.mapView()),i+1);
        CHECK(!menu.visible());
        CHECK(menu.showingAnything());
    }
    CHECK(!menu.handle(key(SDLK_1))); // Desert teleport still reaches the explorer.
    CHECK(!menu.handle(key(SDLK_F2,true)));
    CHECK_EQ(menu.mapView(),world::MapView::Wind);
    CHECK(!menu.selectMap("not-a-map"));
    CHECK_EQ(menu.mapView(),world::MapView::Wind);
    for (int i=static_cast<int>(world::MapView::Grey);i<static_cast<int>(world::MapView::Count);++i) {
        CHECK(menu.handle(key(SDLK_M)));
        CHECK_EQ(static_cast<int>(menu.mapView()),i);
        CHECK(menu.showingAnything());
        CHECK(!menu.visible());
    }
    CHECK(menu.handle(key(SDLK_M)));
    CHECK_EQ(menu.mapView(),world::MapView::Natural);
    CHECK(!menu.showingAnything());
    for (const char* name:world::kMapNames) CHECK(menu.selectMap(name));
    CHECK(menu.handle(key(SDLK_0)));
    CHECK_EQ(menu.mapView(),world::MapView::Natural);
}

TEST(environment_menu_stage_shortcuts_are_independent_of_display_and_camera) {
    client::ExploreMenu menu;
    menu.stagesAvailable(true);
    CHECK(menu.selectMap("grey"));
    for (int i=0;i<8;++i) {
        CHECK(menu.handle(stageKey(i,i%2 ? SDL_KMOD_RSHIFT : SDL_KMOD_LSHIFT)));
        CHECK_EQ(static_cast<int>(menu.terrainStage()),i);
        CHECK_EQ(menu.mapView(),world::MapView::Grey);
        CHECK(!menu.visible());
    }
    CHECK(menu.handle(stageKey(0)));
    CHECK(menu.handle(key(SDLK_0)));
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Noise);
    CHECK_EQ(menu.mapView(),world::MapView::Natural);
    CHECK(menu.showingAnything());
    CHECK(!menu.handle(stageKey(7,SDL_KMOD_NONE)));
    CHECK(!menu.handle(stageKey(8))); // Shift+9 is not a stage selector.
    auto repeated=stageKey(7); repeated.key.repeat=true;
    CHECK(!menu.handle(repeated));
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Noise);
    CHECK(menu.handle(key(SDLK_M)));
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Noise);
    CHECK_EQ(menu.mapView(),world::MapView::Temperature);
    CHECK(menu.handle(key(SDLK_TAB)));
    CHECK(menu.handle(stageKey(7)));
    CHECK(menu.visible());
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Final);
}

TEST(environment_menu_legacy_worlds_cannot_select_missing_stages) {
    client::ExploreMenu menu;
    for (int i=0;i<8;++i) {
        CHECK(menu.handle(stageKey(i))); // Consume the shortcut, not a teleport.
        CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Final);
    }
    menu.stagesAvailable(true);
    CHECK(menu.handle(stageKey(2)));
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Thermal);
    CHECK(menu.selectMap("slope"));
    menu.stagesAvailable(false);
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Final);
    CHECK_EQ(menu.mapView(),world::MapView::Slope);
    CHECK(menu.handle(stageKey(0)));
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Final);
    menu.stagesAvailable(true);
    CHECK_EQ(menu.terrainStage(),generation::TerrainStage::Final);
}

TEST(environment_menu_weather_soil_and_base_toggle_are_independent) {
    client::ExploreMenu menu;
    CHECK(menu.selectMap("fertility"));
    const core::WorldPos at{core::Fixed::fromInt(64),core::Fixed::fromInt(64)};
    CHECK(menu.handle(key(SDLK_H)));
    menu.applySoilRequest(at);
    const auto depleted=menu.soil().deltaAt(at);
    CHECK(depleted<core::kZero);
    CHECK(menu.handle(key(SDLK_B)));
    CHECK(menu.potentialOnly());
    CHECK_EQ(menu.soil().deltaAt(at),depleted);
    CHECK(menu.handle(key(SDLK_T)));
    CHECK(menu.conditions().rainfall<core::kOne);
    CHECK_EQ(menu.soil().deltaAt(at),depleted);
    CHECK(menu.handle(key(SDLK_J)));
    menu.applySoilRequest(at);
    CHECK(menu.soil().deltaAt(at)>depleted);
    for (int i=0;i<6;++i) {
        CHECK(menu.handle(key(SDLK_N)));
        menu.applySoilRequest(at);
    }
    CHECK_EQ(menu.soil().deltaAt(at),depleted);
    CHECK(menu.handle(key(SDLK_H)));
    menu.resetEnvironment();
    menu.applySoilRequest(at);
    CHECK_EQ(menu.soil().deltaAt(at),core::kZero);
    CHECK(menu.selectMap("flood"));
    CHECK(menu.takeFloodRequest());
    CHECK(!menu.takeFloodRequest());
    CHECK(menu.handle(key(SDLK_R)));
    CHECK(menu.takeFloodRequest());
}

TEST(environment_menu_hides_ice_without_changing_weather_or_maps) {
    client::ExploreMenu menu;
    const auto before = menu.weatherSnapshot();
    CHECK(menu.iceVisible());
    CHECK(menu.handle(key(SDLK_F11)));
    CHECK(!menu.iceVisible());
    CHECK(menu.showingAnything());
    CHECK(!menu.visible()); // The camera remains usable.
    CHECK(!menu.handle(key(SDLK_F11, true)));
    CHECK(!menu.iceVisible());
    CHECK_EQ(menu.mapView(), world::MapView::Natural);
    CHECK(menu.weatherSnapshot().data == before.data);
    CHECK(menu.handle(key(SDLK_F11)));
    CHECK(menu.iceVisible());
    menu.iceVisible(false); // Command-line initialization follows the same path.
    menu.resetEnvironment();
    CHECK(!menu.iceVisible());
    CHECK(menu.weatherSnapshot().data == before.data);
}

