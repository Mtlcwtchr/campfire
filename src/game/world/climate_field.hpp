#pragma once
// The climate of the whole world, at 256-512 metres, held once.
//
// Climate is a macro quantity. It comes off cells five hundred and forty
// metres across and nothing in it moves in less than a few hundred: the four
// community weights, the desert cover, temperature, fertility, moisture, the
// wind and the drainage. Sixty-four metres to the sample is already finer than
// anything it does.
//
// It used to be worked out per mesh vertex - two hundred and twenty-nine
// nanoseconds an answer that is the same over a hundred metres - and then, to
// stop that, baked into a channel of every page. A page is the wrong home for
// it: there are ten thousand pages to a world and each carried its own copy of
// a field that is one field, at a resolution far beyond what it holds, and the
// same square of country was baked again at every level.
//
// One grid over the world instead. Eight hundred and ten samples a side on the
// default world, thirteen bytes each - nine megabytes, and thirty-five for the
// largest. Held whole, read by interpolation, never streamed and never
// evicted, because at that size streaming it would cost more than keeping it.

#include <array>
#include <cstdint>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/height_field.hpp"

namespace generation { struct WorldMapData; }

namespace world {

class ClimateField {
public:
    // How far apart the samples are. Not a page's business and not a level's:
    // one number for the world. The world spec (hybrid_authored_procedural_
    // living_world_spec, "Climate Bias ... 256-512 m") holds climate at two
    // hundred and fifty-six to five hundred and twelve metres, and so does
    // this: 256 m, 512 m once a world is past kFineSamples at 256, coarser
    // still only if even that would not fit kSampleBudget. It was sixty-four
    // metres whatever the world - a field older than the spec, sized for the
    // shaders rather than for what climate is - and the largest preset wanted
    // a billion samples, thirteen gigabytes, before a frame could be drawn.
    static constexpr std::int32_t kFinestMetres = 256;
    static constexpr std::size_t kFineSamples = 4u * 1024 * 1024 + 16384;   // four regions a side at 256 m
    static constexpr std::size_t kSampleBudget = 16u * 1024 * 1024 + 32768;
    [[nodiscard]] std::int32_t metres() const { return metres_; }

    ClimateField() = default;
    // Builds it. Walks the whole world once, which is a fifth of a second on
    // the default map, and is done beside the coarse bake.
    void raise(const generation::WorldMapData& world, const HeightField& field);

    [[nodiscard]] bool ready() const { return wide_ > 0; }
    // Between the samples, so that nothing in it steps at a grid line.
    [[nodiscard]] HeightField::SurfaceClimate at(core::WorldPos where) const;
    [[nodiscard]] float forestCoverAt(double x,double y) const;
    // What is held: the distinct chunks and the table that points at them.
    [[nodiscard]] std::size_t bytes() const {
        return pool_.size() + table_.size() * sizeof(table_[0]);
    }
    [[nodiscard]] std::size_t chunksHeld() const { return pool_.size() / kChunkBytes; }

    [[nodiscard]] std::int32_t wide() const { return wide_; }
    [[nodiscard]] std::int32_t high() const { return high_; }

    // The same field, laid out the way the card wants it.
    //
    // Climate plus forest cover occupy three four-channel pictures, and
    // a picture is what this is: a value per place, read by interpolation, the
    // same for every level and every frame. Carried on the vertex it was
    // fifty-two bytes a corner repeated over a hundred and sixty thousand
    // corners to the square kilometre, for a field whose samples are sixty-four
    // metres apart. One texel here serves two hundred and fifty-six vertices of
    // a four-metre mesh.
    //
    //   plane 0: steppe, boreal, temperate, tropical
    //   plane 1: desert, temperature, fertility, moisture
    //   plane 2: wind x, wind y, drainage, woodland habitat * forest density
    static constexpr int kPlanes = 3;
    [[nodiscard]] std::vector<std::uint8_t> plane(int which) const;

    // The terrain categories (engine/biomes), the fourth picture: four raw
    // ids a sample - the ground's category, the forest, water and decor
    // biomes (0: as the category says) - read nearest, never interpolated.
    // From the import's categorical layers where it painted them; elsewhere
    // the registry's derive rules over the climate (a generated world's
    // deserts), else nought, the engine's own ground.
    [[nodiscard]] std::vector<std::uint8_t> categoryPlane() const;
    // The ids of the sample nearest a point (metres).
    [[nodiscard]] std::array<std::uint8_t, 4> categoriesAt(double x, double y) const;
    // Whether any sample names anything but nought.
    [[nodiscard]] bool anyCategory() const { return anyCategory_; }

private:
    static constexpr std::size_t kChannels = 17;   // 12 GPU channels + CPU woodland habitat + 4 category ids
    static constexpr std::size_t kFirstCategory = 13;
    // Wind blows west and south as readily as east and north, so its two
    // channels are stored about a half rather than from nought - which is what
    // the first cut of this field got wrong, clamping every westward wind in
    // the world to none at all.
    static constexpr std::size_t kFirstSigned = 8, kLastSigned = 9;
    static constexpr int kSignedSpan = 4;   // metres per second, either way
    [[nodiscard]] static bool signedSlot(std::size_t slot) {
        return slot >= kFirstSigned && slot <= kLastSigned;
    }
    [[nodiscard]] core::Fixed channel(std::int64_t x, std::int64_t y, std::size_t slot) const;
    [[nodiscard]] std::uint8_t sample(std::int64_t column, std::int64_t row, std::size_t slot) const {
        const auto chunk = table_[static_cast<std::size_t>(row / kChunk) * chunksWide_ + std::size_t(column / kChunk)];
        return pool_[std::size_t(chunk) * kChunkBytes +
                     (static_cast<std::size_t>(row % kChunk) * kChunk + std::size_t(column % kChunk)) * kChannels + slot];
    }
    std::int32_t metres_ = kFinestMetres;
    bool anyCategory_ = false;

    std::int32_t wide_ = 0, high_ = 0;
    // The samples in chunks of kChunk x kChunk, each distinct chunk held once.
    //
    // Most of a world is open sea, and the sea's climate changes with latitude
    // and nothing else: across a row of the reference world every sea chunk is
    // the same bytes. Held densely that was seventy-three megabytes for a
    // world with no land on it; here it is a chunk per latitude band. Exact:
    // a chunk is shared only when every byte of it is the same, so every
    // sample reads what it would have read from the dense grid.
    static constexpr std::int32_t kChunk = 32;
    static constexpr std::size_t kChunkBytes = std::size_t(kChunk) * kChunk * kChannels;
    std::int32_t chunksWide_ = 0;
    std::vector<std::uint32_t> table_;   // chunk -> its place in pool_, in chunks
    std::vector<std::uint8_t> pool_;     // distinct chunks, kChunkBytes each
};

} // namespace world
