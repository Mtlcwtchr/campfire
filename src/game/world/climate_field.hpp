#pragma once
// The climate of the whole world, at sixty-four metres, held once.
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
    // one number for the world.
    static constexpr std::int32_t kMetres = 64;

    ClimateField() = default;
    // Builds it. Walks the whole world once, which is a fifth of a second on
    // the default map, and is done beside the coarse bake.
    void raise(const generation::WorldMapData& world, const HeightField& field);

    [[nodiscard]] bool ready() const { return wide_ > 0; }
    // Between the samples, so that nothing in it steps at a grid line.
    [[nodiscard]] HeightField::SurfaceClimate at(core::WorldPos where) const;
    [[nodiscard]] float forestCoverAt(double x,double y) const;
    [[nodiscard]] std::size_t bytes() const { return samples_.size(); }

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
    //   plane 2: wind x, wind y, drainage, woodland habitat * forest density (64 m)
    static constexpr int kPlanes = 3;
    [[nodiscard]] std::vector<std::uint8_t> plane(int which) const;

private:
    static constexpr std::size_t kChannels = 13;   // 12 GPU channels + CPU woodland habitat
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

    std::int32_t wide_ = 0, high_ = 0;
    std::vector<std::uint8_t> samples_;
};

} // namespace world
