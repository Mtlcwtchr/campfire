#include "game/render/world_renderer.hpp"

#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "game/render/gpu_terrain.hpp"
#include "game/render/passes/foliage_pass.hpp"
#include "game/render/passes/scene_models_pass.hpp"

namespace game {
std::string WorldRenderer::vegetationReport() const {
    auto report = models_ ? nlohmann::json::parse(models_->report()) : nlohmann::json::object();
    if (foliage_) report["grass"] = nlohmann::json::parse(foliage_->report());
    return report.dump();
}

bool WorldRenderer::screenshot(const std::string& path) {
    if (!runner_->screenshot(device_, path)) return false;
    auto report = nlohmann::json::parse(vegetationReport());
    if (foliage_) {
        std::uint32_t drawn = 0;
        if (!foliage_->readDrawnCandidates(device_, drawn)) {
            device_.fail("cannot read grass indirect draw count"); return false;
        }
        report["grass"]["drawn_candidates"] = drawn;
        report["grass"]["drawn_triangles"] = std::uint64_t(drawn) * 2;
    }
    const auto& map = world_->worldMap();
    const auto& frame = runner_->last();
    const auto& gpu = terrain();
    const auto& p = gpu.progress();
    report["seed"] = map.seed;
    report["world_cells"] = {map.width, map.height};
    report["world_metres"] = {std::int64_t(map.width) * generation::kMetresPerCell,
                               std::int64_t(map.height) * generation::kMetresPerCell};
    report["viewport"] = {frame.width, frame.height}; report["frame"] = frame.index;
    report["shader_time"] = frame.scene.viewport[2]; report["wind"] = frame.scene.wind;
    report["camera"] = frame.scene.camera; report["view_projection"] = frame.scene.viewProjection;
    report["terrain"] = {{"pages_percent", p.viewPages.percent()}, {"mesh_percent", p.viewMesh.percent()},
        {"capacity_limited", p.capacityLimited}, {"upload_failed", p.uploadFailed},
        {"missing", gpu.missing()}, {"coarse", gpu.coarse()}, {"final_stage", gpu.finalStage()},
        {"mesh_step_m", {p.meshStepMin, p.meshStepMax}}, {"data_step_m", {p.dataStepMin, p.dataStepMax}},
        {"ram_bytes", p.ramBytes}, {"cache_bytes", p.cacheBytes}, {"gpu_bytes", p.gpuBytes}};
    std::ofstream out(path + ".scene.json"); out << report.dump(2) << '\n';
    if (!out) { device_.fail("cannot write scene screenshot metadata"); return false; }
    std::cout << "Scene objects: " << report.dump() << '\n';
    return true;
}

bool WorldRenderer::compareGrassCulling(const client::Camera& camera, const WorldRenderSettings& settings,
                                       const std::string& path) {
    if (!foliage_ || !foliage_->gpuCulling()) {
        device_.fail("grass comparison requires the GPU path at setup"); return false;
    }
    foliage_->gpuCulling(false);
    const bool direct = draw(camera, settings) && screenshot(path + ".direct.png");
    foliage_->gpuCulling(true);
    return direct && draw(camera, settings) && screenshot(path + ".repeat.png");
}
} // namespace game
