#include "game/world/edit_layer.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>

#include "engine/core/rng.hpp"

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
// Whole metres around a rectangle, outward: the floor of its low corner and
// the ceiling of its high one.
std::int64_t lowMetres(core::Fixed v) { return v.toInt(); }
std::int64_t highMetres(core::Fixed v) { return -((-v).toInt()); }
core::WorldRect metreRect(std::int64_t minX, std::int64_t minY, std::int64_t maxX, std::int64_t maxY) {
    return {{core::Fixed::fromInt(minX), core::Fixed::fromInt(minY)},
            {core::Fixed::fromInt(maxX), core::Fixed::fromInt(maxY)}};
}
std::uint64_t keyOf(std::int64_t blockX, std::int64_t blockY) {
    return (std::uint64_t(std::uint32_t(blockX)) << 32) | std::uint32_t(blockY);
}
std::pair<std::int64_t, std::int64_t> blockOfKey(std::uint64_t key) {
    return {std::int64_t(std::int32_t(key >> 32)), std::int64_t(std::int32_t(key))};
}
}

template<class Visit>
void EditLayer::eachBlockLocked(const Blocks& blocks, std::int64_t minX, std::int64_t minY, std::int64_t maxX,
                                std::int64_t maxY, Visit&& visit) const {
    if (maxX < minX || maxY < minY || blocks.empty()) return;
    const auto span = std::uint64_t(maxX - minX + 1) * std::uint64_t(maxY - minY + 1);
    if (span <= blocks.size()) {
        for (auto by = minY; by <= maxY; ++by)
            for (auto bx = minX; bx <= maxX; ++bx)
                if (const auto it = blocks.find(keyOf(bx, by)); it != blocks.end()) visit(bx, by, *it->second);
    } else {
        for (const auto& [key, block] : blocks) {
            const auto [bx, by] = blockOfKey(key);
            if (bx >= minX && bx <= maxX && by >= minY && by <= maxY) visit(bx, by, *block);
        }
    }
}

core::Fixed EditLayer::levelAt(int level, core::Fixed x, core::Fixed y) const {
    if (empty() || level < 0 || level >= kLevels) return core::kZero;
    std::shared_lock lock(mutex_);
    return levelAtLocked(level, x, y);
}

core::Fixed EditLayer::levelAtLocked(int level, core::Fixed x, core::Fixed y) const {
    const Blocks& blocks = levels_[std::size_t(level)];
    if (blocks.empty()) return core::kZero;
    // Bilinear between the four samples around the point, so a brush leaves a
    // surface rather than a staircase of steps.
    const double step = stepOf(level);
    const std::int64_t side = blockSamplesOf(level);
    const double px = x.toDouble() / step, py = y.toDouble() / step;
    const auto sx = std::int64_t(std::floor(px)), sy = std::int64_t(std::floor(py));
    const auto fx = core::Fixed::fromDoubleForContent(px - double(sx));
    const auto fy = core::Fixed::fromDoubleForContent(py - double(sy));
    const auto sample = [&](std::int64_t ix, std::int64_t iy) {
        const auto block = blocks.find(keyOf(floorDiv(ix, side), floorDiv(iy, side)));
        if (block == blocks.end()) return core::kZero;
        return block->second->delta[std::size_t(floorMod(iy, side) * side + floorMod(ix, side))];
    };
    const auto a = sample(sx, sy), b = sample(sx + 1, sy);
    const auto c = sample(sx, sy + 1), d = sample(sx + 1, sy + 1);
    const auto top = a + (b - a) * fx;
    const auto bottom = c + (d - c) * fx;
    return top + (bottom - top) * fy;
}

core::Fixed EditLayer::at(core::Fixed x, core::Fixed y) const {
    // The empty check without the lock, and deliberately.
    //
    // This is asked for every height in the world. An unedited world must pay
    // one predictable branch here and nothing else - not a mutex, not a hash.
    // A world that has just gained its first block reading empty for one query
    // is a sample drawn a frame early, which the revision below corrects anyway.
    if (!any_.load(std::memory_order_acquire)) return core::kZero;
    std::shared_lock lock(mutex_);
    core::Fixed sum = core::kZero;
    for (int level = 0; level < kLevels; ++level) sum += levelAtLocked(level, x, y);
    return sum;
}

