#include "game/generation/range_field.hpp"

#include <algorithm>
#include <cmath>

#include "game/generation/field_noise.hpp"

namespace generation {
namespace {
double smoothstep(double edge0, double edge1, double x) {
    if (!(edge1 > edge0)) return x >= edge1 ? 1.0 : 0.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// The section across a range: triangular, with only the foot rounded.
//
// `q * q` - what the old crest used - has zero slope at the summit, and zero
// slope at a summit is a dome. It is why the mountains in this world look like
// loaves whatever is done to them afterwards. Linear in distance puts the
// steepest ground just below the crest, which is where it is on a mountain;
// easing only the last quarter keeps the foot from being a crease where the
// range meets the plain.
double section(double distance, double halfWidth) {
    if (!(halfWidth > 0)) return 0;
    const double t = std::clamp(1.0 - std::abs(distance) / halfWidth, 0.0, 1.0);
    return t * smoothstep(0.0, 0.25, t);
}
}

RangeField::RangeField(std::uint64_t seed, const PlateField& plates, RangeSettings settings)
    : seed_(core::splitmix64(seed ^ 0x2A17D9C3F1B45E07ull)), plates_(&plates),
      settings_(settings) {
    settings_.halfWidthMetres = std::max(1.0, settings_.halfWidthMetres);
    settings_.widthVariation = std::clamp(settings_.widthVariation, 0.0, 0.9);
    settings_.wanderMetres = std::max(0.0, settings_.wanderMetres);
    settings_.wanderBroadMetres = std::max(1.0, settings_.wanderBroadMetres);
    settings_.wanderFineMetres = std::max(1.0, settings_.wanderFineMetres);
    settings_.crestScaleMetres = std::max(1.0, settings_.crestScaleMetres);
    settings_.crestOctaves = std::clamp(settings_.crestOctaves, 1, 10);
    settings_.crestShare = std::clamp(settings_.crestShare, 0.0, 1.0);
}

RangeSample RangeField::at(double x, double y) const {
    RangeSample out;
    if (plates_ == nullptr || !std::isfinite(x) || !std::isfinite(y)) return out;
    const PlateSample plate = plates_->at(x, y);
    if (plate.margin == Margin::None || plate.stress <= 0) {
        out.crestMetres = plate.boundaryMetres;
        return out;
    }

    // Where the crest actually runs. A bisector is a diagram; a range that
    // follows one reads as a diagram however good everything above it is.
    const double wander =
            settings_.wanderMetres *
            (noise::value(seed_ ^ 0x5101, x, y, settings_.wanderBroadMetres) +
             noise::value(seed_ ^ 0x5107, x, y, settings_.wanderFineMetres) * 0.45);
    out.crestMetres = std::abs(plate.boundaryMetres + wander);

    // And how wide it is here. A range of constant width is a wall.
    const double halfWidth =
            settings_.halfWidthMetres *
            (1.0 + settings_.widthVariation *
                           noise::value(seed_ ^ 0x5113, x, y, settings_.wanderBroadMetres * 0.8));
    out.flank = section(out.crestMetres, std::max(1.0, halfWidth));
    if (out.flank <= 0) return out;

    double peak = 0;
    switch (plate.margin) {
        case Margin::Collision: peak = settings_.collisionMetres; break;
        case Margin::Cordillera: peak = settings_.cordilleraMetres; break;
        case Margin::Arc: peak = settings_.arcMetres; break;
        case Margin::Trench: peak = settings_.trenchMetres; break;
        // A rift on land is a valley; the same opening on the sea floor builds
        // a ridge, because there is no crust above it to drop.
        case Margin::Rift:
            peak = plate.oceanic && plate.neighbourOceanic ? settings_.oceanRidgeMetres
                                                           : settings_.riftMetres;
            break;
        default: return out;
    }

    // Peaks, passes and spurs. Ridged rather than summed: summed noise is
    // smooth everywhere and would round off the section that was just
    // sharpened, which is how every attempt at this ends up as a row of domes.
    const double crest =
            noise::ridged(seed_ ^ 0x5119, x, y, settings_.crestScaleMetres, settings_.crestOctaves);
    // A pass is lower, not a gap: the section keeps its share whatever the
    // ridged field says, so the range stays continuous along its length.
    const double along = (1.0 - settings_.crestShare) + settings_.crestShare * crest;
    out.metres = peak * plate.stress * out.flank * along;
    return out;
}

} // namespace generation
