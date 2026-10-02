#include "game/render/passes/menu_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "engine/ui/canvas.hpp"
#include "engine/ui/font.hpp"
#include "game/client/explore_menu.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/gpu_terrain.hpp"

namespace game {
namespace {
// Every picture here is drawn by SDL's software renderer into a surface of the
// canvas's layout (canvas.hpp: the one its blending is fast in) and read by
// the card as the same bytes.
constexpr SDL_PixelFormat kSurfaceFormat = ui::Canvas::kFormat;
constexpr SDL_GPUTextureFormat kTextureFormat = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
} // namespace

void MenuPass::Window::release() {
    if (ui) ui->shutdown();
    ui.reset();
    if (software) SDL_DestroyRenderer(software);
    if (surface) SDL_DestroySurface(surface);
    software = nullptr;
    surface = nullptr;
    drawn = false;
}

MenuPass::~MenuPass() {
    editor_.release();
    graphics_.release();
    if (progressSoftware_) SDL_DestroyRenderer(progressSoftware_);
    if (progressSurface_) SDL_DestroySurface(progressSurface_);
    if (software_) SDL_DestroyRenderer(software_);
    if (surface_) SDL_DestroySurface(surface_);
}

bool MenuPass::makePicture(engine::Device& device, Picture& picture, int width, int height) {
    if (picture.texture && picture.width == width && picture.height == height) return true;
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    // Blue, green, red, alpha in memory: the surfaces' layout, byte for byte.
    info.format = kTextureFormat;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = std::uint32_t(std::max(1, width));
    info.height = std::uint32_t(std::max(1, height));
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    engine::Texture made = device.makeTexture(info);
    if (!made) return false;
    std::vector<SDL_GPUTextureSamplerBinding> binding{{made.get(), sampler_.get()}};
    if (picture.bindings == engine::kNoBindings) picture.bindings = owner_->take(std::move(binding));
    else owner_->replace(picture.bindings, std::move(binding));
    picture.texture = std::move(made);
    picture.width = width;
    picture.height = height;
    picture.ready = false;
    return true;
}

bool MenuPass::upload(engine::Device& device, Picture& picture, const SDL_Surface* from) {
    if (!from || !picture.texture) return false;
    const int wide = std::min(picture.width, from->w), high = std::min(picture.height, from->h);
    engine::Device::Uploader uploader(device);
    bool copied = false;
    // A surface's rows may be padded; the card wants them packed.
    if (from->pitch == picture.width * 4 && wide == picture.width) {
        copied = uploader.refillRegion(picture.texture.get(), from->pixels, 0, 0, std::uint32_t(wide),
                                       std::uint32_t(high), 4, 0, 0, true);
    } else {
        std::vector<std::uint8_t> packed(std::size_t(wide) * high * 4);
        for (int y = 0; y < high; ++y)
            std::memcpy(packed.data() + std::size_t(y) * wide * 4,
                        static_cast<const std::uint8_t*>(from->pixels) + std::size_t(y) * from->pitch,
                        std::size_t(wide) * 4);
        copied = uploader.refillRegion(picture.texture.get(), packed.data(), 0, 0, std::uint32_t(wide),
                                       std::uint32_t(high), 4, 0, 0, true);
    }
    picture.ready = uploader.finish() && copied;
    return picture.ready;
}

void MenuPass::push(engine::DrawQueue& queue, const Picture& picture, float x, float y, float w, float h) const {
    if (!picture.ready) return;
    engine::DrawItem item;
    item.pipeline = pipeline_;
    item.bindings = picture.bindings;
    item.vertexCount = 6;
    item.ownToVertex = true;
    item.own[0] = x;
    item.own[1] = y;
    item.own[2] = w;
    item.own[3] = h;
    queue.push(item);
}

bool MenuPass::makeWindow(engine::Device& device, Window& window, int wide, int high) {
    const auto& shown = menu_.presentation();
    const float scale = std::clamp(shown.panelScale, 0.25f, 8.0f);
    const void* font = shown.font.get();
    if (window.ui && window.scale == scale && window.font == font && window.high == high) return true;
    window.release();
    // As many pixels as it covers on the screen, and the widgets told how many
    // pixels a window pixel is: sharp at any scale, not a stretched picture.
    const int w = std::max(1, int(std::lround(wide * scale))), h = std::max(1, int(std::lround(high * scale)));
    window.surface = SDL_CreateSurface(w, h, kSurfaceFormat);
    if (!window.surface) { device.fail(SDL_GetError()); return false; }
    window.software = SDL_CreateSoftwareRenderer(window.surface);
    if (!window.software) { device.fail(SDL_GetError()); return false; }
    SDL_SetRenderScale(window.software, scale, scale);
    window.ui = std::make_unique<ui::Ui>();
    if (!window.ui->init(window.software)) { device.fail("window font"); return false; }
    if (shown.font) window.ui->setFont(shown.font, shown.bold, shown.fontPoints, scale);
    window.scale = scale;
    window.high = high;
    window.font = font;
    return makePicture(device, window.picture, w, h);
}

engine::PassPlace MenuPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    owner_ = &into;
    engine::PipelineWanted wanted;
    wanted.shaderFile = "menu.hlsl";
    wanted.vertexEntry = "MenuVS";
    wanted.fragmentEntry = "MenuPS";
    wanted.blend = true;
    wanted.depthTest = false;
    wanted.depthWrite = false;
    engine::GraphicsPipeline pipeline = device.makePipeline(wanted);
    if (!pipeline) return {};

