#pragma once
// Everything that is going to be drawn this frame, and the order to draw it in.
//
// Passes push items; the queue sorts them; the render pipeline issues them and
// re-binds only where two neighbouring items actually differ. That is the whole
// of the batching, and it is worth stating why it is done here rather than in
// each pass.
//
// A pass drawing its own work binds its pipeline once and then issues its draws.
// Six passes is six pipeline binds and six sets of texture binds, which sounds
// cheap and is - until the passes have to be interleaved. Water must be drawn
// after ground, but ground and grass need not be, and grass on one patch and
// grass on the next want the same bindings; sorting the whole frame at once
// finds those runs wherever they come from, and no pass has to know that any
// other pass exists for it to happen.
//
// The sort key is one word so the sort is one pass over an array of a few
// thousand words, which is nothing next to what it saves.

#include <SDL3/SDL.h>

#include <cstdint>
#include <array>
#include <optional>
#include <vector>

#include "engine/pipeline/ids.hpp"

namespace engine {

// One draw call, as a pass asks for it.
//
// The buffers are borrowed: they belong to whatever cache built the geometry and
// have to outlive the frame, which they do - geometry is uploaded before any of
// this runs and released frames later.
struct DrawItem {
    // Who queued this, for counting only. The frame's work tally is useless if
    // it cannot say which pass spent it: "eighty thousand triangles" is a
    // number, "eighty thousand of which the ground is a third" is a finding.
    std::uint8_t author = 0;
    PipelineSlot pipeline = 0;
    BindingSet bindings = kNoBindings;
    // What the vertex stage samples, if anything.
    //
    // A separate set from the fragment one because it is a separate binding
    // point on the card and because the two are wanted at different rates: a
    // pass may bind its materials once for the frame and its per-tile ground
    // for every draw. Most passes leave this alone - a vertex shader that
    // reads no texture is the ordinary case - but the ground is not one of
    // them any more. Its height, its water and its weather are properties of
    // a place at a scale rather than of a vertex, and a vertex that carries
    // them is a copy of a texture, one per corner.
    BindingSet vertexBindings = kNoBindings;
    SDL_GPUBuffer* vertex[2]{nullptr, nullptr};
    // Where in each stream this draw's data begins, in bytes.
    //
    // What lets one buffer hold every instance in the frame. Without it a batch
    // is a buffer, so a thousand batches are a thousand buffers created and
    // destroyed every frame; with it they are a thousand offsets into one, and
    // the only thing that changes between two draws is a number.
    std::uint32_t vertexOffset[2]{0, 0};
    // The second stream comes out of the frame's instance arena, and which
    // buffer that is, is not known when a pass collects: the arena is still
    // being filled, and the buffer it ends up in may be a new one if this frame
    // wants more room than the last did. So the pass says "the arena" and the
    // pipeline fills in the pointer on the way to the card.
    bool instancesFromArena = false;
    std::uint8_t vertexStreams = 0;
    SDL_GPUBuffer* index = nullptr;
    SDL_GPUIndexElementSize indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
    std::uint32_t indexCount = 0;
    // Where in the index buffer this draw starts. What lets one mesh hold
    // several levels of detail: the vertices are shared, the levels are ranges,
    // and choosing a coarser one changes two numbers rather than a binding.
    std::uint32_t firstIndex = 0;
    // For a draw with no index buffer at all: how many vertices to run. What a
    // full-screen pass wants - it has no geometry, it makes its corners out of
    // the vertex id - and there is no sense in keeping a three-vertex buffer on
    // the card to say so.
    std::uint32_t vertexCount = 0;
    std::uint32_t instances = 1;
    // Where the card reads the draw's own arguments, when the CPU does not know
    // them. The whole point of a GPU-driven path: a compute pass decides how
    // many instances survived and writes it here, and this draw is recorded
    // before that number exists. `instances` and `indexCount` are ignored when
    // this is set, because the card is about to read better ones.
    SDL_GPUBuffer* indirect = nullptr;
    // Or the frame's argument arena, for the same reason the instance stream
    // has a flag rather than a pointer: the arena may still grow into a new
    // buffer after this item is recorded.
    bool indirectFromArena = false;
    std::uint32_t indirectOffset = 0;
    // How many draw argument structures to read from that offset. More than one
    // is how several buckets - a model at each of its levels, say - become one
    // recorded command instead of one per bucket.
    std::uint32_t indirectDraws = 1;
    // CPU-authored arguments have an exact count even when issued indirectly.
    // Leave unset if a compute pass will change the counts before drawing.
    std::optional<std::uint64_t> indirectTriangles;
    [[nodiscard]] std::optional<std::uint64_t> triangleCount() const {
        if (indirect != nullptr || indirectFromArena) return indirectTriangles;
        return std::uint64_t(index != nullptr ? indexCount : vertexCount) / 3 * instances;
    }

    // Eight floats the fragment shader gets for this draw and no other, at
    // slot 1. What a pass needs to say about one piece of geometry rather than
    // about the frame: where this patch is, what is covered, what colour this
    // thing is.
    //
    // Carried by value rather than by pointer because the queue outlives
    // whatever built the item and a pointer into a pass's scratch is a pointer
    // into last frame by the time it is issued.
    bool hasOwnData = false;
    // The same block, to the vertex stage, at its slot 1. Its own flag rather
    // than the one above because the two stages have separate sets and a push
    // to a slot the shader never declared is a push into nothing - so a pass
    // says which stage is actually going to read it.
    //
    // What wants it: how far a patch has morphed towards the next level up,
    // which is a property of one patch and of this frame, and which only the
    // vertex stage can act on.
    bool ownToVertex = false;
    float own[16]{};
    // Optional b2 material block, copied into the frame like object data.
    std::array<float, 16> materialData{};
    bool materialToVertex = false, materialToFragment = false;
};

class DrawQueue {
public:
    // Called by the pipeline before the passes run: everything pushed after
    // this belongs to the pass with this place, and its stage and order go into
    // the top of every key so the sort cannot move it across a boundary that
    // matters.
    void openFor(const struct PassPlace& place);

    void push(const DrawItem& item);

    // Sorts into issue order. Stage first, then the pass's declared order, then
    // pipeline, then bindings, then the geometry itself - so that within a run
    // of one pipeline and one binding set the items are also grouped by buffer,
    // which is the last thing a driver charges for.
    void sort();

    void clear();

    struct Sorted {
        std::uint64_t key;
        std::uint32_t item;
    };
    const std::vector<Sorted>& order() const { return order_; }
    const DrawItem& at(std::uint32_t index) const { return items_[index]; }
    std::size_t size() const { return items_.size(); }

private:
    std::vector<DrawItem> items_;
    std::vector<Sorted> order_;
    std::uint64_t prefix_ = 0;   // stage and order of the pass currently pushing
};

} // namespace engine
