#include "framework.hpp"

#include <atomic>
#include <fstream>
#include "game/world/terrain_config.hpp"
#include "game/world/terrain_plan.hpp"

namespace {
using namespace world::terrain;

struct ConfigFile {
    std::filesystem::path directory, file;
    std::filesystem::file_time_type stamp = std::filesystem::file_time_type::clock::now();
    ConfigFile() {
        static std::atomic<unsigned> sequence{0};
        directory = std::filesystem::temp_directory_path() / ("asr-terrain-config-" +
            std::to_string(TerrainConfigWatch::Clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence++));
        std::filesystem::create_directory(directory);
        file = directory / "terrain.json";
    }
    ~ConfigFile() { std::error_code ec; std::filesystem::remove_all(directory, ec); }
    void write(const char* text) {
        std::ofstream out(file);
        out << text;
        out.close();
        CHECK(bool(out));
        stamp += std::chrono::seconds(1);
        std::filesystem::last_write_time(file, stamp);
    }
};
}

TEST(terrain_config_defaults_and_partial_overrides) {
    ConfigFile file;
    TerrainConfig config;
    std::string error;
    file.write("{}");
    CHECK(readTerrainConfig(file.file, config, error));
    CHECK(config == TerrainConfig{});
    file.write(R"({"detail_distance_scale":3, "chunk_cells":64, "h8_atlas_side":24,
        "morph_seconds":1.5, "mesh_builds_per_batch":8, "preload_pages":0, "preload_margin_pixels":80,
        "prediction_seconds":0.5, "prediction_max_metres":1024,
        "local_detail_height_metres":64, "upload_mib_per_frame":1.5,
        "target_triangle_pixels":48,"refine_triangle_pixels":64,"coarsen_triangle_pixels":24})");
    CHECK(readTerrainConfig(file.file, config, error));
    CHECK(error.empty());
    CHECK_EQ(config.detailDistanceScale, 3.0);
    CHECK_EQ(config.chunkCells, 64);
    CHECK_EQ(config.h8AtlasSide, 24);
    CHECK_EQ(config.morphSeconds, 1.5);
    CHECK_EQ(config.meshesPerPlan, std::size_t(8));
    CHECK_EQ(config.preloadPages, std::size_t(0));
    CHECK_EQ(config.preloadMarginPixels, 80.0);
    CHECK_EQ(config.lookAheadSeconds, 0.5);
    CHECK_EQ(config.maxLookAheadMetres, 1024.0);
    CHECK_EQ(config.localDetailHeightMetres, 64.0);
    CHECK_EQ(config.uploadBytesPerFrame, std::size_t(1572864));
    CHECK_EQ(config.lod.targetTrianglePixels, 48.0);
    CHECK_EQ(config.lod.refineTrianglePixels, 64.0);
    CHECK_EQ(config.lod.coarsenTrianglePixels, 24.0);
    file.write("{}");
    CHECK(readTerrainConfig(file.file, config, error));
    CHECK(config == TerrainConfig{}); // deleting an override restores its default
}

TEST(terrain_config_legacy_names_match_canonical_fields) {
    ConfigFile file;
    TerrainConfig legacy, canonical;
    std::string error;
    file.write(R"({"meshes_per_plan":8,"look_ahead_seconds":0.5,"max_look_ahead_metres":1024})");
    CHECK(readTerrainConfig(file.file, legacy, error));
    file.write(R"({"mesh_builds_per_batch":8,"prediction_seconds":0.5,"prediction_max_metres":1024})");
    CHECK(readTerrainConfig(file.file, canonical, error));
    CHECK(legacy == canonical);
    file.write(R"({"morph_seconds":0,"prediction_seconds":0,"prediction_max_metres":0})");
    CHECK(readTerrainConfig(file.file, canonical, error));
    CHECK_EQ(canonical.morphSeconds, 0.0);
    CHECK_EQ(canonical.lookAheadSeconds, 0.0);
    CHECK_EQ(canonical.maxLookAheadMetres, 0.0);
}