    // Nearest and clamped: every picture is drawn at the pixels it was drawn
    // for, and a font smoothed between its own pixels is a font nobody can read.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};
    pipeline_ = into.take(std::move(pipeline));

    surface_ = SDL_CreateSurface(client::ExploreMenu::kWide, client::ExploreMenu::kHigh, kSurfaceFormat);
    if (!surface_) { device.fail(SDL_GetError()); return {}; }
    software_ = SDL_CreateSoftwareRenderer(surface_);
    if (!software_) { device.fail(SDL_GetError()); return {}; }
    if (!makePicture(device, strip_, client::ExploreMenu::kWide, client::ExploreMenu::kHigh)) return {};
#if ASR_ENABLE_DIAGNOSTICS
    if (terrain_) {
        progressSurface_ = SDL_CreateSurface(kProgressWide, kProgressHigh, kSurfaceFormat);
        if (!progressSurface_) { device.fail(SDL_GetError()); return {}; }
        progressSoftware_ = SDL_CreateSoftwareRenderer(progressSurface_);
        if (!progressSoftware_) { device.fail(SDL_GetError()); return {}; }
        if (!makePicture(device, progress_, kProgressWide, kProgressHigh)) return {};
    }
#endif
    if (!makeWindow(device, graphics_, client::GraphicsPanel::kWide, client::GraphicsPanel::kHigh)) return {};
    if (!makeWindow(device, editor_, client::WorldEditor::kWide, client::WorldEditor::kHigh)) return {};
    // The interface's own stage, after the grade: text is not tinted.
    return {engine::passOf(Pass::Menu), engine::stageOf(Stage::Interface),
            static_cast<engine::PassOrder>(Order::Interface)};
}

bool MenuPass::anything(const engine::Frame& frame) const {
    (void)frame;
    return true; // the compact view/stage/display readout is always present
}

void MenuPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    const auto& shown = menu_.presentation();
    collectCanvas(frame, queue);
    if (shown.developer) {
#if ASR_ENABLE_DIAGNOSTICS
        if (terrain_) collectProgress(frame, queue);
#endif
        if (menu_.dirty() || !strip_.ready) {
            menu_.draw(software_, client::ExploreMenu::kWide, client::ExploreMenu::kHigh);
            SDL_RenderPresent(software_);
            upload(*frame.device, strip_, surface_);
        }
        // A fixed margin from the top left, at one pixel to one pixel, so the
        // letters are the size they were drawn.
        push(queue, strip_, 16, 16, client::ExploreMenu::kWide, client::ExploreMenu::kHigh);
    }
    collectPanel(frame, queue);
    collectEditor(frame, queue);
}

