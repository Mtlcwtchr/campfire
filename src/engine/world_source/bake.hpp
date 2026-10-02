#pragma once
// What is worked out from a WorldSource once and kept beside it, never in it
// (D158): the source stays the reference as it was authored or imported, and
// what the world is built on is derived from it.
//
// For now two layers, at the source's own 256 m and only where there is land:
//
//  - height, its drainage made whole. A picture - drawn, generated, shaded -
//    is not a drained surface: its valleys are dark lines that do not join,
//    and on a continent made from them there were fifteen thousand closed
//    hollows deeper than twenty metres, every one a lake to the generator.
//    Each hollow is breached instead (Lindsay's complete breaching over a
//    priority flood): the path its water takes out is lowered to the hollow's
//    floor, so a broken valley becomes a valley. A hollow deeper than
//    `maxBreachMetres` below its rim is kept - that one is a basin somebody
//    meant, and it becomes a lake.
//  - flow: how much ground drains through each sample, as log2 of the samples
//    upstream (0..32, stored in a byte). What the runtime draws the streams
//    too small to be rivers from.
//
// Baked into a WorldSource of its own (`baked`), with the key of the source
// it was made from; stale when the source's height has changed since.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace engine::world_source {

struct BakeOptions {
    double maxBreachMetres = 400;   // a hollow deeper than this below its rim is kept
};

struct BakeReport {
    bool rebuilt = false;           // false: it was current and left as it was
    std::int64_t landSamples = 0;
    std::int64_t breached = 0;      // hollows opened
    std::int64_t kept = 0;          // hollows left as basins
    std::int64_t lowered = 0;       // samples lowered to open them
    double seconds = 0;
};

// The key a bake of this source carries: every height chunk's hash.
std::optional<std::uint64_t> heightKeyOf(const std::filesystem::path& source, std::string* why = nullptr);
// Whether `baked` was made from `source` as it is now.
bool bakeCurrent(const std::filesystem::path& source, const std::filesystem::path& baked);
// Makes `baked` from `source` unless it is current already.
std::optional<BakeReport> bakeSource(const std::filesystem::path& source, const std::filesystem::path& baked,
                                     const BakeOptions& options = {}, std::string* why = nullptr);

} // namespace engine::world_source
