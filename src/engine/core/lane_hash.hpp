#pragma once
// A content key over large arrays: four independent multiply-xor lanes folded
// at the end.
//
// A cache key has to change whenever what it covers does, and nothing more; it
// is not an identity anybody stores or compares across versions. Byte-wise FNV
// and a splitmix per value both put every step behind the previous one, and
// over a world's worth of cells and foundation points that is hundreds of
// milliseconds of a single dependency chain. Four lanes let the multiplies
// overlap: the same values, a few tens of milliseconds.
//
// Deterministic: the same values in the same order give the same key on every
// machine and every run.
#include <cstddef>
#include <cstdint>

#include "engine/core/rng.hpp"

namespace core {

class LaneHash {
public:
    explicit LaneHash(std::uint64_t seed = 0) {
        for (auto& h : lane_) h ^= seed;
    }

    void add(std::uint64_t value) {
        auto& h = lane_[count_++ & 3];
        h = (h ^ value) * kPrime;
    }

    // Every element of a contiguous range, through `key` (element -> uint64),
    // preceded by its length so that two ranges cannot trade elements.
    template <class Range, class Key>
    void addAll(const Range& values, Key key) {
        add(static_cast<std::uint64_t>(values.size()));
        const std::size_t n = values.size(), whole = n & ~std::size_t(3);
        std::uint64_t a = lane_[0], b = lane_[1], c = lane_[2], d = lane_[3];
        for (std::size_t i = 0; i < whole; i += 4) {
            a = (a ^ key(values[i])) * kPrime;
            b = (b ^ key(values[i + 1])) * kPrime;
            c = (c ^ key(values[i + 2])) * kPrime;
            d = (d ^ key(values[i + 3])) * kPrime;
        }
        lane_[0] = a; lane_[1] = b; lane_[2] = c; lane_[3] = d;
        for (std::size_t i = whole; i < n; ++i) add(key(values[i]));
    }

    // Integers of any width, as their unsigned bits.
    template <class Range>
    void addAll(const Range& values) {
        addAll(values, [](auto v) {
            using T = std::remove_cvref_t<decltype(v)>;
            return static_cast<std::uint64_t>(static_cast<std::make_unsigned_t<T>>(v));
        });
    }

    [[nodiscard]] std::uint64_t value() const {
        std::uint64_t hash = splitmix64(count_);
        for (const auto h : lane_) hash = splitmix64(hash ^ h);
        return hash;
    }

private:
    static constexpr std::uint64_t kPrime = 0x9e3779b97f4a7c15ull;
    // Multiply, then rotate the product's well-mixed high bits back down, so a
    // change in one value is not undone by the same change in another.
    static std::uint64_t step(std::uint64_t h, std::uint64_t v) {
        return std::rotl((h ^ v) * kPrime, 29);
    }
    std::uint64_t lane_[4]{0x243f6a8885a308d3ull, 0x13198a2e03707344ull,
                           0xa4093822299f31d0ull, 0x082efa98ec4e6c89ull};
    std::uint64_t count_ = 0;
};

} // namespace core

