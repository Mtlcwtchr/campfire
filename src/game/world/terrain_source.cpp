#include "game/world/terrain_source.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace world::terrain {
namespace {
// What a dry sample stores for its water surface. The same sentinel the
// planner already reads, so a page from this source and a page from the legacy
// storage mean the same thing by "there is no water here".
constexpr float kNoWater = -6000;
}

ScaleTerrainSource::ScaleTerrainSource(generation::WorldDescriptor descriptor, Config config)
    : descriptor_(std::move(descriptor)), config_(config), field_(descriptor_) {
    descriptor_.validate();
    if (config_.residentBytes < (1u << 20) || config_.padding > 8)
        throw std::invalid_argument("invalid terrain source budget");
    overview_ = std::make_shared<const generation::MacroOverview>(
            generation::buildMacroOverview(field_));
    coverage_ = std::make_unique<const generation::LandCoverage>(overview_);
}

ScaleTerrainSource::ScaleTerrainSource(generation::WorldDescriptor descriptor)
    : ScaleTerrainSource(std::move(descriptor), Config{}) {}

std::uint8_t ScaleTerrainSource::levelFor(double metresPerSample) {
    if (!std::isfinite(metresPerSample) || metresPerSample <= 0)
        return SourcePageLayout::kLevels - 1;
    std::uint8_t level = 0;
    while (level + 1 < SourcePageLayout::kLevels &&
           double(SourcePageLayout::stepMetres(level)) < metresPerSample)
        ++level;
    return level;
}

generation::WorldRect ScaleTerrainSource::rectOf(streaming::TileKey key, double halo) const {
    const double extent = double(SourcePageLayout::extentMetres(key.level));
    const double x = double(SourcePageLayout::originX(key)), y = double(SourcePageLayout::originY(key));
    return generation::WorldRect{x - halo, y - halo, x + extent + halo, y + extent + halo};
}

generation::Coverage ScaleTerrainSource::coverage(streaming::TileKey key) const {
    if (key.level >= SourcePageLayout::kLevels) return generation::Coverage::Unknown;
    // The halo is the padding the page itself carries: a page whose padding
    // reaches onto a beach is not an open-water page, however dry its interior.
    const double halo = double(SourcePageLayout::stepMetres(key.level)) * config_.padding;
    return coverage_->classify(rectOf(key, halo));
}

bool ScaleTerrainSource::mayHaveLand(streaming::TileKey key) const {
    return coverage(key) != generation::Coverage::OpenWater;
}

bool ScaleTerrainSource::mayHaveWater(streaming::TileKey key) const {
    if (key.level >= SourcePageLayout::kLevels) return true;
    const double halo = double(SourcePageLayout::stepMetres(key.level)) * config_.padding;
    return coverage_->mayHaveWater(rectOf(key, halo));
}

std::vector<streaming::TileKey> ScaleTerrainSource::keysOverlapping(double minX, double minY,
                                                                    double maxX, double maxY,
                                                                    std::uint8_t level) const {
    std::vector<streaming::TileKey> keys;
    if (level >= SourcePageLayout::kLevels || !(maxX >= minX) || !(maxY >= minY) ||
        !std::isfinite(minX + minY + maxX + maxY))
        return keys;
    const double extent = double(SourcePageLayout::extentMetres(level));
    const auto first = [extent](double v) { return std::int64_t(std::floor(v / extent)); };
    const auto lastX = first(maxX), lastY = first(maxY);
    // A view that reaches beyond the world still asks for whole pages; the
    // pages outside it simply answer Unknown and are never derived.
    const std::size_t wide = std::size_t(lastX - first(minX) + 1);
    const std::size_t tall = std::size_t(lastY - first(minY) + 1);
    if (wide > 4096 || tall > 4096 || wide * tall > 1u << 20) return keys;  // a caller asking for a world
    keys.reserve(wide * tall);
    for (auto y = first(minY); y <= lastY; ++y)
        for (auto x = first(minX); x <= lastX; ++x)
            keys.push_back({std::int32_t(x), std::int32_t(y), level});
    return keys;
}

