#include "game/world/climate_field.hpp"

#include <algorithm>
#include <cmath>

#include "game/generation/world_map_gen.hpp"
#include "game/world/scene_scatter.hpp"

namespace world {
using core::Fixed;

void ClimateField::raise(const generation::WorldMapData& world, const HeightField& field) {
    if (world.width <= 0 || world.height <= 0) return;
    const std::int64_t metres = generation::kMetresPerCell;
    // One sample past the far edge, so a point on the last metre of the map
    // still has four corners to interpolate between.
    wide_ = static_cast<std::int32_t>(world.width * metres / kMetres) + 2;
    high_ = static_cast<std::int32_t>(world.height * metres / kMetres) + 2;
    samples_.assign(static_cast<std::size_t>(wide_) * high_ * kChannels, 0);
    for (std::int32_t row = 0; row < high_; ++row)
        for (std::int32_t column = 0; column < wide_; ++column) {
            const core::WorldPos at{Fixed::fromInt(std::int64_t(column) * kMetres),
                                    Fixed::fromInt(std::int64_t(row) * kMetres)};
            const HeightField::SurfaceClimate climate = field.surfaceClimateAt(at);
            const auto into = (static_cast<std::size_t>(row) * wide_ + column) * kChannels;
            const auto put = [&](std::size_t slot, Fixed unit) {
                const Fixed stored = signedSlot(slot)
                                             ? unit / Fixed::fromInt(kSignedSpan) + Fixed::ratio(1, 2)
                                             : unit;
                samples_[into + slot] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
                        (stored * Fixed::fromInt(255)).roundToInt(), 0, 255));
            };
            for (std::size_t i = 0; i < 4; ++i) put(i, climate.foliage[i]);
            put(4, climate.desert);
            for (std::size_t i = 0; i < 6; ++i) put(5 + i, climate.environment[i]);
            // A low-frequency representation of the SAME seeded scatter field.
            // It survives outside local object residency; no per-frame height queries.
            put(11, climate.woodland * Fixed::fromDoubleForContent(decor::forestDensity(world.seed,
                double(column) * kMetres, double(row) * kMetres)));
            put(12, climate.woodland);
        }
}

std::vector<std::uint8_t> ClimateField::plane(int which) const {
    std::vector<std::uint8_t> out;
    if (!ready() || which < 0 || which >= kPlanes) return out;
    const auto texels = static_cast<std::size_t>(wide_) * high_;
    out.assign(texels * 4, 0);
    for (std::size_t i = 0; i < texels; ++i)
        for (std::size_t c = 0; c < 4; ++c) {
            const std::size_t slot = static_cast<std::size_t>(which) * 4 + c;
            out[i * 4 + c] = samples_[i * kChannels + slot];
        }
    return out;
}

Fixed ClimateField::channel(std::int64_t x, std::int64_t y, std::size_t slot) const {
    const auto column = std::clamp<std::int64_t>(x, 0, wide_ - 1);
    const auto row = std::clamp<std::int64_t>(y, 0, high_ - 1);
    const Fixed stored = Fixed::ratio(
            samples_[(static_cast<std::size_t>(row) * wide_ + column) * kChannels + slot], 255);
    return signedSlot(slot) ? (stored - Fixed::ratio(1, 2)) * Fixed::fromInt(kSignedSpan) : stored;
}

float ClimateField::forestCoverAt(double x,double y) const {
    if (!ready() || !std::isfinite(x+y) || x<0 || y<0 || x>double(wide_-1)*kMetres || y>double(high_-1)*kMetres) return 0;
    x/=kMetres;y/=kMetres;
    const auto ix=std::int64_t(std::floor(x)),iy=std::int64_t(std::floor(y));
    const auto at=[&](auto a,auto b){return channel(a,b,11).toDouble();};
    return float(std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),x-ix),
        std::lerp(at(ix,iy+1),at(ix+1,iy+1),x-ix),y-iy));
}

HeightField::SurfaceClimate ClimateField::at(core::WorldPos where) const {
    HeightField::SurfaceClimate out{};
    if (!ready()) {
        out.foliage[2] = core::kOne;   // temperate, where there is nothing to say
        return out;
    }
    const std::int64_t x = floorDiv(where.x.toInt(), std::int64_t(kMetres));
    const std::int64_t y = floorDiv(where.y.toInt(), std::int64_t(kMetres));
    const Fixed fx = Fixed::ratio(floorMod(where.x.toInt(), std::int64_t(kMetres)), kMetres);
    const Fixed fy = Fixed::ratio(floorMod(where.y.toInt(), std::int64_t(kMetres)), kMetres);
    const auto blended = [&](std::size_t slot) {
        const Fixed top = channel(x, y, slot) + (channel(x + 1, y, slot) - channel(x, y, slot)) * fx;
        const Fixed bottom =
                channel(x, y + 1, slot) + (channel(x + 1, y + 1, slot) - channel(x, y + 1, slot)) * fx;
        return top + (bottom - top) * fy;
    };
    for (std::size_t i = 0; i < 4; ++i) out.foliage[i] = blended(i);
    out.desert = blended(4);
    for (std::size_t i = 0; i < 6; ++i) out.environment[i] = blended(5 + i);
    out.woodland = blended(12);
    return out;
}

} // namespace world