TEST(terrain_config_rejects_invalid_values_without_partial_application) {
    ConfigFile file;
    TerrainConfig config;
    config.detailDistanceScale = 3;
    const auto before = config;
    std::string error;
    CHECK(!readTerrainConfig(file.file, config, error));
    CHECK(!error.empty());
    for (const char* text : {"{", "[]", "null", R"({"detail_distance_scale":1e999})",
        R"({"detail_distance_scale":0})", R"({"detail_distance_scale":"2"})",
        R"({"chunk_cells":24})", R"({"chunk_cells":32.5})", R"({"chunk_cells":128})",
        R"({"h8_atlas_side":0})", R"({"h8_atlas_side":33})",
        R"({"meshes_per_plan":0})", R"({"meshes_per_plan":-1})",
        R"({"meshes_per_plan":18446744073709551615})", R"({"preload_pages":-1})",
        R"({"look_ahead_seconds":true})", R"({"max_look_ahead_metres":-1})",
        R"({"morph_seconds":-0.1})", R"({"morph_seconds":11})", R"({"morph_seconds":true})",
        R"({"morph_seconds":1e999})", R"({"mesh_builds_per_batch":0})",
        R"({"mesh_builds_per_batch":1025})", R"({"mesh_builds_per_batch":1.5})",
        R"({"prediction_seconds":-1})", R"({"prediction_seconds":3})",
        R"({"prediction_max_metres":-1})", R"({"prediction_max_metres":4097})",
        R"({"mesh_builds_per_batch":4,"meshes_per_plan":8})",
        R"({"prediction_seconds":0.5,"look_ahead_seconds":0.5})",
        R"({"prediction_max_metres":512,"max_look_ahead_metres":1024})",
        R"({"upload_mib_per_frame":0})", R"({"preload_margin_pixels":-1})",
        R"({"local_detail_height_metres":null})", R"({"detail_distnace_scale":2})",
        R"({"detail_distance_scale":2,"coarsen_triangle_pixels":80})",
        R"({"coarsen_triangle_pixels":64})", R"({"target_triangle_pixels":20})",
        R"({"target_triangle_pixels":256})"}) {
        file.write(text);
        CHECK(!readTerrainConfig(file.file, config, error));
        CHECK(!error.empty());
        CHECK(config == before);
    }
}

TEST(terrain_config_watch_throttles_retries_and_keeps_last_good_config) {
    ConfigFile file;
    TerrainConfigWatch watch(file.file);
    TerrainConfig config;
    std::string error;
    auto now = TerrainConfigWatch::Clock::now();
    file.write(R"({"detail_distance_scale":3})");
    CHECK(watch.poll(config, error, now));
    CHECK_EQ(config.detailDistanceScale, 3.0);
    file.write("{");
    CHECK(!watch.poll(config, error, now + std::chrono::milliseconds(100)));
    CHECK(error.empty()); // no filesystem read inside the polling interval
    now += std::chrono::milliseconds(500);
    CHECK(!watch.poll(config, error, now));
    CHECK(!error.empty());
    CHECK_EQ(config.detailDistanceScale, 3.0);
    now += std::chrono::milliseconds(500);
    CHECK(!watch.poll(config, error, now));
    CHECK(error.empty()); // identical parse failure is not logged every frame
    file.write(R"({"detail_distance_scale":2})");
    now += std::chrono::milliseconds(500);
    CHECK(watch.poll(config, error, now));
    CHECK(error.empty());
    CHECK_EQ(config.detailDistanceScale, 2.0);
    now += std::chrono::milliseconds(500);
    CHECK(!watch.poll(config, error, now));
    std::filesystem::remove(file.file);
    now += std::chrono::milliseconds(500);
    CHECK(!watch.poll(config, error, now));
    CHECK(!error.empty());
    CHECK_EQ(config.detailDistanceScale, 2.0);
    file.write("{}");
    now += std::chrono::milliseconds(500);
    CHECK(watch.poll(config, error, now));
    CHECK(config == TerrainConfig{});
}

TEST(terrain_config_watch_reloads_morph_batch_and_prediction_as_one_snapshot) {
    ConfigFile file;
    TerrainConfigWatch watch(file.file);
    TerrainConfig config;
    std::string error;
    auto now = TerrainConfigWatch::Clock::now();
    file.write("{}");
    CHECK(watch.poll(config, error, now));
    const auto previous = config;
    file.write(R"({"morph_seconds":2,"mesh_builds_per_batch":1,"preload_pages":0,
        "prediction_seconds":0.75,"prediction_max_metres":256})");
    now += std::chrono::milliseconds(500);
    CHECK(watch.poll(config, error, now));
    CHECK_EQ(config.morphSeconds, 2.0);
    CHECK_EQ(config.meshesPerPlan, std::size_t(1));
    CHECK_EQ(config.preloadPages, std::size_t(0));
    CHECK_EQ(config.lookAheadSeconds, 0.75);
    CHECK_EQ(config.maxLookAheadMetres, 256.0);
    CHECK(previous == TerrainConfig{});
    TerrainPlan oldPlan;
    oldPlan.view.config = previous;
    auto view = oldPlan.view;
    view.config = config;
    CHECK(oldPlan.dirty(view, 0));
    const auto lastGood = config;
    file.write(R"({"morph_seconds":0,"mesh_builds_per_batch":0})");
    now += std::chrono::milliseconds(500);
    CHECK(!watch.poll(config, error, now));
    CHECK(!error.empty());
    CHECK(config == lastGood);
}

