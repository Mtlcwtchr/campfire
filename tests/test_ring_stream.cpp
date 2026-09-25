#include "framework.hpp"
#include "game/client/explorer.hpp"
#include "game/world/tile_mesh.hpp"
#include "game/client/controls.hpp"
#include "game/world/foliage_catalog.hpp"
#include "../assets/shaders/foliage_field.hlsli"
#include "../assets/shaders/sand_motion.hlsli"
#include "../assets/shaders/landscape_look.hlsli"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <thread>
#include <utility>

namespace shore_shader {
// Compile the shader's scalar envelopes verbatim; no second animation model.
using std::asin;
using std::cos;
using std::sin;
float frac(float value) { return value - std::floor(value); }
float smoothstep(float low, float high, float value) {
    const float t = std::clamp((value - low) / (high - low), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
#include "../assets/shaders/shore_motion.hlsli"
#include "../assets/shaders/water_motion.hlsli"
}

namespace {
using namespace std::chrono_literals;
const generation::WorldMapData& country() {
    static const auto world = [] {
        generation::WorldMapParams params;
        params.seed = 42;
        params.width = params.height = 32;
        params.erosionPasses = 1;
        return generation::generateWorldMap(params);
    }();
    return world;
}
template<class Predicate> bool eventually(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
// A square of tiles around the origin, nearest first. The epoch is gone: a
// tile is named by the ground it covers, so an order is a place.
std::vector<client::PatchWorkshop::Order> orders(std::int32_t lod = 0) {
    std::vector<client::PatchWorkshop::Order> result;
    for (std::int32_t y = 0; y < 3; ++y)
        for (std::int32_t x = 0; x < 3; ++x)
            result.push_back({world::TileId{x, y, lod}, double(x + y)});
    return result;
}
}

TEST(landscape_highlights_keep_midtones_and_roll_off_without_clipping) {
    using namespace world::look;
    CHECK_EQ(lookHighlight(-1), 0.0f);
    float previous = 0;
    for (int i = 0; i <= 2000; ++i) {
        const float value = static_cast<float>(i) * 0.002f;
        const float mapped = lookHighlight(value);
        CHECK(std::isfinite(mapped) && mapped >= previous && mapped < 1);
        if (value <= 0.78f) CHECK(std::abs(value - mapped) < 1e-6f);
        previous = mapped;
    }
    CHECK(lookHighlight(1) > 0.88f);
    CHECK(std::abs(lookHighlight(0.7801f) - lookHighlight(0.7799f)) < 0.0003f);
}

TEST(camera_controls_orbit_pan_and_height_are_independent) {
    client::Camera camera; camera.isometric=true;
    client::CameraControl controls; controls.edgePan=false;
    ui::Input input; input.mouseX=500; input.mouseY=300; input.rightDown=true;
    bool keys[SDL_SCANCODE_COUNT]{};
    const auto yaw=camera.yaw;
    controls.update(camera,input,keys,1.0/60,false);
    input.mouseX+=100; input.mouseY+=20;
    controls.update(camera,input,keys,1.0/60,false);
    CHECK(std::abs(camera.yaw-yaw-0.6)<1e-9);
    CHECK_EQ(camera.centreX,0); CHECK_EQ(camera.centreY,0);
    input.rightDown=false; input.middleDown=true;
    controls.update(camera,input,keys,1.0/60,false);
    double wx,wy; camera.worldOfScreen(600,320,wx,wy);
    input.mouseX+=20; input.mouseY-=40;
    controls.update(camera,input,keys,1.0/60,false);
    float sx,sy; camera.worldToScreen(wx,wy,sx,sy);
    CHECK(std::abs(sx-input.mouseX)<0.001); CHECK(std::abs(sy-input.mouseY)<0.001);
    input.middleDown=false; keys[SDL_SCANCODE_PAGEUP]=true;
    controls.update(camera,input,keys,0.1,false);
    CHECK(camera.heightOffset>0); CHECK_EQ(camera.focusHeight,camera.heightOffset);
    keys[SDL_SCANCODE_PAGEUP]=false; keys[SDL_SCANCODE_HOME]=true;
    controls.update(camera,input,keys,0.1,false);
    CHECK_EQ(camera.heightOffset,0); CHECK_EQ(camera.yaw,client::kDefaultCameraYaw);
}

TEST(camera_controls_follow_screen_after_rotation_and_respect_ui) {
    client::Camera camera; camera.isometric=true; camera.yaw=2.4;
    client::CameraControl controls; controls.edgePan=false;
    ui::Input input; input.mouseX=500; input.mouseY=300;
    bool keys[SDL_SCANCODE_COUNT]{}; keys[SDL_SCANCODE_W]=true;
    controls.update(camera,input,keys,1.0/60,false);
    float sx,sy; camera.worldToScreen(0,0,sx,sy);
    CHECK(std::abs(sx-camera.viewportWidth/2.0)<0.001);
    CHECK(sy>camera.viewportHeight/2.0);
    keys[SDL_SCANCODE_W]=false; input.middleDown=true; input.shift=true;
    const auto yaw=camera.yaw;
    controls.update(camera,input,keys,0.01,true);
    input.mouseX+=100;
    controls.update(camera,input,keys,0.01,true);
    CHECK_EQ(camera.yaw,yaw); CHECK(!controls.dragging); CHECK(!controls.orbiting);
}

TEST(camera_controls_free_flight_follows_eye_and_preserves_independent_height) {
    client::Camera camera;
    camera.mode = client::Camera::Mode::Free;
    camera.yaw = 0; camera.pitch = std::numbers::pi / 6;
    camera.focusHeight = 100;
    camera.flightSpeed = 80;
    client::CameraControl controls;
    ui::Input input;
    bool keys[SDL_SCANCODE_COUNT]{};
    keys[SDL_SCANCODE_W] = true;
    controls.update(camera, input, keys, 0.1, false);
    CHECK(std::abs(camera.centreX + 8 * std::cos(camera.pitch)) < 1e-9);
    CHECK_EQ(camera.centreY, 0);
    CHECK(std::abs(camera.focusHeight - 96) < 1e-9);
    CHECK_EQ(camera.heightOffset, 0);
    const auto eye = camera.eyePosition();
    controls.update(camera, input, keys, 0.1, true);
    CHECK_EQ(camera.eyePosition(), eye);
    keys[SDL_SCANCODE_W] = false;
    keys[SDL_SCANCODE_E] = keys[SDL_SCANCODE_LSHIFT] = true;
    controls.update(camera, input, keys, 0.1, false);
    CHECK(std::abs(camera.focusHeight - 128) < 1e-9);
    camera.mode = client::Camera::Mode::Orbit;
    const auto height = camera.focusHeight;
    controls.update(camera, input, keys, 0.1, false);
    CHECK(camera.heightOffset > 0);
    CHECK(std::abs(camera.focusHeight - height - camera.heightOffset) < 1e-9);
}

TEST(landscape_haze_is_bounded_and_keeps_close_ground_clear) {
    using namespace world::look;
    CHECK_EQ(lookHaze(0, 0), 0.0f);
    CHECK_EQ(lookHaze(-100, 0), 0.0f);
    CHECK(lookHaze(100, 0) < 0.01f);
    for (float height : {-100.0f, 0.0f, 200.0f, 2000.0f}) {
        float previous = 0;
        for (float distance : {0.0f, 100.0f, 1000.0f, 5000.0f, 50000.0f}) {
            const float haze = lookHaze(distance, height);
            CHECK(haze >= previous && haze <= 0.32f);
            previous = haze;
        }
    }
    CHECK(lookHaze(2000, 1000) < lookHaze(2000, 0));
}

TEST(landscape_water_wetness_and_roots_have_smooth_bounded_profiles) {
    using namespace world::look;
    CHECK_EQ(lookWaterDepth(-2), 0.0f);
    CHECK(std::abs(lookWaterDepth(3.5f) - 0.5f) < 1e-6f);
    float previous = 0;
    for (int i = 0; i <= 100; ++i) {
        const float depth = lookWaterDepth(static_cast<float>(i) * 0.2f);
        CHECK(depth >= previous && depth <= 1);
        previous = depth;
    }
    CHECK_EQ(lookWetness(-1, 1), 0.0f);
    CHECK_EQ(lookWetness(0, 1), 1.0f);
    CHECK_EQ(lookWetness(0, 0.3f), 0.0f);
    CHECK(lookWetness(-0.45f, 1) > 0.45f && lookWetness(-0.45f, 1) < 0.55f);
    previous = 0;
    for (int i = -10; i <= 110; ++i) {
        const float root = lookRootOcclusion(static_cast<float>(i) / 100.0f);
        CHECK(root >= 0.64f && root <= 1.0f && root >= previous);
        previous = root;
    }
}

TEST(ring_workers_freeze_deduplicate_and_return_owned_data) {
    client::PatchWorkshop workshop(country(), 42);
    workshop.debugFreeze(true);
    const auto wanted = orders(1);
    auto repeated = wanted;
    repeated.insert(repeated.end(), wanted.begin(), wanted.end());
    workshop.wants(repeated);
    CHECK_EQ(workshop.waiting(), wanted.size());
    CHECK(workshop.collect().empty());
    workshop.debugFreeze(false);
    std::vector<client::PatchWorkshop::Finished> done;
    auto remaining = wanted;
    CHECK(eventually([&] {
        workshop.wants(remaining); // Repeated frame requests must not duplicate running jobs.
        for (auto& result : workshop.collect()) {
            std::erase_if(remaining, [&](const auto& order) {
                return world::tileKeyOf(order.tile) == world::tileKeyOf(result.tile);
            });
            done.push_back(std::move(result));
        }
        return done.size() == wanted.size() && workshop.waiting() == 0;
    }));
    CHECK_EQ(done.size(), wanted.size());
    std::set<std::int64_t> keys;
    for (const auto& result : done) {
        CHECK(keys.insert(world::tileKeyOf(result.tile)).second);
        CHECK(!result.mesh.vertices.empty());
        CHECK_EQ(result.waterLevel.size(), result.mesh.vertices.size());
        CHECK_EQ(result.waterCover.size(), result.mesh.vertices.size());
    }
}

TEST(workers_drop_ground_the_view_no_longer_wants_and_shut_down_promptly) {
    const auto start = std::chrono::steady_clock::now();
    {
        client::PatchWorkshop workshop(country(), 42);
        workshop.debugArtificialDelay(5s);
        // Level nought asked for, then level two instead: the first lot is
        // ground the view no longer wants, and a worker holding one of them
        // stops rather than finishing it. It used to be keyed on the snapshot
        // an order belonged to, which threw work away for having moved even
        // when the ground was still on screen.
        workshop.wants(orders(0));
        std::this_thread::sleep_for(10ms);
        workshop.debugArtificialDelay(0ms);
        workshop.wants(orders(2));
        std::vector<client::PatchWorkshop::Finished> done;
        CHECK(eventually([&] {
            for (auto& result : workshop.collect()) done.push_back(std::move(result));
            return done.size() == orders(2).size() && workshop.waiting() == 0;
        }));
        CHECK_EQ(done.size(), orders(2).size());
        for (const auto& result : done) CHECK_EQ(result.tile.lod, 2);
        workshop.debugArtificialDelay(5s);
        workshop.wants(orders(3));
    }
    CHECK(std::chrono::steady_clock::now() - start < 5s);
}
TEST(explorer_draws_one_level_once_and_follows_the_camera) {
    // What survives of the old frontier test. It checked the ring key, the
    // epoch and the frontier's own value, none of which exist: a tile is
    // named by the ground it covers, so there is no snapshot to belong to and
    // no curtain to travel. What it was for still holds.
    // Exercise camera/world lifecycle, not a 32x32 H64/H16 baking benchmark.
    // Startup now owns the shared pool until both immutable levels publish;
    // repeating the large world's complete bake could exhaust the 15 s budget
    // in Debug before the mesh work under test was allowed to start.
    generation::WorldMapParams params;
    params.seed = 42;
    params.width = params.height = 4;
    params.erosionPasses = 1;
    const auto smallCountry = generation::generateWorldMap(params);
    client::Explorer explorer(smallCountry, 42);
    client::Camera camera;
    camera.viewportWidth = camera.viewportHeight = 128;
    camera.pixelsPerTile = 8;
    camera.centreX = camera.centreY = 100;
    const auto settle = [&] {
        return eventually([&] {
            explorer.update(camera, 1.0 / 30);
            const auto& visible = explorer.prepareVisible(camera);
            std::set<const void*> drawn;
            for (const auto& seen : visible) {
                // Each piece of ground once, and the sharp ones all at the
                // level the view asked for.
                CHECK(drawn.insert(seen.patch).second);
                if (!seen.backdrop) CHECK_EQ(seen.patch->mesh.lod, explorer.lastLevel());
            }
            return !explorer.stillFillingIn();
        });
    };
    CHECK(settle());
    const auto oldLevel = explorer.lastLevel();
    camera.pixelsPerTile = 0.2;
    explorer.update(camera, 1.0 / 30);
    CHECK(!explorer.prepareVisible(camera).empty());   // coverage survives the zoom
    CHECK(explorer.lastLevel() > oldLevel);
    CHECK(settle());
    camera.centreX += 100000;
    CHECK(settle());
    CHECK(!explorer.prepareVisible(camera).empty());
    explorer.restart(smallCountry, 43);
    CHECK_EQ(explorer.held(), 0u);
    CHECK(settle());
}

TEST(tile_mesh_stops_when_it_is_told_to) {
    world::HeightField field(&country(), 42);
    const auto graph = world::streaming::buildHydrologyGraph(country());
    world::streaming::PageStore pages(country(), graph,
                                      world::streaming::hsimQuantisationFor(country()),
                                      {8u << 20, world::streaming::kDefaultPaddingSamples});
    world::ClimateField climate;
    climate.raise(country(), field);
    const auto mesh =
            world::buildTileMesh(field, pages, climate, {0, 0, 0}, [] { return true; });
    CHECK(mesh.vertices.empty());
}

TEST(moving_keeps_the_ground_it_has_and_pays_for_the_strip_it_entered) {
    // What the square tile is for.
    //
    // An annulus is addressed by a snapshot of the camera, so a step sideways
    // renames every piece of ground and the whole view is built again from
    // nothing - which is why moving used to cost as much as arriving. A tile
    // is named by the ground it covers: step sideways and the tiles that were
    // wanted before and are wanted still are the same tiles, already built.
    client::Explorer explorer(country(), 42);
    client::Camera camera;
    camera.viewportWidth = camera.viewportHeight = 512;
    camera.pixelsPerTile = 4;
    camera.centreX = camera.centreY = 8000;
    const auto settle = [&] {
        return eventually([&] {
            explorer.update(camera, 0.02);
            explorer.prepareVisible(camera);
            return explorer.lastWanted() > 8 && !explorer.stillFillingIn();
        });
    };
    CHECK(settle());
    const std::size_t before = explorer.held();
    CHECK(before > 8);

    // A step of a quarter of what the view reaches: a strip enters, the rest
    // of the view is the ground that was already there.
    const double step = explorer.streamRadius() * 0.25;
    camera.centreX += step;
    CHECK(settle());

    // Most of what was built is still held, and what the move cost is the
    // strip rather than the view. Generous bounds: the point is the order of
    // magnitude, not a particular number of tiles.
    CHECK(explorer.held() >= before);
    CHECK(explorer.arrived() * 4 < before);
}

TEST(the_coarsest_level_is_the_world_and_is_never_taken_away) {
    // The difference between a cache and a foundation.
    //
    // Everything else the explorer holds is a tenancy: it is cut because the
    // eye came near and forgotten a few seconds after it left, which is right
    // for a level whose whole world would not fit. The coarsest level does
    // fits. At the widest zoom the immutable H64 data is drawn through 256 m
    // topology, so the default map needs only a handful of land instances. It
    // is cut once and kept, and that is what makes a place never blank: an
    // arriving tile refines something already on screen rather than replacing
    // nothing at all.
    client::Explorer explorer(country(), 42);
    client::Camera camera;
    camera.viewportWidth = camera.viewportHeight = 512;
    camera.pixelsPerTile = 4;
    camera.centreX = camera.centreY = 8000;
    const auto settle = [&] {
        return eventually([&] {
            explorer.update(camera, 0.02);
            explorer.prepareVisible(camera);
            return explorer.onOrder() == 0 && !explorer.stillFillingIn() &&
                   explorer.coarseHeld() > 0;
        });
    };
    CHECK(settle());
    const std::size_t foundation = explorer.coarseHeld();
    CHECK(foundation > 0);

    // Away, far enough that nothing built here is anywhere near the view, and
    // long enough that a tenancy would have run out several times over.
    camera.centreX = camera.centreY = 40000;
    CHECK(settle());
    for (int frame = 0; frame < 2000; ++frame) {
        explorer.update(camera, 0.02);
        explorer.prepareVisible(camera);
    }
    // Not a tile of it has gone, and none of it was cut a second time.
    CHECK_EQ(explorer.coarseHeld(), foundation);
}

TEST(zooming_while_the_view_fills_keeps_what_is_already_on_screen) {
    // Ground already drawn is not taken away because the level changed. It
    // used to be checked through the retained snapshot's own key and frontier;
    // there is no snapshot now, and what keeps the screen covered is the
    // coarse ground under it, which is the same answer arrived at honestly.
    client::Explorer explorer(country(), 42);
    client::Camera camera;
    camera.viewportWidth = camera.viewportHeight = 1024;
    camera.pixelsPerTile = 1;
    explorer.setStreamingDebugDelay(std::chrono::milliseconds(40));
    CHECK(eventually([&] {
        explorer.update(camera, 0.001);
        return !explorer.prepareVisible(camera).empty();
    }));
    explorer.setStreamingFrozen(true);
    explorer.prepareVisible(camera);
    const std::size_t before = explorer.held();
    CHECK(before > 0);
    camera.pixelsPerTile = 0.2;
    explorer.update(camera, 0.001);
    CHECK(!explorer.prepareVisible(camera).empty());
    // Nothing built is thrown away for a change of level: a tile is the same
    // tile whatever the camera is doing.
    CHECK(explorer.held() >= before);
}

TEST(ring_view_is_ready_the_moment_its_ground_is) {
    // The opposite of what this used to check.
    //
    // It was called "readiness waits for the reveal after workers finish", and
    // it asserted that a view whose every ring had been built still reported
    // itself unfinished - because an animated frontier had not yet crawled out
    // to the edge. That was not a fact about the data, and it was expensive:
    // measured on a camera sweep, a snapshot with everything built was held for
    // another hundred and seventy frames while the curtain travelled, by which
    // time the camera stood four kilometres outside the ground it covered.
    //
    // Built is ready. What is still worth holding is that readiness is stable
    // once reached, and that nothing appears or disappears afterwards.
    client::Explorer explorer(country(), 42);
    client::Camera camera;
    camera.viewportWidth = camera.viewportHeight = 128;
    camera.pixelsPerTile = 8;
    CHECK(!explorer.fullyRevealed());
    CHECK(eventually([&] {
        explorer.update(camera, 0.000001);
        explorer.prepareVisible(camera);
        return explorer.lastWanted() > 0 && !explorer.stillFillingIn() &&
               explorer.onOrder() == 0;
    }));
    // No further frames, no animation time: the ground is there, so the view is.
    explorer.prepareVisible(camera);
    CHECK(explorer.fullyRevealed());
    const auto count = explorer.prepareVisible(camera).size();
    for (int i = 0; i < 4; ++i) {
        explorer.update(camera, 0.1);
        CHECK_EQ(explorer.prepareVisible(camera).size(), count);
        CHECK(explorer.fullyRevealed());
    }
}

TEST(ring_workers_really_execute_concurrently) {
    client::PatchWorkshop workshop(country(), 42);
    workshop.debugArtificialDelay(150ms);
    std::vector<client::PatchWorkshop::Order> wanted;
    for (std::int32_t y = 0; y < 6; ++y)
        for (std::int32_t x = 0; x < 6; ++x)
            wanted.push_back({world::TileId{x, y, 0}, double(x + y)});
    workshop.wants(std::move(wanted));
    const auto workers = workshop.workerStats().workers;
    CHECK(eventually([&] { return workshop.workerStats().peakBusy >= std::min<std::size_t>(workers, 2); }));
    std::cout << "  worker threads: " << workers << ", peak active: " << workshop.workerStats().peakBusy << '\n';
}

TEST(ring_plateau_stream_does_not_load_down_to_sea_level) {
    auto plateau = country();
    for (auto& cell : plateau.cells) {
        cell.elevation = 140;
        cell.sea = cell.river = false;
        cell.drainOut = cell.riverOut = -1;
        cell.drainSize = 0;
    }
    client::Explorer explorer(plateau, 42);
    client::Camera camera;
    camera.isometric = true;
    camera.pixelsPerTile = 14;
    camera.centreX = camera.centreY = 8.0 * generation::kMetresPerCell;
    camera.focusHeight = 140 * generation::kMetresPerElevationStep;
    explorer.update(camera, 1.0 / 60);
    explorer.prepareVisible(camera);
    CHECK_EQ(explorer.lastLevel(), 0);
    CHECK(explorer.streamRadius() < 700); // previously > 1900 m on this plateau
}

TEST(ring_orbit_footprint_contains_water_at_all_screen_corners) {
    auto ocean = country();
    for (auto& cell : ocean.cells) {
        cell.sea = true;
        cell.elevation = 0;
        cell.river = false;
        cell.drainOut = cell.riverOut = -1;
        cell.drainSize = 0;
    }
    for (double yaw : {-2.4, -0.7, 0.8, 2.1})
        for (double pitch : {client::kMinCameraPitch, client::kDefaultCameraPitch, client::kMaxCameraPitch}) {
            client::Explorer explorer(ocean, 42);
            client::Camera camera;
            camera.isometric = true;
            camera.viewportWidth = 160;
            camera.viewportHeight = 100;
            camera.pixelsPerTile = 8;
            camera.centreX = camera.centreY = 8.0 * generation::kMetresPerCell;
            camera.yaw = yaw;
            camera.pitch = pitch;
            camera.focusHeight = camera.heightOffset = 60;
            explorer.update(camera, 1.0 / 60);
            explorer.prepareVisible(camera);
            for (int x : {0, camera.viewportWidth}) for (int y : {0, camera.viewportHeight}) {
                double wx, wy;
                camera.worldOfScreenAtHeight(x, y, 0, wx, wy);
                CHECK(std::hypot(wx-camera.centreX, wy-camera.centreY) <= explorer.streamRadius());
            }
        }
}

TEST(ring_height_bounds_contain_sampled_terrain_and_water) {
    world::HeightField field(&country(), 42);
    for (int lod : {0, 3, 6}) {
        for (int centre : {-400, 100, 8000, 17000}) {
            const core::WorldPos low{core::Fixed::fromInt(centre - 200), core::Fixed::fromInt(centre - 200)};
            const core::WorldPos high{core::Fixed::fromInt(centre + 200), core::Fixed::fromInt(centre + 200)};
            const auto bounds = world::ringHeightBounds(country(), low, high, lod);
            for (int y = centre - 200; y <= centre + 200; y += 80)
                for (int x = centre - 200; x <= centre + 200; x += 80) {
                    const auto height = field.sampleHeight(world::floorDiv(x, 4), world::floorDiv(y, 4),
                                                           world::sampleMetresAt(lod)).toDouble();
                    CHECK(height >= bounds.first && height <= bounds.second);
                    const core::WorldPos p{core::Fixed::fromInt(x), core::Fixed::fromInt(y)};
                    const auto water = field.waterOver(p, core::Fixed::fromInt(world::sampleMetresAt(lod)), core::kZero);
                    if (water.cover.raw > 0) CHECK(water.level.toDouble() <= bounds.second);
                }
        }
    }
}
TEST(ring_foliage_is_deterministic_cancellable_and_avoids_water) {
    world::TerrainMesh mesh;
    for (const auto& p : {std::pair{0, 0}, std::pair{4, 0}, std::pair{0, 4}, std::pair{4, 4}}) {
        world::TerrainVertex v{};
        v.position = {core::Fixed::fromInt(p.first), core::Fixed::fromInt(p.second)};
        v.height = core::Fixed::fromInt(5);
        v.normal.z = core::kOne;
        v.materials.weight[static_cast<std::size_t>(world::Material::Grass)] = core::kOne;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 2, 3, 0, 3, 1};
    std::vector<core::Fixed> water(4, core::kZero);
    const auto first = world::buildRingFoliage(mesh, water);
    const auto again = world::buildRingFoliage(mesh, water);
    CHECK(!first.empty());
    CHECK_EQ(first.size(), again.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK_EQ(first[i].position[0], again[i].position[0]);
        CHECK_EQ(first[i].position[1], again[i].position[1]);
        CHECK_EQ(first[i].position[2], 5.0f);
    }
    CHECK(world::buildRingFoliage(mesh, water, [] { return true; }).empty());
    water.assign(4, core::Fixed::fromInt(6));
    CHECK(world::buildRingFoliage(mesh, water).empty());
}

TEST(ring_foliage_lods_are_nested_and_bounded) {
    world::TerrainMesh mesh;
    for (const auto& p : {std::pair{-64, -64}, std::pair{64, -64},
                          std::pair{-64, 64}, std::pair{64, 64}}) {
        world::TerrainVertex v{};
        v.position = {core::Fixed::fromInt(p.first), core::Fixed::fromInt(p.second)};
        v.height = core::Fixed::fromInt(5);
        v.normal.z = core::kOne;
        v.materials.weight[static_cast<std::size_t>(world::Material::Grass)] = core::kOne;
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 2, 3, 0, 3, 1};
    const std::vector<core::Fixed> water(4, core::kZero);
    std::set<std::pair<float, float>> previous;
    for (int lod = 0; lod <= 2; ++lod) {
        mesh.lod = lod;
        const auto plants = world::buildRingFoliage(mesh, water);
        CHECK(!plants.empty());
        std::set<std::pair<float, float>> positions;
        for (const auto& plant : plants) {
            const auto xy = std::pair{plant.position[0], plant.position[1]};
            CHECK(positions.insert(xy).second);
            if (lod > 0) CHECK(previous.contains(xy));
            CHECK(plant.scale > 0 && plant.scale < 4.0f);
            CHECK(std::abs(plant.position[2] - 5.0f) < 1e-5f);
        }
        if (lod > 0) CHECK(plants.size() * 4 < previous.size());
        CHECK(world::buildRingFoliage(mesh, water, [] { return true; }).empty());
        previous = std::move(positions);
    }
    for (int lod = 3; lod <= 12; ++lod) {
        mesh.lod = lod;
        CHECK(world::buildRingFoliage(mesh, water).empty()); // Aggregate GPU cover instead.
    }
}

TEST(foliage_field_preserves_meadows_at_every_footprint) {
    using namespace world::foliage;
    // Damp, level, unbroken ground: the meadow case.
    const float suitable = foliageSuitability(0.8f, 0.1f, 0, 1, -1, 0.6f, 0);
    CHECK(suitable > 0.4f);
    CHECK_EQ(foliageSuitability(1, 0, 0, 1, 1, 0.6f, 0), 0.0f);      // under water
    CHECK_EQ(foliageSuitability(1, 0, 0, 0.4f, -1, 0.6f, 0), 0.0f);  // too steep
    CHECK_EQ(foliageSuitability(0, 1, 0, 1, -1, 0.6f, 0), 0.0f);     // bare rock
    CHECK_EQ(foliageSuitability(1, 0, 0.7f, 1, -1, 0.6f, 0), 0.0f);  // on the shore
    // Dry country is thinner than damp country, and a hollow makes up for it.
    const float parched = foliageSuitability(0.8f, 0.1f, 0, 1, -1, 0.05f, 0);
    CHECK(parched > 0.0f);
    CHECK(parched < suitable);
    CHECK(foliageSuitability(0.8f, 0.1f, 0, 1, -1, 0.05f, 1.0f) > parched);
    // The grass-rock ecotone carries less than the weights alone suggest.
    CHECK(foliageSuitability(0.5f, 0.5f, 0, 1, -1, 0.6f, 0) <
          foliageSuitability(0.5f, 0.0f, 0, 1, -1, 0.6f, 0) * 0.5f);
    float low = 1, high = 0;
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 7.3f;
        const float field = foliageField(x, -31.7f, 0);
        low = std::min(low, field); high = std::max(high, field);
        CHECK_EQ(field, foliageField(x, -31.7f, 0));
        CHECK(field >= 0 && field <= 1);
        CHECK(std::abs(field - foliageField(x + 0.001f, -31.7f, 0)) < 0.001f);
        for (int lod = 0; lod <= 12; ++lod) {
            const float filtered = foliageField(x, -31.7f, static_cast<float>(1 << lod));
            CHECK(foliageDensity(suitable, filtered) > 0.5f);
        }
        CHECK_EQ(foliageField(x, -31.7f, 10000), 0.5f);
    }
    CHECK(high - low > 0.2f);
}

TEST(foliage_communities_follow_world_climate_and_blend_at_edges) {
    using core::Fixed;
    using generation::Climate;
    auto map = country();
    const std::array climates{Climate::Steppe, Climate::Taiga,
                              Climate::TemperateForest, Climate::TropicalForest};
    for (std::size_t biome = 0; biome < climates.size(); ++biome) {
        for (auto& cell : map.cells) { cell.sea = false; cell.climate = climates[biome]; }
        world::HeightField field(&map, 42);
        for (const auto p : {core::WorldPos{}, core::WorldPos{Fixed::fromInt(-50), Fixed::fromInt(1700)}}) {
            const auto weights = field.foliageAt(p);
            for (std::size_t j = 0; j < weights.size(); ++j)
                CHECK_EQ(weights[j], j == biome ? core::kOne : core::kZero);
        }
    }
    for (int y = 0; y < map.height; ++y)
        for (int x = 0; x < map.width; ++x)
            map.at({x, y}).climate = x < map.width / 2 ? Climate::Steppe : Climate::Taiga;
    world::HeightField field(&map, 42), otherWorker(&map, 42);
    const int edge = map.width / 2 * generation::kMetresPerCell;
    bool mixed = false;
    float previous = -1;
    for (int x = edge - 1200; x < edge + 1200; x += 5) {
        const core::WorldPos p{Fixed::fromInt(x), Fixed::fromInt(2300)};
        const auto weights = field.foliageAt(p);
        CHECK(weights == otherWorker.foliageAt(p));
        const float steppe = static_cast<float>(weights[0].toDouble());
        const float boreal = static_cast<float>(weights[1].toDouble());
        CHECK(steppe >= 0 && boreal >= 0);
        CHECK(std::abs(steppe + boreal - 1) < 0.003f);
        if (previous >= 0) CHECK(std::abs(steppe - previous) < 0.05f);
        mixed |= steppe > 0.1f && steppe < 0.9f;
        previous = steppe;
    }
    CHECK(mixed);
    const std::array<Fixed, 4> empty{};
    for (auto& cell : map.cells) cell.climate = Climate::Ice;
    CHECK(field.foliageAt({}) == empty);
    for (auto& cell : map.cells) { cell.climate = Climate::TropicalForest; cell.sea = true; }
    CHECK(field.foliageAt({}) == empty);
}

TEST(foliage_palettes_are_distinct_and_share_the_same_transition) {
    using namespace world::foliage;
    const auto steppe = foliageCommunity(1, 0, 0, 0, 0.5f);
    const auto boreal = foliageCommunity(0, 1, 0, 0, 0.5f);
    const auto temperate = foliageCommunity(0, 0, 1, 0, 0.5f);
    const auto tropical = foliageCommunity(0, 0, 0, 1, 0.5f);
    CHECK(steppe.red > steppe.green);
    CHECK(boreal.blue > temperate.blue);
    CHECK(boreal.green < temperate.green);
    CHECK(tropical.red < temperate.red);
    CHECK(tropical.green > tropical.red * 2);
    CHECK(tropical.density > temperate.density && temperate.density > steppe.density);
    CHECK(tropical.height > temperate.height && steppe.height < temperate.height);
    const auto mix = foliageCommunity(0.5f, 0.5f, 0, 0, 0.5f);
    CHECK(std::abs(mix.red - (steppe.red + boreal.red) * 0.5f) < 1e-5f);
    CHECK(std::abs(mix.blue - (steppe.blue + boreal.blue) * 0.5f) < 1e-5f);
    const auto none = foliageCommunity(0, 0, 0, 0, 0.5f);
    CHECK_EQ(none.density, 0.0f);
    CHECK(std::isfinite(none.red) && std::isfinite(none.height));
}

TEST(foliage_worker_uses_community_palette_and_only_wild_assets) {
    using namespace world::foliage;
    world::TerrainMesh mesh;
    for (const auto& p : {std::pair{0, 0}, std::pair{64, 0}, std::pair{0, 64}, std::pair{64, 64}}) {
        world::TerrainVertex v{};
        v.position = {core::Fixed::fromInt(p.first), core::Fixed::fromInt(p.second)};
        v.height = core::Fixed::fromInt(5);
        v.normal.z = core::kOne;
        v.materials.weight[static_cast<std::size_t>(world::Material::Grass)] = core::Fixed::ratio(3, 5);
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 2, 3, 0, 3, 1};
    const std::vector<core::Fixed> water(4, core::kZero);
    std::array<std::size_t, 4> counts{};
    for (std::size_t biome = 0; biome < 4; ++biome) {
        for (auto& v : mesh.vertices) { v.foliage.fill(core::kZero); v.foliage[biome] = core::kOne; }
        const auto plants = world::buildRingFoliage(mesh, water);
        CHECK(!plants.empty());
        counts[biome] = plants.size();
        for (const auto& plant : plants) {
            const float field = foliageField(plant.position[0], plant.position[1], 0);
            const auto expected = foliageCommunity(biome == 0 ? 1.0f : 0.0f, biome == 1 ? 1.0f : 0.0f,
                    biome == 2 ? 1.0f : 0.0f, biome == 3 ? 1.0f : 0.0f, field);
            CHECK(std::abs(plant.tint[0] - expected.red) < 1e-5f);
            CHECK(std::abs(plant.tint[1] - expected.green) < 1e-5f);
            const auto layer = static_cast<std::size_t>(plant.variant);
            CHECK(layer < kWildGrassSprites.size());
            if (layer < kWildGrassSprites.size()) CHECK(assetRole(kWildGrassSprites[layer]) == AssetRole::WildGrass);
        }
    }
    CHECK(counts[1] < counts[0] && counts[0] < counts[2] && counts[2] < counts[3]);
    for (auto& v : mesh.vertices) v.foliage.fill(core::kZero);
    CHECK(world::buildRingFoliage(mesh, water).empty());
    for (int id = 58; id <= 101; ++id) CHECK(assetRole(id) != AssetRole::WildGrass);
    CHECK(assetRole(60) == AssetRole::ReservedGameplayPlants);
    CHECK(assetRole(80) == AssetRole::ReservedPlantParts);
}

TEST(desert_drift_mask_is_climate_specific_and_shared_with_foliage) {
    using generation::Climate;
    using core::Fixed;
    auto map = country();
    for (const auto climate : {Climate::Desert, Climate::Steppe, Climate::Savanna,
                              Climate::Mediterranean, Climate::TemperateForest,
                              Climate::TropicalForest, Climate::Taiga, Climate::Ice}) {
        for (auto& cell : map.cells) { cell.sea = false; cell.climate = climate; }
        world::HeightField field(&map, 42);
        const auto sampled = field.surfaceClimateAt({Fixed::fromInt(2300), Fixed::fromInt(1700)});
        CHECK_EQ(sampled.desert, climate == Climate::Desert ? core::kOne : core::kZero);
        CHECK(sampled.foliage == field.foliageAt({Fixed::fromInt(2300), Fixed::fromInt(1700)}));
    }
    for (auto& cell : map.cells) { cell.sea = true; cell.climate = Climate::Desert; }
    world::HeightField sea(&map, 42);
    CHECK_EQ(sea.surfaceClimateAt({}).desert, core::kZero);
    world::HeightField fallback(nullptr, 42);
    CHECK_EQ(fallback.surfaceClimateAt({}).desert, core::kZero);
    for (int y = 0; y < map.height; ++y)
        for (int x = 0; x < map.width; ++x) {
            auto& cell = map.at({x, y});
            cell.sea = false;
            cell.climate = x < map.width / 2 ? Climate::Desert : Climate::Steppe;
        }
    world::HeightField edge(&map, 42);
    const int boundary = map.width / 2 * generation::kMetresPerCell;
    double previous = -1;
    bool mixed = false;
    for (int x = boundary - 1200; x < boundary + 1200; x += 5) {
        const double weight = edge.surfaceClimateAt({Fixed::fromInt(x), Fixed::fromInt(2300)}).desert.toDouble();
        CHECK(weight >= 0 && weight <= 1);
        if (previous >= 0) CHECK(std::abs(weight - previous) < 0.05);
        mixed |= weight > 0.1 && weight < 0.9;
        previous = weight;
    }
    CHECK(mixed);
}

TEST(the_climate_field_keeps_the_wind_that_blows_the_wrong_way) {
    // Nine of the eleven channels are weights and run from nought to one, so
    // one byte a channel is a byte from nought to one. Wind is not: it blows
    // west and south as readily as east and north, and quantising it the same
    // way clamped every westward metre per second in the world to none at all
    // - which is a still afternoon over half the map, drawn and simulated.
    const auto map = country();
    world::HeightField field(&map, 42);
    world::ClimateField climate;
    climate.raise(map, field);

    // Inside the map. Past its edge both this field and the one it was raised
    // from hold their last value, but they hold it from different places, so
    // a comparison out there measures the clamp rather than the encoding.
    const std::int64_t edge = std::int64_t(map.width) * generation::kMetresPerCell - 64;
    std::size_t compared = 0, blowing = 0, westerly = 0;
    double worst = 0;
    for (std::int64_t y = 64; y < edge; y += 512)
        for (std::int64_t x = 64; x < edge; x += 512) {
            const core::WorldPos at{core::Fixed::fromInt(x), core::Fixed::fromInt(y)};
            const auto want = field.surfaceClimateAt(at).environment;
            const auto got = climate.at(at).environment;
            for (const std::size_t slot : {std::size_t(3), std::size_t(4)}) {
                const double expected = want[slot].toDouble(), actual = got[slot].toDouble();
                worst = std::max(worst, std::abs(actual - expected));
                if (std::abs(expected) > 0.05) ++blowing;
                if (expected < -0.05 && actual < -0.025) ++westerly;
                ++compared;
            }
        }
    CHECK(compared > 100);
    CHECK(blowing > 10);
    // It blows the other way somewhere, and where it does the field says so
    // rather than saying nothing.
    CHECK(westerly > 0);
    // A byte spread over eight metres a second either way is a step of thirty
    // millimetres; the rest is the sixty-four metre grid against the warped
    // lookup it was raised from.
    CHECK(worst < 0.05);
}

TEST(desert_cover_reaches_tile_vertices_at_each_lod) {
    auto map = country();
    for (auto& cell : map.cells) { cell.sea = false; cell.climate = generation::Climate::Desert; }
    world::HeightField field(&map, 42);
    const core::WorldPos centre{core::Fixed::fromInt(2300), core::Fixed::fromInt(1700)};
    const auto graph = world::streaming::buildHydrologyGraph(map);
    world::streaming::PageStore pages(map, graph, world::streaming::hsimQuantisationFor(map),
                                      {128, world::streaming::kDefaultPaddingSamples});
    world::ClimateField climate;
    climate.raise(map, field);
    const auto tileAt = [&](int lod) {
        const auto side = static_cast<double>(world::tileMetresAt(lod));
        return world::TileId{static_cast<std::int32_t>(std::floor(centre.x.toDouble() / side)),
                             static_cast<std::int32_t>(std::floor(centre.y.toDouble() / side)), lod};
    };
    // What is asserted here changed with the layered scheme. It used to be
    // that the same spot carried the same wind exposure at every level,
    // because a ring at any level resampled the one full-resolution field.
    // A tile now reads the page its level earns, and exposure is a function
    // of that page's height against its upwind horizon - so a 256 m tile
    // genuinely sees a smoother skyline than a 4 m one, and must. What has
    // to survive every level is the climate itself, which is one field over
    // the whole world rather than a channel baked into each page.
    for (int lod : {0, 1, 3, 6}) {
        const auto mesh = world::buildTileMesh(field, pages, climate, tileAt(lod));
        CHECK(!mesh.vertices.empty());
        for (const auto& v : mesh.vertices) {
            CHECK_EQ(v.desertCover, core::kOne);
            CHECK(v.windExposure >= core::kZero && v.windExposure <= core::kOne);
        }
    }
}

TEST(sand_drift_excludes_coasts_water_vegetation_and_rock) {
    using world::sand::sandDriftSupport;
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 0, 0, -2, 1), 1.0f);
    CHECK_EQ(sandDriftSupport(0, 1, 0, 0, 0, 0, -2, 1), 0.0f); // Ordinary sandy beach.
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 0, 0, 0, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 0, 0, 1, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 0, 0, 0, 0, 0, -2, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 1, 0, 0, 0, -2, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 0, 1, 0, 0, -2, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 1, 0, -2, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 0, 1, -2, 1), 0.0f);
    CHECK_EQ(sandDriftSupport(1, 1, 0, 0, 0, 0, -2, 0.4f), 0.0f);
}

