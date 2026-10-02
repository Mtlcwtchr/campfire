#pragma once
// Pictures drawn on the processor, put on the screen by the card.
//
// The explorer needs a menu, and a menu needs letters. There is no font on this
// path and no pass to put one through: text on the card wants an atlas, a glyph
// layout and a pass of its own, and that work belongs with moving the game's
// whole interface over rather than in front of a menu.
//
// So the menu is drawn where every other interface in this project is drawn -
// into an SDL_Renderer - except that the renderer is a software one over a
// plain surface. The surface is then one texture and one quad, and the world
// behind it is still drawn on the card. It costs a few thousand pixels of
// software blitting on the frames where the menu actually changes, which is the
// frames somebody pressed a key.
//
// What it puts up, back to front: the game client's whole-window interface (a
// ui::Canvas it is handed, kept as tiles and uploaded tile by tile as their
// revisions move), the explorer's status strip and terrain progress (the
// developer's, hidden in the client), and the two windows - graphics settings
// and the world editor - each drawn with ui::Ui into a surface of its own, at
// the scale and in the face the explorer's presentation asks for. Every
// surface is in the canvas's layout (canvas.hpp), the one SDL blends fast.

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/ui/ui.hpp"

namespace client { class ExploreMenu; }
namespace engine { class RenderPipeline; }

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
    // A texture on the card, its binding, and how big it is.
    struct Picture {
        engine::Texture texture;
        engine::BindingSet bindings = engine::kNoBindings;
        int width = 0, height = 0;
        bool ready = false;
    };
    // A window: its surface, the software renderer over it, the widgets that
    // draw into it and the picture of it on the card.
    struct Window {
        SDL_Surface* surface = nullptr;
        SDL_Renderer* software = nullptr;
        std::unique_ptr<ui::Ui> ui;
        Picture picture;
        ui::Input last;
        float scale = 0;
        int high = 0;
        const void* font = nullptr;
        bool drawn = false;
        void release();
    };
    bool makePicture(engine::Device& device, Picture& picture, int width, int height);
    bool upload(engine::Device& device, Picture& picture, const SDL_Surface* from);
    void push(engine::DrawQueue& queue, const Picture& picture, float x, float y, float w, float h) const;
    bool makeWindow(engine::Device& device, Window& window, int wide, int high);

    void collectCanvas(const engine::Frame& frame, engine::DrawQueue& queue);
    void collectProgress(const engine::Frame& frame, engine::DrawQueue& queue);
    void collectPanel(const engine::Frame& frame, engine::DrawQueue& queue);
    void collectEditor(const engine::Frame& frame, engine::DrawQueue& queue);

    client::ExploreMenu& menu_;
    const GpuTerrain* terrain_ = nullptr;
    engine::RenderPipeline* owner_ = nullptr;
    engine::Sampler sampler_;
    engine::PipelineSlot pipeline_ = 0;

    // The status strip.
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* software_ = nullptr;
    Picture strip_;
    // The terrain progress readout (diagnostic builds).
    static constexpr int kProgressWide = 516, kProgressHigh = 380;
    SDL_Surface* progressSurface_ = nullptr;
    SDL_Renderer* progressSoftware_ = nullptr;
    Picture progress_;
    int shownViewPercent_ = -1;
    // The graphics settings window and the world editor.
    Window graphics_, editor_;
    // The game client's interface, as the canvas's tiles: each its own
    // texture, uploaded whole (and so safely cycled) when its revision moved,
    // and not drawn at all while nothing is on it - most of the window, most
    // of the time.
    struct CanvasTile {
        Picture picture;
        std::uint64_t shown = 0;
        bool empty = true;
    };
    std::vector<CanvasTile> canvasTiles_;
    int canvasWide_ = 0, canvasHigh_ = 0;
    std::uint64_t canvasShown_ = 0;
    std::vector<std::uint8_t> packed_;   // a tile's rows, packed for the card
};

} // namespace game

