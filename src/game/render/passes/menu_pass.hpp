#pragma once
// A picture drawn on the processor, put on the screen by the card.
//
// The explorer needs a menu, and a menu needs letters. There is no font on this
// path and no pass to put one through: text on the card wants an atlas, a glyph
// layout and a pass of its own, and that work belongs with moving the game's
// whole interface over rather than in front of a menu.
//
// So the menu is drawn where every other interface in this project is drawn -
// into an SDL_Renderer, with SDL's own debug font - except that the renderer is
// a software one over a plain surface. The surface is then one texture and one
// quad, and the world behind it is still drawn on the card. It costs a few
// thousand pixels of software blitting on the frames where the menu actually
// changes, which is the frames somebody pressed a key.

#include <SDL3/SDL.h>

#include <memory>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/ui/ui.hpp"

namespace client { class ExploreMenu; }

namespace game {

class GpuTerrain;

class MenuPass : public engine::DrawPass {
public:
    explicit MenuPass(client::ExploreMenu& menu, const GpuTerrain* terrain = nullptr)
        : menu_(menu), terrain_(terrain) {}
    ~MenuPass() override;

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

private:
    void collectProgress(const engine::Frame& frame, engine::DrawQueue& queue);
    void collectPanel(const engine::Frame& frame, engine::DrawQueue& queue);
    client::ExploreMenu& menu_;
    const GpuTerrain* terrain_ = nullptr;
    // The graphics settings window: its own surface and immediate-mode Ui.
    SDL_Surface* panelSurface_ = nullptr;
    SDL_Renderer* panelSoftware_ = nullptr;
    std::unique_ptr<ui::Ui> panelUi_;
    engine::Texture panelPicture_;
    engine::BindingSet panelBindings_ = engine::kNoBindings;
    ui::Input lastPanelInput_;
    bool panelDrawn_ = false;
    static constexpr int kProgressWide = 516, kProgressHigh = 380;
    SDL_Surface* progressSurface_ = nullptr;
    SDL_Renderer* progressSoftware_ = nullptr;
    engine::Texture progressPicture_;
    engine::BindingSet progressBindings_ = engine::kNoBindings;
    bool progressDrawn_ = false;
    int shownViewPercent_ = -1;
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* software_ = nullptr;
    engine::Texture picture_;
    engine::Sampler sampler_;
    engine::PipelineSlot pipeline_ = 0;
    engine::BindingSet bindings_ = engine::kNoBindings;
};

} // namespace game