TEST(sand_drift_moves_downwind_without_resets_or_distant_shimmer) {
    using namespace world::sand;
    float peak = 0;
    double movement = 0;
    for (int i = -80; i <= 80; ++i) {
        const float x = static_cast<float>(i) * 3.7f;
        const float y = static_cast<float>(i % 17) * 2.1f;
        const float first = sandDriftOpacity(x, y, 2.3f, 1, 1, 0.1f);
        CHECK(first >= 0 && first <= 0.38f);
        peak = std::max(peak, first);
        movement += std::abs(first - sandDriftOpacity(x, y, 3.3f, 1, 1, 0.1f));
        CHECK_EQ(sandDriftOpacity(x, y, 2.3f, 0, 1, 0.1f), 0.0f);
        CHECK_EQ(sandDriftOpacity(x, y, 2.3f, 1, 1, 30), 0.0f);
        const float carried = sandDriftOpacity(x + sandDriftSpeed(1) * 0.25f, y, 2.55f, 1, 0, 0.1f);
        CHECK(std::abs(carried - sandDriftOpacity(x, y, 2.3f, 1, 0, 0.1f)) < 1e-4f);
        float previous = first;
        for (float pixel : {0.5f, 2.0f, 6.0f, 10.0f, 30.0f}) {
            const float filtered = sandDriftOpacity(x, y, 2.3f, 1, 1, pixel);
            CHECK(filtered <= previous + 1e-6f);
            previous = filtered;
        }
        for (float time : {0.0f, 4.0f, 8.0f, 100.0f})
            CHECK(std::abs(sandDriftOpacity(x, y, time - 1e-4f, 1, 1, 0.1f) -
                           sandDriftOpacity(x, y, time + 1e-4f, 1, 1, 0.1f)) < 0.001f);
    }
    CHECK(peak > 0.08f);
    CHECK(movement > 0.1);
}

