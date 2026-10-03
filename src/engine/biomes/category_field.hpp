#pragma once
// The categorical control layers over the world: four ids a 256 m sample -
// the ground's category and the forest, water and decor layers' biomes (0:
// as the category says) - and the forest_bias channel the forest biomes'
// density follows, kept only where any of them is said. Land, in 128 x
// 128-sample chunks; the sea and the rest of the world are nothing (store
// only land).
//
// Sample (sx, sy) covers [sx, sx + 1) x [sy, sy + 1) * 256 m, as the world
// source keeps it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine::biomes {

class CategoryField {
public:
    static constexpr std::int64_t kSampleMetres = 256;
    static constexpr std::int64_t kChunk = 128;
    // ground, forest, water, decor, and forest_bias as 1 + 254 * bias (0: not said)
    using Ids = std::array<std::uint8_t, 5>;
    void setSampleMetres(std::int64_t value) { sampleMetres_ = value; }
    [[nodiscard]] static double forestBias(const Ids& ids) { return ids[4] ? double(ids[4] - 1) / 254.0 : 0.5; }
    [[nodiscard]] static std::uint8_t forestBiasByte(double bias) {
        return std::uint8_t(1 + std::lround(std::clamp(bias, 0.0, 1.0) * 254.0));
    }

    void set(std::int64_t sx, std::int64_t sy, const Ids& ids);
    [[nodiscard]] Ids sample(std::int64_t sx, std::int64_t sy) const;
    // The sample a point of the world (metres) lies in.
    [[nodiscard]] Ids at(double x, double y) const;

    // The ground's two strongest categories round a point and the second's
    // share, by the bilinear weights of the four samples about it - what the
    // shader reconstructs (terrain_biomes.hlsli), without its tearing noise.
    struct Pair { std::uint8_t a = 0, b = 0; double share = 0; };
    [[nodiscard]] Pair groundPair(double x, double y) const;

    // Drops chunks that hold nothing but noughts.
    void settle();
    [[nodiscard]] bool empty() const { return chunks_.empty(); }
    [[nodiscard]] std::size_t chunksHeld() const { return chunks_.size(); }
    [[nodiscard]] std::size_t bytes() const { return chunks_.size() * std::size_t(kChunk * kChunk) * sizeof(Ids); }
    // Changes when any id does.
    [[nodiscard]] std::uint64_t key() const;
    // How many samples hold each ground id (land samples only).
    [[nodiscard]] std::array<std::size_t, 256> groundCounts() const;

private:
    static std::uint64_t chunkKey(std::int64_t cx, std::int64_t cy) {
        return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint32_t(cy);
    }
    std::unordered_map<std::uint64_t, std::vector<Ids>> chunks_;
    std::int64_t sampleMetres_ = kSampleMetres;
};

} // namespace engine::biomes