core::Fixed EditLayer::add(int level, std::int64_t sampleX, std::int64_t sampleY, core::Fixed metres) {
    level = std::clamp(level, 0, kLevels - 1);
    std::unique_lock lock(mutex_);
    const std::int64_t side = blockSamplesOf(level), patch = patchSamplesOf(level);
    const auto blockX = floorDiv(sampleX, side), blockY = floorDiv(sampleY, side);
    auto& block = levels_[std::size_t(level)][keyOf(blockX, blockY)];
    if (!block) block = std::make_unique<Block>(level);
    any_.store(true, std::memory_order_release);
    const auto localX = floorMod(sampleX, side), localY = floorMod(sampleY, side);
    const auto i = localY * side + localX;
    block->delta[std::size_t(i)] += metres;
    // The sample first, then the number: a reader that sees this revision
    // sees this sample (release here, acquire in revision()).
    const auto revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    block->patches[std::size_t((localY / patch) * kPatchesPerBlock + localX / patch)] = revision;
    block->revision = revision;
    if (observer_) observer_(level, blockX, blockY, revision);
    return block->delta[std::size_t(i)];
}

core::Fixed EditLayer::sample(int level, std::int64_t sampleX, std::int64_t sampleY) const {
    if (empty() || level < 0 || level >= kLevels) return core::kZero;
    std::shared_lock lock(mutex_);
    const Blocks& blocks = levels_[std::size_t(level)];
    const std::int64_t side = blockSamplesOf(level);
    const auto block = blocks.find(keyOf(floorDiv(sampleX, side), floorDiv(sampleY, side)));
    if (block == blocks.end()) return core::kZero;
    return block->second->delta[std::size_t(floorMod(sampleY, side) * side + floorMod(sampleX, side))];
}

void EditLayer::observe(Observer observer) {
    std::unique_lock lock(mutex_);
    observer_ = std::move(observer);
}

std::uint64_t EditLayer::copyBlocks(int level, std::int64_t minBlockX, std::int64_t minBlockY,
                                    std::int64_t maxBlockX, std::int64_t maxBlockY,
                                    std::vector<BlockCopy>& out) const {
    std::shared_lock lock(mutex_);
    if (level >= 0 && level < kLevels)
        eachBlockLocked(levels_[std::size_t(level)], minBlockX, minBlockY, maxBlockX, maxBlockY,
                        [&](std::int64_t bx, std::int64_t by, const Block& block) {
                            out.push_back({level, bx, by, block.delta});
                        });
    return revision_.load(std::memory_order_acquire);
}

std::uint64_t EditLayer::copyBlocksWithin(std::int64_t minX, std::int64_t minY, std::int64_t maxX,
                                          std::int64_t maxY, std::vector<BlockCopy>& out) const {
    std::shared_lock lock(mutex_);
    for (int level = 0; level < kLevels; ++level) {
        const std::int64_t metres = blockMetresOf(level);
        // Blocks whose corner is inside: from the first corner at or past the
        // low edge to the last one before the high edge.
        const auto lowX = floorDiv(minX + metres - 1, metres), lowY = floorDiv(minY + metres - 1, metres);
        const auto highX = floorDiv(maxX - 1, metres), highY = floorDiv(maxY - 1, metres);
        eachBlockLocked(levels_[std::size_t(level)], lowX, lowY, highX, highY,
                        [&](std::int64_t bx, std::int64_t by, const Block& block) {
                            out.push_back({level, bx, by, block.delta});
                        });
    }
    return revision_.load(std::memory_order_acquire);
}

std::size_t EditLayer::blocks() const {
    std::shared_lock lock(mutex_);
    std::size_t n = 0;
    for (const auto& blocks : levels_) n += blocks.size();
    return n;
}

std::vector<std::pair<std::int64_t, std::int64_t>> EditLayer::blockKeys(int level) const {
    std::shared_lock lock(mutex_);
    std::vector<std::pair<std::int64_t, std::int64_t>> keys;
    if (level < 0 || level >= kLevels) return keys;
    keys.reserve(levels_[std::size_t(level)].size());
    for (const auto& [key, block] : levels_[std::size_t(level)]) keys.push_back(blockOfKey(key));
    return keys;
}

bool EditLayer::touches(core::WorldRect area) const {
    if (empty()) return false;
    std::shared_lock lock(mutex_);
    bool found = false;
    for (int level = 0; level < kLevels && !found; ++level) {
        const std::int64_t metres = blockMetresOf(level);
        eachBlockLocked(levels_[std::size_t(level)], floorDiv(lowMetres(area.min.x), metres),
                        floorDiv(lowMetres(area.min.y), metres), floorDiv(highMetres(area.max.x), metres),
                        floorDiv(highMetres(area.max.y), metres),
                        [&](std::int64_t, std::int64_t, const Block&) { found = true; });
    }
    return found;
}

