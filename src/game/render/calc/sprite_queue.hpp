#pragma once
// Everything the game wants drawn as a card this frame.
//
// The seam for sprites, and the same shape as the seam for ground: on one side
// the game says "this pawn, here, this big, wearing this"; on the other there
// are flat arrays the engine uploads and draws without knowing what any of it
// means. Whoever draws the game fills this; the pass reads it and nothing else.
//
// Sorted into batches rather than kept in the order it arrived. A batch is one
// page of sprites drawn one way, and the whole point of the exercise is that a
// batch is *one* draw call however many cards are in it - so the queue's job is
// to put the cards that can share a call next to each other.

#include <cstdint>
#include <vector>

#include "game/render/sprite_data.hpp"

namespace game {

class SpriteQueue {
public:
    // Start a frame. Keeps the memory: a frame that drew ten thousand cards
    // will draw about ten thousand next frame too.
    void begin();

    void add(const SpriteInstanceGpu& sprite, SpriteBlend blend = SpriteBlend::Cutout);

    // Groups what was added into runs that can share a draw call. Cut-out cards
    // are grouped by page in any order, because none of them needs sorting
    // against another; soft ones keep the order they were added in, because for
    // them that order is the picture.
    void sort();

    struct Batch {
        std::uint32_t first = 0;    // into instances()
        std::uint32_t count = 0;
        float page = 0;
        SpriteBlend blend = SpriteBlend::Cutout;
    };
    const std::vector<SpriteInstanceGpu>& instances() const { return sorted_; }
    const std::vector<Batch>& batches() const { return batches_; }
    std::size_t size() const { return added_.size(); }

private:
    struct Entry {
        SpriteInstanceGpu sprite;
        SpriteBlend blend;
        std::uint32_t order;
    };
    std::vector<Entry> added_;
    std::vector<SpriteInstanceGpu> sorted_;
    std::vector<Batch> batches_;
};

} // namespace game