TEST(sand_ripple_changes_normals_not_geometry_and_has_its_own_speed) {
    using namespace world::sand;
    CHECK(sandRippleSpeed(1) > 0 && sandRippleSpeed(1) < 0.02f);
    CHECK(sandRidgeSpeed(1) > 0 && sandRidgeSpeed(1) < sandRippleSpeed(1) * 0.3f);
    CHECK(sandDriftSpeed(1) > sandRippleSpeed(1) * 5 && sandDriftSpeed(1) < 0.1f);
    CHECK_EQ(sandRidgeSpeed(0), 0.0f);
    CHECK_EQ(sandRippleSpeed(0), 0.0f);
    CHECK_EQ(sandDriftSpeed(0), 0.0f);
    float peak = 0;
    double movement = 0;
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.031f;
        const auto a = sandRipple(x, 1.7f, 0, 1, 0.02f);
        const auto b = sandRipple(x, 1.7f, 4, 1, 0.02f);
        CHECK(std::abs(a.alongSlope) < 0.4f && std::abs(a.acrossSlope) < 0.12f);
        CHECK(a.roughness >= 0.75f && a.roughness <= 0.97f);
        peak = std::max(peak, std::abs(a.alongSlope));
        movement += std::abs(a.alongSlope - b.alongSlope);
        // Once the fine layer is filtered, only the slower parent translates.
        const auto parent = sandRipple(x, 1.7f, 0, 1, 0.5f);
        const auto carried = sandRipple(x + sandRidgeSpeed(1) * 4, 1.7f, 4, 1, 0.5f);
        CHECK(std::abs(parent.alongSlope - carried.alongSlope) < 1e-5f);
        const auto calm = sandRipple(x, 1.7f, 0, 0, 0.02f);
        CHECK_EQ(calm.alongSlope, sandRipple(x, 1.7f, 50, 0, 0.02f).alongSlope);
        for (float pixel : {1.8f, 3.0f, 20.0f}) {
            const auto far = sandRipple(x, 1.7f, 4, 1, pixel);
            CHECK_EQ(far.alongSlope, 0.0f);
            CHECK_EQ(far.acrossSlope, 0.0f);
        }
        CHECK_EQ(sandDriftOpacity(x, 1.7f, 4, 1, 1, 1.2f), 0.0f);
    }
    CHECK(peak > 0.15f);
    CHECK(movement > 1.0);
}

