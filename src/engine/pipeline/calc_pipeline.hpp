#pragma once
// Work that is not drawing.
//
// Two quite different things use this, and what tells them apart is the cadence
// they are built with.
//
// A fixed one is the world: the tick, the systems, the whole of the simulation.
// It runs a whole number of steps per frame and is handed the same step every
// time, because a tick whose length depended on how long the last frame took
// would give a different world on a fast machine than on a slow one.
//
// An every-frame one is everything that looks at the world without deciding it:
// input, the camera, streaming ground in and out of memory, turning what is
// there into vertices and putting them on the card. All of it has to be finished
// before drawing starts, which is why it is its own phase rather than being
// smuggled into the top of a draw call.
//
// Passes here run in the order they were added. Unlike drawing, where the
// batcher is free to reorder, this is a dependency chain: nothing collects
// geometry the streamer has not delivered, and no system reads a world the
// system before it has not finished writing.

#include <memory>
#include <vector>

#include "engine/pipeline/ids.hpp"
#include "engine/pipeline/pass.hpp"
#include "engine/pipeline/pipeline.hpp"

namespace engine {

class CalcPipeline : public Pipeline {
public:
    CalcPipeline(PipelineId id, Cadence cadence) : id_(id), cadence_(cadence) {}

    PipelineId id() const override { return id_; }
    Cadence cadence() const override { return cadence_; }

    template <class T>
    T* add(std::unique_ptr<T> pass) {
        T* raw = pass.get();
        passes_.push_back(std::move(pass));
        return raw;
    }

    bool build(Device& device) override;
    bool run(Frame& frame) override;

    const std::vector<std::pair<PassId, double>>& passMillis() const { return passMillis_; }

private:
    PipelineId id_;
    Cadence cadence_;
    std::vector<std::unique_ptr<CalcPass>> passes_;
    std::vector<std::pair<PassId, double>> passMillis_;
};

} // namespace engine
