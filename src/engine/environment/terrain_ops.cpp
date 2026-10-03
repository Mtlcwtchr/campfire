#include "engine/environment/terrain_ops.hpp"

#include <algorithm>
#include <cmath>

#include "engine/environment/random.hpp"

namespace engine::environment {

using core::kOne;
using core::kZero;

namespace {

std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

Fixed unitHash(std::uint64_t h) { return Fixed::fromRaw(Fixed::Raw(h & 0xffffffffULL)); }

// (1 - u^2)^2 inside, 0 outside: a bowl with a soft rim and a flat floor's
// worth of roundness at the bottom.
Fixed bowl(Fixed u) {
    if (u.raw < 0) u = -u;
    if (u >= kOne) return kZero;
    const Fixed a = kOne - u * u;
    return a * a;
}

// u^e for e = whole + part, by multiplication and a blend to the next power.
Fixed power(Fixed u, std::int32_t whole, Fixed part) {
    Fixed lo = kOne;
    for (std::int32_t i = 0; i < whole; ++i) lo = lo * u;
    if (part.raw == 0) return lo;
    return core::lerp(lo, lo * u, part);
}

Fixed content(double v) { return Fixed::fromDoubleForContent(v); }

// How much an operation is softened by the time that has passed over it.
double ageWidth(FeatureAge age) {
    switch (age) {
        case FeatureAge::Geological: return 1.35;
        case FeatureAge::Ancient: return 1.12;
        case FeatureAge::Recent: return 0.92;
        case FeatureAge::Cataclysmic: return 0.8;
    }
    return 1.0;
}

} // namespace

Fixed fixedSmoothstep(Fixed edge0, Fixed edge1, Fixed x) {
    if (edge1.raw == edge0.raw) return x >= edge1 ? kOne : kZero;
    const Fixed t = core::saturate((x - edge0) / (edge1 - edge0));
    return t * t * (Fixed::fromInt(3) - t * 2);
}

Fixed fixedNoise(std::uint64_t seed, Fixed x, Fixed y, std::int64_t cellMetres) {
    const Fixed::Raw cell = Fixed::Raw(cellMetres) << Fixed::kFracBits;
    const std::int64_t cx = floorDiv(x.raw, cell), cy = floorDiv(y.raw, cell);
    Fixed fx = Fixed::fromRaw(x.raw - cx * cell) / cellMetres;
    Fixed fy = Fixed::fromRaw(y.raw - cy * cell) / cellMetres;
    fx = fx * fx * (Fixed::fromInt(3) - fx * 2);
    fy = fy * fy * (Fixed::fromInt(3) - fy * 2);
    const Fixed a = unitHash(hashOf(seed, cx, cy)), b = unitHash(hashOf(seed, cx + 1, cy));
    const Fixed c = unitHash(hashOf(seed, cx, cy + 1)), d = unitHash(hashOf(seed, cx + 1, cy + 1));
    const Fixed top = core::lerp(a, b, fx), bottom = core::lerp(c, d, fx);
    return core::lerp(top, bottom, fy);
}

void FixedSpline::finish() {
    cumulative.assign(points.size(), kZero);
    length = kZero;
    for (std::size_t i = 1; i < points.size(); ++i) {
        length += core::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);
        cumulative[i] = length;
    }
}

