#include "engine/environment/scatter.hpp"

#include <algorithm>
#include <cmath>

#include "engine/environment/random.hpp"

namespace engine::environment {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t pick(const std::vector<ModelChoice>& models, Rng& rng) {
    double total = 0;
    for (const auto& m : models) total += m.weight;
    double at = rng.unit() * total;
    for (const auto& m : models) {
        if (at < m.weight) return m.id;
        at -= m.weight;
    }
    return models.back().id;
}

struct Placer {
    const FeatureInstance& in;
    const ScatterRule& rule;
    std::uint16_t index;
    const GroundAt& ground;
    std::vector<PlacedObject>& out;
    std::size_t first;   // where this rule's objects begin in `out`, for spacing

    bool tooClose(double x, double y, double spacing) const {
        for (std::size_t i = first; i < out.size(); ++i) {
            const double dx = out[i].x - x, dy = out[i].y - y;
            if (dx * dx + dy * dy < spacing * spacing) return true;
        }
        return false;
    }
    void put(Rng& rng, double x, double y, double scale, std::uint32_t model, std::uint64_t salt) {
        if (tooClose(x, y, rule.minSpacing * std::max(0.3, scale))) return;
        PlacedObject o;
        o.id = hashOf(in.id, index, std::int64_t(salt), 0xd7e5);
        o.x = x;
        o.y = y;
        o.z = ground ? ground(x, y) : 0;
        o.scale = float(scale);
        o.yaw = float(rng.range(0, 2 * kPi));
        o.tint = float(rng.range(0.9, 1.1));
        o.sink = float(rule.sink);
        o.model = model;
        o.recipe = in.recipe;
        o.rule = index;
        o.alignToGround = rule.alignToGround;
        out.push_back(o);
    }
};

double splineLength(const FeatureInstance& in) { return in.spline.length.toDouble(); }

// A point on the instance's spline `along` metres in, offset `side` metres to
// the left of travel, and the direction there.
void onSpline(const FeatureInstance& in, double along, double side, double& x, double& y) {
    const auto at = in.spline.pointAt(quantised(along));
    const auto ahead = in.spline.pointAt(quantised(std::min(along + 1.0, splineLength(in))));
    const auto behind = in.spline.pointAt(quantised(std::max(along - 1.0, 0.0)));
    double tx = (ahead.x - behind.x).toDouble(), ty = (ahead.y - behind.y).toDouble();
    const double len = std::hypot(tx, ty);
    if (len > 1e-9) { tx /= len; ty /= len; } else { tx = 1; ty = 0; }
    x = at.x.toDouble() - ty * side;
    y = at.y.toDouble() + tx * side;
}

double bedHalf(const Catalogue& cat, const FeatureInstance& in, bool outer) {
    double half = 0;
    for (const auto& op : cat.recipes()[in.recipe].terrain) {
        if (op.kind == TerrainOpKind::CarveProfile) half = std::max(half, (outer ? op.outerWidth : op.innerWidth) * 0.5);
        if (op.kind == TerrainOpKind::Step && outer) half = std::max(half, op.width * 0.5);
    }
    return half * in.scale.toDouble();
}

} // namespace

