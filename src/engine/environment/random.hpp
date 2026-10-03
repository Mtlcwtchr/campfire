#pragma once
// Hashes and noise the environment is placed with. Every number is a pure
// function of its arguments: two pages that ask about the same point get the
// same answer, which is the whole seam strategy.
#include <cmath>
#include <cstdint>

namespace engine::environment {

inline std::uint64_t mix64(std::uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}
inline std::uint64_t hashOf(std::uint64_t seed, std::int64_t a, std::int64_t b, std::uint64_t salt = 0) {
    return mix64(seed ^ mix64(std::uint64_t(a) * 0x9e3779b97f4a7c15ULL ^ mix64(std::uint64_t(b) + salt * 0xd1b54a32d192ed03ULL)));
}
// 0..1, 53 bits.
inline double unitOf(std::uint64_t h) { return double(h >> 11) * (1.0 / 9007199254740992.0); }

// A small deterministic sequence from one hash.
struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t s) : state(mix64(s + 0x632be59bd9b4e019ULL)) {}
    std::uint64_t next() { state += 0x9e3779b97f4a7c15ULL; return mix64(state); }
    double unit() { return unitOf(next()); }
    double range(double lo, double hi) { return lo + (hi - lo) * unit(); }
    int below(int n) { return n <= 0 ? 0 : int(next() % std::uint64_t(n)); }
};

// Value noise, 0..1, smooth, one feature per unit.
inline double valueNoise(std::uint64_t seed, double x, double y) {
    const double fx = std::floor(x), fy = std::floor(y);
    const auto ix = std::int64_t(fx), iy = std::int64_t(fy);
    double px = x - fx, py = y - fy;
    px = px * px * (3 - 2 * px);
    py = py * py * (3 - 2 * py);
    const double a = unitOf(hashOf(seed, ix, iy)), b = unitOf(hashOf(seed, ix + 1, iy));
    const double c = unitOf(hashOf(seed, ix, iy + 1)), d = unitOf(hashOf(seed, ix + 1, iy + 1));
    const double top = a + (b - a) * px, bottom = c + (d - c) * px;
    return top + (bottom - top) * py;
}

inline double smoothstep(double a, double b, double x) {
    const double t = std::fmin(1.0, std::fmax(0.0, (x - a) / (b - a)));
    return t * t * (3 - 2 * t);
}

} // namespace engine::environment