std::shared_ptr<const SurfacePage> ScaleTerrainSource::derive(streaming::TileKey key) const {
    const int side = SourcePageLayout::kSamples;
    const int padding = config_.padding;
    const int width = side + 1 + 2 * padding;
    const double step = double(SourcePageLayout::stepMetres(key.level));
    const double ox = double(SourcePageLayout::originX(key)) - padding * step;
    const double oy = double(SourcePageLayout::originY(key)) - padding * step;

    // Pages are metres above this world's waterline, not raw field metres, so
    // everything downstream keeps the contract it already has: zero is the sea.
    const double sea = coverage_->seaLevelMetres();
    auto page = std::make_shared<SurfacePage>();
    page->side = width;
    page->padding = padding;
    page->step = int(step);
    page->bed.resize(std::size_t(width) * width);
    page->head.resize(page->bed.size());
    for (int row = 0; row < width; ++row)
        for (int column = 0; column < width; ++column) {
            const double h = field_.elevationAt(ox + column * step, oy + row * step) - sea;
            const auto at = std::size_t(row) * width + column;
            page->bed[at] = float(h);
            // Sea only, and only where the ground is under it. Lakes, rivers
            // and their banks belong to the hydrology stage, which does not
            // exist yet in this mode - so this is deliberately the ocean and
            // nothing else, rather than a guess that would have to be unlearnt.
            page->head[at] = h < 0 ? 0.0f : kNoWater;
        }
    return page;
}

std::shared_ptr<const SurfacePage> ScaleTerrainSource::page(streaming::TileKey key) {
    if (key.level >= SourcePageLayout::kLevels) return {};
    {
        const std::lock_guard lock(guard_);
        if (const auto found = pages_.find(key); found != pages_.end()) {
            order_.splice(order_.end(), order_, found->second.age);
            ++stats_.hits;
            return found->second.page;
        }
    }
    // The mask decides before a single sample is evaluated. This is the whole
    // reason it exists: refusing to DRAW open water saves nothing, refusing to
    // derive it saves the derivation.
    if (coverage(key) == generation::Coverage::OpenWater) {
        const std::lock_guard lock(guard_);
        ++stats_.refusedOpenWater;
        return {};
    }
    auto derived = derive(key);
    const auto samples = derived->bed.size();
    const std::lock_guard lock(guard_);
    // Another worker may have derived the same page meanwhile. The answer is
    // identical - the field is a closed form - so the first one published wins
    // and the second is dropped rather than held under a lock across a bake.
    if (const auto found = pages_.find(key); found != pages_.end()) {
        order_.splice(order_.end(), order_, found->second.age);
        ++stats_.hits;
        return found->second.page;
    }
    ++stats_.derived;
    stats_.fieldSamples += samples;
    keepLocked(key, derived);
    return derived;
}

std::shared_ptr<const SurfacePage> ScaleTerrainSource::resident(streaming::TileKey key) const {
    const std::lock_guard lock(guard_);
    const auto found = pages_.find(key);
    if (found == pages_.end()) return {};
    order_.splice(order_.end(), order_, found->second.age);
    return found->second.page;
}

void ScaleTerrainSource::keepLocked(streaming::TileKey key, std::shared_ptr<const SurfacePage> page) {
    const auto cost = (page->bed.size() + page->head.size()) * sizeof(float) + sizeof(SurfacePage);
    order_.push_back(key);
    pages_.emplace(key, Entry{std::move(page), std::prev(order_.end())});
    bytes_ += cost;
    // Oldest first, and never the page just added: a budget smaller than one
    // page would otherwise evict the answer it was asked for.
    while (bytes_ > config_.residentBytes && order_.size() > 1) {
        const auto oldest = order_.front();
        const auto found = pages_.find(oldest);
        if (found != pages_.end()) {
            bytes_ -= (found->second.page->bed.size() + found->second.page->head.size()) * sizeof(float) +
                      sizeof(SurfacePage);
            pages_.erase(found);
            ++stats_.evicted;
        }
        order_.pop_front();
    }
}

ScaleTerrainSource::Stats ScaleTerrainSource::stats() const {
    const std::lock_guard lock(guard_);
    auto copy = stats_;
    copy.resident = pages_.size();
    copy.residentBytes = bytes_;
    return copy;
}

std::size_t ScaleTerrainSource::bytes() const {
    const std::lock_guard lock(guard_);
    return overview_->bytes() + coverage_->bytes() + bytes_;
}

void ScaleTerrainSource::clear() {
    const std::lock_guard lock(guard_);
    pages_.clear();
    order_.clear();
    bytes_ = 0;
}

} // namespace world::terrain