void dress(const Catalogue& catalogue, const FeatureInstance& in, const GroundAt& ground, std::vector<PlacedObject>& out) {
    const auto& recipe = catalogue.recipes()[in.recipe];
    const double s = in.scale.toDouble();
    const double ax = in.anchor.x.toDouble(), ay = in.anchor.y.toDouble();
    const double yaw = in.yaw();
    const double elong = std::max(1.0, recipe.composition.elongation);
    for (std::uint16_t ri = 0; ri < recipe.scatter.size(); ++ri) {
        const auto& rule = recipe.scatter[ri];
        if (rule.models.empty()) continue;
        Rng rng(hashOf(in.seed, ri, 0, 0x5ca7));
        Placer placer{in, rule, ri, ground, out, out.size()};
        const int count = rule.countMin + rng.below(std::max(1, rule.countMax - rule.countMin + 1));
        const double radius = rule.radius * s;
        const auto scaleAt = [&](double t) {
            return rng.range(rule.scaleMin, rule.scaleMax) * s * (1 - rule.sizeFalloff * std::clamp(t, 0.0, 1.0));
        };
        switch (rule.primitive) {
            case ScatterPrimitive::Cluster: {
                // Round the anchor, denser in the middle, stretched along the feature.
                for (int i = 0; i < count; ++i) {
                    const double a = rng.range(0, 2 * kPi);
                    const double t = std::sqrt(rng.unit()) * rng.unit();
                    const double lx = std::cos(a) * radius * t * elong, ly = std::sin(a) * radius * t;
                    const double x = ax + lx * std::cos(yaw) - ly * std::sin(yaw);
                    const double y = ay + lx * std::sin(yaw) + ly * std::cos(yaw);
                    placer.put(rng, x, y, scaleAt(t), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::Fan: {
                for (int i = 0; i < count; ++i) {
                    double x, y, t;
                    if (!in.spline.empty()) {
                        // Below the line: debris falls from the face onto its low side.
                        const double along = rng.unit() * splineLength(in);
                        t = std::pow(rng.unit(), 0.7);
                        onSpline(in, along, -(bedHalf(catalogue, in, true) + t * radius), x, y);
                    } else {
                        const double a = yaw + rng.range(-rule.spread, rule.spread);
                        t = std::pow(rng.unit(), 0.7);
                        x = ax + std::cos(a) * t * radius;
                        y = ay + std::sin(a) * t * radius;
                    }
                    placer.put(rng, x, y, scaleAt(t), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::AlongChannel: {
                if (in.spline.empty()) break;
                const double half = std::max(0.5, bedHalf(catalogue, in, false));
                const double length = splineLength(in);
                for (int i = 0; i < count; ++i) {
                    const double along = rng.unit() * length;
                    double x, y;
                    onSpline(in, along, rng.range(-half, half), x, y);
                    // Rounder and smaller downstream.
                    placer.put(rng, x, y, scaleAt(along / std::max(1.0, length)), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::Ring: {
                for (int i = 0; i < count; ++i) {
                    const double a = (i + rng.range(-0.3, 0.3)) * 2 * kPi / count;
                    const double d = radius * rng.range(0.85, 1.15);
                    placer.put(rng, ax + std::cos(a) * d * elong, ay + std::sin(a) * d, scaleAt(0), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::Edge: {
                if (in.spline.empty()) break;
                const double brow = bedHalf(catalogue, in, true);
                const double length = splineLength(in);
                for (int i = 0; i < count; ++i) {
                    double x, y;
                    const double side = (rng.unit() < 0.5 ? -1 : 1) * brow + rng.range(-rule.spread, rule.spread) * 2;
                    onSpline(in, rng.unit() * length, side, x, y);
                    placer.put(rng, x, y, scaleAt(0), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::Line: {
                for (int i = 0; i < count; ++i) {
                    const double t = rng.unit();
                    const double lateral = rng.range(-rule.spread, rule.spread) * radius * 0.2;
                    const double x = ax + std::cos(yaw) * t * radius - std::sin(yaw) * lateral;
                    const double y = ay + std::sin(yaw) * t * radius + std::cos(yaw) * lateral;
                    placer.put(rng, x, y, scaleAt(t), pick(rule.models, rng), i);
                }
                break;
            }
            case ScatterPrimitive::RockHierarchy: {
                // Big pieces near the source, smaller further out, spread
                // along the fall direction (a face's low side, or the yaw).
                const std::size_t levels = std::size(kRockLevels);
                const auto& models = rule.models;
                for (std::size_t level = 0; level < levels; ++level) {
                    const auto& L = kRockLevels[level];
                    const std::size_t m0 = level * models.size() / levels;
                    const std::size_t m1 = std::max(m0 + 1, (level + 1) * models.size() / levels);
                    const std::vector<ModelChoice> these(models.begin() + std::ptrdiff_t(m0),
                                                         models.begin() + std::ptrdiff_t(std::min(m1, models.size())));
                    const int n = L.countMin + rng.below(L.countMax - L.countMin + 1);
                    for (int i = 0; i < n; ++i) {
                        double x, y;
                        const double t = L.reach * std::sqrt(rng.unit());
                        if (!in.spline.empty()) {
                            onSpline(in, rng.unit() * splineLength(in), -(bedHalf(catalogue, in, true) + t * radius), x, y);
                        } else {
                            const double a = yaw + rng.range(-rule.spread, rule.spread);
                            x = ax + std::cos(a) * t * radius;
                            y = ay + std::sin(a) * t * radius;
                        }
                        const double sc = rule.scaleMax * L.scale * s * rng.range(0.8, 1.2);
                        placer.put(rng, x, y, sc, pick(these, rng), level * 64 + i);
                    }
                }
                break;
            }
        }
    }
}

bool keptClear(const FeatureLayer& features, double x, double y) {
    const core::WorldPos p{quantised(x), quantised(y)};
    std::vector<FeatureLayer::Hit> hits;
    features.probe(p, core::kZero, hits);
    const auto& cat = features.catalogue();
    for (const auto& h : hits) {
        const auto& comp = cat.recipes()[h.instance->recipe].composition;
        const double s = h.instance->scale.toDouble();
        const double dx = x - h.instance->anchor.x.toDouble(), dy = y - h.instance->anchor.y.toDouble();
        if (comp.negativeSpace > 0 && dx * dx + dy * dy < comp.negativeSpace * comp.negativeSpace * s * s) return true;
        if (comp.revealWidth > 0 && comp.revealLength > 0) {
            const double c = h.instance->yawCos.toDouble(), sn = h.instance->yawSin.toDouble();
            // The corridor runs back from the anchor: the way the place is approached.
            const double along = -(dx * c + dy * sn), across = -dx * sn + dy * c;
            if (along > 0 && along < comp.revealLength * s && std::abs(across) < comp.revealWidth * 0.5 * s) return true;
        }
    }
    return false;
}

MaskValues masksAt(const FeatureLayer* features, const ZoneField* zones, const ZoneMasks& zoneMasks,
                   const HeightAt& height, double x, double y, EnvironmentZone* zoneOut, const EnvironmentZone* known) {
    MaskValues values{};
    EnvironmentZone zone;
    if (known) zone = *known;
    else if (zones) zone = zones->at(x, y);
    if (zoneMasks) zoneMasks(x, y, zone, values);
    if (features) {
        const core::WorldPos p{quantised(x), quantised(y)};
        std::vector<FeatureLayer::Hit> hits;
        features->probe(p, height ? quantised(height(x, y)) : core::kZero, hits);
        for (const auto& h : hits)
            applyMaskWrites(features->catalogue().recipes()[h.instance->recipe], *h.instance, h.geometry, p, values);
    }
    if (zoneOut) *zoneOut = zone;
    return values;
}

void cover(const CoverContext& ctx, double x0, double y0, double x1, double y1, std::vector<PlacedObject>& out) {
    if (!ctx.catalogue) return;
    const auto& rules = ctx.catalogue->cover().rules;
    const std::size_t start = out.size();
    for (std::uint16_t ri = 0; ri < rules.size(); ++ri) {
        const auto& rule = rules[ri];
        if (rule.tier != CoverTier::Secondary || rule.models.empty() || rule.density <= 0) continue;
        // One candidate clump a lattice cell; the cell is the size that holds
        // the rule's density at full strength. Global lattice: pages agree.
        const double clump = 0.5 * (rule.clusterMin + rule.clusterMax);
        const double cell = std::clamp(std::sqrt(100.0 * clump / rule.density), 0.5, 256.0);
        const auto c0 = std::int64_t(std::floor(x0 / cell)), c1 = std::int64_t(std::ceil(x1 / cell));
        const auto r0 = std::int64_t(std::floor(y0 / cell)), r1 = std::int64_t(std::ceil(y1 / cell));
        for (auto r = r0; r < r1; ++r) {
            for (auto c = c0; c < c1; ++c) {
                if (out.size() - start >= ctx.budget) return;
                Rng rng(hashOf(ctx.seed, c, r, 0xc0e0 + ri));
                const double x = (double(c) + rng.unit()) * cell, y = (double(r) + rng.unit()) * cell;
                if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
                const double draw = rng.unit();
                // The zone first, off its cached page; the fields and the masks
                // only when the rule's modifiers read them.
                EnvironmentZone zone;
                if (ctx.zones) zone = ctx.zones->at(x, y);
                const float share = rule.anyZone ? 1.0f : zone.weights.of(rule.zoneId);
                if (share <= 0) continue;
                bool wantsFields = false, wantsMasks = false;
                for (const auto& m : rule.modifiers) {
                    wantsFields |= m.source == CoverModifier::Source::Field;
                    wantsMasks |= m.source == CoverModifier::Source::Mask;
                }
                FieldSample f;
                if (wantsFields && ctx.fields) ctx.fields->sample(x, y, f);
                MaskValues masks{};
                if (wantsMasks) masks = masksAt(ctx.features, nullptr, ctx.zoneMasks, ctx.height, x, y, nullptr, &zone);
                const CoverInputs inputs{wantsFields ? &f : nullptr, &zone.scalars, &masks};
                const double contour = (wantsFields ? f.get(field::Aspect, 0) : 0.0) + kPi * 0.5;
                const double p = share * ruleDensity(rule, inputs) / rule.density *
                                 coverPatch(ctx.seed + ri, rule, x, y, contour);
                if (draw >= p) continue;
                if (ctx.features && keptClear(*ctx.features, x, y)) continue;
                const int n = rule.clusterMin + rng.below(std::max(1, rule.clusterMax - rule.clusterMin + 1));
                for (int k = 0; k < n; ++k) {
                    const double a = rng.range(0, 2 * kPi), d = k == 0 ? 0 : rule.clusterRadius * std::sqrt(rng.unit());
                    PlacedObject o;
                    o.x = x + std::cos(a) * d;
                    o.y = y + std::sin(a) * d;
                    o.id = hashOf(ctx.seed ^ 0xc0e7ULL, c, r, std::uint64_t(ri) * 64 + k);
                    o.z = ctx.ground ? ctx.ground(o.x, o.y) : 0;
                    o.scale = float(rng.range(rule.scaleMin, rule.scaleMax));
                    o.yaw = float(rng.range(0, 2 * kPi));
                    o.tint = float(rng.range(0.92, 1.08));
                    o.model = pick(rule.models, rng);
                    o.rule = ri;
                    out.push_back(o);
                }
            }
        }
    }
}

} // namespace engine::environment
