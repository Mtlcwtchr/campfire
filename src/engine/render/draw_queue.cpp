#include "engine/render/draw_queue.hpp"

#include <algorithm>

#include "engine/pipeline/pass.hpp"

namespace engine {
namespace {

// The bottom of the key: what the driver charges for, most expensive first.
// Pipeline changes cost the most, then the texture set, then the buffers.
std::uint64_t geometryKey(const DrawItem& item) {
    const std::uint64_t buffer =
            reinterpret_cast<std::uintptr_t>(item.vertex[0]) >> 4;   // pointers are aligned
    return (static_cast<std::uint64_t>(item.pipeline) << 32) |
           (static_cast<std::uint64_t>(item.bindings) << 16) | (buffer & 0xffffu);
}

} // namespace

void DrawQueue::openFor(const PassPlace& place) {
    prefix_ = (static_cast<std::uint64_t>(place.stage) << 56) |
              (static_cast<std::uint64_t>(place.order) << 48);
}

void DrawQueue::push(const DrawItem& item) {
    order_.push_back({prefix_ | geometryKey(item), static_cast<std::uint32_t>(items_.size())});
    items_.push_back(item);
}

void DrawQueue::sort() {
    // Stable, because two items with the same key came from the same pass in the
    // order that pass wanted them, and for anything blended that order is the
    // picture.
    std::stable_sort(order_.begin(), order_.end(),
                     [](const Sorted& a, const Sorted& b) { return a.key < b.key; });
}

void DrawQueue::clear() {
    items_.clear();
    order_.clear();
    prefix_ = 0;
}

} // namespace engine