std::uint64_t EditLayer::revisionIn(core::WorldRect area) const {
    // Nothing was ever written: the question every page of an unedited world
    // asks, answered without the lock.
    if (revision() == 0 || !area.valid()) return 0;
    const auto minX = lowMetres(area.min.x), minY = lowMetres(area.min.y);
    const auto maxX = highMetres(area.max.x), maxY = highMetres(area.max.y);
    std::shared_lock lock(mutex_);
    std::uint64_t newest = 0;
    for (const auto& [revision, rect] : cleared_)
        if (revision > newest && rect.overlaps(area)) newest = revision;
    for (int level = 0; level < kLevels; ++level) {
        // A sample shapes the ground a step either side of it: a patch just
        // outside the rectangle still reaches into it.
        const std::int64_t step = stepOf(level), patch = patchMetresOf(level), metres = blockMetresOf(level);
        const auto lowPX = floorDiv(minX - step, patch), lowPY = floorDiv(minY - step, patch);
        const auto highPX = floorDiv(maxX + step, patch), highPY = floorDiv(maxY + step, patch);
        const auto reach = level == 0 ? 0 : step;
        eachBlockLocked(levels_[std::size_t(level)], floorDiv(minX - reach, metres), floorDiv(minY - reach, metres),
                        floorDiv(maxX + reach, metres), floorDiv(maxY + reach, metres),
                        [&](std::int64_t bx, std::int64_t by, const Block& block) {
            if (block.revision <= newest) return;
            // Level 0 keeps the rule it always had: the patches the rectangle
            // is in. The coarse levels reach a step further.
            const auto fromPX = level == 0 ? floorDiv(minX, patch) : lowPX;
            const auto fromPY = level == 0 ? floorDiv(minY, patch) : lowPY;
            const auto toPX = level == 0 ? floorDiv(maxX, patch) : highPX;
            const auto toPY = level == 0 ? floorDiv(maxY, patch) : highPY;
            const auto fromX = std::max<std::int64_t>(0, fromPX - bx * kPatchesPerBlock);
            const auto fromY = std::max<std::int64_t>(0, fromPY - by * kPatchesPerBlock);
            const auto toX = std::min<std::int64_t>(kPatchesPerBlock - 1, toPX - bx * kPatchesPerBlock);
            const auto toY = std::min<std::int64_t>(kPatchesPerBlock - 1, toPY - by * kPatchesPerBlock);
            for (auto py = fromY; py <= toY; ++py)
                for (auto px = fromX; px <= toX; ++px)
                    newest = std::max(newest, block.patches[std::size_t(py * kPatchesPerBlock + px)]);
        });
    }
    return newest;
}

std::uint64_t EditLayer::fingerprint(core::WorldRect area) const {
    if (revision() == 0 || !area.valid()) return 0;
    std::shared_lock lock(mutex_);
    // A sum of per-sample hashes: independent of the order the blocks are
    // walked in, and nought for zeros, so ground dug and filled back in is
    // named like ground nobody touched.
    std::uint64_t sum = 0;
    bool any = false;
    for (int level = 0; level < kLevels; ++level) {
        const std::int64_t step = stepOf(level), side = blockSamplesOf(level), patch = patchSamplesOf(level);
        // Every sample a point inside the rectangle interpolates between.
        const auto lowSX = floorDiv(lowMetres(area.min.x), step);
        const auto lowSY = floorDiv(lowMetres(area.min.y), step);
        const auto highSX = floorDiv(highMetres(area.max.x), step) + 1;
        const auto highSY = floorDiv(highMetres(area.max.y), step) + 1;
        // Level 0 hashes as it always did, so a name a page was filed under
        // before there were levels is the name it still has.
        const std::uint64_t salt = level == 0 ? 0 : core::splitmix64(0x1e7e1ull + std::uint64_t(level));
        eachBlockLocked(levels_[std::size_t(level)], floorDiv(lowSX, side), floorDiv(lowSY, side),
                        floorDiv(highSX, side), floorDiv(highSY, side),
                        [&](std::int64_t bx, std::int64_t by, const Block& block) {
            const auto originX = bx * side, originY = by * side;
            const auto fromX = std::max<std::int64_t>(0, lowSX - originX);
            const auto fromY = std::max<std::int64_t>(0, lowSY - originY);
            const auto toX = std::min<std::int64_t>(side - 1, highSX - originX);
            const auto toY = std::min<std::int64_t>(side - 1, highSY - originY);
            for (auto y = fromY; y <= toY; ++y) {
                // A patch nobody wrote holds only zeros: skip its samples in this row.
                for (auto x = fromX; x <= toX; ++x) {
                    if (block.patches[std::size_t((y / patch) * kPatchesPerBlock + x / patch)] == 0) {
                        x = (x / patch) * patch + patch - 1;
                        continue;
                    }
                    const auto value = block.delta[std::size_t(y * side + x)];
                    if (value == core::kZero) continue;
                    any = true;
                    const auto sx = std::uint64_t(originX + x), sy = std::uint64_t(originY + y);
                    sum += core::splitmix64(sx * 0x9E3779B97F4A7C15ull ^
                                            core::splitmix64(sy ^ std::uint64_t(value.raw) * 0xC2B2AE3D27D4EB4Full)) ^
                           salt;
                }
            }
        });
    }
    return any && sum == 0 ? 1 : sum;
}

