#pragma once
// Keeping the ground in memory around the view.
//
// The explorer holds the patches and the worker threads that cut them; this is
// only the pass that tells it where the camera is once a frame. It is a
// calculation rather than a draw because everything it does has to be finished
// before anything is collected, let alone drawn.

#include "engine/pipeline/pass.hpp"
#include "game/client/camera.hpp"

namespace client { class Explorer; }

namespace game {

class TerrainStreamPass : public engine::CalcPass {
public:
    TerrainStreamPass(client::Explorer& explorer, const client::Camera& camera)
        : explorer_(explorer), camera_(camera) {}

    engine::PassId id() const override;
    void run(engine::Frame& frame) override;

private:
    client::Explorer& explorer_;
    const client::Camera& camera_;
};

} // namespace game