FixedSpline::Nearest FixedSpline::nearest(WorldPos p) const {
    Nearest best;
    bool found = false;
    std::size_t bestSeg = 0;
    Fixed bestT{};
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        const WorldPos a = points[i], b = points[i + 1];
        const Fixed dx = b.x - a.x, dy = b.y - a.y;
        const Fixed len2 = dx * dx + dy * dy;
        Fixed t = kZero;
        if (len2.raw > 0) t = core::saturate(((p.x - a.x) * dx + (p.y - a.y) * dy) / len2);
        const Fixed qx = a.x + dx * t, qy = a.y + dy * t;
        const Fixed dist = core::hypot(p.x - qx, p.y - qy);
        if (!found || dist < best.distance) {
            found = true;
            best.distance = dist;
            best.at = {qx, qy};
            bestSeg = i;
            bestT = t;
        }
    }
    if (!found) return best;
    const WorldPos a = points[bestSeg], b = points[bestSeg + 1];
    const Fixed dx = b.x - a.x, dy = b.y - a.y;
    const Fixed len = core::hypot(dx, dy);
    if (len.raw > 0) {
        best.tangent = {dx / len, dy / len};
        best.side = (dx * (p.y - a.y) - dy * (p.x - a.x)) / len;
    }
    best.along = cumulative[bestSeg] + len * bestT;
    // Turning at either end of the segment, blended along it.
    const auto turning = [&](std::size_t vertex) -> Fixed {
        if (vertex == 0 || vertex + 1 >= points.size()) return kZero;
        const WorldPos p0 = points[vertex - 1], p1 = points[vertex], p2 = points[vertex + 1];
        const Fixed ax = p1.x - p0.x, ay = p1.y - p0.y, bx = p2.x - p1.x, by = p2.y - p1.y;
        const Fixed la = core::hypot(ax, ay), lb = core::hypot(bx, by);
        if (la.raw == 0 || lb.raw == 0) return kZero;
        const Fixed sinTurn = (ax / la) * (by / lb) - (ay / la) * (bx / lb);
        return sinTurn / ((la + lb) / 2);
    };
    best.curvature = core::lerp(turning(bestSeg), turning(bestSeg + 1), bestT);
    return best;
}

WorldPos FixedSpline::pointAt(Fixed along) const {
    if (points.empty()) return {};
    if (along.raw <= 0) return points.front();
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (cumulative[i] >= along) {
            const Fixed seg = cumulative[i] - cumulative[i - 1];
            const Fixed t = seg.raw > 0 ? (along - cumulative[i - 1]) / seg : kZero;
            return {core::lerp(points[i - 1].x, points[i].x, t), core::lerp(points[i - 1].y, points[i].y, t)};
        }
    }
    return points.back();
}

core::WorldRect FixedSpline::bounds(Fixed margin) const {
    core::WorldRect r{points.front(), points.front()};
    for (const auto& p : points) {
        r.min.x = core::min(r.min.x, p.x); r.min.y = core::min(r.min.y, p.y);
        r.max.x = core::max(r.max.x, p.x); r.max.y = core::max(r.max.y, p.y);
    }
    r.min.x -= margin; r.min.y -= margin; r.max.x += margin; r.max.y += margin;
    return r;
}

double FeatureInstance::yaw() const { return std::atan2(yawSin.toDouble(), yawCos.toDouble()); }

std::vector<CompiledOp> compileOps(const FeatureRecipe& recipe) {
    std::vector<CompiledOp> out;
    const double soften = ageWidth(recipe.age);
    for (const auto& op : recipe.terrain) {
        CompiledOp c;
        c.kind = op.kind;
        c.outerHalf = content(op.outerWidth * 0.5 * soften);
        c.outerDepth = content(op.outerDepth);
        c.innerHalf = content(op.innerWidth * 0.5 * std::sqrt(soften));
        c.innerDepth = content(op.innerDepth);
        c.waterHalf = content(op.waterWidth * 0.5);
        c.asymmetry = content(std::clamp(op.asymmetry, 0.0, 1.0));
        c.taper = content(std::max(op.taper, 0.0));
        c.height = content(op.height);
        c.halfWidth = content(std::max(0.5, op.width * 0.5 * soften));
        c.breaks = content(std::clamp(op.breaks, 0.0, 1.0));
        c.amphitheatre = content(op.amphitheatre);
        c.radiusA = content(std::max(0.5, op.radiusA));
        c.radiusB = content(std::max(0.5, op.radiusB));
        c.edgeNoise = content(std::clamp(op.edgeNoise, 0.0, 0.9));
        c.blend = content(std::max(0.5, op.blend * soften));
        const double e = std::clamp(op.exponent, 0.25, 16.0);
        c.exponentWhole = std::int32_t(std::floor(e));
        c.exponentPart = content(e - std::floor(e));
        c.terraceStep = content(std::max(0.25, op.terraceStep));
        c.terraceSharpness = content(std::clamp(op.terraceSharpness, 0.0, 0.95));
        c.harden = op.harden;
        out.push_back(c);
    }
    return out;
}