std::uint64_t EditLayer::changedSince(std::uint64_t since, std::vector<core::WorldRect>& out) const {
    std::shared_lock lock(mutex_);
    const auto now = revision_.load(std::memory_order_acquire);
    if (now <= since) return now;
    for (const auto& [revision, rect] : cleared_)
        if (revision > since) out.push_back(rect);
    for (int level = 0; level < kLevels; ++level) {
        const std::int64_t metres = blockMetresOf(level), patch = patchMetresOf(level);
        // A coarse sample shapes the ground a step past its patch.
        const std::int64_t reach = level == 0 ? 0 : stepOf(level);
        for (const auto& [key, block] : levels_[std::size_t(level)]) {
            if (block->revision <= since) continue;
            const auto [bx, by] = blockOfKey(key);
            const auto x = bx * metres, y = by * metres;
            // A run of written patches along a row is one rectangle: a stroke is a
            // few of them, and a consumer tests each against what it holds.
            for (std::int64_t py = 0; py < kPatchesPerBlock; ++py)
                for (std::int64_t px = 0; px < kPatchesPerBlock; ++px) {
                    if (block->patches[std::size_t(py * kPatchesPerBlock + px)] <= since) continue;
                    auto end = px + 1;
                    while (end < kPatchesPerBlock && block->patches[std::size_t(py * kPatchesPerBlock + end)] > since)
                        ++end;
                    out.push_back(metreRect(x + px * patch - reach, y + py * patch - reach,
                                            x + end * patch + reach, y + (py + 1) * patch + reach));
                    px = end;
                }
        }
    }
    return now;
}

void EditLayer::clear() {
    std::unique_lock lock(mutex_);
    const auto revision = revision_.fetch_add(1, std::memory_order_acq_rel) + 1;
    // One rectangle around everything that goes. Coarse, and clearing a layer
    // is rare enough that coarse costs nothing worth saving.
    std::int64_t lowX = INT64_MAX, lowY = INT64_MAX, highX = INT64_MIN, highY = INT64_MIN;
    for (int level = 0; level < kLevels; ++level) {
        const std::int64_t metres = blockMetresOf(level), reach = level == 0 ? 0 : stepOf(level);
        for (const auto& [key, block] : levels_[std::size_t(level)]) {
            const auto [bx, by] = blockOfKey(key);
            lowX = std::min(lowX, bx * metres - reach); lowY = std::min(lowY, by * metres - reach);
            highX = std::max(highX, (bx + 1) * metres + reach); highY = std::max(highY, (by + 1) * metres + reach);
        }
    }
    if (lowX <= highX) {
        cleared_.emplace_back(revision, metreRect(lowX, lowY, highX, highY));
        // Bounded: the two oldest become one rectangle around both.
        if (cleared_.size() > 16) {
            auto& [firstRevision, first] = cleared_[0];
            const auto& second = cleared_[1].second;
            first = {{std::min(first.min.x, second.min.x), std::min(first.min.y, second.min.y)},
                     {std::max(first.max.x, second.max.x), std::max(first.max.y, second.max.y)}};
            firstRevision = std::max(firstRevision, cleared_[1].first);
            cleared_.erase(cleared_.begin() + 1);
        }
    }
    if (observer_)
        for (int level = 0; level < kLevels; ++level)
            for (const auto& [key, block] : levels_[std::size_t(level)]) {
                const auto [bx, by] = blockOfKey(key);
                observer_(level, bx, by, revision);
            }
    for (auto& blocks : levels_) blocks.clear();
    any_.store(false, std::memory_order_release);
}

} // namespace world
