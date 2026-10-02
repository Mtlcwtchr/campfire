#pragma once
// Compression of a chunk's blocks, and the hash that says a block is intact.
//
// zstd at a low level: the data is mostly small sparse records and runs of
// nought, which it takes apart at hundreds of megabytes a second. A block that
// does not get smaller is stored as it is, so an incompressible block costs
// its own size and never more.
//
// The hash is over the RAW bytes, not over what was written: it names what a
// block says, so recompressing a chunk at a different level, or not at all,
// does not change its identity - which is what a derived cache keyed on its
// inputs needs to hold. Not cryptographic; it is there to notice a damaged or
// foreign file, not to resist someone forging one.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace engine::world_store {

enum class Codec : std::uint8_t { Raw = 0, Zstd = 1 };

struct Encoded {
    Codec codec = Codec::Raw;
    std::vector<std::uint8_t> bytes;
};

// Zstd unless that does not make it smaller.
Encoded encode(std::span<const std::uint8_t> raw, int level = 3);
// Nothing when the stored bytes are not what their codec and size say they
// are: a truncated or damaged block, or a codec this build does not know.
std::optional<std::vector<std::uint8_t>> decode(Codec codec, std::span<const std::uint8_t> stored,
                                                std::uint64_t rawSize);
// The largest block a file may claim. A damaged size field must not become a
// request for sixteen exabytes of memory.
inline constexpr std::uint64_t kMaxBlockBytes = std::uint64_t(1) << 30;

// Stable across machines and builds: eight bytes at a time, read little-endian
// whatever the machine is.
std::uint64_t contentHash(std::span<const std::uint8_t> bytes, std::uint64_t seed = 0);

} // namespace engine::world_store


