#pragma once
// CPU-only tuning. A view carries a value snapshot, never a mutable file watcher.
#include <chrono>
#include <filesystem>
#include <string>
#include "game/world/terrain_lod.hpp"

namespace world::terrain {

struct TerrainConfig {
    LodPolicy lod;
    // Bigger means finer geometry farther away; physical projection stays intact.
    double detailDistanceScale = 1.0;
    int chunkCells = kTerrainChunkCells; // restart world: changes tile coordinates
    std::array<int, kGeometryLevels> chunkMetres{}; // explicit metres per LOD; zero = legacy chunkCells
    int h8AtlasSide = 16; // restart world: side*side slots, not page width
    double morphSeconds = 0.25; // live rate change, 0 = immediate once geometry is ready
    std::size_t meshesPerPlan = kMeshesPerPlan;
    std::size_t preloadPages = kCameraPreloadPages;
    double preloadMarginPixels = 32.0;
    double lookAheadSeconds = 0.2;
    double maxLookAheadMetres = 512.0;
    double localDetailHeightMetres = 32.0; // bounded 200 m H4 window remains fixed
    // Skip the ground's back faces. Restart to change: a pipeline's cull mode is
    // fixed when the pipeline is built.
    //
    // On, because the engine drew both sides of every triangle in the world and
    // for the ground that is half the rasterisation thrown away. It is settable
    // because whether a face is "front" is decided after the projection, and if
    // this projection turns out to flip handedness the ground would vanish -
    // which is a thing to be able to undo in a text file rather than in a
    // compiler.
    bool cullGround = true;
    std::size_t uploadBytesPerFrame = 8u << 20;
    // Meshes kept after the cut stops drawing them, least recently used out
    // first: coming back to ground just left (a zoom out and in, a turn of
    // the head) takes them from here instead of building them again. The
    // CPU's built meshes and the card's uploaded buffers, each its budget.
    std::size_t meshCacheBytes = 256u << 20;
    std::size_t gpuMeshCacheBytes = 512u << 20;
    bool operator==(const TerrainConfig&) const = default;
};

// Missing fields use defaults. Invalid files leave `into` untouched.
// Unknown fields are rejected, except _comment; misspelled knobs must not be silent.
bool readTerrainConfig(const std::filesystem::path& file, TerrainConfig& into, std::string& error);

class TerrainConfigWatch {
public:
    using Clock = std::chrono::steady_clock;
    explicit TerrainConfigWatch(std::filesystem::path file) : file_(std::move(file)) {}
    // Poll at most twice a second. Retry partial writes; report identical errors once.
    bool poll(TerrainConfig& into, std::string& error, Clock::time_point now = Clock::now());
    const std::filesystem::path& file() const { return file_; }
private:
    std::filesystem::path file_;
    std::filesystem::file_time_type stamp_{};
    Clock::time_point next_{};
    std::string lastError_;
    bool loaded_ = false;
};

} // namespace world::terrain