TEST(sand_nested_ripples_wrap_the_parent_with_continuous_normals) {
    using namespace world::sand;
    double largeEnergy = 0, fineEnergy = 0, bentEnergy = 0;
    constexpr float epsilon = 0.002f;
    for (int i = -100; i <= 100; ++i) {
        const float x = static_cast<float>(i) * 0.17f;
        const float y = static_cast<float>(i % 19) * 0.43f;
        const auto near = sandRipple(x, y, 4, 1, 0.02f);
        const auto parent = sandRipple(x, y, 4, 1, 0.32f);
        const auto middle = sandRipple(x, y, 4, 1, 0.7f);
        largeEnergy += std::abs(middle.alongSlope);
        fineEnergy += std::abs(near.alongSlope - parent.alongSlope);
        bentEnergy += std::abs(near.acrossSlope);
        for (float pixel : {0.02f, 0.2f, 0.7f}) {
            const auto surface = sandRipple(x, y, 4, 1, pixel);
            const float dx = (sandRipple(x + epsilon, y, 4, 1, pixel).height -
                              sandRipple(x - epsilon, y, 4, 1, pixel).height) / (2 * epsilon);
            const float dy = (sandRipple(x, y + epsilon, 4, 1, pixel).height -
                              sandRipple(x, y - epsilon, 4, 1, pixel).height) / (2 * epsilon);
            CHECK(std::abs(surface.alongSlope - dx) < 0.003f);
            CHECK(std::abs(surface.acrossSlope - dy) < 0.003f);
        }
        for (float boundary : {0.10f, 0.32f, 0.40f, 1.80f}) {
            const auto before = sandRipple(x, y, 4, 1, boundary - 0.0001f);
            const auto after = sandRipple(x, y, 4, 1, boundary + 0.0001f);
            CHECK(std::abs(before.alongSlope - after.alongSlope) < 0.001f);
        }
        const auto before = sandRipple(x, y, 4 - 0.0001f, 1, 0.02f);
        const auto after = sandRipple(x, y, 4 + 0.0001f, 1, 0.02f);
        CHECK(std::abs(before.alongSlope - after.alongSlope) < 0.001f);
    }
    CHECK(largeEnergy > 10.0);
    CHECK(fineEnergy > 5.0);
    CHECK(bentEnergy > 1.0);
}

