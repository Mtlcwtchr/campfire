#include "game/generation/world_brush.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "game/generation/world_noise.hpp"

namespace generation {
namespace {

constexpr std::array<BrushToolDef, kBrushToolCount> kTools{{
        {BrushTool::Paint, "paint", "Paint", false},
        {BrushTool::Raise, "raise", "Raise", false},
        {BrushTool::Lower, "lower", "Lower", false},
        {BrushTool::Smooth, "smooth", "Smooth", false},
        {BrushTool::Erase, "erase", "Erase (back to generated)", false},
        {BrushTool::Continents, "continents", "Continents (pass G1)", true},
        {BrushTool::Ranges, "ranges", "Ranges (plate margin)", true},
        {BrushTool::Hills, "hills", "Hills (pass G3 relief)", true},
}};

// Full strength inside `hardness` of the radius, easing to nothing at the rim.
double falloff(double distance, double radius, double hardness) {
    if (radius <= 0.0 || distance >= radius) return 0.0;
    const double t = distance / radius;
    const double h = std::clamp(hardness, 0.0, 0.98);
    if (t <= h) return 1.0;
    const double s = (t - h) / (1.0 - h);
    return 1.0 - s * s * (3.0 - 2.0 * s);
}

// A plate margin's stress, carried inland: the plates pass spreads it from the
// boundary losing a fixed amount a cell, strongest wins. Along a stroke the
// same thing is a wedge from the line the brush ran down - with a crest as
// wide as the hardness says, because a range has a top as well as sides.
double wedge(double distance, double radius, double hardness) {
    if (radius <= 0.0 || distance >= radius) return 0.0;
    const double t = distance / radius;
    const double h = std::clamp(hardness, 0.0, 0.95) * 0.9;
    return t <= h ? 1.0 : (1.0 - t) / (1.0 - h);
}

std::int32_t cellsOfKm(float km) { return std::int32_t(std::lround(double(km) * 1000.0 / kMetresPerCell)); }

} // namespace

const BrushToolDef& brushToolDef(BrushTool tool) {
    return kTools[std::min(std::size_t(tool), kBrushToolCount - 1)];
}

std::vector<BrushTool> toolsFor(LayerId layer) {
    const std::vector<BrushTool> manual{BrushTool::Paint, BrushTool::Raise, BrushTool::Lower, BrushTool::Smooth,
                                        BrushTool::Erase};
    std::vector<BrushTool> tools;
    if (layer == LayerId::Continents) tools.push_back(BrushTool::Continents);
    if (layer == LayerId::Ranges) tools.push_back(BrushTool::Ranges);
    if (layer == LayerId::Hills) tools.push_back(BrushTool::Hills);
    tools.insert(tools.end(), manual.begin(), manual.end());
    return tools;
}

void TexelRect::add(const TexelRect& o) {
    if (o.empty()) return;
    if (empty()) { *this = o; return; }
    x0 = std::min(x0, o.x0);
    y0 = std::min(y0, o.y0);
    x1 = std::max(x1, o.x1);
    y1 = std::max(y1, o.y1);
}

BrushLimits brushLimits(LayerId layer, const WorldLayout& layout) {
    BrushLimits limits;
    limits.texel = layerDef(layer).texelMetres;
    limits.minRadius = limits.texel;
    // A brush wider than half the world has nothing left to be wider than.
    const double world = double(std::max(layout.widthMetres(), layout.heightMetres())) * 0.5;
    const double widest = std::max(1.0, std::floor(world / limits.texel)) * limits.texel;
    limits.maxRadius = std::max(limits.minRadius, std::min(limits.texel * kMaxBrushTexels, widest));
    return limits;
}

double legalRadius(LayerId layer, const WorldLayout& layout, double radius) {
    const BrushLimits limits = brushLimits(layer, layout);
    const double texels = std::round(radius / limits.texel);
    return std::clamp(texels * limits.texel, limits.minRadius, limits.maxRadius);
}

double dabSpacing(LayerId layer, const Brush& brush) {
    return std::max(double(layerDef(layer).texelMetres) * 0.5, brush.radius * 0.25);
}

float generatorSizeKm(BrushTool tool, std::int32_t worldWidth) {
    if (tool == BrushTool::Continents) return float(continentLobeCells(worldWidth)) * kMetresPerCell / 1000.0f;
    if (tool == BrushTool::Hills) return float(octaveScale(90, worldWidth, 100)) * kMetresPerCell / 1000.0f;
    return 0.0f;
}

float continentsAt(const Brush& brush, std::int32_t worldWidth, std::int32_t cellX, std::int32_t cellY) {
    const std::int32_t lobe = brush.sizeKm > 0.0f ? cellsOfKm(brush.sizeKm) : continentLobeCells(worldWidth);
    return float(continentShape(brush.seed, cellX, cellY, std::clamp(lobe, 4, 4096)));
}

float hillsAt(const Brush& brush, std::int32_t worldWidth, std::int32_t cellX, std::int32_t cellY) {
    // The relief noise's octaves follow the world's width from the longest
    // one down; asking for another size is asking for the width at which the
    // longest one is that size.
    std::int32_t width = worldWidth;
    if (brush.sizeKm > 0.0f) width = std::max(1, cellsOfKm(brush.sizeKm) * kReferenceWidth / 90);
    const float noise = float(landNoise(brush.seed, cellX, cellY, width));
    return 512.0f + (noise - 512.0f) * std::clamp(brush.amount, 0.0f, 4.0f);
}

float effectiveAt(const LayerMap& map, const LayerBase* base, std::int32_t tx, std::int32_t ty) {
    const LayerDef& def = map.def();
    if (def.kind == LayerKind::Delta) return map.value(tx, ty);
    const float made = base ? base->at(tx, ty) : (def.low + def.high) * 0.5f;
    const float c = map.cover(tx, ty);
    return made * (1.0f - c) + map.value(tx, ty) * c;
}

TexelRect dab(WorldLayout& layout, LayerId layer, const Brush& brush, double x, double y, const LayerBase* base) {
    TexelRect changed;
    const LayerDef& def = layerDef(layer);
    LayerMap& map = layout.layer(layer);
    const std::vector<BrushTool> allowed = toolsFor(layer);
    if (std::find(allowed.begin(), allowed.end(), brush.tool) == allowed.end()) return changed;
    if (map.texelsX() <= 0 || map.texelsY() <= 0) return changed;

    const double radius = legalRadius(layer, layout, brush.radius);
    const double texel = def.texelMetres;
    // Every texel whose centre is inside the brush.
    const std::int32_t tx0 = std::max(0, std::int32_t(std::ceil((x - radius) / texel - 0.5)));
    const std::int32_t ty0 = std::max(0, std::int32_t(std::ceil((y - radius) / texel - 0.5)));
    const std::int32_t tx1 = std::min(map.texelsX() - 1, std::int32_t(std::floor((x + radius) / texel - 0.5)));
    const std::int32_t ty1 = std::min(map.texelsY() - 1, std::int32_t(std::floor((y + radius) / texel - 0.5)));
    if (tx1 < tx0 || ty1 < ty0) return changed;

    const bool delta = def.kind == LayerKind::Delta;
    const float span = def.high - def.low;
    const std::int32_t worldWidth = layout.widthCells();
    const double perCell = texel / kMetresPerCell;

    // Smooth reads its neighbours as they were before this dab, so the result
    // does not depend on the order the texels are visited in.
    std::vector<float> before;
    const std::int32_t sx0 = std::max(0, tx0 - 1), sy0 = std::max(0, ty0 - 1);
    const std::int32_t sx1 = std::min(map.texelsX() - 1, tx1 + 1), sy1 = std::min(map.texelsY() - 1, ty1 + 1);
    const std::int32_t sw = sx1 - sx0 + 1;
    if (brush.tool == BrushTool::Smooth) {
        before.resize(std::size_t(sw) * std::size_t(sy1 - sy0 + 1));
        for (std::int32_t ty = sy0; ty <= sy1; ++ty)
            for (std::int32_t tx = sx0; tx <= sx1; ++tx)
                before[std::size_t(ty - sy0) * std::size_t(sw) + std::size_t(tx - sx0)] =
                        effectiveAt(map, base, tx, ty);
    }
    const auto was = [&](std::int32_t tx, std::int32_t ty) {
        tx = std::clamp(tx, sx0, sx1);
        ty = std::clamp(ty, sy0, sy1);
        return before[std::size_t(ty - sy0) * std::size_t(sw) + std::size_t(tx - sx0)];
    };

    for (std::int32_t ty = ty0; ty <= ty1; ++ty)
        for (std::int32_t tx = tx0; tx <= tx1; ++tx) {
            const double cx = (tx + 0.5) * texel, cy = (ty + 0.5) * texel;
            const double distance = std::hypot(cx - x, cy - y);
            const double w = falloff(distance, radius, brush.hardness);
            if (w <= 0.0 && brush.tool != BrushTool::Ranges) continue;
            const float v = map.value(tx, ty), c = map.cover(tx, ty);
            const float a = std::clamp(float(w) * brush.strength, 0.0f, 1.0f);
            float nextValue = v, nextCover = c;

            if (brush.tool == BrushTool::Erase) {
                if (delta) {
                    nextValue = v * (1.0f - a);
                    if (std::abs(nextValue) < span * 1e-3f) nextValue = 0.0f;
                } else {
                    nextCover = c * (1.0f - a);
                    if (nextCover < 1e-3f) nextCover = 0.0f;
                }
            } else if (brush.tool == BrushTool::Ranges) {
                // The strongest thing to reach a place is what it keeps, as it
                // is for the plates: a stroke is a range, not a pile of them.
                const double lift = wedge(distance, radius, brush.hardness) * brush.strength * def.high;
                if (lift <= 0.0) continue;
                nextValue = std::max(v, float(lift));
            } else {
                // Everything else moves the place towards a target.
                const std::int32_t cellX = std::min(worldWidth - 1, std::int32_t((tx + 0.5) * perCell));
                const std::int32_t cellY = std::min(layout.heightCells() - 1, std::int32_t((ty + 0.5) * perCell));
                float target = 0.0f;
                switch (brush.tool) {
                    case BrushTool::Paint: target = brush.value; break;
                    case BrushTool::Raise: target = effectiveAt(map, base, tx, ty) + span * 0.1f; break;
                    case BrushTool::Lower: target = effectiveAt(map, base, tx, ty) - span * 0.1f; break;
                    case BrushTool::Smooth:
                        target = (was(tx, ty) * 4.0f +
                                  (was(tx - 1, ty) + was(tx + 1, ty) + was(tx, ty - 1) + was(tx, ty + 1)) * 2.0f +
                                  was(tx - 1, ty - 1) + was(tx + 1, ty - 1) + was(tx - 1, ty + 1) +
                                  was(tx + 1, ty + 1)) / 16.0f;
                        break;
                    case BrushTool::Continents: target = continentsAt(brush, worldWidth, cellX, cellY); break;
                    case BrushTool::Hills: target = hillsAt(brush, worldWidth, cellX, cellY); break;
                    default: continue;
                }
                target = std::clamp(target, def.low, def.high);
                if (delta) {
                    nextValue = v + (target - v) * a;
                } else {
                    // Premultiplied: the new cover is the old one plus the
                    // dab's share of what was left, and the value is the
                    // weighted mean of the old paint and the target. What the
                    // generator made underneath cancels out, which is why
                    // Paint and the procedural brushes need no base.
                    nextCover = c + a * (1.0f - c);
                    nextValue = nextCover > 0.0f ? (v * c * (1.0f - a) + target * a) / nextCover : v;
                }
            }
            nextValue = std::clamp(nextValue, def.low, def.high);
            if (nextValue == v && nextCover == c) continue;
            map.set(tx, ty, nextValue, nextCover);
            changed.add({tx, ty, tx, ty});
        }
    if (!changed.empty()) map.prune(changed.x0, changed.y0, changed.x1, changed.y1);
    return changed;
}

std::vector<std::pair<double, double>> strokeDabs(double fromX, double fromY, double toX, double toY,
                                                  double spacing, double& carry) {
    std::vector<std::pair<double, double>> dabs;
    spacing = std::max(spacing, 1.0);
    const double length = std::hypot(toX - fromX, toY - fromY);
    double along = spacing - carry;
    while (along <= length) {
        const double t = length > 0.0 ? along / length : 0.0;
        dabs.emplace_back(fromX + (toX - fromX) * t, fromY + (toY - fromY) * t);
        along += spacing;
    }
    carry = length - (along - spacing);
    return dabs;
}

} // namespace generation

