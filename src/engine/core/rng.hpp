#pragma once
// Deterministic PRNG. Every stochastic decision in the simulation draws from a
// Rng owned by a specific system, seeded from the world seed plus a stream id,
// so adding a new random draw in one system cannot shift the numbers another
// system sees (GDD 11: reproducible from seed and input commands alone).

#include <cstdint>
#include <vector>

namespace core {

// splitmix64: used to derive stream seeds and to hash content ids into seeds.
constexpr std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

class Rng {
public:
    Rng() : Rng(0, 0) {}
    Rng(std::uint64_t seed, std::uint64_t stream) {
        state_ = splitmix64(seed ^ (stream * 0x2545f4914f6cdd1dULL));
        inc_ = (splitmix64(state_) | 1u);
    }

    std::uint64_t nextU64() {
        // xorshift64* — small, fast, and fully specified in integer terms.
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return (state_ * 0x2545f4914f6cdd1dULL) ^ inc_;
    }

    std::uint32_t nextU32() { return static_cast<std::uint32_t>(nextU64() >> 32); }

    // Uniform in [0, n). Rejection-sampled so the distribution stays exact.
    std::uint32_t below(std::uint32_t n) {
        if (n <= 1) return 0;
        const std::uint32_t limit = std::uint32_t(-1) - (std::uint32_t(-1) % n) - 1;
        std::uint32_t v;
        do { v = nextU32(); } while (v > limit);
        return v % n;
    }

    // Inclusive on both ends.
    std::int32_t range(std::int32_t lo, std::int32_t hi) {
        if (hi <= lo) return lo;
        return lo + static_cast<std::int32_t>(below(static_cast<std::uint32_t>(hi - lo + 1)));
    }

    // True with probability numerator/denominator.
    bool chance(std::int64_t numerator, std::int64_t denominator) {
        if (numerator <= 0) return false;
        if (numerator >= denominator) return true;
        return static_cast<std::int64_t>(below(static_cast<std::uint32_t>(denominator))) < numerator;
    }

    // Fisher-Yates. Autonomous pawns are iterated in a shuffled order so that
    // low entity ids do not silently win every contested job.
    template <typename T>
    void shuffle(std::vector<T>& v) {
        for (std::size_t i = v.size(); i > 1; --i) {
            std::size_t j = below(static_cast<std::uint32_t>(i));
            std::swap(v[i - 1], v[j]);
        }
    }

    std::uint64_t state() const { return state_; }
    void setState(std::uint64_t s, std::uint64_t inc) { state_ = s ? s : 1; inc_ = inc | 1u; }
    std::uint64_t inc() const { return inc_; }

private:
    std::uint64_t state_ = 1;
    std::uint64_t inc_ = 1;
};

// Stream ids. Each system gets its own so draws stay independent.
namespace stream {
inline constexpr std::uint64_t kWorldGen   = 1;
inline constexpr std::uint64_t kPopulation = 2;
inline constexpr std::uint64_t kTraits     = 3;
inline constexpr std::uint64_t kWork       = 4;
inline constexpr std::uint64_t kHealth     = 5;
inline constexpr std::uint64_t kSocial     = 6;
inline constexpr std::uint64_t kWeather    = 7;
inline constexpr std::uint64_t kWildlife   = 8;
inline constexpr std::uint64_t kDiscovery  = 9;
} // namespace stream

} // namespace core
