#include "engine/environment/masks.hpp"

#include <algorithm>
#include <cmath>

#include "engine/environment/random.hpp"

namespace engine::environment {

namespace {
float fade(double d, double reach, double falloff) {
    if (d <= reach) return 1;
    if (falloff <= 0) return 0;
    return float(1 - smoothstep(reach, reach + falloff, d));
}
} // namespace

float maskShapeWeight(const MaskWrite& w, const FeatureInstance& in, const OpGeometry& g, core::WorldPos p) {
    const double scale = in.scale.toDouble();
    const double radius = w.radius * scale, falloff = std::max(0.01, w.falloff * scale);
    const double d = g.distance;
    switch (w.shape) {
        case MaskShape::Footprint:
            if (g.footprint > 0) return std::clamp(g.footprint + float(radius > 0 ? fade(d, radius, falloff) : 0), 0.0f, 1.0f);
            return fade(d, radius, falloff);
        case MaskShape::Bed:
            return fade(d, std::max<double>(g.bedHalf, g.waterHalf) + radius, falloff);
        case MaskShape::Bank: {
            if (g.bankHalf <= 0) return 0;
            const double inner = g.bedHalf;
            const float rise = float(smoothstep(inner - falloff, inner, d));
            return rise * fade(d, g.bankHalf + radius, falloff);
        }
        case MaskShape::Edge: {
            if (g.bankHalf <= 0) return fade(d, radius, falloff);
            return fade(std::abs(d - g.bankHalf), radius, falloff);
        }
        case MaskShape::Ring:
            return fade(std::abs(d - radius), 0, falloff);
        case MaskShape::Fan: {
            const double length = w.length * scale;
            if (!in.spline.empty()) {
                // Below a line: on its low side (negative), out to `length` past the face.
                const double below = -double(g.side) - g.bankHalf;
                if (below < -falloff) return 0;
                return float(smoothstep(-falloff, 0, below)) * fade(std::max(0.0, below), length * 0.4, length * 0.6);
            }
            const double dx = (p.x - in.anchor.x).toDouble(), dy = (p.y - in.anchor.y).toDouble();
            const double dist = std::hypot(dx, dy);
            if (dist < 1e-6) return 1;
            const double along = (dx * in.yawCos.toDouble() + dy * in.yawSin.toDouble()) / dist;
            const double angle = std::acos(std::clamp(along, -1.0, 1.0));
            if (angle > w.spread + 0.3) return 0;
            const float lateral = float(1 - smoothstep(w.spread, w.spread + 0.3, angle));
            return lateral * fade(dist, radius + length * 0.4, length * 0.6);
        }
        case MaskShape::Corridor: {
            const double dx = (p.x - in.anchor.x).toDouble(), dy = (p.y - in.anchor.y).toDouble();
            const double along = dx * in.yawCos.toDouble() + dy * in.yawSin.toDouble();
            const double across = -dx * in.yawSin.toDouble() + dy * in.yawCos.toDouble();
            const double length = w.length * scale;
            if (along < -falloff || along > length + falloff) return 0;
            return fade(std::abs(across), radius, falloff) * fade(std::max(0.0, along - length), 0, falloff) *
                   float(smoothstep(-falloff, 0, along));
        }
    }
    return 0;
}

void applyMaskWrites(const FeatureRecipe& recipe, const FeatureInstance& in, const OpGeometry& g, core::WorldPos p,
                     MaskValues& values) {
    for (const auto& w : recipe.masks) {
        const float shape = maskShapeWeight(w, in, g, p);
        if (shape <= 0) continue;
        float& v = values[w.channelId];
        const float target = w.value * shape;
        switch (w.mode) {
            case MaskMode::Max: v = std::max(v, target); break;
            case MaskMode::Add: v = v + target; break;
            case MaskMode::Min: v = std::min(v, 1 - shape + w.value * shape); break;
            case MaskMode::Replace: v = v + (w.value - v) * shape; break;
        }
        v = std::clamp(v, 0.0f, 1.0f);
    }
}

float groundCover(const CoverRules& cover, const EnvironmentZone& zone, const FieldSample* fields,
                  const MaskValues& masks, double x, double y, std::uint64_t seed) {
    const auto ruleFor = [&](ZoneTypeId type) -> const CoverRule* {
        const CoverRule* any = nullptr;
        for (const auto& r : cover.rules) {
            if (r.tier != CoverTier::Ground) continue;
            if (!r.anyZone && r.zoneId == type) return &r;
            if (r.anyZone && !any) any = &r;
        }
        return any;
    };
    const double contour = (fields ? fields->get(field::Aspect, 0) : 0.0f) + 1.5707963267948966;
    const auto amount = [&](const CoverRule* rule) {
        if (!rule) return 1.0f;
        const CoverInputs in{fields, &zone.scalars, &masks};
        return ruleDensity(*rule, in) * coverPatch(seed, *rule, x, y, contour);
    };
    float total = 0, weight = 0;
    for (std::size_t s = 0; s < kZoneSlots; ++s) {
        const float w = zone.weights.weight[s];
        if (w <= 0) continue;
        total += w * amount(ruleFor(zone.weights.type[s]));
        weight += w;
    }
    if (weight <= 0) return amount(ruleFor(kNoZone));
    return total / weight;
}

PageMasks rasteriseMasks(const FeatureLayer* features, const ZoneField* zones, const ZoneMasks& zoneMasks,
                         const HeightAt& height, double x0, double y0, double step, int side,
                         const CoverRules* cover, const FieldSource* fields, std::uint64_t seed) {
    PageMasks out;
    out.x0 = x0; out.y0 = y0; out.step = step; out.side = side;
    const std::size_t n = std::size_t(side) * side;
    out.channels.assign(n * kMaskChannels, 0);
    out.zones.assign(n * 4, 0);
    std::vector<FeatureLayer::Hit> hits;
    std::shared_ptr<const FeatureLayer::Block> keep;
    // The ground tier, and the fields its rules read, a page at a time.
    bool anyGround = false, wantsFields = false;
    if (cover)
        for (const auto& r : cover->rules)
            if (r.tier == CoverTier::Ground) {
                anyGround = true;
                for (const auto& m : r.modifiers) wantsFields |= m.source == CoverModifier::Source::Field;
            }
    std::vector<FieldSample> sampled;
    if (anyGround && wantsFields && fields) {
        sampled.resize(n);
        fields->sampleGrid(x0, y0, step, side, side, sampled);
    }
    // Zones are cached a page at a time; fetching per sample would take the
    // zone cache's lock a quarter of a million times a page.
    std::shared_ptr<const ZoneGrid> grid;
    std::int64_t gridX = 0, gridY = 0;
    for (int r = 0; r < side; ++r) {
        for (int c = 0; c < side; ++c) {
            const double x = x0 + c * step, y = y0 + r * step;
            const std::size_t i = std::size_t(r) * side + c;
            MaskValues values{};
            EnvironmentZone zone;
            if (zones) {
                const double page = zones->settings().pageMetres;
                const auto px = std::int64_t(std::floor(x / page)), py = std::int64_t(std::floor(y / page));
                if (!grid || px != gridX || py != gridY) { grid = zones->page(px, py); gridX = px; gridY = py; }
                zone = grid->at(x, y);
                out.zones[i * 4] = zone.weights.type[0];
                out.zones[i * 4 + 1] = zone.weights.type[1];
                const float second = zone.weights.weight[0] + zone.weights.weight[1] > 0
                        ? zone.weights.weight[1] / (zone.weights.weight[0] + zone.weights.weight[1]) : 0.0f;
                out.zones[i * 4 + 2] = std::uint8_t(std::lround(std::clamp(second, 0.0f, 1.0f) * 255));
            }
            if (zoneMasks) zoneMasks(x, y, zone, values);
            // A page read coarser than 32 m carries the zones only: a feature is
            // narrower than its samples, and asking for them would plan its
            // whole kilometres of country.
            if (features && step <= 32.0) {
                const core::WorldPos p{quantised(x), quantised(y)};
                hits.clear();
                const auto base = height ? quantised(height(x, y)) : core::kZero;
                features->probe(p, base, hits, &keep);
                for (const auto& h : hits)
                    applyMaskWrites(features->catalogue().recipes()[h.instance->recipe], *h.instance, h.geometry, p, values);
            }
            for (std::size_t k = 0; k < kMaskChannels; ++k)
                out.channels[i * kMaskChannels + k] = std::uint8_t(std::lround(std::clamp(values[k], 0.0f, 1.0f) * 255));
            const float ground = anyGround ? groundCover(*cover, zone, sampled.empty() ? nullptr : &sampled[i], values, x, y, seed) : 1.0f;
            out.zones[i * 4 + 3] = std::uint8_t(std::lround(std::clamp(ground * kCoverEncode, 0.0f, 255.0f)));
        }
    }
    return out;
}

} // namespace engine::environment
