#include "engine/biomes/category_field.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace engine::biomes {
namespace {
std::int64_t floorDiv(std::int64_t a, std::int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
} // namespace

void CategoryField::set(std::int64_t sx, std::int64_t sy, const Ids& ids) {
    const std::int64_t cx = floorDiv(sx, kChunk), cy = floorDiv(sy, kChunk);
    auto it = chunks_.find(chunkKey(cx, cy));
    if (it == chunks_.end()) {
        if (ids == Ids{}) return;
        it = chunks_.emplace(chunkKey(cx, cy), std::vector<Ids>(std::size_t(kChunk * kChunk), Ids{})).first;
    }
    it->second[std::size_t((sy - cy * kChunk) * kChunk + (sx - cx * kChunk))] = ids;
}

CategoryField::Ids CategoryField::sample(std::int64_t sx, std::int64_t sy) const {
    const std::int64_t cx = floorDiv(sx, kChunk), cy = floorDiv(sy, kChunk);
    const auto it = chunks_.find(chunkKey(cx, cy));
    if (it == chunks_.end()) return {};
    return it->second[std::size_t((sy - cy * kChunk) * kChunk + (sx - cx * kChunk))];
}

CategoryField::Ids CategoryField::at(double x, double y) const {
    if (chunks_.empty() || !std::isfinite(x) || !std::isfinite(y)) return {};
    return sample(std::int64_t(std::floor(x / double(sampleMetres_))), std::int64_t(std::floor(y / double(sampleMetres_))));
}

CategoryField::Pair CategoryField::groundPair(double x, double y) const {
    Pair p;
    if (chunks_.empty() || !std::isfinite(x) || !std::isfinite(y)) return p;
    // Sample centres are half a sample in.
    const double u = x / double(sampleMetres_) - 0.5, v = y / double(sampleMetres_) - 0.5;
    const auto i0 = std::int64_t(std::floor(u)), j0 = std::int64_t(std::floor(v));
    const double fx = u - double(i0), fy = v - double(j0);
    const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    const std::uint8_t g[4] = {sample(i0, j0)[0], sample(i0 + 1, j0)[0], sample(i0, j0 + 1)[0], sample(i0 + 1, j0 + 1)[0]};
    std::map<std::uint8_t, double> sum;
    for (int k = 0; k < 4; ++k) sum[g[k]] += w[k];
    std::uint8_t best = g[0], next = g[0];
    double bw = -1, nw = -1;
    for (const auto& [id, s] : sum)
        if (s > bw) { next = best; nw = bw; best = id; bw = s; }
        else if (s > nw) { next = id; nw = s; }
    p.a = best;
    p.b = nw >= 0 ? next : best;
    p.share = nw > 0 ? nw / (bw + nw) : 0.0;
    return p;
}

void CategoryField::settle() {
    for (auto it = chunks_.begin(); it != chunks_.end();)
        if (std::all_of(it->second.begin(), it->second.end(), [](const Ids& i) { return i == Ids{}; })) it = chunks_.erase(it);
        else ++it;
}

std::uint64_t CategoryField::key() const {
    // In key order, so the same field is the same number however it was filled.
    std::vector<std::uint64_t> keys;
    keys.reserve(chunks_.size());
    for (const auto& [k, v] : chunks_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
    mix(std::uint64_t(sampleMetres_));
    for (const auto k : keys) {
        mix(k);
        for (const auto& ids : chunks_.at(k))
            mix(std::uint64_t(ids[0]) | std::uint64_t(ids[1]) << 8 | std::uint64_t(ids[2]) << 16 | std::uint64_t(ids[3]) << 24 |
                std::uint64_t(ids[4]) << 32);
    }
    return h;
}

std::array<std::size_t, 256> CategoryField::groundCounts() const {
    std::array<std::size_t, 256> out{};
    for (const auto& [k, v] : chunks_)
        for (const auto& ids : v)
            if (ids != Ids{}) ++out[ids[0]];
    return out;
}

} // namespace engine::biomes
