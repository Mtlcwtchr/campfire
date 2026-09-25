#pragma once
// Cards in the world: pawns, trees, barrels, markers, decals.
//
// One quad on the card, one entry per copy, one draw call per page. What the
// pass itself does is almost nothing - take the queue the game filled, put the
// instances in the frame's arena, and push a draw item per batch - and that is
// the point: everything that decides how a crowd is drawn is either in the
// queue (what shares a call) or in the shader (how a card stands), and neither
// of them is a loop over objects.
//
// Two pipelines out of one shader. Cut out and writing depth, for anything
// there might be a thousand of; blended and not writing depth, for the few
// things that need a soft edge. Which one a batch uses is the queue's business.

#include <string>
#include <vector>

#include "engine/pipeline/pass.hpp"
#include "engine/render/device.hpp"
#include "engine/render/geometry/instanced.hpp"
#include "engine/render/mesh_renderer.hpp"

namespace game {

class SpriteQueue;

class SpritePass : public engine::DrawPass {
public:
    // `pages` are sprite names under the assets directory, in the order the
    // instances index them.
    SpritePass(const SpriteQueue& queue, std::vector<std::string> pages);

    engine::PassPlace setup(engine::Device& device, engine::RenderPipeline& into) override;
    bool anything(const engine::Frame& frame) const override;
    void collect(const engine::Frame& frame, engine::DrawQueue& queue) override;

    // For the profiler: how many cards went out and in how many calls. The two
    // numbers that say whether a crowd is being batched or drawn one at a time.
    std::size_t cards() const { return cards_; }
    std::size_t calls() const { return calls_; }

private:
    const SpriteQueue& queue_;
    std::vector<std::string> pageNames_;
    engine::Texture pages_;
    engine::Sampler sampler_;
    // The mesh. A quad today; a loaded model the day something wants one, and
    // that is the only line of this pass that would change.
    engine::MeshRenderer cutout_, soft_;
    std::size_t cards_ = 0;
    std::size_t calls_ = 0;
};

} // namespace game