void MenuPass::collectCanvas(const engine::Frame& frame, engine::DrawQueue& queue) {
    const ui::Canvas* canvas = menu_.presentation().canvas;
    if (!canvas || !*canvas) return;
    constexpr int kTile = ui::Canvas::kTile;
    const int columns = canvas->columns(), rows = canvas->rows();
    if (canvas->width() != canvasWide_ || canvas->height() != canvasHigh_) {
        // A new size: every tile a new texture (the ones already made keep
        // their binding slots) and all of it uploaded again.
        canvasTiles_.resize(std::max(canvasTiles_.size(), std::size_t(columns) * std::size_t(rows)));
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                auto& tile = canvasTiles_[std::size_t(r) * std::size_t(columns) + std::size_t(c)];
                const int w = std::min(kTile, canvas->width() - c * kTile), h = std::min(kTile, canvas->height() - r * kTile);
                if (!makePicture(*frame.device, tile.picture, w, h)) return;
                tile.shown = 0;
                tile.empty = true;
            }
        canvasWide_ = canvas->width();
        canvasHigh_ = canvas->height();
        canvasShown_ = 0;
    }
    if (canvas->revision() != canvasShown_) {
        // Only the tiles whose revision moved, each whole: a whole texture can
        // be cycled, so the frame still drawing the old one keeps it.
        const SDL_Surface* from = canvas->surface();
        engine::Device::Uploader uploader(*frame.device);
        bool ok = true;
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                auto& tile = canvasTiles_[std::size_t(r) * std::size_t(columns) + std::size_t(c)];
                const std::uint64_t revision = canvas->tileRevision(c, r);
                if (tile.shown == revision) continue;
                const int w = tile.picture.width, h = tile.picture.height;
                packed_.resize(std::size_t(w) * std::size_t(h) * 4);
                bool any = false;
                for (int y = 0; y < h; ++y) {
                    const auto* row = static_cast<const std::uint8_t*>(from->pixels) +
                                      std::size_t(r * kTile + y) * std::size_t(from->pitch) + std::size_t(c * kTile) * 4;
                    std::uint8_t* to = packed_.data() + std::size_t(y) * std::size_t(w) * 4;
                    std::memcpy(to, row, std::size_t(w) * 4);
                    if (!any)
                        for (int x = 0; x < w; ++x)
                            if (to[x * 4 + 3] != 0) { any = true; break; }
                }
                tile.empty = !any;
                if (tile.empty) { tile.shown = revision; continue; }   // nothing to see: not drawn, so not sent
                const bool sent = uploader.refillRegion(tile.picture.texture.get(), packed_.data(), 0, 0,
                                                        std::uint32_t(w), std::uint32_t(h), 4, 0, 0, true);
                tile.picture.ready = sent;
                if (sent) tile.shown = revision;
                ok = sent && ok;
            }
        ok = uploader.finish() && ok;
        if (ok) canvasShown_ = canvas->revision();
    }
    // Canvas pixels to screen pixels: the same, but for a frame caught between
    // a window resize and the canvas following it.
    const float sx = frame.scene.viewport[0] / float(std::max(1, canvasWide_));
    const float sy = frame.scene.viewport[1] / float(std::max(1, canvasHigh_));
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const auto& tile = canvasTiles_[std::size_t(r) * std::size_t(columns) + std::size_t(c)];
            if (tile.empty) continue;
            push(queue, tile.picture, float(c * kTile) * sx, float(r * kTile) * sy, float(tile.picture.width) * sx,
                 float(tile.picture.height) * sy);
        }
}

