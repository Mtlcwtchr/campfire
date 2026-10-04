#include "engine/environment/painted_layer.hpp"

#include <algorithm>
#include <vector>

#include "engine/environment/random.hpp"

namespace engine::environment {

namespace {
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}
} // namespace

void PaintedLayer::set(std::int64_t sx, std::int64_t sy, std::uint8_t id) {
    const auto cx = floorDiv(sx, kChunk), cy = floorDiv(sy, kChunk);
    auto it = chunks_.find(key(cx, cy));
    if (it == chunks_.end()) {
        if (id == 0) return;
        it = chunks_.emplace(key(cx, cy), std::vector<std::uint8_t>(std::size_t(kChunk * kChunk), 0)).first;
    }
    it->second[std::size_t((sy - cy * kChunk) * kChunk + (sx - cx * kChunk))] = id;
}

std::uint8_t PaintedLayer::sample(std::int64_t sx, std::int64_t sy) const {
    const auto cx = floorDiv(sx, kChunk), cy = floorDiv(sy, kChunk);
    auto it = chunks_.find(key(cx, cy));
    if (it == chunks_.end()) return 0;
    return it->second[std::size_t((sy - cy * kChunk) * kChunk + (sx - cx * kChunk))];
}

std::size_t PaintedLayer::painted() const {
    std::size_t n = 0;
    for (const auto& [k, v] : chunks_) n += std::size_t(std::count_if(v.begin(), v.end(), [](std::uint8_t id) { return id != 0; }));
    return n;
}

std::uint64_t PaintedLayer::fingerprint() const {
    std::vector<std::uint64_t> keys;
    for (const auto& [k, v] : chunks_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::uint64_t h = mix64(std::uint64_t(sampleMetres_ * 16));
    for (auto k : keys) {
        h = mix64(h ^ k);
        for (auto id : chunks_.at(k)) h = mix64(h ^ id);
    }
    for (const auto& [id, name] : legend)
        for (unsigned char c : name) h = mix64(h ^ (std::uint64_t(id) << 8) ^ c);
    return h;
}

} // namespace engine::environment
