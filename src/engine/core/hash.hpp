#pragma once
// FNV-1a. Two uses: stable ids for content strings, and a running checksum of
// world state that the headless runner prints each tick so a desync between two
// runs of the same seed shows up at the exact tick it appears.

#include <cstdint>
#include <string_view>

namespace core {

inline constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
inline constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

constexpr std::uint64_t hashBytes(const void* data, std::size_t len, std::uint64_t seed = kFnvOffset) {
    const auto* p = static_cast<const unsigned char*>(data);
    std::uint64_t h = seed;
    for (std::size_t i = 0; i < len; ++i) { h ^= p[i]; h *= kFnvPrime; }
    return h;
}

constexpr std::uint64_t hashString(std::string_view s, std::uint64_t seed = kFnvOffset) {
    std::uint64_t h = seed;
    for (char c : s) { h ^= static_cast<unsigned char>(c); h *= kFnvPrime; }
    return h;
}

// Accumulates a world checksum without allocating.
class Checksum {
public:
    void add(std::uint64_t v) {
        h_ ^= v;
        h_ *= kFnvPrime;
        h_ ^= h_ >> 29;
    }
    void add(std::int64_t v) { add(static_cast<std::uint64_t>(v)); }
    void add(std::int32_t v) { add(static_cast<std::uint64_t>(static_cast<std::int64_t>(v))); }
    void add(std::string_view s) { h_ = hashString(s, h_); }
    std::uint64_t value() const { return h_; }

private:
    std::uint64_t h_ = kFnvOffset;
};

} // namespace core
