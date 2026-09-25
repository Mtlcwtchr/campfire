#pragma once
// The ground as square tiles, addressed by where they are in the world.
//
// The mesh this replaces is an annulus around a snapshot of the camera. That
// has two consequences and both are fatal to it. Its vertices are polar, so no
// vertex of one level stands where a vertex of the level above stands and
// there is nothing for a morph to walk towards - which is why a change of
// level had to be a dissolve. And it is addressed by an epoch and a ring
// index, so moving the camera renames every piece of ground: the same hillside
// the player was looking at a moment ago is a different key, and is built
// again from nothing.
//
// A tile is named by the ground it covers. Move the camera and the tiles that
// were already built are the same tiles, because a tile is where it is rather
// than how far it is from the eye. What entering a new area costs is the strip
// that entered it.
//
// Levels nest exactly. A tile is always sixty-four cells across, so its side
// doubles with the level and a tile of level L+1 covers precisely four of
// level L, corner to corner. The vertex lattices nest with them: every vertex
// of the coarse tile is a vertex of the fine one at the same world coordinate.
// That is what a morph needs and what the polar mesh could never offer.

#include <cstdint>
#include <functional>
#include <unordered_map>

#include "game/world/climate_field.hpp"
#include "game/world/terrain_mesh.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace world {

// Cells across a tile, both edges included as vertices.
inline constexpr std::int32_t kTileCells = 64;

// Which ground a tile covers, in tiles of its own level's size.
struct TileId {
    std::int32_t x = 0, y = 0;
    std::int32_t lod = 0;
    friend constexpr bool operator==(TileId a, TileId b) {
        return a.x == b.x && a.y == b.y && a.lod == b.lod;
    }
};

// Legacy CPU tiles retain 64 cells; GPU chunk policies pass their own width.
inline std::int64_t tileMetresAt(std::int32_t lod, std::int32_t cells = kTileCells) {
    return static_cast<std::int64_t>(cells) * sampleMetresAt(lod);
}

// One key for a tile, for the caches that hold meshes by it.
inline std::int64_t tileKeyOf(TileId id) {
    return (static_cast<std::int64_t>(id.lod) << 56) ^
           ((static_cast<std::int64_t>(id.x) & 0xfffffff) << 28) ^
           (static_cast<std::int64_t>(id.y) & 0xfffffff);
}

// Worker-local memory of lattice samples, so that neighbouring tiles do not
// each work out the vertices they share. Discard it when the world changes.
struct TileSampleCache {
    struct Key {
        std::int64_t x = 0, y = 0;
        friend bool operator==(Key a, Key b) { return a.x == b.x && a.y == b.y; }
    };
    struct Hash {
        std::size_t operator()(Key k) const noexcept {
            const auto mixed = static_cast<std::uint64_t>(k.x) * 0x9e3779b97f4a7c15ull ^
                               (static_cast<std::uint64_t>(k.y) * 0xc2b2ae3d27d4eb4full);
            return static_cast<std::size_t>(mixed ^ (mixed >> 29));
        }
    };
    std::int32_t lod = -1;
    std::unordered_map<Key, core::Fixed, Hash> heights;
    std::unordered_map<Key, TerrainVertex, Hash> vertices;
};

// The tile's ground, read off the pages the navmesh and the simulation read.
//
// A skirt hangs from the border: a ring of vertices at the tile's edge dropped
// straight down. Where two tiles of different levels meet, the fine one has
// vertices the coarse one does not and the surface between them can part by
// a few centimetres; a skirt fills that with ground rather than with sky, and
// costs one quad per edge cell. It is the cheap half of what stitching does,
// and the honest half until the levels morph into each other.
TerrainMesh buildTileMesh(const HeightField& field, streaming::PageStore& pages,
                          const ClimateField& climate, TileId id,
                          const std::function<bool()>& cancelled = {},
                          TileSampleCache* samples = nullptr);

} // namespace world

template <> struct std::hash<world::TileId> {
    std::size_t operator()(world::TileId id) const noexcept {
        return std::hash<std::int64_t>{}(world::tileKeyOf(id));
    }
};
