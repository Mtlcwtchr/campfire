#include "engine/world_store/chunk_key.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

#include "engine/core/rng.hpp"

namespace engine::world_store {

const char* levelName(ChunkLevel level) {
    switch (level) {
        case ChunkLevel::RuntimePatch: return "patch";
        case ChunkLevel::GenerationTile: return "tile";
        case ChunkLevel::AuthoringChunk: return "chunk";
        case ChunkLevel::SourceChunk: return "source";
        case ChunkLevel::Region: return "region";
    }
    return "?";
}

MetreRect MetreRect::merged(const MetreRect& o) const {
    if (empty()) return o;
    if (o.empty()) return *this;
    return {std::min(minX, o.minX), std::min(minY, o.minY), std::max(maxX, o.maxX), std::max(maxY, o.maxY)};
}

ChunkKey ChunkKey::within(ChunkLevel coarser) const {
    if (coarser <= level) return *this;
    const std::int64_t ratio = chunkMetres(coarser) / metres();
    return {coarser, floorDiv(x, ratio), floorDiv(y, ratio)};
}

bool ChunkKey::contains(const ChunkKey& finer) const {
    if (finer.level > level) return false;
    return finer.within(level) == *this;
}

bool ChunkKey::containsMetres(double px, double py) const {
    return px >= double(minX()) && px < double(maxX()) && py >= double(minY()) && py < double(maxY());
}

std::string ChunkKey::stem() const {
    return std::to_string(x) + "_" + std::to_string(y);
}

std::optional<ChunkKey> ChunkKey::parse(ChunkLevel level, std::string_view stem) {
    // Split at the first underscore that is not a sign: "-3_-7" is -3 and -7.
    const std::size_t split = stem.find('_', stem.empty() || stem[0] != '-' ? 0 : 1);
    if (split == std::string_view::npos || split == 0 || split + 1 >= stem.size()) return std::nullopt;
    ChunkKey key{level, 0, 0};
    const auto first = stem.substr(0, split), second = stem.substr(split + 1);
    const auto a = std::from_chars(first.data(), first.data() + first.size(), key.x);
    const auto b = std::from_chars(second.data(), second.data() + second.size(), key.y);
    if (a.ec != std::errc{} || a.ptr != first.data() + first.size()) return std::nullopt;
    if (b.ec != std::errc{} || b.ptr != second.data() + second.size()) return std::nullopt;
    return key;
}

std::size_t ChunkKeyHash::operator()(const ChunkKey& k) const noexcept {
    std::uint64_t h = core::splitmix64(std::uint64_t(k.level) * 0x9e3779b97f4a7c15ULL);
    h = core::splitmix64(h ^ std::uint64_t(k.x));
    h = core::splitmix64(h ^ std::uint64_t(k.y));
    return std::size_t(h);
}

ChunkKey chunkAt(ChunkLevel level, std::int64_t xMetres, std::int64_t yMetres) {
    const auto side = chunkMetres(level);
    return {level, floorDiv(xMetres, side), floorDiv(yMetres, side)};
}

ChunkKey chunkAt(ChunkLevel level, double xMetres, double yMetres) {
    const double side = double(chunkMetres(level));
    return {level, std::int64_t(std::floor(xMetres / side)), std::int64_t(std::floor(yMetres / side))};
}

std::vector<ChunkKey> chunksOverlapping(ChunkLevel level, const MetreRect& area) {
    std::vector<ChunkKey> keys;
    if (area.empty() || !std::isfinite(area.minX + area.minY + area.maxX + area.maxY)) return keys;
    const double side = double(chunkMetres(level));
    const auto lowX = std::int64_t(std::floor(area.minX / side));
    const auto lowY = std::int64_t(std::floor(area.minY / side));
    // Half-open: the last chunk is the one holding the point just short of max.
    const auto highX = std::int64_t(std::ceil(area.maxX / side)) - 1;
    const auto highY = std::int64_t(std::ceil(area.maxY / side)) - 1;
    if (highX < lowX || highY < lowY) return keys;
    keys.reserve(std::size_t((highX - lowX + 1) * (highY - lowY + 1)));
    for (auto y = lowY; y <= highY; ++y)
        for (auto x = lowX; x <= highX; ++x) keys.push_back({level, x, y});
    return keys;
}

} // namespace engine::world_store


