#include "game/generation/plate_field.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "engine/core/rng.hpp"
#include "game/generation/field_noise.hpp"

namespace generation {
namespace {
std::int64_t floorDiv(std::int64_t n, std::int64_t d) { return n / d - (n % d < 0 && n % d != 0); }

using noise::cellKey;
using noise::take;
constexpr auto warpNoise = noise::value;

struct Site {
    double x = 0, y = 0;
    double vx = 0, vy = 0;
    bool oceanic = false;
    std::uint64_t id = 0;
};

Site siteOf(std::uint64_t seed, const PlateFieldSettings& settings, std::int64_t cx,
            std::int64_t cy) {
    auto key = cellKey(seed, cx, cy);
    Site site;
    site.id = key;
    // Jittered rather than random: purely random points clump, and a clump of
    // plate seeds is a shatter zone the size of a country with no plate in it.
    // Held off the cell edge so two seeds cannot end up on top of each other.
    site.x = (double(cx) + 0.2 + take(key) * 0.6) * settings.plateMetres;
    site.y = (double(cy) + 0.2 + take(key) * 0.6) * settings.plateMetres;
    const double angle = take(key) * 6.283185307179586;
    const double speed = 0.35 + take(key) * 0.65;
    site.vx = std::cos(angle) * speed;
    site.vy = std::sin(angle) * speed;
    site.oceanic = take(key) < settings.oceanShare;
    return site;
}

double smoothstep(double edge0, double edge1, double x) {
    if (!(edge1 > edge0)) return x >= edge1 ? 1.0 : 0.0;
    const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
}

PlateField::PlateField(std::uint64_t seed, PlateFieldSettings settings)
    : seed_(core::splitmix64(seed ^ 0x51ED270BD7373BFDull)), settings_(settings) {
    settings_.plateMetres = std::max(1.0, settings_.plateMetres);
    settings_.marginMetres =
            std::clamp(settings_.marginMetres, 1.0, settings_.plateMetres * 0.4);
    settings_.shelfMetres = std::clamp(settings_.shelfMetres, 1.0, settings_.plateMetres * 0.12);
    settings_.oceanShare = std::clamp(settings_.oceanShare, 0.0, 1.0);
    settings_.warpMetres = std::max(0.0, settings_.warpMetres);
    settings_.warpBroadMetres = std::max(1.0, settings_.warpBroadMetres);
    settings_.warpFineMetres = std::max(1.0, settings_.warpFineMetres);
}

void PlateField::drift(std::uint64_t plate, double& vx, double& vy) const {
    // The identity IS the site's key, so the drift comes back out of it without
    // knowing where the plate is.
    auto key = plate;
    take(key);
    take(key);
    const double angle = take(key) * 6.283185307179586;
    const double speed = 0.35 + take(key) * 0.65;
    vx = std::cos(angle) * speed;
    vy = std::sin(angle) * speed;
}

PlateSample PlateField::at(double x, double y) const {
    PlateSample out;
    if (!std::isfinite(x) || !std::isfinite(y)) return out;

    // The lie. Two scales: a broad one that swings whole stretches of margin
    // about, and a fine one that frays the edge itself.
    const double wx = x + settings_.warpMetres *
                                  (warpNoise(seed_ ^ 0x4001, x, y, settings_.warpBroadMetres) +
                                   warpNoise(seed_ ^ 0x4007, x, y, settings_.warpFineMetres) * 0.4);
    const double wy = y + settings_.warpMetres *
                                  (warpNoise(seed_ ^ 0x4013, x, y, settings_.warpBroadMetres) +
                                   warpNoise(seed_ ^ 0x4019, x, y, settings_.warpFineMetres) * 0.4);

    const std::int64_t cx = floorDiv(std::int64_t(std::floor(wx / settings_.plateMetres)), 1);
    const std::int64_t cy = floorDiv(std::int64_t(std::floor(wy / settings_.plateMetres)), 1);

    // Nearest and second nearest over the 5x5 around it. Five rather than three
    // because a seed may sit a whole cell away from its own centre once it is
    // jittered, and a second-nearest that was missed is a boundary in the wrong
    // place - which shows as a margin that stops in the middle of nothing.
    Site around[25];
    double distances[25];
    int count = 0;
    Site best{}, second{};
    double bestDistance = std::numeric_limits<double>::infinity();
    double secondDistance = bestDistance;
    for (std::int64_t j = cy - 2; j <= cy + 2; ++j)
        for (std::int64_t i = cx - 2; i <= cx + 2; ++i) {
            const Site site = siteOf(seed_, settings_, i, j);
            const double dx = wx - site.x, dy = wy - site.y;
            const double distance = std::sqrt(dx * dx + dy * dy);
            around[count] = site;
            distances[count] = distance;
            ++count;
            if (distance < bestDistance) {
                secondDistance = bestDistance;
                second = best;
                bestDistance = distance;
                best = site;
            } else if (distance < secondDistance) {
                secondDistance = distance;
                second = site;
            }
        }
    if (!std::isfinite(secondDistance)) return out;

    out.plate = best.id;
    out.neighbour = second.id;
    out.oceanic = best.oceanic;
    out.neighbourOceanic = second.oceanic;
    // Half the difference of the two distances is the distance to their
    // bisector, exactly where the sites are equidistant and close enough
    // elsewhere. The flood fill this replaces had no such number at all - it
    // counted cells, so a margin's width followed the macro map's resolution
    // rather than the geology.
    out.boundaryMetres = (secondDistance - bestDistance) * 0.5;

    // Convergence along the line between the two, shear across it.
    double nx = second.x - best.x, ny = second.y - best.y;
    const double norm = std::max(1e-9, std::sqrt(nx * nx + ny * ny));
    nx /= norm;
    ny /= norm;
    const double rx = best.vx - second.vx, ry = best.vy - second.vy;
    out.closing = rx * nx + ry * ny;
    out.shear = std::abs(rx * ny - ry * nx);

    // What kind of margin this is, which decides what the stress does rather
    // than how much of it there is.
    if (std::abs(out.closing) <= out.shear * 0.6) {
        out.margin = Margin::Transform;
    } else if (out.closing > 0) {
        if (!best.oceanic && !second.oceanic) out.margin = Margin::Collision;
        else if (!best.oceanic) out.margin = Margin::Cordillera;
        else if (!second.oceanic) out.margin = Margin::Trench;
        else out.margin = Margin::Arc;
    } else {
        out.margin = Margin::Rift;
    }

    // And how much reaches this far inland. Smooth to zero at the margin's
    // reach so nothing downstream has a crease along a contour of it.
    const double reach = 1.0 - smoothstep(0.0, settings_.marginMetres, out.boundaryMetres);
    const double force = std::min(1.0, std::abs(out.closing) + out.shear * 0.35);
    out.stress = reach * reach * force;

    // The crust step, softened over a shelf.
    //
    // Weighted over EVERY nearby plate rather than blended between the nearest
    // two, and that is not fastidiousness - it is what the numbers demanded.
    // Blending towards "the neighbour" makes the answer depend on which plate
    // that is, and which it is changes abruptly along the line equidistant from
    // three of them. Measured: the crust jumped by a quarter at every such
    // line, which is a cliff in the middle of open water.
    //
    // A weight that reaches zero before a site can take over as the neighbour
    // has no such line. Each site counts for as much as it is close to the
    // nearest one, and a site arriving or leaving the neighbourhood arrives and
    // leaves at zero.
    double weighted = 0, total = 0;
    for (int i = 0; i < count; ++i) {
        const double reach = (distances[i] - bestDistance) / settings_.shelfMetres;
        if (reach >= 1.0) continue;
        const double w = 1.0 - smoothstep(0.0, 1.0, reach);
        weighted += w * (around[i].oceanic ? 0.0 : 1.0);
        total += w;
    }
    out.crust = total > 0 ? weighted / total : (best.oceanic ? 0.0 : 1.0);
    return out;
}

} // namespace generation
