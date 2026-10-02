#include "game/world/terrain_brush.hpp"

#include <algorithm>
#include <array>
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

double brushCellMetres(const Brush& brush) {
    double cell = EditLayer::kSampleMetres;
    while (cell < 64 && cell * 2 <= brush.radiusMetres / 10.0) cell *= 2;
    return cell;
}

bool brushReadsGround(BrushKind kind) {
    return kind == BrushKind::Smooth || kind == BrushKind::Flatten || kind == BrushKind::Thermal ||
           kind == BrushKind::Hydraulic;
}

namespace {
// Uniform cubic B-spline weights: smooth to the second derivative and never
// negative, so a change field laid onto the fine samples through them has no
// crease at the coarse nodes, and a change that only lowers still only lowers.
std::array<double, 4> bspline(double t) {
    const double t2 = t * t, t3 = t2 * t, u = 1 - t;
    return {u * u * u / 6, (3 * t3 - 6 * t2 + 4) / 6, (-3 * t3 + 3 * t2 + 3 * t + 1) / 6, t3 / 6};
}

// Ridged fractal: three octaves of the fold, each half the wavelength and half
// the height of the one before, centred on nought so a stroke roughens the
// ground rather than lifting it.
double ridgedFractal(std::uint64_t seed, double x, double y, double wavelength) {
    double sum = 0, weight = 0, amplitude = 1;
    for (int octave = 0; octave < 3; ++octave) {
        sum += (ridged(seed + std::uint64_t(octave) * 0x9E37ull, x, y, wavelength) - 0.3) * amplitude;
        weight += amplitude;
        amplitude *= 0.5;
        wavelength = std::max(4.0, wavelength * 0.5);
    }
    return sum / weight * 1.6;
}
} // namespace

