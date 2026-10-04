#pragma once

#include <filesystem>
#include <mutex>
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/tile_cache.hpp"

namespace world::streaming {

// Full persistent H16/H64 pages, not mesh buffers or partial BaseTile records.
// Bump the format for layout changes, the generation version for changes to
// HeightField, GraphCarver, material/travel rules or parent refinement semantics.
inline constexpr std::uint32_t kBakedPageCacheVersion = 3;       // + environment masks
inline constexpr std::uint32_t kBakedPageGenerationVersion = 14; // bog pools

class BakedPageCache {
public:
    BakedPageCache(std::filesystem::path root, const generation::WorldMapData& world,
                   const HydrologyGraph& graph, HsimQuantisation quantisation,
                   std::uint16_t padding);
    // Worker-only. Bad/partial/foreign files leave `into` untouched. Cache I/O
    // failures are non-fatal: the store can always bake the missing page.
    //
    // `ground` is the fingerprint of the edits within the page's reach
    // (BakedPage::ground). Nought is the generator's own ground, filed as it
    // always was; anything else is filed beside it under its own name, so a
    // dig never overwrites the page it was dug into and filling it back in
    // finds the original again.
    CacheResult read(TileKey key, BakedPage& into, std::uint64_t ground = 0);
    CacheResult write(const BakedPage& page);

private:
    void prepare(); // fingerprint once, lazily on a worker, not the frame thread
    bool layout(TileKey key, BakedPage& page) const;
    std::vector<std::uint8_t> prefix(TileKey key, std::uint64_t ground) const;
    std::filesystem::path file(TileKey key, std::uint64_t ground);

    const generation::WorldMapData& world_;
    const HydrologyGraph& graph_;
    HsimQuantisation quantisation_;
    std::uint16_t padding_;
    std::filesystem::path root_, directory_;
    CacheIdentity identity_;
    std::once_flag prepared_;
};

} // namespace world::streaming