TEST(terrain_config_distance_scale_moves_lod_not_the_physical_projection) {
    TerrainView view;
    view.width = view.height = 1000;
    view.matrix[0] = view.matrix[5] = view.matrix[14] = 1;
    view.matrix[15] = 0;
    const ViewBounds near{0, 0, 1, 1, 100, 100};
    const ViewBounds twiceAsFar{0, 0, 1, 1, 200, 200};
    CHECK(view.perspective());
    const auto original = view;
    const double mpp = view.metresPerPixel(twiceAsFar);
    CHECK(!view.refine(twiceAsFar, 3)); // 32 / 0.4 = 80 px, below 128
    view.config.detailDistanceScale = 2;
    CHECK(view != original);
    CHECK_EQ(view.metresPerPixel(twiceAsFar), mpp);
    CHECK_EQ(view.lodMetresPerPixel(twiceAsFar), original.metresPerPixel(near));
    CHECK(view.refine(twiceAsFar, 3));
    CHECK_EQ(view.targetFor(twiceAsFar), original.targetFor(near));
    for (int lod = 1; lod <= 6; ++lod) {
        CHECK_EQ(view.refine(twiceAsFar, lod), original.refine(near, lod));
        CHECK_EQ(view.refine(twiceAsFar, lod, true), original.refine(near, lod, true));
    }
    view.config.detailDistanceScale = 1;
    view.config.lod.refineTrianglePixels = 64;
    view.config.lod.coarsenTrianglePixels = 24;
    view.config.lod.targetTrianglePixels = 48;
    CHECK(view.refine(twiceAsFar, 3));
    // Same effective screen budget is used for orthographic zoom in GpuTerrain.
    CHECK(geometryLevelFor(mpp / 2) < geometryLevelFor(mpp));
    TerrainPlan plan;
    plan.view = original;
    CHECK(plan.dirty(view, 0));
}

TEST(terrain_config_checked_in_file_is_valid_and_chunks_align_with_pages) {
    TerrainConfig config;
    std::string error;
    const auto root = std::filesystem::path(__FILE__).parent_path().parent_path();
    CHECK(readTerrainConfig(root / "content/config/terrain.json", config, error));
    CHECK(error.empty());
    // Check the shipped tuning, not TerrainConfig's fallback defaults.
    CHECK_EQ(config.detailDistanceScale, 8.0);
    CHECK_EQ(config.chunkCells, 32);
    // Two hundred and fifty-six metres to a tile until sixteen of its steps
    // fill it, and from there sixteen cells a tile, doubling with the step: a
    // coarse tile is a mesh with shape in it, not one cell, and the root is
    // sixty-four kilometres - a whole world is hundreds of roots, not the tens
    // of thousands of one-cell squares a tile one step wide made of it.
    for (std::size_t lod = 0; lod < config.chunkMetres.size(); ++lod) {
        const int metres = config.chunkMetres[lod];
        CHECK_EQ(metres, std::max(256, (4 << lod) * 16));
        CHECK(metres > 0 && (metres & (metres - 1)) == 0);
        // Whole pages either way round: a tile fits inside a page or covers them.
        CHECK(512 % metres == 0 || metres % 512 == 0);
    }
    CHECK_EQ(config.morphSeconds, 0.25);
    // A plan comes only once the last one's morph is done: twelve meshes a
    // plan left a static view refining for over seven hundred frames.
    CHECK_EQ(config.meshesPerPlan, std::size_t(192));
    CHECK_EQ(config.meshCacheBytes, std::size_t(256) << 20);
    CHECK_EQ(config.gpuMeshCacheBytes, std::size_t(512) << 20);
    CHECK_EQ(config.preloadPages, std::size_t(16));
    CHECK_EQ(config.preloadMarginPixels, 32.0);
    CHECK_EQ(config.lookAheadSeconds, 0.2);
    CHECK_EQ(config.maxLookAheadMetres, 256.0);
    for (int cells : {16,32,64}) for (int lod = 0; lod <= 6; ++lod) {
        const auto side = world::tileMetresAt(lod, cells);
        CHECK(side <= 512 ? 512 % side == 0 : side % 512 == 0);
    }
    CHECK_EQ(world::tileMetresAt(2, 32), std::int64_t(512));
    CHECK_EQ(world::tileMetresAt(1, 32), std::int64_t(256));
}

