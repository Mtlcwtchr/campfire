#include "game/world/terrain_brush.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/core/rng.hpp"

namespace world {
namespace {
using core::Fixed;

// Squared cosine, which is the falloff that leaves no rim.
//
// A linear brush leaves a visible cone edge and a gaussian never quite stops,
// so its edge lands wherever the cutoff was put. This is one at the middle,
// nought at the radius, and flat at both - the ground leaves and rejoins the
// stroke without a crease either side.
double falloff(double distance, double radius, double softness) {
    if (!(radius > 0) || distance >= radius) return 0;
    const double hard = std::clamp(1.0 - softness, 0.0, 1.0);
    const double t = distance / radius;
    if (t <= hard) return 1.0;
    const double edge = (t - hard) / std::max(1e-6, 1.0 - hard);
    const double eased = std::cos(edge * 1.5707963267948966);
    return eased * eased;
}

// Ridged value noise, so the Noise brush lays down ground rather than fuzz.
// The same fold the generator uses: one minus the absolute value creases at
// every zero crossing, and a crease is what a ridge is.
double ridged(std::uint64_t seed, double x, double y, double wavelength) {
    const double sx = x / wavelength, sy = y / wavelength;
    const auto ix = std::int64_t(std::floor(sx)), iy = std::int64_t(std::floor(sy));
    const double fx = sx - double(ix), fy = sy - double(iy);
    const auto corner = [&](std::int64_t cx, std::int64_t cy) {
        auto h = core::splitmix64(seed ^ (std::uint64_t(std::uint32_t(cx)) * 0x9e3779b97f4a7c15ull));
        h = core::splitmix64(h ^ (std::uint64_t(std::uint32_t(cy)) * 0xc2b2ae3d27d4eb4full));
        return double(h & 0xffff) / 65535.0 * 2.0 - 1.0;
    };
    const auto ease = [](double t) { return t * t * (3 - 2 * t); };
    const double ex = ease(fx), ey = ease(fy);
    const double top = corner(ix, iy) + (corner(ix + 1, iy) - corner(ix, iy)) * ex;
    const double bottom = corner(ix, iy + 1) + (corner(ix + 1, iy + 1) - corner(ix, iy + 1)) * ex;
    const double n = top + (bottom - top) * ey;
    const double q = 1.0 - std::abs(n);
    return q * q;
}
}

std::size_t applyBrush(EditLayer& into, const GroundAt& ground, const Brush& brush,
                       core::WorldPos at, double seconds) {
    if (!ground || !(brush.radiusMetres > 0) || !(seconds > 0)) return 0;
    const double step = EditLayer::kSampleMetres;
    const auto centreX = at.x.toDouble(), centreY = at.y.toDouble();
    const auto lowX = std::int64_t(std::floor((centreX - brush.radiusMetres) / step));
    const auto lowY = std::int64_t(std::floor((centreY - brush.radiusMetres) / step));
    const auto highX = std::int64_t(std::ceil((centreX + brush.radiusMetres) / step));
    const auto highY = std::int64_t(std::ceil((centreY + brush.radiusMetres) / step));
    const auto heightAt = [&](double x, double y) {
        return ground(core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y))
                .toDouble();
    };

    // Read the whole patch before writing any of it.
    //
    // Smooth, flatten and both erosions are functions of the ground AROUND a
    // sample, so writing as it goes would feed a half-moved neighbourhood back
    // into itself: the stroke would run downhill across the patch and leave a
    // comet tail pointing whichever way the loop happened to iterate.
    const auto wide = std::size_t(highX - lowX + 1), high = std::size_t(highY - lowY + 1);
    std::vector<double> before(wide * high, 0.0);
    for (std::size_t j = 0; j < high; ++j)
        for (std::size_t i = 0; i < wide; ++i)
            before[j * wide + i] =
                    heightAt(double(lowX + std::int64_t(i)) * step,
                             double(lowY + std::int64_t(j)) * step);
    // Signed, and clamped at both ends. Taking a neighbour with unsigned
    // indices underflows at the edge of the patch and reads the far side of it
    // instead - which is a seam along two sides of every stroke.
    const auto read = [&](std::int64_t i, std::int64_t j) {
        const auto ci = std::clamp<std::int64_t>(i, 0, std::int64_t(wide) - 1);
        const auto cj = std::clamp<std::int64_t>(j, 0, std::int64_t(high) - 1);
        return before[std::size_t(cj) * wide + std::size_t(ci)];
    };