namespace {

// The weight along a spline: nothing past its ends, fading in over `taper`.
Fixed endTaper(const FixedSpline& s, Fixed along, Fixed taper) {
    if (taper.raw <= 0) return kOne;
    return core::min(fixedSmoothstep(kZero, taper, along), fixedSmoothstep(kZero, taper, s.length - along));
}

// Where a point falls in an instance's ellipse: 0 at the centre, 1 on the rim.
Fixed ellipseU(const CompiledOp& op, const FeatureInstance& in, WorldPos p) {
    const Fixed dx = p.x - in.anchor.x, dy = p.y - in.anchor.y;
    const Fixed lx = dx * in.yawCos + dy * in.yawSin;
    const Fixed ly = -dx * in.yawSin + dy * in.yawCos;
    const Fixed ra = op.radiusA * in.scale, rb = op.radiusB * in.scale;
    Fixed u = core::hypot(lx / ra, ly / rb);
    if (op.edgeNoise.raw > 0) {
        const auto cell = std::max<std::int64_t>(2, (core::min(ra, rb) / 2).toInt());
        const Fixed n = fixedNoise(in.seed, p.x, p.y, cell);
        u = u / (kOne + op.edgeNoise * (n * 2 - kOne));
    }
    return u;
}

} // namespace

void applyOps(std::span<const CompiledOp> ops, const FeatureInstance& in, WorldPos p, Fixed base,
              std::int64_t strideMetres, OpTotals& totals, OpGeometry* geo) {
    const Fixed stride = Fixed::fromInt(strideMetres);
    for (const auto& op : ops) {
        switch (op.kind) {
            case TerrainOpKind::CarveProfile: {
                if (in.spline.empty()) break;
                const Fixed outer = op.outerHalf * in.scale;
                // A valley narrower than the samples cannot be cut by them.
                if (outer * 2 < stride) break;
                const auto n = in.spline.nearest(p);
                if (n.distance > outer + op.taper) break;
                // Asymmetry: the bank on the outside of a bend is cut steeper,
                // and every valley leans a little one way even where it runs straight.
                Fixed bias = core::clamp(n.curvature * 30, -kOne, kOne);
                if (n.side.raw > 0) bias = -bias;
                bias = core::clamp(bias + Fixed::ratio(3, 10) * (n.side.raw > 0 ? in.hand : -in.hand), -kOne, kOne);
                const Fixed f = kOne - op.asymmetry * bias / 2;
                const Fixed taper = endTaper(in.spline, n.along, op.taper);
                const Fixed outerHere = outer * f, innerHere = op.innerHalf * in.scale * f;
                Fixed depth = op.outerDepth * in.scale * bowl(n.distance / outerHere);
                if (innerHere * 2 >= stride && innerHere.raw > 0)
                    depth += op.innerDepth * in.scale * bowl(n.distance / innerHere);
                depth = depth * taper;
                totals.cut = core::min(totals.cut, -depth);
                if (geo) {
                    geo->any = true;
                    geo->footprint = std::max(geo->footprint, float((taper * (kOne - core::saturate(n.distance / outerHere))).toDouble()));
                    geo->distance = std::min(geo->distance, float(n.distance.toDouble()));
                    geo->side = float(n.side.toDouble());
                    geo->along = float(n.along.toDouble());
                    geo->bedHalf = float(innerHere.toDouble());
                    geo->bankHalf = float(outerHere.toDouble());
                    geo->waterHalf = float((op.waterHalf * in.scale).toDouble());
                }
                break;
            }
            case TerrainOpKind::Step: {
                if (in.spline.empty()) break;
                const Fixed half = op.halfWidth * in.scale;
                const Fixed reach = half + op.amphitheatre + op.blend * in.scale;
                const auto n = in.spline.nearest(p);
                if (n.distance > reach * 2) break;
                const Fixed taper = endTaper(in.spline, n.along, op.blend * in.scale);
                // Bays bitten back into the face, and a rise that breaks along it.
                const Fixed bay = op.amphitheatre * fixedNoise(in.seed ^ 0xba7ULL, n.along, kZero, 60);
                const Fixed s = n.side + bay;
                const Fixed broken = kOne - op.breaks * fixedNoise(in.seed ^ 0xb4eULL, n.along, kZero, 40);
                const Fixed h = op.height * in.scale * broken;
                // A band steepened into a face: the high side up by half the rise,
                // the low side down by half, both easing back over `blend`.
                const Fixed sideFade = kOne - fixedSmoothstep(half, reach * 2, core::abs(s));
                const Fixed delta = h * (fixedSmoothstep(-half, half, s) - Fixed::ratio(1, 2)) * sideFade * taper;
                totals.add += delta;
                if (geo) {
                    geo->any = true;
                    const Fixed face = kOne - core::saturate(core::abs(s) / half);
                    geo->footprint = std::max(geo->footprint, float((face * taper).toDouble()));
                    geo->distance = std::min(geo->distance, float(n.distance.toDouble()));
                    geo->side = float(s.toDouble());
                    geo->along = float(n.along.toDouble());
                    geo->bankHalf = float(half.toDouble());
                    geo->rock = std::max(geo->rock, float((face * taper).toDouble()));
                }
                break;
            }
            case TerrainOpKind::Raise:
            case TerrainOpKind::Depress:
            case TerrainOpKind::Terrace:
            case TerrainOpKind::SmoothTo: {
                const Fixed u = ellipseU(op, in, p);
                const Fixed blendU = op.blend / (core::min(op.radiusA, op.radiusB) * in.scale);
                if (u > kOne + blendU) break;
                // The profile over the ellipse and its blend, eased to nothing
                // across the rim so a flat-topped form still has a soft edge.
                const Fixed uu = core::min(u / (kOne + blendU), kOne);
                Fixed w = (kOne - power(uu, op.exponentWhole, op.exponentPart)) *
                          (kOne - fixedSmoothstep(kOne - blendU, kOne + blendU, u));
                w = core::saturate(w);
                if (op.kind == TerrainOpKind::Raise) {
                    totals.lift = core::max(totals.lift, op.height * in.scale * w);
                } else if (op.kind == TerrainOpKind::Depress) {
                    totals.cut = core::min(totals.cut, -op.height * in.scale * w);
                } else if (op.kind == TerrainOpKind::Terrace) {
                    const Fixed rel = base - in.anchorHeight;
                    const Fixed step = op.terraceStep * in.scale;
                    const Fixed q = rel / step;
                    const Fixed whole = Fixed::fromInt(q.toInt());
                    Fixed t = core::saturate((q - whole - op.terraceSharpness) / (kOne - op.terraceSharpness));
                    t = t * t * (Fixed::fromInt(3) - t * 2);
                    const Fixed terraced = (whole + t) * step;
                    totals.add += (terraced - rel) * w;
                } else {
                    totals.add += (in.anchorHeight - base) * w;
                }
                if (geo) {
                    geo->any = true;
                    geo->footprint = std::max(geo->footprint, float(w.toDouble()));
                    geo->distance = std::min(geo->distance, float((u * op.radiusA * in.scale).toDouble()));
                    if (op.harden) geo->rock = std::max(geo->rock, float(w.toDouble()));
                }
                break;
            }
        }
    }
}

} // namespace engine::environment
