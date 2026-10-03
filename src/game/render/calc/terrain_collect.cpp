#include "game/render/calc/terrain_collect.hpp"

#include <algorithm>
#include <cmath>

#include "game/render/pass_ids.hpp"
#include "game/render/passes/terrain_pass.hpp"

namespace game {

engine::PassId TerrainCollectPass::id() const { return engine::passOf(Pass::Collect); }

void TerrainCollectPass::run(engine::Frame& frame) {
    gpu_.update(frame, camera_);
    cache_.begin(frame.index);
    patchesDrawn_ = gpu_.drawing().size();
    trianglesDrawn_ = 0;
    standingIn_ = gpu_.coarse();
    coarseLevels_ = 0;
    muchCoarser_ = 0;
#if ASR_ENABLE_PROFILING
    world::terrain::TerrainView view;
    camera_.viewProjection(view.matrix.data(), 0, 1);
    view.width = camera_.viewportWidth; view.height = camera_.viewportHeight;
    view.target = gpu_.target();
    for (const auto& block : gpu_.drawing()) {
        trianglesDrawn_ += gpu_.indexCount(block) / 3;
        if (block.mesh && block.mesh->detailError <= view.metresPerPixel(block.bounds)*2.0) continue;
        const int difference = block.tile.lod - view.targetFor(block.bounds);
        if (difference > 0) coarseLevels_ += difference;
        if (difference >= 3) ++muchCoarser_;
    }
#endif
    engine::Device::Uploader uploader(*frame.device);
    // The sea: one grid, built once, in coordinates relative to where the
    // camera looks (the water pass hands the vertex stage that centre). Dense
    // there and exponentially coarser outwards, so its vertices can carry the
    // waves rolling onto a beach up close and still reach past the horizon.
    {
        const auto key = static_cast<std::int64_t>(0x5EA0000000000001ull);
        cache_.want(uploader, key, kMeshHasWater, [&] { return buildSea(0, 0, kSeaHalfSpan * 2); }, {});
    }
    uploader.finish();
    // The same tail the explorer keeps its own patches for. Shorter here and the
    // card would be re-uploading ground the CPU still has in its hand.
    // Epoch meshes cannot be addressed by a later snapshot. Keep only a short
    // GPU tail; the retained snapshot is requested every frame explicitly.
    cache_.forget(2);
}

engine::Scene sceneFor(const client::Camera& camera,
                       const std::vector<content::GroundMaterial>& ground) {
    engine::Scene scene{};
    scene.camera[0] = static_cast<float>(camera.centreX);
    scene.camera[1] = static_cast<float>(camera.centreY);
    scene.camera[2] = static_cast<float>(camera.focusHeight);
    scene.camera[3] = static_cast<float>(camera.pixelsPerTile);
    if (camera.perspective()) {
        const auto eye = camera.eyePosition();
        for (int i = 0; i < 3; ++i) scene.camera[i] = static_cast<float>(eye[i]);
    }
    // How much ground one turn of a material covers. Wider than it was: at
    // eight metres the repeat was a grid the eye picked out at once, and the
    // detail lost by going wider is put back by the second, broader read the
    // shader multiplies in.
    scene.viewport[3] = static_cast<float>(kMaterialMetres);

    // Depth, as a slab around the view rather than a projection. The camera is
    // isometric, so what stands in front of what is decided by x + y + z alone;
    // the span has to cover the whole view or distant ground clips, and it is
    // taken from how much world the window is showing.
    const double across =
            std::max(1000.0, static_cast<double>(std::max(camera.viewportWidth,
                                                          camera.viewportHeight)) /
                                     std::max(0.001, camera.pixelsPerTile) * 8.0);
    scene.extra[0] = 0;
    scene.extra[1] = static_cast<float>(1.0 / across);
    // And the projection itself, from the camera. The depth slab goes into it,
    // which is why it is built here rather than by the runner: how much world
    // the depth buffer has to cover is a decision about this view of this game,
    // and the engine has no business having an opinion on it.
    camera.viewProjection(scene.viewProjection, scene.extra[0], scene.extra[1]);
    // Foliage has its own screen-size fade and a ground-cover representation
    // at every distance. Do not switch the entire meadow off with terrain LOD.
    scene.extra[2] = 1.0f;
    // Left over from the cloud that used to cover unfinished ground (D136). The
    // mask that replaced it needs nothing from the scene.
    scene.extra[3] = 1.0f;

    // Which way it is blowing.
    //
    // One wind for the whole picture, decided here and carried in the scene, so
    // that the grass and the water agree about it: a field combed south-west
    // beside water rippling north is two weathers in one view, and the eye finds
    // that immediately even when it cannot say what is wrong.
    //
    // Fixed for now, and honestly fixed rather than pretend-random: when the
    // simulation has weather it will hand its own direction and strength over,
    // and nothing here changes but where the two numbers come from. The
    // travelling of the gusts is the shaders' business - it is a function of
    // position and the clock, and it costs nothing to advance.
    const double angle = 2.6;   // radians, about south-west
    scene.wind[0] = static_cast<float>(std::cos(angle));
    scene.wind[1] = static_cast<float>(std::sin(angle));
    scene.wind[2] = 1.0f;       // how hard, one being a fresh breeze
    scene.wind[3] = 1.0f;       // how gusty: the size of the swing between lulls
    std::copy(std::begin(scene.wind), std::end(scene.wind), scene.swellWind);

    // What each material of the ground keeps, for the blend between them.
    //
    // The first number is turns of the texture rather than metres: the vertex
    // already carries a coordinate in turns of kMaterialMetres, worked out from
    // the patch's own corner so a float still has the precision for it out at
    // four hundred kilometres, so what the shader wants is how many of its own
    // turns fit in one of those.
    for (std::size_t i = 0; i < content::kBlendedMaterials && i < ground.size(); ++i) {
        scene.table[i][0] = static_cast<float>(kMaterialMetres) /
                            std::max(0.5f, ground[i].metresPerTurn);
        scene.table[i][1] = ground[i].blendWidth;
        scene.table[i][2] = ground[i].tear;
        scene.table[i][3] = std::max(0.2f, ground[i].tearMetres);
    }
    return scene;
}

engine::MeshUpload TerrainCollectPass::buildSea(double centreX, double centreY, double span) {
    // Keep the beach around the view target at one-metre spacing. The former
    // sinh grid already had six-metre cells only fifty metres from the target,
    // so a breaking wave was reduced to a handful of triangle-wide strips.
    // Spend the same 256 cells on a dense core and a smooth exponential tail;
    // indices and vertex count stay unchanged, including the far horizon.
    constexpr int kSide = 256;
    constexpr int kCoreCells = 96; // on each side: a 192 m wide dense core
    constexpr double kCoreStep = 1.0;
    constexpr int kTailCells = kSide / 2 - kCoreCells;
    const double half = span * 0.5;
    const double core = std::min(kCoreCells * kCoreStep, half * kCoreCells / (kSide / 2));
    const double step = core / kCoreCells;
    double low = 0.0, high = 1.0;
    for (int iteration = 0; iteration < 48; ++iteration) {
        const double curve = (low + high) * 0.5;
        const double tail = step * std::expm1(curve * kTailCells) / curve;
        if (tail < half - core) low = curve;
        else high = curve;
    }
    const double curve = (low + high) * 0.5;
    const auto place = [&](int i) {
        const int cell = i - kSide / 2;
        const int distance = std::abs(cell);
        const double x = distance <= kCoreCells ? distance * step :
                         core + step * std::expm1(curve * (distance - kCoreCells)) / curve;
        return cell < 0 ? -x : x;
    };
    const auto spacingAt = [&](int i) {
        const double x = place(i);
        return std::max(x - place(std::max(i - 1, 0)), place(std::min(i + 1, kSide)) - x);
    };
    // The shelf the world falls to past its last cell, so the shader reads
    // this as deep ocean rather than as a beach.
    constexpr float kBed = -60.0f;
    vertices_.clear();
    vertices_.reserve((kSide + 1) * (kSide + 1));
    for (int row = 0; row <= kSide; ++row)
        for (int column = 0; column <= kSide; ++column) {
            TerrainVertexGpu gpu{};
            gpu.position[0] = static_cast<float>(centreX + place(column));
            gpu.position[1] = static_cast<float>(centreY + place(row));
            gpu.position[2] = kBed;
            gpu.normal[2] = 1.0f;
            gpu.weights0[2] = 1.0f;   // sand under it, for what little shows
            gpu.uv[0] = gpu.position[0] / static_cast<float>(kMaterialMetres);
            gpu.uv[1] = gpu.position[1] / static_cast<float>(kMaterialMetres);
            gpu.morphUv[0] = static_cast<float>(std::max(spacingAt(column), spacingAt(row)));
            gpu.morphUv[1] = gpu.uv[1];
            gpu.morphHeight = kBed;
            gpu.morphNormal[2] = 1.0f;
            gpu.waterHeight = 0.0f;   // sea level is nought everywhere
            gpu.waterCover = 1.0f;
            // Neither river nor lake: the ocean, which is what the water
            // shader calls the absence of both.
            gpu.waterMotion[2] = 0.0f;
            gpu.waterMotion[3] = 0.0f;
            vertices_.push_back(gpu);
        }
    if (seaIndices_.empty()) {
        seaIndices_.reserve(kSide * kSide * 6);
        for (int row = 0; row < kSide; ++row)
            for (int column = 0; column < kSide; ++column) {
                const auto at = static_cast<std::uint32_t>(row * (kSide + 1) + column);
                const auto below = at + kSide + 1;
                for (const std::uint32_t index : {at, at + 1, below, at + 1, below + 1, below})
                    seaIndices_.push_back(index);
            }
    }
    engine::MeshUpload out;
    out.vertices = vertices_.data();
    out.vertexBytes = vertices_.size() * sizeof(TerrainVertexGpu);
    out.indices = seaIndices_.data();
    out.indexBytes = seaIndices_.size() * sizeof(std::uint32_t);
    out.indexCount = static_cast<std::uint32_t>(seaIndices_.size());
    out.tags = kMeshHasWater;
    return out;
}


} // namespace game