    const double middle = heightAt(centreX, centreY);
    const double talus = std::tan(34.0 * 3.14159265358979323846 / 180.0) * step;
    std::size_t written = 0;

    for (std::size_t j = 0; j < high; ++j)
        for (std::size_t i = 0; i < wide; ++i) {
            const auto sx = lowX + std::int64_t(i), sy = lowY + std::int64_t(j);
            const double x = double(sx) * step, y = double(sy) * step;
            const double weight =
                    falloff(std::hypot(x - centreX, y - centreY), brush.radiusMetres,
                            brush.softness);
            if (weight <= 0) continue;
            const double here = read(std::int64_t(i), std::int64_t(j));
            double change = 0;
            switch (brush.kind) {
                case BrushKind::Raise: change = brush.strength * seconds; break;
                case BrushKind::Lower: change = -brush.strength * seconds; break;
                case BrushKind::Smooth: {
                    // Towards the mean of the eight around it, by a share of
                    // the way rather than all of it: a smooth that arrives in
                    // one step is a flatten with a different name.
                    double sum = 0;
                    int seen = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            sum += read(std::int64_t(i) + dx, std::int64_t(j) + dy);
                            ++seen;
                        }
                    change = (sum / seen - here) * std::min(1.0, brush.strength * seconds / 6.0);
                    break;
                }
                case BrushKind::Flatten:
                    change = (middle - here) * std::min(1.0, brush.strength * seconds / 6.0);
                    break;
                case BrushKind::Noise:
                    change = (ridged(brush.seed, x, y, std::max(4.0, brush.scaleMetres)) - 0.25) *
                             brush.strength * seconds;
                    break;
                case BrushKind::Thermal: {
                    // The angle of repose, as a lower envelope of cones over
                    // the eight neighbours - the generator's own talus cut, run
                    // over a patch. Only ever removes.
                    double limit = here;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (!dx && !dy) continue;
                            const double reach = talus * (dx && dy ? 1.41421356 : 1.0);
                            limit = std::min(limit, read(std::int64_t(i) + dx,
                                                         std::int64_t(j) + dy) + reach);
                        }
                    change = (limit - here) * std::min(1.0, brush.strength * seconds / 4.0);
                    break;
                }
                case BrushKind::Hydraulic: {
                    // Water takes from the steep and leaves it lower down. The
                    // curvature says which is which: convex ground - a shoulder
                    // or a spur - sheds, concave ground - a hollow - fills.
                    const double laplace =
                            read(std::int64_t(i) + 1, std::int64_t(j)) +
                            read(std::int64_t(i) - 1, std::int64_t(j)) +
                            read(std::int64_t(i), std::int64_t(j) + 1) +
                            read(std::int64_t(i), std::int64_t(j) - 1) - 4 * here;
                    change = laplace * 0.25 * std::min(1.0, brush.strength * seconds / 4.0);
                    break;
                }
                case BrushKind::Carve: {
                    // A channel: a rounded trough of the given width, cut to a
                    // depth that follows it. Rounded rather than V, because a
                    // river bed is a bed and not a crack.
                    const double half = std::max(step, brush.scaleMetres * 0.5);
                    const double across = std::hypot(x - centreX, y - centreY) / half;
                    if (across >= 1) break;
                    const double profile = std::cos(across * 1.5707963267948966);
                    change = -brush.strength * seconds * profile * profile;
                    break;
                }
                case BrushKind::Count: break;
            }
            if (change == 0) continue;
            into.add(sx, sy, Fixed::fromDoubleForContent(change * weight));
            ++written;
        }
    return written;
}

} // namespace world
