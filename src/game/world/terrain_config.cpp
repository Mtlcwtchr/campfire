#include "game/world/terrain_config.hpp"

#include <cmath>
#include <fstream>
#include <stdexcept>
#include <type_traits>
#include <nlohmann/json.hpp>

namespace world::terrain {

bool readTerrainConfig(const std::filesystem::path& file, TerrainConfig& into, std::string& error) {
    error.clear();
    try {
        std::ifstream input(file);
        if (!input) throw std::runtime_error("cannot read file");
        const auto json = nlohmann::json::parse(input);
        if (!json.is_object()) throw std::runtime_error("expected a JSON object");
        for (const auto& [key, value] : json.items()) {
            if (key == "_comment" || key == "detail_distance_scale" || key == "chunk_cells" ||
                key == "chunk_metres" || key == "chunk_metres_per_lod" ||
                key == "h8_atlas_side" || key == "target_triangle_pixels" ||
                key == "refine_triangle_pixels" || key == "coarsen_triangle_pixels" ||
                key == "meshes_per_plan" || key == "preload_pages" || key == "preload_margin_pixels" ||
                key == "morph_seconds" || key == "mesh_builds_per_batch" ||
                key == "prediction_seconds" || key == "prediction_max_metres" ||
                key == "look_ahead_seconds" || key == "max_look_ahead_metres" ||
                key == "local_detail_height_metres" || key == "upload_mib_per_frame" ||
                key == "cull_ground" || key == "mesh_cache_mib" || key == "gpu_mesh_cache_mib") continue;
            throw std::runtime_error("unknown parameter: " + key);
        }
        const auto number = [&](const char* key, auto& destination, double low, double high) {
            const auto it = json.find(key);
            if (it == json.end()) return;
            using T = std::remove_reference_t<decltype(destination)>;
            if (!it->is_number() || (std::is_integral_v<T> && !it->is_number_integer()))
                throw std::runtime_error(std::string(key) + ": expected " +
                    (std::is_integral_v<T> ? "an integer" : "a number"));
            const double value = it->template get<double>();
            if (!std::isfinite(value) || value < low || value > high)
                throw std::runtime_error(std::string(key) + ": expected range [" +
                    std::to_string(low) + ", " + std::to_string(high) + "]");
            destination = static_cast<T>(value);
        };
        const auto aliasedNumber = [&](const char* key, const char* legacy, auto& destination,
                                       double low, double high) {
            if (json.contains(key) && json.contains(legacy))
                throw std::runtime_error(std::string(key) + ": cannot also specify " + legacy);
            number(json.contains(key) ? key : legacy, destination, low, high);
        };
        TerrainConfig fresh;
        if (const auto it = json.find("cull_ground"); it != json.end()) {
            if (!it->is_boolean()) throw std::runtime_error("cull_ground: expected true or false");
            fresh.cullGround = it->template get<bool>();
        }
        number("detail_distance_scale", fresh.detailDistanceScale, 0.25, 16);
        number("chunk_cells", fresh.chunkCells, 1, 64);
        if ((fresh.chunkCells & (fresh.chunkCells - 1)) != 0)
            throw std::runtime_error("chunk_cells: expected a power of two in [1, 64]");
        if (json.contains("chunk_cells") && (json.contains("chunk_metres") || json.contains("chunk_metres_per_lod")))
            throw std::runtime_error("use chunk_metres / chunk_metres_per_lod instead of chunk_cells, not both");
        if (json.contains("chunk_metres")) {
            int metres = 256;
            number("chunk_metres", metres, 128, 1024);
            // A tile has to be at least one cell across, so past the level where
            // the step catches up with the tile the tile doubles with it.
            // Filling every level with the same number instead pins the step
            // there - which is what put a two-hundred-and-fifty-six-metre floor
            // under the whole pyramid and left the far view drawn at sixty
            // times the density it asked for.
            for (std::size_t lod = 0; lod < fresh.chunkMetres.size(); ++lod)
                fresh.chunkMetres[lod] = std::max(metres, 4 << lod);
        }
        if (const auto it = json.find("chunk_metres_per_lod"); it != json.end()) {
            if (!it->is_array() || it->size() != fresh.chunkMetres.size())
                throw std::runtime_error(
                        "chunk_metres_per_lod: expected " + std::to_string(fresh.chunkMetres.size()) +
                        " integers, for steps 4,8,16,32,64,128,256,512,1024,2048,4096 m");
            for (std::size_t lod = 0; lod < fresh.chunkMetres.size(); ++lod) {
                const auto& value = (*it)[lod];
                if (!value.is_number_integer() || value.get<double>() < 128 ||
                    value.get<double>() > 65536)
                    throw std::runtime_error("chunk_metres_per_lod: expected integers in [128, 65536]");
                fresh.chunkMetres[lod] = value.get<int>();
            }
        }
        if (fresh.chunkMetres[0]) for (int lod = 0; lod < int(fresh.chunkMetres.size()); ++lod) {
            const int metres = fresh.chunkMetres[lod];
            if ((metres & (metres - 1)) || metres > 256 * (4 << lod))
                throw std::runtime_error("chunk metres must be powers of two with at most 256 cells per side");
            if (lod && metres != fresh.chunkMetres[lod-1] && metres != 2 * fresh.chunkMetres[lod-1])
                throw std::runtime_error("each coarser chunk must have the same extent or twice the preceding extent");
        }
        number("h8_atlas_side", fresh.h8AtlasSide, 4, 32);
        number("target_triangle_pixels", fresh.lod.targetTrianglePixels, 1, 1024);
        number("refine_triangle_pixels", fresh.lod.refineTrianglePixels, 1, 2048);
        number("coarsen_triangle_pixels", fresh.lod.coarsenTrianglePixels, 0.25, 512);
        if (!(2 * fresh.lod.coarsenTrianglePixels < fresh.lod.refineTrianglePixels) ||
            fresh.lod.targetTrianglePixels < fresh.lod.coarsenTrianglePixels ||
            fresh.lod.targetTrianglePixels > fresh.lod.refineTrianglePixels)
            throw std::runtime_error("LOD hysteresis requires 2*coarsen < refine and coarsen <= target <= refine");
        number("morph_seconds", fresh.morphSeconds, 0, 10);
        aliasedNumber("mesh_builds_per_batch", "meshes_per_plan", fresh.meshesPerPlan, 1, 1024);
        number("preload_pages", fresh.preloadPages, 0, 256);
        number("preload_margin_pixels", fresh.preloadMarginPixels, 0, 512);
        aliasedNumber("prediction_seconds", "look_ahead_seconds", fresh.lookAheadSeconds, 0, 2);
        aliasedNumber("prediction_max_metres", "max_look_ahead_metres", fresh.maxLookAheadMetres, 0, 4096);
        number("local_detail_height_metres", fresh.localDetailHeightMetres, 0, 512);
        double uploadMiB = double(fresh.uploadBytesPerFrame) / (1u << 20);
        number("upload_mib_per_frame", uploadMiB, 1, 64);
        fresh.uploadBytesPerFrame = static_cast<std::size_t>(uploadMiB * (1u << 20));
        double meshMiB = double(fresh.meshCacheBytes) / (1u << 20), gpuMeshMiB = double(fresh.gpuMeshCacheBytes) / (1u << 20);
        number("mesh_cache_mib", meshMiB, 0, 4096);
        number("gpu_mesh_cache_mib", gpuMeshMiB, 0, 8192);
        fresh.meshCacheBytes = static_cast<std::size_t>(meshMiB * (1u << 20));
        fresh.gpuMeshCacheBytes = static_cast<std::size_t>(gpuMeshMiB * (1u << 20));
        into = fresh;
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool TerrainConfigWatch::poll(TerrainConfig& into, std::string& error, Clock::time_point now) {
    error.clear();
    if (now < next_) return false;
    next_ = now + std::chrono::milliseconds(500);
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(file_, ec);
    std::string failure;
    TerrainConfig fresh;
    if (ec) failure = ec.message();
    else {
        if (loaded_ && stamp == stamp_) return false;
        if (readTerrainConfig(file_, fresh, failure)) {
            const bool changed = !loaded_ || fresh != into;
            into = fresh;
            stamp_ = stamp;
            loaded_ = true;
            lastError_.clear();
            return changed;
        }
    }
    if (failure != lastError_) error = failure;
    lastError_ = std::move(failure);
    return false;
}

} // namespace world::terrain