TEST(desert_macro_dunes_are_static_deterministic_and_lod_invariant) {
    using core::Fixed;
    generation::WorldMapData map;
    map.width = map.height = 16;
    map.cells.resize(16 * 16);
    for (auto& cell : map.cells) {
        cell.elevation = 20;
        cell.moisture = 30;
        cell.temperature = 220;
        cell.climate = generation::Climate::Desert;
    }
    auto plainMap = map;
    for (auto& cell : plainMap.cells) cell.climate = generation::Climate::Steppe;
    world::HeightField dunes(&map, 42), repeated(&map, 42), plain(&plainMap, 42);
    double peak = 0;
    for (int x = 1000; x < 1500; x += 4) {
        const Fixed wx = Fixed::fromInt(x), wy = Fixed::fromInt(1200);
        const auto a = dunes.piecesAt(wx, wy);
        CHECK_EQ(a.moved, repeated.piecesAt(wx, wy).moved);
        CHECK_EQ(a.country, plain.piecesAt(wx, wy).country);
        peak = std::max(peak, std::abs((a.moved - plain.piecesAt(wx, wy).moved).toDouble()));
        for (int stride : {4, 8, 16, 32, 128, 4096}) CHECK_EQ(a.moved, dunes.piecesAt(wx, wy, stride).moved);
    }
    CHECK(peak > 2.0 && peak <= 4.51);
    const core::WorldPos p{Fixed::fromInt(1200), Fixed::fromInt(1200)};
    const Fixed h = dunes.heightAt(p);
    // Above all nearby ridges is exposed; below them is sheltered. Probes are
    // actual field heights, not a local face-normal approximation.
    CHECK_EQ(dunes.sandWindExposureAt(p, h + Fixed::fromInt(30)), core::kOne);
    CHECK_EQ(dunes.sandWindExposureAt(p, h - Fixed::fromInt(30)), core::kZero);
    CHECK_EQ(dunes.sandWindExposureAt(p, h), repeated.sandWindExposureAt(p, h));
}

