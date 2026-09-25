#include "game/world/edit_layer.hpp"

#include <cmath>

namespace world {
namespace {
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    const auto q = a / b;
    return a % b < 0 ? q - 1 : q;
}
std::int64_t floorMod(std::int64_t a, std::int64_t b) {
    const auto r = a % b;
    return r < 0 ? r + b : r;
}
}

core::Fixed EditLayer::at(core::Fixed x, core::Fixed y) const {
    // The empty check without the lock, and deliberately.
    //
    // This is asked for every height in the world. An unedited world must pay
    // one predictable branch here and nothing else - not a mutex, not a hash.
    // `blocks_` only ever grows, and a world that has just gained its first
    // block reading empty for one query is a sample drawn a frame early, which
    // the revision below corrects anyway.
    if (blocks_.empty()) return core::kZero;

    // Bilinear between the four samples around the point, so a brush leaves a
    // surface rather than a staircase of four-metre steps.
    const double px = x.toDouble() / kSampleMetres, py = y.toDouble() / kSampleMetres;
    const auto sx = std::int64_t(std::floor(px)), sy = std::int64_t(std::floor(py));
    const auto fx = core::Fixed::fromDoubleForContent(px - double(sx));
    const auto fy = core::Fixed::fromDoubleForContent(py - double(sy));

    std::shared_lock lock(mutex_);
    const auto sample = [&](std::int64_t ix, std::int64_t iy) {
        const auto block = blocks_.find(keyOf(floorDiv(ix, kBlockSamples),
                                              floorDiv(iy, kBlockSamples)));
        if (block == blocks_.end()) return core::kZero;
        const auto i = floorMod(iy, kBlockSamples) * kBlockSamples + floorMod(ix, kBlockSamples);
        return block->second->delta[std::size_t(i)];
    };
    const auto a = sample(sx, sy), b = sample(sx + 1, sy);
    const auto c = sample(sx, sy + 1), d = sample(sx + 1, sy + 1);
    const auto top = a + (b - a) * fx;
    const auto bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

core::Fixed EditLayer::add(std::int64_t sampleX, std::int64_t sampleY, core::Fixed metres) {
    std::unique_lock lock(mutex_);
    const auto key = keyOf(floorDiv(sampleX, kBlockSamples), floorDiv(sampleY, kBlockSamples));
    auto& block = blocks_[key];
    if (!block) block = std::make_unique<Block>();
    const auto i = floorMod(sampleY, kBlockSamples) * kBlockSamples +
                   floorMod(sampleX, kBlockSamples);
    block->delta[std::size_t(i)] += metres;
    ++revision_;
    return block->delta[std::size_t(i)];
}

bool EditLayer::touches(core::WorldRect area) const {
    if (blocks_.empty()) return false;
    std::shared_lock lock(mutex_);
    const auto lowX = floorDiv(area.min.x.toInt(), kBlockMetres);
    const auto lowY = floorDiv(area.min.y.toInt(), kBlockMetres);
    const auto highX = floorDiv(area.max.x.toInt(), kBlockMetres);
    const auto highY = floorDiv(area.max.y.toInt(), kBlockMetres);
    for (auto by = lowY; by <= highY; ++by)
        for (auto bx = lowX; bx <= highX; ++bx)
            if (blocks_.count(keyOf(bx, by)) != 0) return true;
    return false;
}

void EditLayer::clear() {
    std::unique_lock lock(mutex_);
    blocks_.clear();
    ++revision_;
}

} // namespace world
