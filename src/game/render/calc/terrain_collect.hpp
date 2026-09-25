#pragma once
// Collects page-backed terrain blocks, not copies of CPU terrain vertices.
// MeshCache is retained only for the independent open-sea surface.

#include <algorithm>
#include <cstdint>
#include <array>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/mesh_cache.hpp"
#include "game/client/camera.hpp"
#include "game/world/coords.hpp"
#include "game/content/ground_materials.hpp"
#include "game/render/terrain_data.hpp"
#include "game/render/gpu_terrain.hpp"


namespace game {

// Reference material scale used by sceneFor and the open-sea surface.
inline constexpr double kMaterialMetres = 14.0;


class TerrainCollectPass : public engine::CalcPass {
public:
    TerrainCollectPass(std::shared_ptr<world::WorldPreparation> preparation, const client::Camera& camera,
                       engine::MeshCache& cache)
        : camera_(camera), cache_(cache), gpu_(std::move(preparation)) {}

    GpuTerrain& gpuTerrain() { return gpu_; }
    const GpuTerrain& gpuTerrain() const { return gpu_; }
    void forgetWorld() { gpu_.reset(); }

    engine::PassId id() const override;
    void run(engine::Frame& frame) override;
    void requestFlood(core::WorldPos centre, double span) {
        const float half = static_cast<float>(std::max(0.0, span) * 0.5);
        floodBounds_ = {static_cast<float>(centre.x.toDouble()) - half,
                        static_cast<float>(centre.y.toDouble()) - half,
                        static_cast<float>(centre.x.toDouble()) + half,
                        static_cast<float>(centre.y.toDouble()) + half};
    }
    const std::array<float, 4>& floodBounds() const { return floodBounds_; }
    bool inspectionReady() const { return true; }

    std::size_t patchesDrawn() const { return patchesDrawn_; }
    // How many of them are standing in for a level that has not been cut yet.
    std::size_t standingIn() const { return standingIn_; }
    // And how far off they are, which is the number that matters.
    //
    // "Not the level asked for" counts ground that is *finer* than wanted, and
    // that is not a defect - it is a few more draw calls and a better picture.
    // What is worth measuring is how much coarser than wanted the view is, and
    // especially how much of it is coarser by three levels or more: that is the
    // difference between four metres to the sample and thirty-two, and it is
    // what looks like fog rolling over the world.
    double coarseLevels() const { return coarseLevels_; }
    std::size_t muchCoarser() const { return muchCoarser_; }
    std::size_t trianglesDrawn() const { return trianglesDrawn_; }

private:
    // The open sea, behind everything.
    //
    // Wherever no ground has been drawn - beyond the last page of the world,
    // in the corners a ring's disc does not reach, over a view whose ground has
    // not arrived - what showed was the clear colour, and a hole in the picture
    // is what that reads as. This world is seventy-one per cent ocean and its
    // sea level is nought everywhere, so open water is both the honest answer
    // and the one that is usually true.
    //
    // A grid rather than a quad, and fed through the water pass rather than
    // painted: it wants the swell, the depth colour and the light off it that
    // every other stretch of sea has, or the join where real water meets it is
    // a line. Its bed sits at the shelf depth, so the shader reads it as deep
    // ocean; its surface is nought, which is where the shader puts the vertex.
    engine::MeshUpload buildSea(double centreX, double centreY, double span);
    std::vector<std::uint32_t> seaIndices_;

    const client::Camera& camera_;
    engine::MeshCache& cache_;
    GpuTerrain gpu_;
    std::vector<TerrainVertexGpu> vertices_;
    std::size_t patchesDrawn_ = 0;
    std::size_t trianglesDrawn_ = 0;
    std::size_t standingIn_ = 0;
    double coarseLevels_ = 0;
    std::size_t muchCoarser_ = 0;
    std::array<float, 4> floodBounds_{-1.0f, -1.0f, -1.0f, -1.0f};
};

// What the shaders are told about the view. Everything in it is a decision about
// how the world reads at this distance, which is why it is on this side of the
// seam and not in the runner.
engine::Scene sceneFor(const client::Camera& camera,
                       const std::vector<content::GroundMaterial>& ground);

} // namespace game