TEST(desert_landmark_lands_on_visible_sand_not_just_the_climate) {
    generation::WorldMapParams params;
    params.width = params.height = 256;
    params.seed = 11;
    const auto map = generation::generateWorldMap(params);
    client::Explorer explorer(map, params.seed);
    const auto& landmarks = explorer.landmarks();
    const auto found = std::find_if(landmarks.begin(), landmarks.end(),
            [](const client::Explorer::Landmark& landmark) { return landmark.name == "desert"; });
    CHECK(found != landmarks.end());
    if (found == landmarks.end()) return;
    const auto& field = explorer.field();
    const auto ground = field.groundAt(found->where);
    const auto weight = [&](world::Material m) {
        return static_cast<float>(ground.materials.of(m).toDouble());
    };
    const float support = world::sand::sandDriftSupport(
            static_cast<float>(field.surfaceClimateAt(found->where).desert.toDouble()),
            weight(world::Material::Sand), weight(world::Material::Rock),
            weight(world::Material::Grass), weight(world::Material::Snow), weight(world::Material::Marsh),
            static_cast<float>((field.waterLevelAt(found->where) - ground.height).toDouble()),
            static_cast<float>(ground.normal.z.toDouble()));
    std::cout << "  desert landmark sand support: " << support << "\n";
    CHECK(support > 0.5f);
    CHECK(field.sandWindExposureAt(found->where, ground.height) > core::Fixed::ratio(1, 2));
}

TEST(sand_wind_shadow_detects_an_upwind_terrain_obstacle) {
    using core::Fixed;
    generation::WorldMapData map;
    map.width = map.height = 16;
    map.cells.resize(16 * 16);
    for (int y = 0; y < map.height; ++y)
        for (int x = 0; x < map.width; ++x) {
            auto& cell = map.at({x, y});
            cell.elevation = x >= 8 ? 100 : 20;
            cell.moisture = 30;
            cell.temperature = 220;
            cell.climate = generation::Climate::Desert;
        }
    const world::HeightField field(&map, 42);
    double darkest = 1, brightest = 0;
    const int step = generation::kMetresPerCell;
    for (int x = 6 * step; x <= 10 * step; x += 24) {
        const core::WorldPos p{Fixed::fromInt(x), Fixed::fromInt(8 * step)};
        const double exposure = field.sandWindExposureAt(p, field.heightAt(p)).toDouble();
        CHECK(exposure >= 0 && exposure <= 1);
        darkest = std::min(darkest, exposure);
        brightest = std::max(brightest, exposure);
    }
    CHECK(darkest < 0.3);
    CHECK(brightest > 0.9);
}

TEST(water_vertex_waves_match_slopes_and_mesh_resolution) {
    using namespace shore_shader;
    for (int i = -200; i <= 200; ++i) {
        const float phase = static_cast<float>(i) * 0.03f;
        const float derivative = (waterWaveShape(phase + 0.001f) -
                                  waterWaveShape(phase - 0.001f)) / 0.002f;
        CHECK(std::abs(derivative - waterWaveDerivative(phase)) < 0.001f);
        CHECK(std::abs(waterWaveShape(phase)) <= 1.18f);
    }
    CHECK(waterWaveAmplitude(1) > 1.0f);
    for (float wave : {32.0f, 57.0f, 88.0f, 213.0f}) {
        float previous = 1;
        for (int lod = 0; lod <= 12; ++lod) {
            const float step = static_cast<float>(world::sampleMetresAt(lod));
            const float resolved = waterWaveResolution(wave, step);
            CHECK(resolved >= 0 && resolved <= previous);
            if (step >= wave * 0.25f) CHECK_EQ(resolved, 0.0f);
            previous = resolved;
        }
    }
    CHECK_EQ(waterWaveRoom(-1, 1), 0.0f);
    CHECK_EQ(waterWaveRoom(0, 1), 0.0f);
    CHECK_EQ(waterWaveRoom(10, 0), 0.0f);
    CHECK_EQ(waterWaveRoom(10, 1), 1.0f);
}

