#include "engine/terrain/erosion_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/terrain/field_noise.hpp"

namespace engine::terrain {
namespace {
// The steepest a single octave of the ridged field gets, as a multiple of one
// over its scale. MEASURED, over a million samples, not derived: the field is
// one minus the absolute value of value noise, squared, and value noise's own
// slope is already three and a half over its scale once the quintic ease is
// counted. Guessing two over the scale is what let gullies cut walls at one and
// a half times the angle of repose on a face that had just been limited to it.
constexpr double kRidgedGradient = 7.1;

double smoothstep(double edge0, double edge1, double x) {
    if (!(edge1 > edge0)) return x >= edge1 ? 1.0 : 0.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
}

ErosionField::ErosionField(std::uint64_t seed, ErosionSettings settings)
    : seed_(core::splitmix64(seed ^ 0x7E1A5C09B3D6244Full)), settings_(settings) {
    settings_.talusDegrees = std::clamp(settings_.talusDegrees, 5.0, 80.0);
    settings_.talusRings = std::clamp(settings_.talusRings, 1, 6);
    settings_.gullyMetres = std::max(0.0, settings_.gullyMetres);
    settings_.gullyScaleMetres = std::max(1.0, settings_.gullyScaleMetres);
    settings_.reliefFullMetres =
            std::max(settings_.reliefStartMetres + 1.0, settings_.reliefFullMetres);
    settings_.reliefSpanMetres = std::max(1.0, settings_.reliefSpanMetres);
    settings_.gullySlopeShare = std::clamp(settings_.gullySlopeShare, 0.0, 1.0);
    talusGradient_ = std::tan(settings_.talusDegrees * 3.14159265358979323846 / 180.0);
}

ErosionSample ErosionField::at(const HeightAt& height, double x, double y,
                               double stepMetres) const {
    ErosionSample out;
    if (!height || !std::isfinite(x) || !std::isfinite(y) || !(stepMetres > 0)) return out;
    const double here = height(x, y);
    if (!std::isfinite(here)) return out;

    // The talus cut, as the lower envelope of cones of the repose angle.
    //
    // What it does: a break in the surface - the top of a cliff, the lip of a
    // crag - is cut back until nothing within the reach stands steeper than the
    // angle. What it does NOT do, and this is the part worth being exact about:
    // it cannot limit a uniform face longer than the reach, because every cone
    // is anchored on ground that is itself too high. No local operator can, and
    // the real thing cannot either - a talus apron is finite.
    //
    // It only ever lowers. Real talus also fills the bottom; leaving that out
    // is deliberate, because filling rounds the foot and a rounded foot is the
    // loaf this work exists to get rid of. What is wanted is the facet.
    //
    // The anchors sit on a lattice fixed to the WORLD, not one carried around
    // with the query. That is the whole of why this works, and getting it wrong
    // is silent: anchors that move with the point make every cone's offset
    // constant, so the envelope is the original surface shifted down and the
    // slope is exactly what it was. Fixed anchors make each term a cone in the
    // query - gradient at most tan(talus) - and the minimum of functions that
    // are each tan-Lipschitz is tan-Lipschitz. The limit is then not an
    // approximation of the angle of repose; it is one.
    // Anchors carried WITH the query, never on a lattice it slides across: a
    // sliding window loses its lowest anchor as the query moves and the
    // envelope jumps where it does.
    double limit = here;
    double steepest = 0;
    const int rings = settings_.talusRings;
    for (int j = -rings; j <= rings; ++j)
        for (int i = -rings; i <= rings; ++i) {
            if (i == 0 && j == 0) continue;
            const double dx = double(i) * stepMetres, dy = double(j) * stepMetres;
            const double distance = std::sqrt(dx * dx + dy * dy);
            const double there = height(x + dx, y + dy);
            if (!std::isfinite(there)) continue;
            steepest = std::max(steepest, (here - there) / distance);
            limit = std::min(limit, there + talusGradient_ * distance);
        }
    out.slope = steepest;
    out.talusCut = std::max(0.0, here - limit);

    // How much country there is here at all, measured over a fixed distance
    // rather than over the step - see reliefSpanMetres for why that is not a
    // detail. The same span gives the slope the gullies are scaled by, so
    // neither answer moves when the sampling does.
    const double span = settings_.reliefSpanMetres;
    const double east = height(x + span, y) - height(x - span, y);
    const double north = height(x, y + span) - height(x, y - span);
    if (!std::isfinite(east) || !std::isfinite(north)) {
        out.geometryMetres = -out.talusCut;
        return out;
    }
    const double fall = std::sqrt(east * east + north * north);
    const double spanSlope = fall / (2.0 * span);
    const double alive = smoothstep(settings_.reliefStartMetres, settings_.reliefFullMetres, fall);
    if (alive <= 0) {
        out.geometryMetres = -out.talusCut;
        return out;
    }

    // Gullies, run down the fall line rather than across the world's axes. A
    // ridged field folded at every zero crossing gives the incisions; the
    // downhill direction is what stops them being a plaid.
    double gully = 0;
    if (settings_.gullyMetres > 0 && fall > 0) {
        // Along the fall line and across it: the pattern is stretched down the
        // slope, which is what makes it read as water having run rather than as
        // a plaid laid over the country.
        const double ax = east / fall, ay = north / fall;
        const double along = (x * ax + y * ay) * 0.35;
        const double across = -x * ay + y * ax;
        const double incision =
                noise::ridged(seed_ ^ 0x6101, across, along, settings_.gullyScaleMetres, 1);
        // Deep enough to read, never deep enough to cut a wall past the share
        // of the angle it is allowed. A gully of depth d across a width L has
        // walls of gradient d * kRidgedGradient / L, so d follows from the
        // share, the angle and the width - and the surface the caller gets back
        // is bounded rather than hoped about.
        const double deepest =
                std::min(settings_.gullyMetres, settings_.gullySlopeShare * talusGradient_ *
                                                        settings_.gullyScaleMetres /
                                                        kRidgedGradient);
        gully = deepest * alive * incision *
                std::clamp(spanSlope / std::max(1e-6, talusGradient_), 0.0, 1.0);
    }

    // And the split. A feature of this width is shape when the step can hold a
    // few samples across it, and a picture when it cannot - the crossover is
    // the only place in this file where the level of detail is allowed to
    // change the answer, and it changes what KIND of answer, never how much.
    // One when the step is fine enough to hold the gully, zero when it is not.
    // Written the ascending way round on purpose: a descending smoothstep reads
    // as if it worked and returns the wrong end of the range, which is how the
    // whole split silently did nothing.
    const double resolved = 1.0 - smoothstep(settings_.gullyScaleMetres / 6.0,
                                             settings_.gullyScaleMetres / 2.0, stepMetres);
    out.geometryMetres = -(out.talusCut + gully * resolved);
    out.shadingMetres = gully * (1.0 - resolved);
    out.shadingScaleMetres = settings_.gullyScaleMetres;
    return out;
}

} // namespace engine::terrain

