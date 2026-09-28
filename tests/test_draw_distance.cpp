#include "framework.hpp"
#include "game/client/explore_menu.hpp"
#include "../assets/shaders/landscape_look.hlsli"

TEST(distance_fog_is_clear_before_start_opaque_at_draw_distance_and_monotone) {
    using namespace world::look;
    CHECK_EQ(lookFog(0.0f, 3000.0f, 10000.0f), 0.0f);
    CHECK_EQ(lookFog(2999.0f, 3000.0f, 10000.0f), 0.0f);
    CHECK(std::abs(lookFog(10000.0f, 3000.0f, 10000.0f) - 1.0f) < 1e-6f);
    CHECK(std::abs(lookFog(50000.0f, 3000.0f, 10000.0f) - 1.0f) < 1e-6f);
    float previous = 0;
    for (float d = 0; d <= 12000; d += 250) {
        const float f = lookFog(d, 3000.0f, 10000.0f);
        CHECK(f >= previous - 1e-6f);
        // Smooth: no step of more than a few percent between 250 m samples.
        CHECK(f - previous <= 0.06f);
        previous = f;
    }
    // A degenerate span is still a clean cut, never a division by nought.
    CHECK(std::isfinite(lookFog(100.0f, 500.0f, 500.0f)));
}

TEST(draw_distance_slider_maps_logarithmically_drags_and_steps) {
    using client::ExploreMenu;
    CHECK(std::abs(ExploreMenu::distanceAt(ExploreMenu::kTrackLeft) - ExploreMenu::kMinDrawDistance) < 1e-6);
    CHECK(std::abs(ExploreMenu::distanceAt(ExploreMenu::kTrackRight) - ExploreMenu::kMaxDrawDistance) < 1e-3);
    for (double metres : {2000.0, 5000.0, 12000.0, 30000.0, 60000.0})
        CHECK(std::abs(ExploreMenu::distanceAt(ExploreMenu::trackAt(metres)) - metres) < metres * 1e-4);
    ExploreMenu menu;
    CHECK(!menu.pointer(200, 10, true));                        // not on the track
    const float middle = (ExploreMenu::kTrackLeft + ExploreMenu::kTrackRight) * 0.5f;
    CHECK(menu.pointer(middle, ExploreMenu::kTrackY, true));    // grabbed
    const double atMiddle = menu.drawDistance();
    CHECK(std::abs(atMiddle - std::sqrt(ExploreMenu::kMinDrawDistance * ExploreMenu::kMaxDrawDistance)) < 1.0);
    CHECK(menu.pointer(ExploreMenu::kTrackRight + 300, 300, true)); // held off the track
    CHECK(std::abs(menu.drawDistance() - ExploreMenu::kMaxDrawDistance) < 1e-3);
    CHECK(menu.pointer(0, 0, false));                            // release is reported once
    CHECK(!menu.pointer(0, 0, false));
    SDL_Event key{};key.type = SDL_EVENT_KEY_DOWN;key.key.key = SDLK_COMMA;
    CHECK(menu.handle(key));
    CHECK(menu.drawDistance() < ExploreMenu::kMaxDrawDistance);
    key.key.repeat = true;const double before = menu.drawDistance();
    CHECK(menu.handle(key));CHECK(menu.drawDistance() < before); // held key keeps stepping
    menu.drawDistance(1);CHECK_EQ(menu.drawDistance(), ExploreMenu::kMinDrawDistance);
}

TEST(graphics_settings_round_trip_clamp_and_presets) {
    game::GraphicsSettings s;
    const auto sun = s.sunDirection();
    // The default is the direction the renderer always used.
    CHECK(std::abs(sun[0] + 0.55f) < 0.02f); CHECK(std::abs(sun[1] + 0.55f) < 0.02f);
    CHECK(std::abs(sun[2] - 0.63f) < 0.02f);
    s.sunElevation = 500; s.cloudCoverage = -3; s.drawDistanceKm = 1000; s.quality = 9;
    s.clampAll();
    CHECK(s.sunElevation <= 89.0f); CHECK_EQ(s.cloudCoverage, 0.0f); CHECK_EQ(s.drawDistanceKm, 60.0f);
    CHECK_EQ(s.quality, 3);
    const auto low = game::graphicsPreset(0), ultra = game::graphicsPreset(3);
    CHECK(low.lodScale() > ultra.lodScale()); CHECK(low.drawDistanceKm < ultra.drawDistanceKm);
    CHECK_EQ(low.cloudSteps() , 12); CHECK_EQ(ultra.cloudSteps(), 48);
    const auto file = std::filesystem::temp_directory_path() / ("campfire-graphics-" + std::to_string(SDL_GetTicksNS()) + ".json");
    game::GraphicsSettings saved = ultra; saved.exposure = 1.7f; saved.skybox = false;
    CHECK(game::saveGraphicsSettings(file, saved));
    const auto loaded = game::loadGraphicsSettings(file);
    CHECK(loaded == saved);
    std::filesystem::remove(file);
    CHECK(game::loadGraphicsSettings(file) == game::GraphicsSettings{}); // missing file: defaults
}