TEST(water_flow_has_no_reset_jump) {
    using namespace shore_shader;
    const auto sampled = [](float phase, float uv, float travel) {
        phase = frac(phase);
        const float first = std::sin(6.2831853f * (uv - travel * (phase - 0.5f)));
        const float second = std::sin(6.2831853f * (uv - travel * (frac(phase + 0.5f) - 0.5f)));
        return first + (second - first) * waterFlowWeight(phase);
    };
    for (float reset : {0.0f, 0.5f, 1.0f})
        for (float uv : {-3.0f, 0.0f, 2.5f})
            for (float travel : {-0.7f, 0.0f, 0.8f})
                CHECK(std::abs(sampled(reset - 1e-5f, uv, travel) -
                               sampled(reset + 1e-5f, uv, travel)) < 3e-4f);
    CHECK(std::abs(sampled(0.2f, 0.5f, 0.8f) - sampled(0.4f, 0.5f, 0.8f)) > 0.01f);
}

TEST(ring_water_apron_extends_onto_dry_ground) {
    auto coast = country();
    for (int y = 0; y < coast.height; ++y)
        for (int x = 0; x < coast.width; ++x) {
            auto& cell = coast.cells[static_cast<std::size_t>(y * coast.width + x)];
            cell.elevation = x < coast.width / 2 ? -2 : 2;
            cell.sea = x < coast.width / 2;
            cell.river = false;
            cell.drainOut = cell.riverOut = -1;
            cell.drainSize = 0;
        }
    world::HeightField field(&coast, 42);
    const auto point = [&](double x) {
        return core::WorldPos{core::Fixed::fromDoubleForContent(x),
                              core::Fixed::fromInt(8 * generation::kMetresPerCell)};
    };
    double low = 4 * generation::kMetresPerCell;
    double high = 28 * generation::kMetresPerCell;
    CHECK(field.heightAt(point(low)).raw < 0);
    CHECK(field.heightAt(point(high)).raw > 0);
    for (int i = 0; i < 24; ++i) {
        const double middle = (low + high) * 0.5;
        if (field.heightAt(point(middle)).raw < 0) low = middle;
        else high = middle;
    }
    client::PatchWorkshop workshop(coast, 42);
    std::vector<client::PatchWorkshop::Order> wanted;
    {
        const auto middle = point((low + high) * 0.5);
        const auto side = static_cast<double>(world::tileMetresAt(0));
        const auto tx = static_cast<std::int32_t>(std::floor(middle.x.toDouble() / side));
        const auto ty = static_cast<std::int32_t>(std::floor(middle.y.toDouble() / side));
        for (std::int32_t y = -1; y <= 1; ++y)
            for (std::int32_t x = -1; x <= 1; ++x)
                wanted.push_back({world::TileId{tx + x, ty + y, 0}, double(std::abs(x) + std::abs(y))});
    }
    workshop.wants(wanted);
    CHECK(eventually([&] { return workshop.waiting() == 0; }));
    const auto done = workshop.collect();
    CHECK_EQ(done.size(), wanted.size());
    int apronVertices = 0;
    for (const auto& patch : done) {
        bool hasCoverage = false;
        for (std::size_t i = 0; i < patch.mesh.vertices.size(); ++i) {
            const auto& v = patch.mesh.vertices[i];
            const auto slope = v.normal.z.raw > 0 ? core::hypot(v.normal.x, v.normal.y) / v.normal.z
                                                  : core::Fixed::fromInt(8);
            // The water is the page's own now, not the field's, so there is
            // nothing here to compare it against - and the apron does not
            // need one. What it is, is coverage carried onto ground that
            // stands above the water: that is the strip a shader fades surf
            // over, and without it the sea ends at a line.
            (void)slope;
            hasCoverage |= patch.waterCover[i].raw > 0;
            if (v.height.raw > patch.waterLevel[i].raw && patch.waterCover[i].raw > 0)
                ++apronVertices;
        }
        CHECK_EQ(patch.anyWater, hasCoverage);
    }
    CHECK(apronVertices > 0);
}

TEST(shore_swash_approaches_holds_recedes_and_dissolves) {
    using namespace shore_shader;
    CHECK_EQ(surfRunup(0), 0.0f);
    CHECK_EQ(surfRunup(1), 0.0f);
    CHECK_EQ(surfRunup(0.30f), 1.0f);
    CHECK_EQ(surfRunup(0.42f), 1.0f);
    CHECK_EQ(surfStrength(0), 0.0f);
    CHECK_EQ(surfStrength(1), 0.0f);
    CHECK_EQ(surfResidue(0), 0.0f);
    CHECK_EQ(surfResidue(1), 0.0f);
    CHECK_EQ(surfResidue(0.65f), 1.0f);
    CHECK(surfRunup(0.80f) < 0.25f);
    CHECK(surfResidue(0.80f) > 0.70f);
    for (int i = 1; i <= 100; ++i) {
        const float phase = i / 100.0f, before = (i - 1) / 100.0f;
        const float runup = surfRunup(phase);
        CHECK(std::isfinite(runup) && runup >= 0 && runup <= 1);
        CHECK(surfStrength(phase) >= 0 && surfStrength(phase) <= 1);
        CHECK(surfResidue(phase) >= 0 && surfResidue(phase) <= 1);
        CHECK(std::abs(runup - surfRunup(before)) < 0.07f);
        CHECK(std::abs(surfResidue(phase) - surfResidue(before)) < 0.07f);
        if (phase <= 0.28f) CHECK(runup >= surfRunup(before));
        if (before >= 0.44f) CHECK(runup <= surfRunup(before));
        if (before >= 0.70f) CHECK(surfResidue(phase) <= surfResidue(before));
    }
}

TEST(shore_residue_waits_for_local_water_retreat) {
    using namespace shore_shader;
    for (float blowing : {0.45f, 0.7f, 1.0f}) {
        for (float crossed : {0.55f, 0.65f, 0.8f, 0.9f}) {
            const float bank = surfDryFront(crossed, blowing);
            CHECK(std::abs(surfResidueAge(crossed + 0.03f, bank, blowing) - 0.03f) < 1e-4f);
            CHECK_EQ(surfResidualFoam(crossed - 0.001f, bank, blowing), 0.0f);
            CHECK(surfResidualFoam(crossed + 0.03f, bank, blowing) > 0.0f);
            CHECK_EQ(surfResidualFoam(crossed + kSurfResidueLifetime + 0.01f, bank, blowing), 0.0f);
            for (int i = 0; i <= 300; ++i) {
                const float time = i / 100.0f;
                const float foam = surfResidualFoam(time, bank, blowing);
                CHECK(std::isfinite(foam) && foam >= 0 && foam <= 1);
                if (bank >= surfDryFront(frac(time), blowing)) CHECK_EQ(foam, 0.0f);
                if (time <= 0.44f) CHECK_EQ(foam, 0.0f);
            }
        }
        for (float bank : {surfDryFront(0.3f, blowing) - 0.01f,
                           surfDryFront(1.0f, blowing) + 0.01f, 1.0f})
            for (float time : {0.3f, 0.7f, 1.02f, 2.8f})
                CHECK_EQ(surfResidualFoam(time, bank, blowing), 0.0f);
    }
}

TEST(shore_residue_fades_by_local_age_and_finishes_before_next_wave) {
    using namespace shore_shader;
    const float early = surfDryFront(0.60f, 1.0f);
    const float late = surfDryFront(0.80f, 1.0f);
    CHECK(surfResidualFoam(0.72f, early, 1.0f) > 0);
    CHECK_EQ(surfResidualFoam(0.72f, late, 1.0f), 0.0f);
    CHECK(surfResidualFoam(0.92f, early, 1.0f) < surfResidualFoam(0.92f, late, 1.0f));
    CHECK(surfResidualFoam(0.92f, late, 1.0f) > 0);
    const float last = surfDryFront(0.90f, 1.0f);
    CHECK_EQ(surfResidualFoam(1.001f, last, 1.0f), 0.0f);
    CHECK(std::abs(surfResidualFoam(0.9999f, last, 1.0f) -
                   surfResidualFoam(1.0001f, last, 1.0f)) < 0.01f);
    CHECK(kSurfResidueLifetime * 9.0f < 1.65f);
    CHECK(kSurfResidueHold * 9.0f < 0.32f);
    for (int i = 0; i <= 440; ++i) {
        const float time = 1.0f + static_cast<float>(i) / 1000.0f;
        CHECK_EQ(surfResidualFoam(time, last, 1.0f), 0.0f);
    }
}

