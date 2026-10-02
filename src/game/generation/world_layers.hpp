#pragma once
// Layers: the world's maps, each at its own resolution, painted with brushes.
//
// A region (world_layout.hpp) is the coarsest thing a world is painted with -
// a whole 131 km square, generated or empty, with its seed and its dials.
// Below it are the layers: one map per thing the generator decides that a
// person might want to decide instead - where the continents are, where the
// ranges run, how hilly the ground is, how much of it is sea, how long it has
// weathered, how much rain it gets. Each is a raster at the resolution that
// thing actually has. Rain changes over tens of kilometres and is held at eight;
// a coastline changes over a few and is held at two.
//
// The resolution is also the brush. Nothing finer than a texel can be painted
// on a layer - a brush smaller than one paints nothing the layer can hold - so
// a layer's texel is its smallest brush, and its largest is a fixed number of
// texels (kMaxBrushTexels): one dab costs the same on every layer, and the
// coarse ones are painted with brushes as coarse as they are.
//
// A layer does NOT hold the world. It holds what was painted over it. Where
// nothing was painted the generator makes the place as it always did - the pass
// that decides the layer's thing is the layer's procedural brush, run over the
// whole world (world_brush.hpp) - so an unpainted layer costs nothing to keep
// or to save, and a world whose layers are all empty is the generated world,
// exactly.
//
// Two kinds of layer. An OVERRIDE layer holds a value and how much of it covers
// what the generator made there (the cover, 0..1); it is composited
// premultiplied, so a stroke's soft edge blends into the generated country
// instead of dragging it towards some default. A DELTA layer adds to what the
// generator made, nought being no change: a range painted onto the ranges the
// plates raised.
//
// Stored sparsely, in tiles of kLayerTile texels a side, made when something is
// first painted on them and shared between copies of the map until one of the
// copies is painted again. A copy is how undo and handing the world to the
// generator are paid for, so it has to cost nothing.
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace generation {

enum class LayerId : std::uint8_t {
    Continents,  // PASS G1: the continental shape - where land and sea are
    Ranges,      // the plates: uplift, which the relief pass puts crests on
    Hills,       // PASS G3: the relief noise - how broken the ground is
    Sea,         // the sea level: what share of the ground is under water
    Weathering,  // PASS G5: how many passes of erosion the rock has had
    Rain,        // PASS C3: how wet the country is meant to be
    Water,       // PASS G6: where water is put or kept out - a lake, a river course, dry ground
    Count,
};
inline constexpr std::size_t kLayerCount = static_cast<std::size_t>(LayerId::Count);

enum class LayerKind : std::uint8_t { Override, Delta };

struct LayerDef {
    LayerId id;
    const char* name;       // in files
    const char* label;      // in the interface
    const char* pass;       // the generator pass it feeds, in the interface
    std::int32_t texelMetres;
    LayerKind kind;
    float low, high;        // what a value may be
    float paint;            // what the Paint tool puts down unless told otherwise
    const char* unit;
};

const LayerDef& layerDef(LayerId id);
std::optional<LayerId> layerNamed(std::string_view name);

// Texels a side of a tile.
inline constexpr std::int32_t kLayerTile = 32;
// The widest brush, in texels of the layer it paints, as a radius.
inline constexpr std::int32_t kMaxBrushTexels = 64;

// How many texels of a layer cover `cells` macro cells (rounded up).
std::int32_t layerTexels(LayerId id, std::int32_t cells);

class LayerMap {
public:
    struct Tile {
        std::array<float, kLayerTile * kLayerTile> value{};
        std::array<float, kLayerTile * kLayerTile> cover{};
        bool operator==(const Tile&) const = default;
    };
    // At a point: the painted value premultiplied by its cover, and the cover.
    // What a place becomes is `made * (1 - cover) + painted` on an override
    // layer, and `made + painted` on a delta layer (whose cover is not used).
    struct Sample {
        float painted = 0.0f;
        float cover = 0.0f;
    };

    LayerMap() = default;
    LayerMap(LayerId id, std::int32_t texelsX, std::int32_t texelsY);

    LayerId id() const { return id_; }
    const LayerDef& def() const { return layerDef(id_); }
    std::int32_t texelsX() const { return texelsX_; }
    std::int32_t texelsY() const { return texelsY_; }
    std::int32_t tilesX() const { return tilesX_; }
    std::int32_t tilesY() const { return tilesY_; }
    bool inBounds(std::int32_t tx, std::int32_t ty) const {
        return tx >= 0 && ty >= 0 && tx < texelsX_ && ty < texelsY_;
    }
    // Nothing painted anywhere.
    bool empty() const;
    std::size_t tileCount() const;

    // What was painted at a texel: value and cover. Nought and nought where
    // nothing was. Out of bounds reads as nothing.
    float value(std::int32_t tx, std::int32_t ty) const;
    float cover(std::int32_t tx, std::int32_t ty) const;
    // Written: the tile is made, or unshared from a copy, first.
    void set(std::int32_t tx, std::int32_t ty, float value, float cover);
    // Tiles that hold nothing any more are let go, in a rectangle of texels.
    void prune(std::int32_t tx0, std::int32_t ty0, std::int32_t tx1, std::int32_t ty1);
    void clear();

    // Bilinear, at a point in texel units (texel centres at .5), clamped to the
    // edge. Premultiplied, so painted and unpainted texels blend properly.
    Sample sample(double u, double v) const;
    // What the layer does to a macro cell (x, y): sampled at the cell's centre.
    Sample atCell(std::int32_t cellX, std::int32_t cellY) const;

    // A different extent, keeping every tile that still fits where it was.
    void resize(std::int32_t texelsX, std::int32_t texelsY);

    // For saving and loading: the tiles that exist.
    const Tile* tile(std::int32_t tileX, std::int32_t tileY) const;
    void putTile(std::int32_t tileX, std::int32_t tileY, const Tile& tile);

    // Equal by contents, not by which tiles happen to be shared.
    bool operator==(const LayerMap& other) const;

private:
    Tile& writable(std::int32_t tileX, std::int32_t tileY);
    bool holdsNothing(const Tile& tile) const;

    LayerId id_ = LayerId::Continents;
    std::int32_t texelsX_ = 0, texelsY_ = 0, tilesX_ = 0, tilesY_ = 0;
    std::vector<std::shared_ptr<Tile>> tiles_;
};

// A layer over every macro cell of a world `width` x `height` cells: what
// LayerMap::atCell gives, for all of them at once. `any` is false (and the
// vectors empty) when nothing is painted, so the generator can skip it.
struct LayerCells {
    std::vector<float> painted, cover;
    bool any = false;
    float paintedAt(std::size_t i) const { return any ? painted[i] : 0.0f; }
    float coverAt(std::size_t i) const { return any ? cover[i] : 0.0f; }
    // An override layer over what the generator made.
    double over(std::size_t i, double made) const {
        return any ? made * (1.0 - double(cover[i])) + double(painted[i]) : made;
    }
};
LayerCells layerCells(const LayerMap& map, std::int32_t width, std::int32_t height);

} // namespace generation

