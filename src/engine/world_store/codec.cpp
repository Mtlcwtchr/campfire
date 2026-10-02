#include "engine/world_store/codec.hpp"

#include <zstd.h>

#include "engine/core/rng.hpp"

namespace engine::world_store {

Encoded encode(std::span<const std::uint8_t> raw, int level) {
    Encoded out;
    if (raw.empty()) return out;
    std::vector<std::uint8_t> packed(ZSTD_compressBound(raw.size()));
    const std::size_t size = ZSTD_compress(packed.data(), packed.size(), raw.data(), raw.size(), level);
    if (!ZSTD_isError(size) && size < raw.size()) {
        packed.resize(size);
        out.codec = Codec::Zstd;
        out.bytes = std::move(packed);
        return out;
    }
    out.bytes.assign(raw.begin(), raw.end());
    return out;
}

std::optional<std::vector<std::uint8_t>> decode(Codec codec, std::span<const std::uint8_t> stored,
                                                std::uint64_t rawSize) {
    if (rawSize > kMaxBlockBytes) return std::nullopt;
    switch (codec) {
        case Codec::Raw:
            if (stored.size() != rawSize) return std::nullopt;
            return std::vector<std::uint8_t>(stored.begin(), stored.end());
        case Codec::Zstd: {
            // The frame says how large it will be; it has to agree with the
            // directory, or one of the two is damaged.
            const unsigned long long framed = ZSTD_getFrameContentSize(stored.data(), stored.size());
            if (framed == ZSTD_CONTENTSIZE_ERROR || framed == ZSTD_CONTENTSIZE_UNKNOWN || framed != rawSize)
                return std::nullopt;
            std::vector<std::uint8_t> raw(static_cast<std::size_t>(rawSize));
            const std::size_t size = ZSTD_decompress(raw.data(), raw.size(), stored.data(), stored.size());
            if (ZSTD_isError(size) || size != rawSize) return std::nullopt;
            return raw;
        }
    }
    return std::nullopt;
}

std::uint64_t contentHash(std::span<const std::uint8_t> bytes, std::uint64_t seed) {
    std::uint64_t h = core::splitmix64(seed ^ (std::uint64_t(bytes.size()) * 0x9e3779b97f4a7c15ULL));
    std::size_t i = 0;
    const auto load = [&](std::size_t at, std::size_t n) {
        std::uint64_t word = 0;
        for (std::size_t b = 0; b < n; ++b) word |= std::uint64_t(bytes[at + b]) << (8 * b);
        return word;
    };
    for (; i + 8 <= bytes.size(); i += 8) h = core::splitmix64(h ^ load(i, 8));
    if (i < bytes.size()) h = core::splitmix64(h ^ load(i, bytes.size() - i) ^ (std::uint64_t(bytes.size() - i) << 56));
    return h;
}

} // namespace engine::world_store


