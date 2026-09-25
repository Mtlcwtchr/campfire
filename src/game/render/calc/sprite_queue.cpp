#include "game/render/calc/sprite_queue.hpp"

#include <algorithm>

namespace game {

void SpriteQueue::begin() {
    added_.clear();
    sorted_.clear();
    batches_.clear();
}

void SpriteQueue::add(const SpriteInstanceGpu& sprite, SpriteBlend blend) {
    added_.push_back({sprite, blend, static_cast<std::uint32_t>(added_.size())});
}

void SpriteQueue::sort() {
    sorted_.clear();
    batches_.clear();
    if (added_.empty()) return;

    // Cut-out first, then soft: the soft ones are drawn over a depth buffer the
    // cut-out ones have already filled in, which is what lets a selection ring
    // be hidden by a wall in front of it.
    //
    // Within the cut-out run, by page, and stably - two cards on the same page
    // came from the same caller in the order it wanted, and although nothing
    // about a cut-out card depends on that order, a stable sort makes the frame
    // reproducible, which is what lets two screenshots be compared.
    std::stable_sort(added_.begin(), added_.end(), [](const Entry& a, const Entry& b) {
        if (a.blend != b.blend) return a.blend == SpriteBlend::Cutout;
        if (a.blend == SpriteBlend::Soft) return a.order < b.order;
        return a.sprite.page < b.sprite.page;
    });

    sorted_.reserve(added_.size());
    for (const Entry& entry : added_) {
        // A batch is a run of cards that want the same page drawn the same way.
        // For the soft ones that means a run in the order they were added, which
        // is exactly what the sort above left behind.
        const bool same = !batches_.empty() && batches_.back().blend == entry.blend &&
                          batches_.back().page == entry.sprite.page;
        if (!same) {
            batches_.push_back({static_cast<std::uint32_t>(sorted_.size()), 0, entry.sprite.page,
                                entry.blend});
        }
        sorted_.push_back(entry.sprite);
        ++batches_.back().count;
    }
}

} // namespace game
