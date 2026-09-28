#pragma once
#include <future>
#include <string>
#include "engine/camera/camera.hpp"
#include "engine/geometry/cluster_dag.hpp"
#include "engine/render/representation_selector.hpp"
#include "engine/ui/ui.hpp"

namespace engine::editor {
// Engine-only workspace. No World, ContentDb or simulation is linked here.
class ModulePanel {
public:
    camera::Camera camera;
    camera::ViewQualityProfile quality;
    std::uint64_t seed = 11;
    double relief = 1;
    bool wireframe = false;
    bool regenerate = true;
    // The world scene is the game's explorer, run as its own process beside
    // the editor (the engine library does not link the game): the button asks,
    // the editor's main loop launches and reports.
    bool launchScene = false, sceneRunning = false;
    std::string sceneStatus;
    ModulePanel();
    void draw(ui::Ui& gui, bool gpuPreview = false);
    void poll();
    void wait();
    [[nodiscard]] bool busy() const { return job_.valid(); }
    [[nodiscard]] const std::string& error() const { return error_; }
    [[nodiscard]] const geometry::ClusterDag& dag() const { return preview_.dag; }
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::span<const std::uint32_t> cut() const { return chosen_; }
    [[nodiscard]] SDL_Rect viewportArea() const { return viewportArea_; }
    void invalidateUi() { lastUiKey_.clear(); }
    [[nodiscard]] std::uint64_t uiPaints() const { return uiPaints_; }
    void resetCamera();
private:
    struct Preview {
        geometry::ClusterDag dag;
        std::uint64_t seed = 0;
        double relief = 0;
    };
    Preview preview_;
    std::future<Preview> job_;
    std::string error_;
    float lastMouseX_ = 0, lastMouseY_ = 0;
    std::size_t selectedTriangles_ = 0, selectedClusters_ = 0;
    std::uint64_t revision_ = 0;
    std::vector<std::uint32_t> chosen_;
    SDL_Rect viewportArea_{};
    std::string lastUiKey_;
    std::uint64_t uiPaints_ = 0;
    void start();
    void viewport(ui::Ui& gui, ui::Rect area, bool gpuPreview);
};
}

