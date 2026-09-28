#pragma once
#include "engine/camera/camera.hpp"
#include "engine/geometry/cluster_dag.hpp"
#include "engine/render/targets.hpp"
#include <span>

namespace engine {
// Engine-only inspection renderer. Selection stays with the caller; geometry,
// clipping and visibility are rendered on the GPU, never sorted/read back on CPU.
// A revision identifies mesh contents, not their address (workers reuse storage).
class DagViewport {
public:
    bool open(SDL_Window* window, const std::filesystem::path& assets);
    bool resize(Uint32 width, Uint32 height);
    bool mesh(const geometry::ClusterDag& dag, std::uint64_t revision);
    bool draw(const camera::Camera& camera, SDL_Rect area,
              std::span<const std::uint32_t> cut, bool wireframe,
              const SDL_Surface* overlay = nullptr);
    bool capture(const std::string& path);
    bool readPixels(std::span<std::uint8_t> pixels);
    const std::string& error() const { return device_.error(); }
    std::uint64_t meshUploads() const { return meshUploads_; }
    std::uint64_t overlayUploads() const { return overlayUploads_; }
    std::size_t residentBytes() const { return residentBytes_; }
    void wait() { device_.waitInFlight(0); }
private:
    // Device must outlive every Owned<> resource, including failed setup paths.
    Device device_;
    Targets targets_;
    GraphicsPipeline solid_, wire_, composite_;
    Buffer vertices_, indices_;
    Texture overlay_;
    Sampler sampler_;
    std::vector<IndexRange> ranges_;
    std::vector<std::uint8_t> overlayPixels_;
    std::uint64_t revision_ = 0, meshUploads_ = 0, overlayUploads_ = 0;
    std::size_t residentBytes_ = 0;
    bool hasMesh_ = false;
    bool uploadOverlay(const SDL_Surface& surface);
};
} // namespace engine
