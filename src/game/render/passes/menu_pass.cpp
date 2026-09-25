#include "game/render/passes/menu_pass.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/client/explore_menu.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/gpu_terrain.hpp"

namespace game {

MenuPass::~MenuPass() {
    if (progressSoftware_) SDL_DestroyRenderer(progressSoftware_);
    if (progressSurface_) SDL_DestroySurface(progressSurface_);
    if (software_) SDL_DestroyRenderer(software_);
    if (surface_) SDL_DestroySurface(surface_);
}

engine::PassPlace MenuPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    engine::PipelineWanted wanted;
    wanted.shaderFile = "menu.hlsl";
    wanted.vertexEntry = "MenuVS";
    wanted.fragmentEntry = "MenuPS";
    wanted.blend = true;
    wanted.depthTest = false;
    wanted.depthWrite = false;
    engine::GraphicsPipeline pipeline = device.makePipeline(wanted);
    if (!pipeline) return {};

    // ABGR8888 is R, G, B, A in memory on this machine, which is what the card
    // is told the texture is.
    surface_ = SDL_CreateSurface(client::ExploreMenu::kWide, client::ExploreMenu::kHigh,
                                 SDL_PIXELFORMAT_ABGR8888);
    if (!surface_) { device.fail(SDL_GetError()); return {}; }
    software_ = SDL_CreateSoftwareRenderer(surface_);
    if (!software_) { device.fail(SDL_GetError()); return {}; }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = client::ExploreMenu::kWide;
    info.height = client::ExploreMenu::kHigh;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    picture_ = device.makeTexture(info);
    if (!picture_) return {};

    // Nearest and clamped: the picture is drawn at the size it is shown at, and
    // a font smoothed between its own pixels is a font nobody can read.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    pipeline_ = into.take(std::move(pipeline));
    bindings_ =
            into.take(std::vector<SDL_GPUTextureSamplerBinding>{{picture_.get(), sampler_.get()}});
#if ASR_ENABLE_DIAGNOSTICS
    if (terrain_) {
        progressSurface_ = SDL_CreateSurface(kProgressWide, kProgressHigh, SDL_PIXELFORMAT_ABGR8888);
        if (!progressSurface_) { device.fail(SDL_GetError()); return {}; }
        progressSoftware_ = SDL_CreateSoftwareRenderer(progressSurface_);
        if (!progressSoftware_) { device.fail(SDL_GetError()); return {}; }
        info.width = kProgressWide; info.height = kProgressHigh;
        progressPicture_ = device.makeTexture(info);
        if (!progressPicture_) return {};
        progressBindings_ = into.take(std::vector<SDL_GPUTextureSamplerBinding>{
            {progressPicture_.get(), sampler_.get()}});
    }
#endif
    return {engine::passOf(Pass::Menu), engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Interface)};
}

bool MenuPass::anything(const engine::Frame& frame) const {
    (void)frame;
    return true; // the compact view/stage/display readout is always present
}

void MenuPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
#if ASR_ENABLE_DIAGNOSTICS
    if (terrain_) collectProgress(frame, queue);
#endif
    if (menu_.dirty()) {
        menu_.draw(software_, client::ExploreMenu::kWide, client::ExploreMenu::kHigh);
        SDL_RenderPresent(software_);
        engine::Device::Uploader uploader(*frame.device);
        // A surface's rows may be padded; the card wants them packed.
        if (surface_->pitch == client::ExploreMenu::kWide * 4) {
            uploader.refill(picture_.get(), surface_->pixels, client::ExploreMenu::kWide,
                            client::ExploreMenu::kHigh, 4);
        } else {
            std::vector<std::uint8_t> packed(static_cast<std::size_t>(client::ExploreMenu::kWide) *
                                             client::ExploreMenu::kHigh * 4);
            for (int y = 0; y < client::ExploreMenu::kHigh; ++y)
                std::memcpy(packed.data() + static_cast<std::size_t>(y) *
                                                    client::ExploreMenu::kWide * 4,
                            static_cast<const std::uint8_t*>(surface_->pixels) +
                                    static_cast<std::size_t>(y) * surface_->pitch,
                            static_cast<std::size_t>(client::ExploreMenu::kWide) * 4);
            uploader.refill(picture_.get(), packed.data(), client::ExploreMenu::kWide,
                            client::ExploreMenu::kHigh, 4);
        }
        uploader.finish();
    }

    // Where it sits on the screen, in pixels: a fixed margin from the top left,
    // at one pixel to one pixel, so the letters are the size they were drawn.
    engine::DrawItem item;
    item.pipeline = pipeline_;
    item.bindings = bindings_;
    item.vertexCount = 6;
    item.ownToVertex = true;
    item.own[0] = 16;
    item.own[1] = 16;
    item.own[2] = client::ExploreMenu::kWide;
    item.own[3] = client::ExploreMenu::kHigh;
    queue.push(item);
}

