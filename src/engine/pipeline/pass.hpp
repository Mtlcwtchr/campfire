#pragma once
// A pass: one thing that happens in a frame.
//
// There are two kinds and they are different enough to be separate types.
//
// A DrawPass puts pictures on the screen. It does not call the graphics API. At
// setup it hands its graphics pipeline and its texture bindings to the render
// pipeline and gets back two small integers; thereafter all it does is push
// draw items into a queue. It never binds anything, never opens a render pass,
// never knows what else is being drawn or in what order. That is what lets the
// batcher reorder and merge its work with everybody else's - a pass that issued
// its own binds could not be reordered without changing what it drew.
//
// A CalcPass works out what the frame is going to contain: streaming, culling,
// building instances, uploading geometry. It runs before anything is drawn,
// which is the whole reason it is a different kind - a copy in the middle of a
// render pass is a frame waiting on itself.
//
// Neither kind fetches its own data out of the world. Both are handed what they
// need at construction. That is the seam that lets the ground come from a patch
// cache today and an ECS query tomorrow with no pass changing a line.

#include "engine/pipeline/ids.hpp"

namespace engine {

class Device;
struct Frame;
class DrawQueue;
class RenderPipeline;

// Where a pass wants to be, declared once. The pipeline reads this and files
// the pass; the pass does not file itself, because then two passes could
// disagree about the order and there would be nowhere to notice.
struct PassPlace {
    PassId id = kNoPass;
    StageId stage = 0;
    PassOrder order = 0;
};

class DrawPass {
public:
    virtual ~DrawPass() = default;

    // Build everything that can fail - shaders, textures, samplers - and say
    // where this pass belongs. Called once. Anything registered with `into`
    // (graphics pipelines, binding sets) belongs to the render pipeline
    // afterwards; the pass keeps only the slots.
    virtual PassPlace setup(Device& device, RenderPipeline& into) = 0;

    // Is there anything at all this frame? Answering false costs nothing;
    // answering true and then pushing no items costs a little more.
    virtual bool anything(const Frame& frame) const { (void)frame; return true; }

    // Push draw items. No graphics calls of any kind belong in here.
    virtual void collect(const Frame& frame, DrawQueue& queue) = 0;

    PassPlace place;
    bool enabled = true;
};

class CalcPass {
public:
    virtual ~CalcPass() = default;

    // Called once; may fail.
    virtual bool setup(Device& device) { (void)device; return true; }
    virtual PassId id() const = 0;

    // One frame's worth of working out. May upload through frame.commands.
    virtual void run(Frame& frame) = 0;

    bool enabled = true;
};

} // namespace engine