std::size_t applyBrush(EditLayer& into, const GroundAt& ground, const Brush& brush,
                       core::WorldPos at, double seconds, const EditLayer* existing) {
    if (!ground || !(brush.radiusMetres > 0) || !(seconds > 0)) return 0;
    // At the scale the brush is: four metres to a sample for a brush a few
    // tens of metres across, a quarter of a kilometre for one that lifts a
    // country (EditLayer::levelFor).
    const int level = EditLayer::levelFor(brush.radiusMetres);
    const double step = EditLayer::stepOf(level);
    const auto centreX = at.x.toDouble(), centreY = at.y.toDouble();
    const auto lowX = std::int64_t(std::floor((centreX - brush.radiusMetres) / step));
    const auto lowY = std::int64_t(std::floor((centreY - brush.radiusMetres) / step));
    const auto highX = std::int64_t(std::ceil((centreX + brush.radiusMetres) / step));
    const auto highY = std::int64_t(std::ceil((centreY + brush.radiusMetres) / step));
    const auto heightAt = [&](double x, double y) {
        return ground(core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y))
                .toDouble();
    };

    // --- the brushes that read the ground: worked out at their own scale ------
    //
    // On a lattice of `cell` metres over the brush and two nodes past it, read
    // whole before anything is written: smooth, flatten and both erosions are
    // functions of the ground AROUND a node, and writing as it went would feed
    // a half-moved neighbourhood back into itself (a comet tail pointing
    // whichever way the loop ran).
    const bool reads = brushReadsGround(brush.kind);
    const double cell = reads ? std::max(brushCellMetres(brush), step) : step;
    const auto nodeLowX = std::int64_t(std::floor((centreX - brush.radiusMetres) / cell)) - 2;
    const auto nodeLowY = std::int64_t(std::floor((centreY - brush.radiusMetres) / cell)) - 2;
    const auto nodesX = reads ? std::size_t(std::int64_t(std::ceil((centreX + brush.radiusMetres) / cell)) + 3 - nodeLowX) : 0;
    const auto nodesY = reads ? std::size_t(std::int64_t(std::ceil((centreY + brush.radiusMetres) / cell)) + 3 - nodeLowY) : 0;
    std::vector<double> before(nodesX * nodesY, 0.0), change(nodesX * nodesY, 0.0);
    // Signed, and clamped at both ends. Taking a neighbour with unsigned
    // indices underflows at the edge of the patch and reads the far side of it
    // instead - which is a seam along two sides of every stroke.
    const auto read = [&](std::int64_t i, std::int64_t j) {
        const auto ci = std::clamp<std::int64_t>(i, 0, std::int64_t(nodesX) - 1);
        const auto cj = std::clamp<std::int64_t>(j, 0, std::int64_t(nodesY) - 1);
        return before[std::size_t(cj) * nodesX + std::size_t(ci)];
    };
    if (reads) {
        for (std::size_t j = 0; j < nodesY; ++j)
            for (std::size_t i = 0; i < nodesX; ++i)
                before[j * nodesX + i] = heightAt(double(nodeLowX + std::int64_t(i)) * cell,
                                                  double(nodeLowY + std::int64_t(j)) * cell);
        const double middle = brush.level ? *brush.level : heightAt(centreX, centreY);
        const double talus = std::tan(34.0 * 3.14159265358979323846 / 180.0) * cell;
        const double rate = brush.strength * seconds;
        // Water's work needs to know where the water goes: every node drains
        // to its lowest neighbour, and what drains through a node is the area
        // above it. Highest first, so a node has had everything above it
        // added before it passes its own share on.
        std::vector<double> area;
        std::vector<std::int64_t> receiver;
        if (brush.kind == BrushKind::Hydraulic) {
            area.assign(before.size(), 1.0);
            receiver.assign(before.size(), -1);
            std::vector<std::size_t> order(before.size());
            for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return before[a] > before[b]; });
            for (const std::size_t k : order) {
                const auto i = std::int64_t(k % nodesX), j = std::int64_t(k / nodesX);
                double steepest = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const auto ni = i + dx, nj = j + dy;
                        if (ni < 0 || nj < 0 || ni >= std::int64_t(nodesX) || nj >= std::int64_t(nodesY)) continue;
                        const double drop = (before[k] - read(ni, nj)) / (dx && dy ? 1.41421356 : 1.0);
                        if (drop > steepest) { steepest = drop; receiver[k] = nj * std::int64_t(nodesX) + ni; }
                    }
                if (receiver[k] >= 0) area[std::size_t(receiver[k])] += area[k];
            }
        }
        for (std::size_t j = 0; j < nodesY; ++j)
            for (std::size_t i = 0; i < nodesX; ++i) {
                const auto ii = std::int64_t(i), jj = std::int64_t(j);
                const double here = read(ii, jj);
                double& out = change[j * nodesX + i];
                switch (brush.kind) {
                    case BrushKind::Smooth: {
                        // Towards the mean of the eight around it, by a share
                        // of the way rather than all of it: a smooth that
                        // arrives in one step is a flatten with a new name.
                        double sum = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (dx || dy) sum += read(ii + dx, jj + dy);
                        out = (sum / 8 - here) * std::min(1.0, rate / 6.0);
                        break;
                    }
                    case BrushKind::Flatten: out = (middle - here) * std::min(1.0, rate / 6.0); break;
                    case BrushKind::Thermal: {
                        // The angle of repose, as a lower envelope of cones
                        // over the eight neighbours - the generator's own
                        // talus cut, over a patch. Only ever removes.
                        double limit = here;
                        for (int dy = -1; dy <= 1; ++dy)
                            for (int dx = -1; dx <= 1; ++dx)
                                if (dx || dy)
                                    limit = std::min(limit, read(ii + dx, jj + dy) + talus * (dx && dy ? 1.41421356 : 1.0));
                        out = (limit - here) * std::min(1.0, rate / 4.0);
                        break;
                    }
                    case BrushKind::Hydraulic: {
                        // Stream power: a node is cut by how steeply it drains
                        // and by the square root of the ground draining
                        // through it, never below where it drains to - so the
                        // water finds the lines of the slope and cuts gullies
                        // down them. A little of the cut settles back into the
                        // hollows through the curvature, so the gully has a
                        // floor and not a crack.
                        const std::size_t k = j * nodesX + i;
                        double cut = 0;
                        if (receiver[k] >= 0) {
                            const double drop = here - before[std::size_t(receiver[k])];
                            cut = std::min(drop * 0.45, rate * 0.02 * std::sqrt(area[k]) * drop / cell);
                        }
                        const double laplace = read(ii + 1, jj) + read(ii - 1, jj) + read(ii, jj + 1) +
                                               read(ii, jj - 1) - 4 * here;
                        out = -cut + std::max(0.0, laplace) * 0.1 * std::min(1.0, rate / 4.0);
                        break;
                    }
                    default: break;
                }
            }
    }
    // What the lattice decided, at a fine sample.
    const auto coarseChange = [&](double x, double y) {
        const double u = x / cell - double(nodeLowX), v = y / cell - double(nodeLowY);
        const auto iu = std::int64_t(std::floor(u)), iv = std::int64_t(std::floor(v));
        const auto wu = bspline(u - double(iu)), wv = bspline(v - double(iv));
        double sum = 0;
        for (int b = 0; b < 4; ++b)
            for (int a = 0; a < 4; ++a) {
                const auto ci = std::clamp<std::int64_t>(iu - 1 + a, 0, std::int64_t(nodesX) - 1);
                const auto cj = std::clamp<std::int64_t>(iv - 1 + b, 0, std::int64_t(nodesY) - 1);
                sum += wu[std::size_t(a)] * wv[std::size_t(b)] * change[std::size_t(cj) * nodesX + std::size_t(ci)];
            }
        return sum;
    };

    std::size_t written = 0;
    for (std::int64_t sy = lowY; sy <= highY; ++sy)
        for (std::int64_t sx = lowX; sx <= highX; ++sx) {
            const double x = double(sx) * step, y = double(sy) * step;
            const double weight =
                    falloff(std::hypot(x - centreX, y - centreY), brush.radiusMetres, brush.softness);
            if (weight <= 0) continue;
            double change = 0;
            switch (brush.kind) {
                case BrushKind::Raise: change = brush.strength * seconds; break;
                case BrushKind::Lower: change = -brush.strength * seconds; break;
                case BrushKind::Smooth:
                case BrushKind::Flatten:
                case BrushKind::Thermal:
                case BrushKind::Hydraulic: change = coarseChange(x, y); break;
                case BrushKind::Noise:
                    change = ridgedFractal(brush.seed, x, y, std::max(4.0, brush.scaleMetres)) *
                             brush.strength * seconds;
                    break;
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
                case BrushKind::Restore: {
                    // What the hand put here, taken away by a share of it a
                    // second: the ground goes back to what the generator made,
                    // and the generator's ground is never itself touched.
                    if (!existing) break;
                    // Everything held here, at every level: the hand's work
                    // taken back whatever scale it was done at.
                    const double held = existing->at(Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)).toDouble();
                    if (held == 0) break;
                    change = -held * std::min(1.0, brush.strength * seconds / 4.0);
                    break;
                }
                case BrushKind::Count: break;
            }
            if (change == 0) continue;
            into.add(level, sx, sy, Fixed::fromDoubleForContent(change * weight));
            ++written;
        }
    return written;
}

} // namespace world