#if ASR_ENABLE_DIAGNOSTICS
void MenuPass::collectProgress(const engine::Frame& frame, engine::DrawQueue& queue) {
    // Always visible, but repaint at a bounded rate. No cursor raycast, bake,
    // filesystem access or worker wait belongs on this UI path.
    const auto& p = terrain_->progress();
    if (!progressDrawn_ || frame.index % 6 == 0 || p.viewMesh.percent() != shownViewPercent_) {
        SDL_SetRenderDrawColor(progressSoftware_, 16, 22, 28, 238);
        SDL_RenderClear(progressSoftware_);
        const auto text = [&](float y, const char* value) {
            SDL_SetRenderDrawColor(progressSoftware_, 219, 229, 235, 255);
            SDL_RenderDebugText(progressSoftware_, 10, y, value);
        };
        const char* phase = !p.initialized ? "initializing" : p.uploadFailed ? "UPLOAD ERROR" :
            p.backgroundRunning ? "prepare H64 -> H16" : p.frozen ? "PAUSED" :
            p.capacityLimited ? "H4/H8 capacity limit" :
            p.viewMesh.percent() == 100 ? "view ready" : p.busy ? "building pages" :
            p.staged || p.ready ? "uploading pages" : "refining / waiting";
        char line[160];
        if (p.cameraMode) std::snprintf(line, sizeof(line), "TERRAIN: %s | LOD by screen size", phase);
        else std::snprintf(line, sizeof(line), "TERRAIN: %s | LOD %d", phase, p.targetLod);
        text(8, line);
        const auto bar = [&](float y, const char* label, const world::terrain::PreparationProgress& progress) {
            const int part = progress.percent();
            if (part < 0) std::snprintf(line, sizeof(line), "%-22s  ...", label);
            else std::snprintf(line, sizeof(line), "%-22s %3d%%  %.1f/%zu", label,
                               part, progress.ready, progress.total);
            text(y, line);
            SDL_FRect track{10, y + 10, kProgressWide - 20.0f, 3};
            SDL_SetRenderDrawColor(progressSoftware_, 48, 60, 69, 255);
            SDL_RenderFillRect(progressSoftware_, &track);
            track.w *= static_cast<float>(std::max(0, part)) / 100.0f;
            SDL_SetRenderDrawColor(progressSoftware_, part == 100 ? 97 : 210, part == 100 ? 184 : 166, 133, 255);
            SDL_RenderFillRect(progressSoftware_, &track);
        };
        bar(26, "RAM prepare foundation", p.ramPrepared);
        bar(48, "RAM published/pinned", p.ramPublished);
        bar(70, "GPU H64 pinned", p.persistentGpu);
        bar(92, "View data / final LOD", p.viewPages);
        bar(114, "View mesh incl. morph", p.viewMesh);
        std::snprintf(line, sizeof(line), "Mesh lattice %d..%d m | height data %d..%d m",
                      p.meshStepMin,p.meshStepMax,p.dataStepMin,p.dataStepMax);
        text(140, line);
        std::snprintf(line, sizeof(line), "Workers %zu/%zu | queue %zu | pending upload %zu",
                      p.busy, p.workers, p.queued, p.ready + p.staged);
        text(156, line);
        std::snprintf(line, sizeof(line), "Page %.0f ms (session max %.0f) | upload CPU %.1f",
                      p.lastBuildMs, p.longestBuildMs, p.uploadMs);
        text(172, line);
        constexpr double mib = 1024.0 * 1024.0;
        std::snprintf(line, sizeof(line), "RAM pin %.0f + cache %.0f | GPU pages %.0f MiB",
                      static_cast<double>(p.ramBytes) / mib, static_cast<double>(p.cacheBytes) / mib,
                      static_cast<double>(p.gpuBytes) / mib);
        text(188, line);
        std::snprintf(line, sizeof(line), "Preload %zu | errors %zu | background %s",
                      p.speculative, p.failed + p.backgroundFailed, p.backgroundRunning ? "active" : "idle");
        text(204, line);
        std::snprintf(line, sizeof(line), "Busy: view %zu preload %zu prep %zu inspect %zu",
                      p.visibleBusy, p.preloadBusy, p.preparationBusy, p.inspectionBusy);
        text(220, line);
        std::snprintf(line, sizeof(line), "Plans %zu/%zu | batch %zu meshes | deferred %zu",
                      p.plansCompleted, p.plansSubmitted, p.meshesBuilt,p.deferredRegions);
        text(236, line);
        std::snprintf(line, sizeof(line), "Plan CPU %.1f ms | latency %.1f ms",
                      p.planBuildMs, p.planLatencyMs);
        text(252, line);
        std::snprintf(line, sizeof(line), "Chunks %d..%d m | %d cells per side",
                      p.chunkMetresMin,p.chunkMetresMax,p.chunkCells);
        text(268, line);
        std::snprintf(line,sizeof(line),"Target at focus: LOD %d (%d m); not height data",
                      p.targetLod,4 << p.targetLod);
        text(284, line);
        std::snprintf(line, sizeof(line), "V: %s | wheel: %s",
                      p.cameraMode == 2 ? "1st / free" : p.cameraMode == 1 ? "3rd / orbit" : "map / ortho",
                      p.cameraMode == 2 ? "flight speed" : "zoom");
        text(300, line);
        if (p.cameraMode == 2) {
            std::snprintf(line, sizeof(line), "RMB look | WASD move | Q/E height | Shift: %.0f m/s",
                          p.flightSpeed * 4);
            text(316, line);
        } else text(316, "RMB orbit | WASD pan | Q/E height | Home reset");
        std::snprintf(line, sizeof(line), "Shift+G grid: %s (world metres)",
                      p.gridMode == 1 ? "HEIGHT DATA (not mesh)" : p.gridMode == 2 ? "ACTUAL TRIANGLES" : "OFF");
        text(332, line);
        text(348, "4 cyan | 8 green | 16 yellow | 32 orange | 64 pink");
        text(364, "Mesh: 128 blue / 256 violet; colour = lattice step");
        SDL_RenderPresent(progressSoftware_);
        std::vector<std::uint8_t> pixels(kProgressWide * kProgressHigh * 4);
        for (int y = 0; y < kProgressHigh; ++y)
            std::memcpy(pixels.data() + y * kProgressWide * 4,
                        static_cast<const std::uint8_t*>(progressSurface_->pixels) + y * progressSurface_->pitch,
                        kProgressWide * 4);
        engine::Device::Uploader upload(*frame.device);
        const bool copied = upload.refillRegion(progressPicture_.get(), pixels.data(), 0, 0,
            kProgressWide, kProgressHigh, 4, 0, 0, true);
        progressDrawn_ = upload.finish() && copied;
        if (progressDrawn_) shownViewPercent_ = p.viewMesh.percent();
    }
    if (!progressDrawn_) return;
    const float width = frame.scene.viewport[0], height = frame.scene.viewport[1];
    const float scale = std::min({1.0f, std::max(1.0f, width - 32) / kProgressWide,
                                 std::max(1.0f, height - 32) / kProgressHigh});
    engine::DrawItem item;
    item.pipeline = pipeline_; item.bindings = progressBindings_;
    item.vertexCount = 6; item.ownToVertex = true;
    item.own[2] = kProgressWide * scale; item.own[3] = kProgressHigh * scale;
    item.own[0] = std::max(0.0f, width - item.own[2] - 16);
    item.own[1] = menu_.visible() && width < kProgressWide + client::ExploreMenu::kWide + 48
        ? std::max(0.0f, height - item.own[3] - 16) : 16;
    queue.push(item);
}
#endif

} // namespace game
