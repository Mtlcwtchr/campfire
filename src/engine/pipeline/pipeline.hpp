#pragma once
// A pipeline: one phase of a frame, made of passes of one kind.
//
// The calculations pipeline works out what the frame contains. The render
// pipeline draws it. A third could read the drawn picture back and filter it, a
// fourth could feed a simulation step. What they have in common is only this:
// they are built once, they run once a frame, and the runner decides when.
//
// A pipeline never acquires a command buffer, never presents and never decides
// what comes before or after it. That is deliberate and it is what "one
// authoritative runner" means: the order of a frame is written down in one
// place, not distributed across the things being ordered.

#include "engine/pipeline/ids.hpp"

namespace engine {

class Device;
struct Frame;

// How often a pipeline runs.
//
// The world runs on a fixed step and nothing else does. That is not a
// preference, it is what makes the simulation the same simulation on every
// machine: a tick that lasted as long as the last frame took would give a
// different answer on a fast machine than a slow one, and this game's whole
// determinism contract - fixed point, seeded streams, a fixed order of systems -
// would be worth nothing. So the world is stepped a whole number of times per
// frame and never a fraction of one.
//
// Everything else runs once a frame: input, the camera, streaming, drawing.
// Those look at the world, they do not decide it, and a frame that took longer
// should show more of what happened rather than less.
enum class Cadence {
    Fixed,        // as many whole steps as have accumulated, and no fraction
    EveryFrame,
};

class Pipeline {
public:
    virtual ~Pipeline() = default;

    // What the game calls this. The engine only compares and profiles by it.
    virtual PipelineId id() const = 0;

    virtual Cadence cadence() const { return Cadence::EveryFrame; }

    // Once. Everything that can fail should fail here.
    virtual bool build(Device& device) = 0;

    // One frame. The command buffer, the targets and the clock are already in
    // `frame`; a pipeline that needs anything else was given it at construction.
    virtual bool run(Frame& frame) = 0;

    double millis = 0;   // filled in by the runner
};

} // namespace engine