TEST(shore_residue_covers_a_broad_exposed_apron) {
    using namespace shore_shader;
    for (float blowing : {0.45f, 1.0f}) {
        const float swept = surfDryFront(1.0f, blowing) - surfDryFront(0.3f, blowing);
        CHECK(swept > 0.39f); // Previously only 0.139--0.26, then depth-clamped again.
    }
    int visibleSamples = 0;
    for (int i = -90; i <= 90; ++i) {
        const float bank = static_cast<float>(i) / 100.0f;
        if (surfResidualFoam(0.72f, bank, 1.0f) > 0.15f) {
            ++visibleSamples;
            CHECK(bank < surfDryFront(0.72f, 1.0f)); // Still strictly after retreat.
        }
    }
    // Faster draining may thin the old tail, but must not collapse to a lip.
    CHECK(visibleSamples >= 18);
}

TEST(shore_ragged_front_stays_inside_geometry_support) {
    using namespace shore_shader;
    for (float noise : {-1.0f, 0.0f, 1.0f}) {
        CHECK_EQ(surfWarpedBank(-1.0f, noise, noise, noise), -1.0f);
        CHECK_EQ(surfWarpedBank(1.0f, noise, noise, noise), 1.0f);
    }
    CHECK(surfWarpedBank(0, 0, 0, 1) - surfWarpedBank(0, 0, 0, -1) > 0.3f);
    for (float shaped : {surfWarpedBank(0, 1, 1, 1), surfWarpedBank(0, -1, -1, -1)}) {
        for (int i = 0; i <= 100; ++i) {
            const float phase = static_cast<float>(i) / 100.0f;
            if (shaped >= surfDryFront(phase, 1.0f))
                CHECK_EQ(surfResidualFoam(phase, shaped, 1.0f), 0.0f);
        }
    }
}

TEST(shore_contact_foam_bridges_residue_and_moving_lip) {
    using namespace shore_shader;
    for (float blowing : {0.45f, 0.7f, 1.0f}) {
        for (float phase : {0.60f, 0.70f, 0.80f, 0.85f}) {
            const float dry = surfDryFront(phase, blowing);
            const float lip = surfFoamFront(phase, blowing);
            // Include the residual exposure guard and initial local-age fade,
            // then the whole formerly empty water ramp up to its bright lip.
            for (int i = 0; i <= 100; ++i) {
                const float t = static_cast<float>(i) / 100.0f;
                const float bank = (dry - 0.025f) * (1 - t) + (lip + 0.02f) * t;
                CHECK(surfContactFoam(phase, bank, blowing) >= surfStrength(phase) * 0.99f);
                if (bank >= dry) CHECK_EQ(surfResidualFoam(phase, bank, blowing), 0.0f);
            }
        }
        for (int i = 0; i <= 100; ++i) {
            const float phase = static_cast<float>(i) / 100.0f;
            CHECK_EQ(surfContactFoam(phase, surfDryFront(0.30f, blowing) - 0.001f, blowing), 0.0f);
            CHECK_EQ(surfContactFoam(phase, surfFoamFront(phase, blowing) + 0.20f, blowing), 0.0f);
            if (phase <= 0.44f)
                CHECK_EQ(surfContactFoam(phase, surfDryFront(phase, blowing) - 0.001f, blowing), 0.0f);
        }
        CHECK_EQ(surfContactFoam(0, 0, blowing), 0.0f);
        CHECK_EQ(surfContactFoam(1, 0, blowing), 0.0f);
    }
}

TEST(shore_residue_alpha_blends_smoothly_without_brightening_overlap) {
    using namespace shore_shader;
    for (float retreat : {0.0f, 0.1f, 0.5f, 1.0f}) {
        for (float residue : {0.0f, 0.25f, 0.8f, 1.0f}) {
            const float peak = std::max(retreat, residue);
            const float low = std::min(retreat, residue);
            float previous = residue;
            for (int i = 0; i <= 200; ++i) {
                const float join = smoothstep(0, 1, static_cast<float>(i) / 200.0f);
                // The real contact foam already includes its spatial ramp.
                const float foam = surfBlendResidualFoam(join, retreat * join, residue);
                CHECK(std::isfinite(foam));
                CHECK(foam >= low - 1e-6f && foam <= peak + 1e-6f);
                CHECK((foam - previous) * (retreat - residue) >= -1e-6f);
                CHECK(std::abs(foam - previous) < 0.008f);
                previous = foam;
            }
            CHECK_EQ(surfBlendResidualFoam(0, 0, residue), residue);
            CHECK_EQ(surfBlendResidualFoam(1, retreat, residue), retreat);
            const float middle = surfBlendResidualFoam(0.5f, retreat * 0.5f, residue);
            CHECK(std::abs(middle - (residue + retreat) * 0.5f) < 1e-6f);
        }
    }
}

TEST(shore_contact_and_residue_form_one_profile_without_a_trough) {
    using namespace shore_shader;
    const float phase = 0.80f, blowing = 1.0f;
    const float dry = surfDryFront(phase, blowing);
    for (float pattern : {0.0f, 0.5f, 1.0f}) {
        const float density = (0.55f + pattern * 0.45f) * 0.98f;
        const float target = surfStrength(phase) * density;
        float previous = 0.0f;
        for (int i = 0; i <= 350; ++i) {
            const float bank = dry - 0.20f + 0.175f * (static_cast<float>(i) / 350.0f);
            const float deposit = surfResidualFoam(phase, bank, blowing) * density;
            const float contact = surfContactFoam(phase, bank, blowing) * density;
            const float join = surfContactJoin(phase, bank, blowing);
            const float foam = surfBlendResidualFoam(join, contact, deposit);
            CHECK(foam <= target + 1e-5f);
            CHECK(foam >= previous - 1e-5f); // One rise from drained sand to surf.
            if (i > 0) CHECK(std::abs(foam - previous) < 0.02f);
            previous = foam;
        }
        CHECK(std::abs(previous - target) < 1e-5f);
    }
}

TEST(shore_fresh_residue_cannot_outshine_fading_surf_at_join) {
    using namespace shore_shader;
    int cappedSamples = 0;
    for (float blowing : {0.45f, 0.7f, 1.0f}) {
        for (float phase : {0.60f, 0.80f, 0.90f, 0.96f, 0.9999f, 1.0001f}) {
            const float dry = surfDryFront(frac(phase), blowing);
            for (float offset : {-0.065f, -0.05f, -0.025f, 0.0f}) {
                const float bank = dry + offset;
                const float residue = surfResidualFoam(phase, bank, blowing) * 0.98f;
                // Bright residue next to both dense and porous retreating foam.
                for (float pattern : {0.0f, 0.5f, 1.0f}) {
                    const float retreat = surfContactFoam(frac(phase), bank, blowing) *
                                          (0.55f + pattern * 0.45f) * 0.98f;
                    const float join = surfContactJoin(frac(phase), bank, blowing);
                    CHECK_EQ(surfBlendResidualFoam(join, retreat, residue), retreat);
                    if (residue > retreat) ++cappedSamples;
                }
            }
        }
    }
    CHECK(cappedSamples > 0); // Exercise the formerly opaque residual boundary.
}

TEST(shore_join_does_not_reveal_a_second_layer_at_cycle_wrap) {
    using namespace shore_shader;
    for (float blowing : {0.45f, 0.7f, 1.0f}) {
        for (int i = 0; i <= 400; ++i) {
            const float bank = -1.0f + static_cast<float>(i) / 200.0f;
            float before = 0;
            for (float time : {0.99999f, 1.00001f}) {
                const float phase = frac(time);
                const float contact = surfContactFoam(phase, bank, blowing) * 0.98f;
                const float deposit = surfResidualFoam(time, bank, blowing) * 0.98f;
                const float foam = surfBlendResidualFoam(
                        surfContactJoin(phase, bank, blowing), contact, deposit);
                if (time < 1.0f) before = foam;
                else CHECK(std::abs(foam - before) < 0.002f);
            }
        }
    }
}

TEST(shore_exposed_deposit_never_brightens_again_as_contact_recedes) {
    using namespace shore_shader;
    int checked = 0;
    for (float blowing : {0.45f, 0.7f, 1.0f}) {
        for (float crossed : {0.46f, 0.55f, 0.65f, 0.75f, 0.85f, 0.93f}) {
            const float bank = surfDryFront(crossed, blowing);
            for (float pattern : {0.0f, 0.5f, 1.0f}) {
                const float density = (0.55f + pattern * 0.45f) * 0.98f;
                float previous = 1.0f;
                for (int i = 480; i <= 1000; ++i) {
                    const float phase = static_cast<float>(i) / 1000.0f;
                    const float join = surfContactJoin(phase, bank, blowing);
                    // Follow a fixed pixel only once it is being uncovered.
                    if (bank >= surfDryFront(phase, blowing) || join >= 0.999f) continue;
                    const float deposit = surfResidualFoam(phase, bank, blowing) * density;
                    const float contact = surfContactFoam(phase, bank, blowing) * density;
                    const float foam = surfBlendResidualFoam(join, contact, deposit);
                    CHECK(foam <= previous + 1e-5f);
                    CHECK(foam <= surfStrength(phase) * density + 1e-5f);
                    previous = foam;
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 1000);
}