void MenuPass::collectEditor(const engine::Frame& frame, engine::DrawQueue& queue) {
    auto& editor = menu_.editor();
    if (!editor.windowShown()) return;
    const auto& wanted = menu_.presentation();
    const int editorHigh = wanted.editorHigh > 0
            ? std::clamp(int(wanted.editorHigh), 200, client::WorldEditor::kHigh) : client::WorldEditor::kHigh;
    if (!makeWindow(*frame.device, editor_, client::WorldEditor::kWide, editorHigh)) return;
    auto& input = editor.panelInput();
    // The pointer spends most of Edit mode over the map, painting; the panel
    // only has to be drawn again when the pointer is over it (or just left).
    const auto over = [&](const ui::Input& in) {
        return in.mouseX >= 0 && in.mouseY >= 0 && in.mouseX < client::WorldEditor::kWide && in.mouseY < editorHigh;
    };
    const bool moved = (over(input) || over(editor_.last)) &&
                       (input.mouseX != editor_.last.mouseX || input.mouseY != editor_.last.mouseY ||
                        input.down != editor_.last.down || input.pressed || input.released ||
                        input.wheel != 0);
    if (!editor_.drawn || editor.panelDirty() || moved) {
        SDL_SetRenderDrawBlendMode(editor_.software, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(editor_.software, 0, 0, 0, 0);
        SDL_RenderClear(editor_.software);
        SDL_SetRenderDrawBlendMode(editor_.software, SDL_BLENDMODE_BLEND);
        editor_.ui->begin(input, client::WorldEditor::kWide, editorHigh);
        editor.draw(*editor_.ui);
        editor_.ui->end();
        SDL_RenderPresent(editor_.software);
        editor_.drawn = upload(*frame.device, editor_.picture, editor_.surface);
        editor_.last = input;
        editor.panelPainted();
    }
    const auto& shown = menu_.presentation();
    const float x = shown.editorX >= 0 ? shown.editorX : client::WorldEditor::kPanelX;
    const float y = shown.editorY >= 0 ? shown.editorY : client::WorldEditor::kPanelY;
    push(queue, editor_.picture, x, y, float(editor_.picture.width), float(editor_.picture.height));
}

void MenuPass::collectPanel(const engine::Frame& frame, engine::DrawQueue& queue) {
    if (!menu_.panelVisible()) return;
    const auto& wanted = menu_.presentation();
    const int graphicsHigh = wanted.graphicsHigh > 0
            ? std::clamp(int(wanted.graphicsHigh), 200, client::GraphicsPanel::kHigh) : client::GraphicsPanel::kHigh;
    if (!makeWindow(*frame.device, graphics_, client::GraphicsPanel::kWide, graphicsHigh)) return;
    auto& input = menu_.panelInput();
    const bool moved = input.mouseX != graphics_.last.mouseX || input.mouseY != graphics_.last.mouseY ||
                       input.down != graphics_.last.down || input.pressed || input.released ||
                       input.wheel != 0;
    if (!graphics_.drawn || menu_.panelDirty() || moved) {
        SDL_SetRenderDrawBlendMode(graphics_.software, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(graphics_.software, 0, 0, 0, 0);
        SDL_RenderClear(graphics_.software);
        SDL_SetRenderDrawBlendMode(graphics_.software, SDL_BLENDMODE_BLEND);
        graphics_.ui->begin(input, client::GraphicsPanel::kWide, graphicsHigh);
        const bool changed = menu_.panel().draw(*graphics_.ui, menu_.graphics());
        graphics_.ui->end();
        SDL_RenderPresent(graphics_.software);
        graphics_.drawn = upload(*frame.device, graphics_.picture, graphics_.surface);
        graphics_.last = input;
        if (changed) menu_.panelChanged(); else menu_.panelPainted();
    }
    const auto& shown = menu_.presentation();
    const float w = float(graphics_.picture.width), h = float(graphics_.picture.height);
    const float x = shown.graphicsX >= 0 ? shown.graphicsX : std::max(0.0f, frame.scene.viewport[0] - w - 16);
    const float y = shown.graphicsY >= 0 ? shown.graphicsY : 16;
    push(queue, graphics_.picture, x, y, w, h);
}

#if ASR_ENABLE_DIAGNOSTICS
void MenuPass::collectProgress(const engine::Frame& frame, engine::DrawQueue& queue) {
    // Always visible, but repaint at a bounded rate. No cursor raycast, bake,
    // filesystem access or worker wait belongs on this UI path.
    const auto& p = terrain_->progress();
    if (!progress_.ready || frame.index % 6 == 0 || p.viewMesh.percent() != shownViewPercent_) {
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
        if (upload(*frame.device, progress_, progressSurface_)) shownViewPercent_ = p.viewMesh.percent();
    }
    const float width = frame.scene.viewport[0], height = frame.scene.viewport[1];
    const float scale = std::min({1.0f, std::max(1.0f, width - 32) / kProgressWide,
                                 std::max(1.0f, height - 32) / kProgressHigh});
    const float w = kProgressWide * scale, h = kProgressHigh * scale;
    const float x = std::max(0.0f, width - w - 16);
    const float y = menu_.visible() && width < kProgressWide + client::ExploreMenu::kWide + 48
        ? std::max(0.0f, height - h - 16) : 16;
    push(queue, progress_, x, y, w, h);
}
#endif

} // namespace game

