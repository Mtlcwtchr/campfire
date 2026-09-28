#pragma once
// Drawing, centralised.
//
// This owns every graphics pipeline and every binding set in the frame, holds
// the stages, collects draw items from its passes, sorts the whole frame at once
// and issues it with the fewest state changes it can manage.
//
// A pass registers what it needs here at setup and afterwards only pushes items.
// That is not politeness - it is the reason the batching can work at all. If a
// pass held its own SDL pipeline and bound it itself, two passes that happen to
// want the same pipeline could never be merged, and the order of the frame would
// be the order the passes were added rather than the order that draws fastest.

#include <SDL3/SDL.h>

#include <memory>
#include <vector>

#include "engine/pipeline/ids.hpp"
#include "engine/pipeline/pass.hpp"
#include "engine/pipeline/pipeline.hpp"
#include "engine/render/device.hpp"
#include "engine/render/draw_queue.hpp"

namespace engine {

// One dispatch of a compute shader, as a pass asks for it.
//
// Compute runs before any render pass of the frame and after the frame's
// instance upload, because that is the only place it can: a compute pass may
// not be opened inside a render pass, and the work it exists to do - culling,
// compaction, writing the arguments of an indirect draw - reads what the frame
// just uploaded and writes what the frame is about to draw.
//
// Dispatches run in the order they were pushed, each in its own compute pass,
// so a dispatch sees everything the one before it wrote. That costs a barrier
// per dispatch and buys an order that can be reasoned about; a frame that
// wanted hundreds of them would want a different shape, and this one does not.
struct ComputeDispatch {
    ComputeSlot pipeline = 0;
    std::uint32_t groupsX = 1, groupsY = 1, groupsZ = 1;
    // What the shader reads, then what it writes, in the order the shader
    // declares them. Read and write are separate bindings on the card and a
    // buffer in the wrong list is a dispatch that silently does nothing.
    std::vector<SDL_GPUBuffer*> reads;
    std::vector<SDL_GPUBuffer*> writes;
    // Read-only/storage textures are separate from buffer bindings in SDL_GPU.
    // A texture write carries its mip because Hi-Z builds one level per
    // dispatch while all levels live in one allocation.
    std::vector<SDL_GPUTexture*> storageReads;
    std::vector<SDL_GPUTextureSamplerBinding> samplers;
    struct TextureWrite {
        SDL_GPUTexture* texture = nullptr;
        std::uint32_t mip = 0;
        std::uint32_t layer = 0;
    };
    std::vector<TextureWrite> storageWrites;
    // Resolve this read slot after InstanceArena::upload(), which may replace
    // the buffer. Capturing its pointer in collect() would read the old frame.
    int instanceReadSlot = -1;
    // What the shader gets at slot 0: the camera, the thresholds, the counts -
    // whatever this dispatch needs to know that is not in a buffer. Twenty-four
    // floats because the cull path carries the view rows plus its counts,
    // thresholds and optional Hi-Z row. A block sized to today's shader is a
    // block that has to be widened by the next one.
    float own[32]{};
};

// One render pass on the card: a set of targets and what to do with them when it
// opens. Declared by the game, indexed by StageId.
struct StageInfo {
    bool clearColour = false;
    SDL_FColor clear{0, 0, 0, 1};
    bool useDepth = true;
    bool clearDepth = true;
    // Copy the finished picture so far (resolved, one sample a pixel) into
    // `Frame::grab` before this stage begins, for passes that have to see
    // what is behind and around them: water refracting its bed and
    // reflecting its shores.
    bool grabColour = false;
};

// What one state change cost, for the overlay. An honest batcher should be able
// to say how much it saved rather than being taken on trust.
struct DrawStats {
    std::uint32_t items = 0;
    std::uint32_t pipelineBinds = 0;
    std::uint32_t bindingBinds = 0;
    std::uint32_t bufferBinds = 0;
    std::uint32_t stages = 0;
};

class RenderPipeline : public Pipeline {
public:
    explicit RenderPipeline(PipelineId id) : id_(id) {}

    PipelineId id() const override { return id_; }

    // The stages, in the order they run. Called before build().
    void stages(std::vector<StageInfo> stages) { stages_ = std::move(stages); }

    // Passes are added in any order; where they end up is what their setup()
    // says, not the order they were added in.
    template <class T>
    T* add(std::unique_ptr<T> pass) {
        T* raw = pass.get();
        passes_.push_back(std::move(pass));
        return raw;
    }

    // Called by passes from inside their setup(). The pipeline takes ownership
    // and hands back a slot; the pass keeps the slot and forgets the pointer.
    PipelineSlot take(GraphicsPipeline&& pipeline);
    ComputeSlot take(ComputePipeline&& pipeline);
    BindingSet take(std::vector<SDL_GPUTextureSamplerBinding> bindings);
    void replace(BindingSet slot, std::vector<SDL_GPUTextureSamplerBinding> bindings) {
        bindings_.at(slot) = std::move(bindings);
    }
    // The same, for what the vertex stage samples.
    BindingSet takeVertex(std::vector<SDL_GPUTextureSamplerBinding> bindings);
    // Pushed by a pass from inside collect(), run before anything is drawn.
    void dispatch(ComputeDispatch job) { dispatches_.push_back(std::move(job)); }
    void postDispatch(ComputeDispatch job) { postDispatches_.push_back(std::move(job)); }
    void replaceVertex(BindingSet slot, std::vector<SDL_GPUTextureSamplerBinding> bindings) {
        vertexBindings_.at(slot) = std::move(bindings);
    }

    bool build(Device& device) override;
    bool run(Frame& frame) override;
    // Records the dispatches pushed this frame. Called by run() between the
    // instance upload and the first render pass; public because the compute
    // half of a frame is worth exercising on its own, and a second private
    // copy of it in a test would be testing the copy.
    void runDispatches(Frame& frame);
    void runPostDispatches(Frame& frame);

    const DrawStats& stats() const { return stats_; }
    std::size_t dispatchesLastFrame() const { return lastDispatches_; }
    // What each pass cost this frame, by its own id.
    const std::vector<std::pair<PassId, double>>& passMillis() const { return passMillis_; }

private:
    void issue(Frame& frame, std::size_t from, std::size_t to);

    PipelineId id_;
    std::vector<std::unique_ptr<DrawPass>> passes_;
    std::vector<StageInfo> stages_;
    std::vector<GraphicsPipeline> graphics_;
    std::vector<ComputePipeline> computes_;
    std::vector<ComputeDispatch> dispatches_;
    std::vector<ComputeDispatch> postDispatches_;
    std::vector<std::vector<SDL_GPUTextureSamplerBinding>> bindings_;
    std::vector<std::vector<SDL_GPUTextureSamplerBinding>> vertexBindings_;
    DrawQueue queue_;
    DrawStats stats_;
    std::size_t lastDispatches_ = 0;
    std::vector<std::pair<PassId, double>> passMillis_;

    void runDispatchList(Frame& frame, std::vector<ComputeDispatch>& jobs);
};

} // namespace engine
