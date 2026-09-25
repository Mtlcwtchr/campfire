#pragma once
// Who a pass is, which stage it belongs to, and which pipeline runs it.
//
// All three are opaque integers as far as the engine is concerned. It sorts by
// them, looks things up by them and profiles by them, and never asks what they
// mean - the game declares its own enumerations over these types and is the
// only thing that knows a terrain pass from a water pass.
//
// They used to be strings. A string is a comparison against memory where an
// integer would do, it puts a pass name in the sort key of every draw call, and
// worst of all it invites looking a pass up by spelling it out somewhere else -
// at which point a typo is a pass that silently is not there. An integer that
// came from an enumeration cannot be misspelled.

#include <cstdint>

namespace engine {

// Which pass. The game numbers its own; the engine only ever compares.
using PassId = std::uint16_t;
inline constexpr PassId kNoPass = 0xffff;

// Which render pass on the card - one set of targets and one set of clears.
// Everything drawn in a stage is drawn between one BeginRenderPass and its End,
// which is where a tiled GPU decides to flush.
using StageId = std::uint8_t;

// Where a pass sits inside its stage. Two passes with the same order may be
// interleaved by the batcher to save state changes; a pass with a higher order
// is drawn strictly after one with a lower, which is what transparency needs.
using PassOrder = std::uint8_t;

// A graphics pipeline and a set of bindings, as the render pipeline numbers
// them. Passes hand theirs over at setup and keep the slot, so a draw call
// carries two small integers rather than two pointers, and the sort key stays
// one word.
using PipelineSlot = std::uint16_t;
// A compute pipeline, numbered the same way and for the same reason.
using ComputeSlot = std::uint16_t;
using BindingSet = std::uint16_t;
inline constexpr BindingSet kNoBindings = 0xffff;

// Which pipeline of the run this is - the calculations, the drawing, whatever
// comes after. The runner keeps them in the order they were added.
using PipelineId = std::uint16_t;

// Turns a game's enumeration into the engine's integer without a cast at every
// call site.
template <class E>
constexpr PassId passOf(E value) {
    return static_cast<PassId>(value);
}
template <class E>
constexpr StageId stageOf(E value) {
    return static_cast<StageId>(value);
}

} // namespace engine
